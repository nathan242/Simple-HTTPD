# Simple HTTPD

Simple HTTP web server I built for learning purposes.

Compile with:
g++ httpd.cpp

Usage: ./httpd -p [TCP_PORT] -r [WEB_ROOT] -i [DEFAULT_INDEX] -l
 -p [TCP_PORT] - TCP port to listen on. Default = 80.
 -r [WEB_ROOT] - Directory for web root. Default = current working directory.
 -i [DEFAULT_INDEX] - Default index file. Default = "index.html".
 -l - Enable logging to STDOUT.

