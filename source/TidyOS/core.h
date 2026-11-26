// TidyOS core: theme, settings, storage helpers, shared declarations
#pragma once
#include <esp_heap_caps.h>
// Memory that ordinary data can use. (ESP.getFreeHeap() also counts the 32-bit-only part of
// instruction RAM, which strings, lists and buffers can't use, so it reads too high.)
inline uint32_t ramFree()    { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }
inline uint32_t ramBiggest() { return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT); }
inline uint32_t ramLowest()  { return heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT); }

#include <Arduino.h>
#include <string>
#include <vector>
#include <functional>
#include "fabgl.h"
#include "fabui.h"
#include "config.h"

using fabgl::RGB888;

// ---------------------------------------------------------------- theme
struct Theme {
  RGB888 desk;        // desktop background
  RGB888 deskText;    // icon labels on desktop
  RGB888 win;         // window / text area background
  RGB888 face;        // buttons, bars
  RGB888 shadow;      // dark lines
  RGB888 text;        // normal text
  RGB888 accent;      // title bars, selection, "Saved" light
  RGB888 accentText;  // text on accent
  RGB888 good;        // positive (paid, done)
  RGB888 warn;        // negative (due, errors)
};
extern Theme T;
extern int  gColors;        // 4, 8 or 16 colours actually running
extern bool gSDMounted;
extern bool gFlashMounted;

void themeSetup(fabgl::BitmappedDisplayController * dc, int colors);
fabgl::uiStyle * themeStyle();

// ---------------------------------------------------------------- settings (NVS)
struct Settings {
  String ssid;
  String pass;
  String proxy;       // "192.168.1.50:8088"  (browser helper on The Shelf / a Pi)
  String home;        // browser home page
  int    tzHours10;   // UTC offset * 10   (e.g. -50 for Detroit EST)
  bool   dst;
  int    colors;      // 0 = auto, else 16 / 8 / 4
  String btNames;     // ';' separated names of paired keyboards / mice
  String printer;     // last network printer address (ipp://...)
  bool   fileShare;   // file sharing web page on
  bool   sdOn;        // use the microSD card (off by default: saves ~23 KB of RAM)
  bool   meshOn;      // Coms wireless mesh (ESP-NOW) on (off by default)
  bool   ledOn;       // blue status light on (off by default)
  bool shelfOn = false;   // Shelf messages over Wi-Fi (start-up switch)
  bool oledSleep = true;  // the little OLED goes off after 5 minutes (BOOT wakes it)
  bool tidyBands = false; // Tidy: shade every other row
  bool hotspot = false;   // keep the TidyOS hotspot running (devices join TidyOS directly)
  bool tidyFreeze = false;// Tidy: keep row 1 at the top
  bool alarmOn = false;   // Clock: alarm set
  int  alarmMin = 420;    // Clock: alarm time, minutes after midnight
  int  alarmPresets[4] = { 360, 390, 420, 480 };   // Clock: saved alarm times
  int  worldCity = 0;     // Clock: world time city
  String storeUrl;        // Git-Store relay address
  int    neatFont;    // Neat Lite font family (index)
  int    neatSize;    // Neat Lite font size (index within the family)
};
extern Settings gSet;
void settingsLoad();
void settingsSave();

// ---------------------------------------------------------------- storage
// Where TidyOS keeps its own data files ("/SD/tidyos" or "/flash")
std::string dataPath(const char * name);
std::string documentsDir();               // default folder for user documents
bool  sdTurnOn();                          // mount the card now (and from now on); false if not found
void  sdTurnOff();                         // unmount it now (and from now on); files go to internal flash
bool  readFileStr(const char * path, std::string & out, size_t maxLen = 48 * 1024);
bool  writeFileStr(const char * path, const std::string & data);
struct RSink;
// Streams a file out through a 256-byte buffer: writes "<path>.tmp" then swaps it in, so a
// failed save (card full, pulled out) leaves the old file alone.
bool  writeFileWith(const char * path, const std::function<void(RSink &)> & fill);
long  fileSize(const char * path);              // -1 if it doesn't exist
const char * fileExt(const char * path);  // lower-case extension without dot, or ""

// ---------------------------------------------------------------- app launching
void openCalculator();
void openNeat(const char * path = nullptr);
void openTidy(const char * path = nullptr);
void openReady();
void openSteady();
void openBrowser(const char * url = nullptr);
void openFiles();
void openSettings();
void openSystem();
void openMessages();
void openGames();
RGB888 themePal(int i);          // the screen palette (index 0 = black, 1 = white ...)
void openPing();
void openDoodle();
void openPictures(const char * file);
void openMusic();
void openScan();
void openShot();
void openClock();
void openLatch();
void openHtmlGames();
void openHtmlPrograms();
void openGitStore();
std::string boardHost();    // "tidyos-a1b2c3": its network name, http://tidyos-a1b2c3.local
std::string boardId();      // "TidyOS-A1B2C3": this board's own name (from its MAC address)
std::string askFile(const char * title, const char * okText, const char * defName, const std::string & startDir);   // file picker dialog
void openFilesAt(const char * dir);
void alarmTick();
extern volatile bool gShotReq;          // Print Screen key
void openSwitches();
void openRugged();

extern fabgl::uiApp * gApp;
extern volatile uint64_t gNetRx, gNetTx;     // TidyOS network traffic since start (bytes)
extern volatile bool gOpenTaskMgr;
extern volatile bool gRestartReq;
extern volatile bool gSleepReq;
extern volatile bool gMusicNextReq;   // screen.cpp: Next from the page's media controls
std::string musicQuickNext();         // media_apps.h
void musicQuickTick();
extern const char * gSDWiring;              // which microSD wiring was found           // Ctrl+Shift+Esc pressed

// Helpers used by every app window
Point clientOrigin(fabgl::uiFrame * f);   // top-left of the client area in window coords
fabgl::uiLabel * makeSavedLight(fabgl::uiWindow * parent, int x, int y);
void setSavedLight(fabgl::uiLabel * l, bool saved);
void bringToFront(fabgl::uiFrame * f);       // also brings it back from the taskbar
void taskbarMinimize(fabgl::uiFrame * f);   // hide it; a button on the taskbar brings it back
void taskbarForget(fabgl::uiFrame * f);     // it closed
bool msgWanted();                        // Shelf messages or the mesh switched on
std::string fmtMoney(double v);
