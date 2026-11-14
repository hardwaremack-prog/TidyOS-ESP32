// TidyOS core: theme, settings, storage helpers, shared declarations
#pragma once

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
void openPing();
void openSwitches();
void openRugged();

extern fabgl::uiApp * gApp;
extern volatile uint64_t gNetRx, gNetTx;     // TidyOS network traffic since start (bytes)
extern volatile bool gOpenTaskMgr;
extern const char * gSDWiring;              // which microSD wiring was found           // Ctrl+Shift+Esc pressed

// Helpers used by every app window
Point clientOrigin(fabgl::uiFrame * f);   // top-left of the client area in window coords
fabgl::uiLabel * makeSavedLight(fabgl::uiWindow * parent, int x, int y);
void setSavedLight(fabgl::uiLabel * l, bool saved);
void bringToFront(fabgl::uiFrame * f);
std::string fmtMoney(double v);
