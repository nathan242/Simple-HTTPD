#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <csignal>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <magic.h>

using namespace std;

//const string server_identity = "NPHTTPD/0.01";
const string server_header = "Server: NPHTTPD/0.01\r\n";

// Global variables
int port = 80;
string webroot (".");
string index_page ("index.html");
string magic_db;
bool auto_index = false;
bool logging = false;

magic_t magic;

struct reqparam {
    string method;
    string path;
    string host;
    string useragent;
    string accept;
};

struct respparam {
    int code;
    string location;
};

void help(char *argv);
int server();
int worker(int conn, char *client_address);
void check_redirect(string &filepath, string &path, string &redirect_path);
bool file_exists(string &filepath);
bool dir_exists(string &filepath);
void http_response(int conn, respparam &response);
void url_decode(string &path);
void exit_signal_handler(int signum);

int main(int argc, char *argv[])
{
    int opt;
    int status = 0;


    while ((opt = getopt(argc, argv, "halp:r:i:m:")) != -1) {
        switch (opt) {
            case 'p':
                port = atoi(optarg);
                break;
            case 'r':
                webroot.assign(optarg);
                break;
            case 'i':
                index_page.assign(optarg);
                break;
            case 'm':
                magic_db.assign(optarg);
                break;
            case 'a':
                auto_index = true;
                break;
            case 'l':
                logging = true;
                break;
            case 'h':
            default:
                help(argv[0]);
                return 0;
        }
    }

    if (!dir_exists(webroot)) {
        cerr << "Webroot does not exist or is not a directory!" << endl;
        exit(2);
    }

    magic = magic_open(MAGIC_MIME);
    if (magic_db.length() > 0) {
        status = magic_load(magic, magic_db.c_str());
    } else {
        status = magic_load(magic, NULL);
    }

    if (status == -1) {
        cerr << "Failed to load magic database!" << endl;
        exit(2);
    }

    cout << "HTTPD starting on port " << port << "." << endl;
    cout << "Webroot: " << webroot << endl;

    server();

    return 0;
}

void help(char *argv)
{
    cout << "Simple HTTP Daemon v0.01." << endl;
    cout << "Usage: " << argv << " -p [TCP_PORT] -r [WEB_ROOT] -i [DEFAULT_INDEX] -m [MAGIC_DB] -a -l" << endl;
    cout << " -p [TCP_PORT] - TCP port to listen on. Default = 80." << endl;
    cout << " -r [WEB_ROOT] - Directory for web root. Default = current working directory." << endl;
    cout << " -i [DEFAULT_INDEX] - Default index file. Default = \"index.html\"." << endl;
    cout << " -m [MAGIC_DB] - Path to alternate magic database." << endl;
    cout << " -a - Use automatic directory index when default index is missing." << endl;
    cout << " -l - Enable logging to STDOUT." << endl;
}

int server()
{
    int error = 0;

    int sock;
    int conn;
    int sockopt = 1;
    pid_t pid;
    struct sockaddr_in serv_addr, cli_addr;
    socklen_t cli_len = sizeof(cli_addr);


    // Register signal handlers
    signal(SIGINT, exit_signal_handler);
    signal(SIGTERM, exit_signal_handler);

    // Create server socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) { 
        error = errno;
        cerr << "Failed to create socket! Error: " << error << endl;
        exit(2);
    }
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof(sockopt));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    if (bind(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) == -1) {
        error = errno;
        cerr << "Failed to bind to socket! Error: " << error << endl;
        exit(2);
    }
    listen(sock, 0);

    // Wait for connection and fork
    while (1) {
        conn = accept(sock, (sockaddr *) &cli_addr, &cli_len);

        pid = fork();

        if (pid == 0) {
            // We are a child
            close(sock);
            worker(conn, inet_ntoa(cli_addr.sin_addr));
            exit(0);
        } else {
            close(conn);
            // Remove all dead children
            while (waitpid(-1, NULL, WNOHANG) != 0) {}
        }
    }

    return 0;
}

