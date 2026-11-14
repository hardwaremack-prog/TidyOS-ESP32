// TidyOS in-app update from GitHub (see update.cpp)
#pragma once
#include <string>
bool updateLoaderPresent();                 // the board has the update loader (installed by the web flasher)
void updateRequest();                       // restart into update mode
bool updateModeRequested();                 // true once after updateRequest() (clears the flag)
void updateRun(void (*status)(const char * msg, int percent));   // never returns: restarts
