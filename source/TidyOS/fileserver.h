// TidyOS file sharing: a small web page (like Shelf Lite) for copying files to and
// from the SD card from any phone or computer on the same Wi-Fi.
#pragma once
#include <string>

void        fileServerSet(bool on);    // starts / stops it (and remembers the choice)
bool        fileServerOn();
std::string fileServerUrl();           // "http://192.168.1.40" or "" when off / no Wi-Fi
void        netMdnsBegin();            // tidyos.local (shared with printer search)
// Music / pictures in the browser: runs the file server without changing the sharing setting
void        fileServerMedia(bool on);
std::string fileServerMediaPath(const std::string & file);
std::string fileServerRunPath(const std::string & file);    // path-style URL for web pages (relative links work)   // "/api/file?path=..." on port fileServerPort()
int         fileServerPort();
