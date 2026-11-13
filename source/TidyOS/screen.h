// TidyOS HTML5 edition: the desktop is shown and controlled in a web browser
#pragma once
#include <string>

#define SCREEN_AP_NAME "TidyOS-Setup"
#define SCREEN_AP_PASS "tidyos123"

#ifdef TIDYOS_HTML5
void        screenBegin();       // start the web screen server (+ setup hotspot when needed)
bool        screenViewer();      // a browser is connected
bool        screenApOn();        // the TidyOS-Setup hotspot is running
std::string screenUrl();         // where to open the desktop, e.g. http://192.168.1.40
#else
inline void        screenBegin()  {}
inline bool        screenViewer() { return false; }
inline bool        screenApOn()   { return false; }
inline std::string screenUrl()    { return ""; }
#endif