int worker(int conn, char *client_address)
{
    char recvbuf[4096];
    string recvdata;
    string line;
    int parampos;
    string param;
    string value;

    reqparam request;
    respparam response;

    string filepath;
    FILE *file;
    long filesize;
    string filesizestr;
    ostringstream intconv;
    size_t readbytes;
    char filebuf[4096];

    struct dirent *dp;
    string auto_index_page;
    #ifdef _DIRENT_HAVE_D_TYPE
    bool idx_is_dir = false;
    #endif

    string file_mime;

    string redirect_path;


    // Clear receive buffer
    memset(recvbuf, 0, 4096);
    recvdata.clear();
    line.clear();

    // Read request
    while (read(conn, recvbuf, 4096)) {
        // Concatenate received data
        recvdata += recvbuf;
        // Check if end of request received
        if (recvdata.find("\r\n\r\n") != string::npos) { break; }
        // Clear receive buffer
        memset(recvbuf, 0, 4096);
    }

    // Interpret request
    istringstream recvdatas(recvdata);
    line.clear();

    // Get HTTP method
    while (getline(recvdatas, line)) {
        if (line.find("HTTP/1.1") != string::npos) {
            // Get method
            parampos = line.find(" ");
            if (parampos == string::npos) {
                // Invalid request
                response.code = 501;
                http_response(conn, response);
                shutdown(conn, SHUT_RDWR);
                close(conn);
                return 0;
            }
            request.method.assign(line.substr(0, parampos));
            line.assign(line.substr(parampos+1));

            // Get path
            parampos = line.find(" ");
            if (parampos == string::npos) {
                // Invalid request
                response.code = 501;
                http_response(conn, response);
                shutdown(conn, SHUT_RDWR);
                close(conn);
                return 0;
            }
            request.path.assign(line.substr(0, parampos));

            break;
        }
    }

    // Parse rest of request
    while (getline(recvdatas, line)) {
        param.clear();
        value.clear();
        parampos = line.find(": ");
        if (parampos == string::npos) {
            param.assign(line);
        } else {
            param.assign(line.substr(0, parampos));
            value.assign(line.substr(parampos+2));
            value.assign(value.substr(0, value.length()-1));
        }
        if (strcasecmp(param.c_str(), "Host") == 0) { request.host.assign(value); }
        else if (strcasecmp(param.c_str(), "User-Agent") == 0) { request.useragent.assign(value); }
        else if (strcasecmp(param.c_str(), "Accept") == 0) { request.accept.assign(value); }
    }

    // Respond to request
    if (request.method == "GET") {
        // Check the requested file
        url_decode(request.path);
        filepath.assign(webroot);
        filepath.append(request.path);
        check_redirect(filepath, request.path, redirect_path);
        if (redirect_path.length() == 0) {
            // Remove reverse path elements
            parampos = filepath.find("../");
            while (parampos != string::npos) {
                filepath.erase(parampos, 1);
                parampos = filepath.find("../");
            }

            // If no file specified, load the page index
            if (filepath.compare(filepath.length()-1, 1, "/") == 0) {
                if (dir_exists(filepath)) {
                    filepath.append(index_page);
                    if (auto_index) {
                        if (!file_exists(filepath)) {
                            // Auto index
                            parampos = filepath.rfind("/");
                            filepath.erase(parampos+1, string::npos);

                            DIR *dirp = opendir(filepath.c_str());
                            auto_index_page.assign("<!doctype html>\r\n  <head>\r\n    <title>");
                            auto_index_page.append(request.path);
                            auto_index_page.append("</title>\r\n  </head>\r\n  <body>\r\n    <a href=\"..\">&lt;&lt;</a><h2>PATH: ");
                            auto_index_page.append(request.path);
                            auto_index_page.append("</h2>\r\n");
                            while ((dp = readdir(dirp)) != NULL) {
                                #ifdef _DIRENT_HAVE_D_TYPE
                                idx_is_dir = false;
                                #endif
                                if (strcmp(dp->d_name, ".") != 0 && strcmp(dp->d_name, "..") != 0) {
                                    auto_index_page.append("    <p><a href=\"");
                                    auto_index_page.append(dp->d_name);
                                    #ifdef _DIRENT_HAVE_D_TYPE
                                    if (dp->d_type == DT_DIR) {
                                        idx_is_dir = true;
                                        auto_index_page.append("/");
                                    }
                                    #endif
                                    auto_index_page.append("\">");
                                    auto_index_page.append(dp->d_name);
                                    #ifdef _DIRENT_HAVE_D_TYPE
                                    if (idx_is_dir) {
                                        auto_index_page.append("/");
                                    }
                                    #endif
                                    auto_index_page.append("</a></p>\r\n");
                                }
                            }
                            closedir(dirp);
                            auto_index_page.append("  </body>\r\n</html>");

                            intconv.str("");
                            intconv << "Content-Length: " << auto_index_page.length() << "\r\n";
                            filesizestr = intconv.str();

                            // Write output response
                            write(conn, "HTTP/1.1 200 OK\r\n", 17);
                            write(conn, server_header.c_str(), server_header.length());
                            write(conn, filesizestr.c_str(), filesizestr.length());
                            write(conn, "Content-Type: text/html\r\n", 25);
                            write(conn, "\r\n", 2);
                            write(conn, auto_index_page.c_str(), auto_index_page.length());
                        }
                    }
                }
            }
            
            if (auto_index_page.length() == 0) {
                // Try to open file
                if (file_exists(filepath) && (file = fopen(filepath.c_str(), "r"))) {
                    // Get file magic
                    file_mime = magic_file(magic, filepath.c_str());
                    intconv.str("");
                    intconv << "Content-Type: ";
                    if (file_mime.length() > 0) {
                        intconv << file_mime << "\r\n";
                    } else {
                        intconv << "text/html\r\n";
                    }
                    file_mime = intconv.str();
                    // Get file size
                    fseek(file, 0, SEEK_END);
                    filesize = ftell(file);
                    rewind(file);
                    intconv.str("");
                    intconv << "Content-Length: " << filesize << "\r\n";
                    filesizestr = intconv.str();

                    // Write output response
                    write(conn, "HTTP/1.1 200 OK\r\n", 17);
                    write(conn, server_header.c_str(), server_header.length());
                    write(conn, filesizestr.c_str(), filesizestr.length());
                    write(conn, file_mime.c_str(), file_mime.length());
                    write(conn, "\r\n", 2);
                    while (ftell(file) < filesize) {
                        readbytes = fread(filebuf,1,4096,file);
                        write(conn, filebuf, readbytes);
                    }

                } else {
                    // 404
                    response.code = 404;
                    http_response(conn, response);
                }
            }
        } else {
            // 301 Redirect
            response.code = 301;
            response.location.assign("Location: ");
            response.location.append(redirect_path);
            response.location.append("\r\n");
            http_response(conn, response);
        }
    } else {
        // 501 Invalid method
        response.code = 501;
        http_response(conn, response);
    }

    // End of request
    shutdown(conn, SHUT_RDWR);
    close(conn);

    if (logging) {
        cout << "CLIENT: " << client_address << " METHOD: " << request.method << " PATH: " << request.path << " HOST: " << request.host << " USERAGENT: " << request.useragent << " ACCEPT: " << request.accept << endl;
    }

    return 0;
}

