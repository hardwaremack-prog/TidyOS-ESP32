// TidyOS in-app update from GitHub (see update.cpp)
#pragma once
#include <string>
bool updateLoaderPresent();                 // the board has the update loader (installed by the web flasher)
void updateRequest();          // restart into update mode (runs in black & white, colours come back after)
bool updateRestoreColors();    // at a normal start: put the colour setting back after an update                       // restart into update mode
bool updateModeRequested();                 // true once after updateRequest() (clears the flag)
void updateRun(void (*status)(const char * msg, int percent));   // never returns: restarts
