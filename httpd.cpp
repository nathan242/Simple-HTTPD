#include <unistd.h>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

using namespace std;

// Global variables
int port = 80;
string webroot (".");
string index_page ("index.html");
bool logging = false;

void help(char *argv);
int server();
int worker(int conn, char *client_address);
void check_redirect(string &filepath, string &path, string &redirect_path);

int main(int argc, char *argv[])
{
    int opt;


    while ((opt = getopt(argc, argv, "hlp:r:i:")) != -1) {
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
            case 'l':
                logging = true;
                break;
            case 'h':
            default:
                help(argv[0]);
                return 0;
        }
    }

    cout << "HTTPD starting on port " << port << "." << endl;
    cout << "Webroot: " << webroot << endl;

    server();

    return 0;
}

void help(char *argv)
{
    cout << "Simple HTTP Daemon v0.01." << endl;
    cout << "Usage: " << argv << " -p [TCP_PORT] -r [WEB_ROOT] -i [DEFAULT_INDEX] -l" << endl;
    cout << " -p [TCP_PORT] - TCP port to listen on. Default = 80." << endl;
    cout << " -r [WEB_ROOT] - Directory for web root. Default = current working directory." << endl;
    cout << " -i [DEFAULT_INDEX] - Default index file. Default = \"index.html\"." << endl;
    cout << " -l - Enable logging to STDOUT." << endl;
}

int server()
{
    int sock;
    int conn;
    int sockopt = 1;
    int pid;
    struct sockaddr_in serv_addr, cli_addr;
    socklen_t cli_len = sizeof(cli_addr);


    // Create server socket
    sock = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &sockopt, sizeof(sockopt));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);

    bind(sock, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
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

    struct reqparam {
        string method;
        string path;
        string host;
        string useragent;
        string accept;
    } request;

    string filepath;
    FILE *file;
    long filesize;
    string filesizestr;
    ostringstream intconv;
    size_t readbytes;
    char filebuf[4096];

    string location_string;
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
            }
            request.method.assign(line.substr(0, parampos));
            line.assign(line.substr(parampos+1));

            // Get path
            parampos = line.find(" ");
            if (parampos == string::npos) {
                // Invalid request
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
            
            // Try to open file
            if (file = fopen(filepath.c_str(), "r")) {
                // Get file size
                fseek(file, 0, SEEK_END);
                filesize = ftell(file);
                rewind(file);
                intconv.str("");
                intconv << "Content-Length: " << filesize << "\r\n";
                filesizestr = intconv.str();

                // Write output response
                write(conn, "HTTP/1.1 200 OK\r\n", 17);
                write(conn, "Server: SIMPLE-HTTPD/0.01\r\n", 27);
                write(conn, filesizestr.c_str(), filesizestr.length());
                write(conn, "Content-Type: text/html\r\n", 25);
                write(conn, "\r\n", 2);
                while (ftell(file) < filesize) {
                    readbytes = fread(filebuf,1,4096,file);
                    write(conn, filebuf, readbytes);
                }

            } else {
                // 404
                write(conn, "HTTP/1.1 404 Not Found\r\n", 24);
                write(conn, "Server: SIMPLE-HTTPD/0.01\r\n", 27);
                write(conn, "Content-Length: 142\r\n", 21);
                write(conn, "Content-Type: text/html\r\n", 25);
                write(conn, "\r\n", 2);
                write(conn, "<!doctype html>\r\n  <head>\r\n    <title>HTTP 404 - NOT FOUND</title>\r\n  </head>\r\n  <body>\r\n    <h1>HTTP 404 - NOT FOUND</h1>\r\n  </body>\r\n</html>", 142);
            }
        } else {
            // 301 Redirect
            location_string.assign("Location: ");
            location_string.append(redirect_path);
            location_string.append("\r\n");
            write(conn, "HTTP/1.1 301 Moved Permanently\r\n", 32);
            write(conn, "Server: SIMPLE-HTTPD/0.01\r\n", 27);
            write(conn, location_string.c_str(), location_string.length());
            write(conn, "Content-Length: 158\r\n", 21);
            write(conn, "Content-Type: text/html\r\n", 25);
            write(conn, "\r\n", 2);
            write(conn, "<!doctype html>\r\n  <head>\r\n    <title>HTTP 301 - MOVED PERMANENTLY</title>\r\n  </head>\r\n  <body>\r\n    <h1>HTTP 301 - MOVED PERMANENTLY</h1>\r\n  </body>\r\n</html>", 158);
        }
    } else {
        // Invalid method
        write(conn, "HTTP/1.1 501 Method Not Implemented\r\n", 37);
        write(conn, "Server: SIMPLE-HTTPD/0.01\r\n", 27);
        write(conn, "Allow: GET\r\n", 12);
        write(conn, "Content-Length: 168\r\n", 21);
        write(conn, "Content-Type: text/html\r\n", 25);
        write(conn, "\r\n", 2);
        write(conn, "<!doctype html>\r\n  <head>\r\n    <title>HTTP 501 - METHOD NOT IMPLEMENTED</title>\r\n  </head>\r\n  <body>\r\n    <h1>HTTP 501 - METHOD NOT IMPLEMENTED</h1>\r\n  </body>\r\n</html>", 168);
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
    if (S_ISDIR(path_stat.st_mode)) {
        redirect_path.assign(path);
        // Does the path end with /?
        if (path.compare(path.length()-1, 1, "/") != 0) {
            redirect_path.append("/");
        }
        redirect_path.append(index_page);
    }

    return;
}