void check_redirect(string &filepath, string &path, string &redirect_path)
{
    struct stat path_stat;
    stat(filepath.c_str(), &path_stat);

    // Is it a directory?
    if (filepath.compare(filepath.length()-1, 1, "/") != 0 && S_ISDIR(path_stat.st_mode)) {
        redirect_path.assign(path);
        redirect_path.append("/");
    }

    return;
}

bool file_exists(string &filepath)
{
    struct stat path_stat;
    stat(filepath.c_str(), &path_stat);

    // Is it a file?
    if (S_ISREG(path_stat.st_mode)) {
        return true;
    } else {
        return false;
    }
}

bool dir_exists(string &filepath)
{
    struct stat path_stat;
    stat(filepath.c_str(), &path_stat);

    // Is it a dir?
    if (S_ISDIR(path_stat.st_mode)) {
        return true;
    } else {
        return false;
    }
}

void http_response(int conn, respparam &response)
{
    if (response.code == 404) {
        write(conn, "HTTP/1.1 404 Not Found\r\n", 24);
        write(conn, server_header.c_str(), server_header.length());
        write(conn, "Content-Length: 142\r\n", 21);
        write(conn, "Content-Type: text/html\r\n", 25);
        write(conn, "\r\n", 2);
        write(conn, "<!doctype html>\r\n  <head>\r\n    <title>HTTP 404 - NOT FOUND</title>\r\n  </head>\r\n  <body>\r\n    <h1>HTTP 404 - NOT FOUND</h1>\r\n  </body>\r\n</html>", 142);
    } else if (response.code == 301) {
        write(conn, "HTTP/1.1 301 Moved Permanently\r\n", 32);
        write(conn, server_header.c_str(), server_header.length());
        write(conn, response.location.c_str(), response.location.length());
        write(conn, "Content-Length: 158\r\n", 21);
        write(conn, "Content-Type: text/html\r\n", 25);
        write(conn, "\r\n", 2);
        write(conn, "<!doctype html>\r\n  <head>\r\n    <title>HTTP 301 - MOVED PERMANENTLY</title>\r\n  </head>\r\n  <body>\r\n    <h1>HTTP 301 - MOVED PERMANENTLY</h1>\r\n  </body>\r\n</html>", 158);
    } else if (response.code == 501) {
        write(conn, "HTTP/1.1 501 Method Not Implemented\r\n", 37);
        write(conn, server_header.c_str(), server_header.length());
        write(conn, "Allow: GET\r\n", 12);
        write(conn, "Content-Length: 168\r\n", 21);
        write(conn, "Content-Type: text/html\r\n", 25);
        write(conn, "\r\n", 2);
        write(conn, "<!doctype html>\r\n  <head>\r\n    <title>HTTP 501 - METHOD NOT IMPLEMENTED</title>\r\n  </head>\r\n  <body>\r\n    <h1>HTTP 501 - METHOD NOT IMPLEMENTED</h1>\r\n  </body>\r\n</html>", 168);
    }

    return;
}

void url_decode(string &path)
{
    int pos;

    pos = path.find("%20");
    while (pos != string::npos) {
        path.erase(pos,2);
        path.replace(pos, 1 , " ");
        pos = path.find("%20");
    }

    return;
}

void exit_signal_handler(int signum)
{
    cout << "Signal " << signum << " received." << endl;
    cout << "Terminating..." << endl;
    exit(0);
}
