// TidyOS file sharing: a small web page (like Shelf Lite) for copying files to and
// from the SD card from any phone or computer on the same Wi-Fi.
#pragma once
#include <string>

void        fileServerSet(bool on);    // starts / stops it (and remembers the choice)
bool        fileServerOn();
std::string fileServerUrl();           // "http://192.168.1.40" or "" when off / no Wi-Fi
void        netMdnsBegin();            // tidyos.local (shared with printer search)
