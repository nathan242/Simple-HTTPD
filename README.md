# Simple HTTPD

Simple HTTP web server I built for learning purposes.

Compile with:

g++ httpd.cpp -lmagic


Simple HTTP Daemon v0.01.
Usage: ./httpd -p [TCP_PORT] -r [WEB_ROOT] -i [DEFAULT_INDEX] -a -l
 -p [TCP_PORT] - TCP port to listen on. Default = 80.
 -r [WEB_ROOT] - Directory for web root. Default = current working directory.
 -i [DEFAULT_INDEX] - Default index file. Default = "index.html".
 -a - Use automatic directory index when default index is missing.
 -l - Enable logging to STDOUT.

