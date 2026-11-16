// TidyOS core: display start-up, theme, settings, storage, desktop, main app
#include <Preferences.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include "screen.h"
#include "led.h"
#include <algorithm>
#include "esp_sleep.h"
#include <WiFi.h>
#include "crashlog.h"
#include "update.h"
#include "driver/spi_master.h"
#include <SPI.h>
#include <SD.h>
#include "driver/gpio.h"
#include "esp_bt.h"
#include "core.h"
#include "stream.h"
#include "input.h"
#include "net.h"
#include "style.h"
#include "spell.h"
#include "shelfmsg.h"
#include "fileserver.h"

using namespace fabgl;

Theme    T;
int      gColors      = 4;
bool     gSDMounted   = false;
bool     gFlashMounted= false;
Settings gSet;
uiApp *  gApp = nullptr;
volatile uint64_t gNetRx = 0, gNetTx = 0;
volatile bool gOpenTaskMgr = false;
volatile bool gRestartReq = false;
static RTC_NOINIT_ATTR uint32_t s_waitViewer;   // set by Restart: the start-up screen waits for the browser to come back
volatile bool gSleepReq = false;     // the page's Sleep button   // the page's Restart button

static Preferences s_prefs;
static BitmappedDisplayController * s_dc = nullptr;
static fabgl::Keyboard s_keyboard;     // never begun: only used to keep key state for FabGL widgets

// ====================================================================== settings

void settingsLoad()
{
  s_prefs.begin("tidyos", false);
  gSet.ssid      = s_prefs.getString("ssid", "");
  gSet.pass      = s_prefs.getString("pass", "");
  gSet.proxy     = s_prefs.getString("proxy", "");
  gSet.home      = s_prefs.getString("home", "http://frogfind.com");
  gSet.tzHours10 = s_prefs.getInt("tz", -50);
  gSet.dst       = s_prefs.getBool("dst", true);
  gSet.colors    = s_prefs.getInt("colors", 0);
  gSet.btNames   = s_prefs.getString("bt", "");
  gSet.printer   = s_prefs.getString("printer", "");
  gSet.fileShare = s_prefs.getBool("share", false);
  gSet.sdOn      = s_prefs.getBool("sdon", false);
  gSet.meshOn    = s_prefs.getBool("mesh", false);
  gSet.ledOn     = s_prefs.getBool("led", false);
  gSet.shelfOn   = s_prefs.getBool("shelf", false);
  gSet.tidyBands = s_prefs.getBool("tbands", false);
  gSet.tidyFreeze = s_prefs.getBool("tfreeze", false);
  gSet.hotspot = s_prefs.getBool("hotspot", false);
  gSet.alarmOn   = s_prefs.getBool("alarm", false);
  gSet.alarmMin  = s_prefs.getInt("alarmm", 420);
  gSet.worldCity = s_prefs.getInt("wcity", 0);
  gSet.storeUrl  = s_prefs.getString("surl", "");
  if (s_prefs.getBytesLength("apre") == sizeof(gSet.alarmPresets)) s_prefs.getBytes("apre", gSet.alarmPresets, sizeof(gSet.alarmPresets));
  gSet.neatFont  = s_prefs.getInt("nfont", 0);
  gSet.neatSize  = s_prefs.getInt("nsize", 1);
}

void settingsSave()
{
  s_prefs.putString("ssid", gSet.ssid);
  s_prefs.putString("pass", gSet.pass);
  s_prefs.putString("proxy", gSet.proxy);
  s_prefs.putString("home", gSet.home);
  s_prefs.putInt("tz", gSet.tzHours10);
  s_prefs.putBool("dst", gSet.dst);
  s_prefs.putInt("colors", gSet.colors);
  s_prefs.putString("bt", gSet.btNames);
  s_prefs.putString("printer", gSet.printer);
  s_prefs.putBool("share", gSet.fileShare);
  s_prefs.putBool("sdon", gSet.sdOn);
  s_prefs.putBool("mesh", gSet.meshOn);
  s_prefs.putBool("led", gSet.ledOn);
  s_prefs.putBool("shelf", gSet.shelfOn);
  s_prefs.putBool("tbands", gSet.tidyBands);
  s_prefs.putBool("tfreeze", gSet.tidyFreeze);
  s_prefs.putBool("hotspot", gSet.hotspot);
  s_prefs.putBool("alarm", gSet.alarmOn);
  s_prefs.putInt("alarmm", gSet.alarmMin);
  s_prefs.putInt("wcity", gSet.worldCity);
  s_prefs.putString("surl", gSet.storeUrl);
  s_prefs.putBytes("apre", gSet.alarmPresets, sizeof(gSet.alarmPresets));
  s_prefs.putInt("nfont", gSet.neatFont);
  s_prefs.putInt("nsize", gSet.neatSize);
}

// ====================================================================== storage

std::string dataPath(const char * name)
{
  if (gSDMounted) return std::string(SD_MOUNT "/tidyos/") + name;
  return std::string(FLASH_MOUNT "/") + name;
}

std::string documentsDir()
{
  if (gSDMounted) return SD_MOUNT "/Documents";
  return FLASH_MOUNT;
}

bool readFileStr(const char * path, std::string & out, size_t maxLen)
{
  FILE * f = fopen(path, "rb");
  if (!f) return false;
  out.clear();
  char buf[256];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (out.size() + n > maxLen) { n = maxLen - out.size(); out.append(buf, n); break; }
    out.append(buf, n);
  }
  fclose(f);
  return true;
}

bool writeFileStr(const char * path, const std::string & data)
{
  FILE * f = fopen(path, "wb");
  if (!f) return false;
  bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  fclose(f);
  return ok;
}

bool writeFileWith(const char * path, const std::function<void(RSink &)> & fill)
{
  std::string tmp = std::string(path) + ".tmp";
  FILE * f = fopen(tmp.c_str(), "wb");
  bool direct = !f;                             // e.g. name too long for the flash filesystem
  if (direct) f = fopen(path, "wb");
  if (!f) return false;
  FileSink o(f);
  fill(o);
  bool ok = o.flush();
  if (fclose(f) != 0) ok = false;
  if (direct) return ok;
  if (!ok) { remove(tmp.c_str()); return false; }
  remove(path);
  return rename(tmp.c_str(), path) == 0;
}

long fileSize(const char * path)
{
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return (long)st.st_size;
}

const char * fileExt(const char * path)
{
  static char ext[8];
  const char * dot = strrchr(path, '.');
  const char * sl  = strrchr(path, '/');
  if (!dot || (sl && dot < sl) || strlen(dot + 1) >= sizeof(ext)) return "";
  int i = 0;
  for (const char * p = dot + 1; *p; ++p) ext[i++] = tolower(*p);
  ext[i] = 0;
  return ext;
}

std::string fmtMoney(double v)
{
  char b[32];
  snprintf(b, sizeof(b), "$%.2f", v);
  return b;
}

// ====================================================================== theme

struct V8  : public VGA8Controller  { void lut() { updateRGB2PaletteLUT(); } };
struct V2  : public VGA2Controller  { void lut() { updateRGB2PaletteLUT(); } };
struct V4  : public VGA4Controller  { void lut() { updateRGB2PaletteLUT(); } };

RGB888 themePal(int i);
static const RGB888 PAL[16] = {
  RGB888(0, 0, 0),       RGB888(255, 255, 255), RGB888(170, 170, 170), RGB888(0, 85, 170),     // 4-colour set
  RGB888(85, 85, 85),    RGB888(0, 170, 170),   RGB888(170, 0, 0),     RGB888(0, 170, 0),      // 8-colour set
  RGB888(255, 255, 85),  RGB888(85, 170, 255),  RGB888(0, 0, 85),      RGB888(255, 170, 0),
  RGB888(255, 85, 170),  RGB888(85, 255, 85),   RGB888(85, 0, 170),    RGB888(170, 85, 0),
};

void themeSetup(BitmappedDisplayController * dc, int colors)
{
  gColors = colors;
  if (colors == 8)  { auto c = (V8*)dc;  for (int i = 0; i < 8; ++i)  c->setPaletteItem(i, PAL[i]); c->lut(); }
  if (colors == 4)  { auto c = (V4*)dc;  for (int i = 0; i < 4; ++i)  c->setPaletteItem(i, PAL[i]); c->lut(); }
  if (colors == 2)  { auto c = (V2*)dc;  for (int i = 0; i < 2; ++i)  c->setPaletteItem(i, PAL[i]); c->lut(); }

  T.win        = PAL[1];
  T.text       = PAL[0];
  T.face       = PAL[2];
  T.accent     = PAL[3];
  T.accentText = PAL[1];
  if (colors == 2) {           // black and white, like the first Macs
    T.face = PAL[1]; T.accent = PAL[0]; T.accentText = PAL[1];
    T.desk = PAL[1]; T.deskText = PAL[0]; T.shadow = PAL[0]; T.good = PAL[0]; T.warn = PAL[0];
  } else if (colors == 4) {
    T.desk = PAL[3]; T.deskText = PAL[1]; T.shadow = PAL[0]; T.good = PAL[3]; T.warn = PAL[0];
  } else {
    T.desk = PAL[5]; T.deskText = PAL[1]; T.shadow = PAL[4]; T.good = PAL[7]; T.warn = PAL[6];
  }
}

// ====================================================================== shared UI helpers

Point clientOrigin(uiFrame * f)
{
  Rect r = f->clientRect(uiOrigin::Window);
  return Point(r.X1, r.Y1);
}

uiLabel * makeSavedLight(uiWindow * parent, int x, int y)
{
  auto l = S(new uiLabel(parent, "  Saved  ", Point(x, y), Size(70, 18)));
  l->labelStyle().textAlign = uiHAlign::Center;
  setSavedLight(l, true);
  return l;
}

void setSavedLight(uiLabel * l, bool saved)
{
  if (!l) return;
  l->labelStyle().backgroundColor = saved ? T.good : T.face;
  l->labelStyle().textColor       = saved ? T.accentText : T.text;
  l->setText(saved ? "Saved" : "Not saved");
  l->repaint();
}

// minimized windows wait on the taskbar (between RAM and the clock)
static std::vector<uiFrame *> s_minimized;
static uiControl * s_deskCtl = nullptr;
static int s_ramX = 330;          // where the RAM text starts on the taskbar
static Rect s_optBtn;             // the Optimize button after it
static std::string s_optMsg;      // what Optimize did (shown for a few seconds)
static uint32_t s_optUntil = 0;
static void optimizeRam()
{
  size_t before = ramFree();
  WiFi.scanDelete();              // old network lists
  screenTrim();                   // idle web connections; the setup hotspot when it's not needed
  vTaskDelay(pdMS_TO_TICKS(400)); // closed connections hand their memory back
  int freed = ((int)ramFree() - (int)before) / 1024;
  char b[64];
  if (freed > 0) snprintf(b, sizeof(b), "Optimized: %dK freed - RAM %uK", freed, (unsigned)(ramFree() / 1024));
  else snprintf(b, sizeof(b), "Already tidy - RAM %uK (close apps to free more)", (unsigned)(ramFree() / 1024));
  s_optMsg = b;
  s_optUntil = millis() + 5000;
}
static void taskbarRepaint() { if (s_deskCtl) s_deskCtl->repaint(Rect(0, SCREEN_H - 20, SCREEN_W - 1, SCREEN_H - 1)); }
void taskbarMinimize(uiFrame * f)
{
  if (std::find(s_minimized.begin(), s_minimized.end(), f) == s_minimized.end()) s_minimized.push_back(f);
  gApp->showWindow(f, false);
  taskbarRepaint();
}
void taskbarForget(uiFrame * f)
{
  auto it = std::find(s_minimized.begin(), s_minimized.end(), f);
  if (it != s_minimized.end()) { s_minimized.erase(it); taskbarRepaint(); }
}

void bringToFront(uiFrame * f)
{
  taskbarForget(f);                       // (back from the taskbar)
  gApp->showWindow(f, true);
  gApp->setActiveWindow(f);
}

// ====================================================================== desktop

static const char * ART_CALC[16] = {
  "..############..", "..#wwwwwwwwww#..", "..#waaaaaaaaw#..", "..#waaaaaaaaw#..",
  "..#wwwwwwwwww#..", "..#w##w##w##w#..", "..#w##w##w##w#..", "..#wwwwwwwwww#..",
  "..#w##w##w##w#..", "..#w##w##w##w#..", "..#wwwwwwwwww#..", "..#w##w##waaw#..",
  "..#w##w##waaw#..", "..#wwwwwwwwww#..", "..############..", "................" };
static const char * ART_NEAT[16] = {
  "..#########.....", "..#wwwwwww##....", "..#wwwwwww#w#...", "..#wwwwwww####..",
  "..#wwwwwwwwwww#.", "..#wa#######aw#.", "..#wwwwwwwwwww#.", "..#w########ww#.",
  "..#wwwwwwwwwww#.", "..#w#########w#.", "..#wwwwwwwwwww#.", "..#w######wwww#.",
  "..#wwwwwwwwwww#.", "..#wwwwwwwwwww#.", "..#############.", "................" };
static const char * ART_TIDY[16] = {
  "################", "#aaaa#aaaa#aaaa#", "################", "#wwww#wwww#wwww#",
  "#wwww#wwww#wwww#", "################", "#wwww#wwww#wwww#", "#wwww#wwww#wwww#",
  "################", "#wwww#wwww#wwww#", "#wwww#wwww#wwww#", "################",
  "#wwww#wwww#wwww#", "#wwww#wwww#wwww#", "################", "................" };
static const char * ART_READY[16] = {
  "################", "#wwwwwwwwwwwwww#", "#w###wwwwwwwwww#", "#w#a#w########w#",
  "#w###wwwwwwwwww#", "#wwwwwwwwwwwwww#", "#w###wwwwwwwwww#", "#w#a#w########w#",
  "#w###wwwwwwwwww#", "#wwwwwwwwwwwwww#", "#w###wwwwwwwwww#", "#w#w#w########w#",
  "#w###wwwwwwwwww#", "#wwwwwwwwwwwwww#", "################", "................" };
static const char * ART_FILES[16] = {
  "................", "................", ".#####..........", "#ggggg#.........",
  "#gggggg#########", "#gggggggggggggg#", "################", "#wwwwwwwwwwwwww#",
  "#gggggggggggggg#", "#gggggggggggggg#", "#gggggggggggggg#", "#gggggggggggggg#",
  "#gggggggggggggg#", "################", "................", "................" };
static const char * ART_GEAR[16] = {
  "......####......", "......#gg#......", "..##.#gggg#.##..", "..#g#gggggg#g#..",
  "...#gggggggg#...", "..#ggg####ggg#..", "###gg#wwww#gg###", "#ggg#wwwwww#ggg#",
  "#ggg#wwwwww#ggg#", "###gg#wwww#gg###", "..#ggg####ggg#..", "...#gggggggg#...",
  "..#g#gggggg#g#..", "..##.#gggg#.##..", "......#gg#......", "......####......" };
static const char * ART_SYS[16] = {
  "################", "#aaaaaaaaaaaaaa#", "#awwwaaaaaaaaaa#", "#aaaaaaaaaaaaaa#",
  "#awwwwwwaaaaaaa#", "#aaaaaaaaaaaaaa#", "#awwwwaaaaaaaaa#", "#aaaaaaaaaaaaaa#",
  "#aaaaaaaaaaaaaa#", "################", "......####......", "......#gg#......",
  "....########....", "....#gggggg#....", "....########....", "................" };

static const char * ART_MSG[16] = {
  "................", ".##############.", "#wwwwwwwwwwwwww#", "#wwwwwwwwwwwwww#",
  "#ww#########www#", "#wwwwwwwwwwwwww#", "#ww######wwwwww#", "#wwwwwwwwwwwwww#",
  "#wwwwwwwwwwwwww#", ".######ww######.", "......#ww#......", ".......#w#......",
  "........##......", "................", "................", "................" };

static const char * ART_PING[16] = {
  "................", "..w..........w..", ".w..w......w..w.", "w..w...##...w..w",
  "w.w...#ww#...w.w", "w.w...#ww#...w.w", "w..w...##...w..w", ".w..w..##..w..w.",
  "..w....##....w..", ".......##.......", "......#gg#......", ".....#gggg#.....",
  "....#gggggg#....", "...##########...", "................", "................" };

static const char * ART_DOODLE[16] = {
  "................", "..############..", ".#wwwwwwwwwww#..", ".#ww##wwwwww##..", ".#w#ww#wwww#a#..", ".#w#ww#www#a#w..",
  ".#ww##wwww#a#w..", ".#wwwwwww#a#ww..", ".#wwwwww#a#www..", ".#wgggw##a#www..", ".#wgggw#g#wwww..", ".#wwwww##wwwww..",
  ".#wwwwwwwwwwww..", "..############..", "................", "................" };

static const char * ART_PICS[16] = {
  "................", ".##############.", ".#wwwwwwwwwwww#.", ".#wwwwwwwwaaww#.", ".#wwwwwwwwaaww#.", ".#wwwwwwwwwwww#.",
  ".#wwww#wwwwwww#.", ".#www###wwwwww#.", ".#ww#####w#www#.", ".#w#######g#ww#.", ".##########gg##.", ".#gggggggggggg#.",
  ".##############.", "................", "................", "................" };

static const char * ART_MUSIC[16] = {
  "................", "......#######...", "......#aaaaa#...", "......#######...", "......#.....#...", "......#.....#...",
  "......#.....#...", "......#.....#...", "....###...###...", "...####..####...", "...####..####...", "....##....##....",
  "................", "................", "................", "................" };

static const char * ART_SHOT[16] = {
  "................",
  "................",
  "....#####.......",
  "..############..",
  ".#gggggggggggg#.",
  ".#gggg####gaag#.",
  ".#ggg#wwww#ggg#.",
  ".#gg#wwaaww#gg#.",
  ".#gg#wwaaww#gg#.",
  ".#ggg#wwww#ggg#.",
  ".#gggg####gggg#.",
  ".#gggggggggggg#.",
  "..############..",
  "................",
  "................",
  "................",
};

static const char * ART_CLOCK[16] = {
  ".......##.......",
  "....########....",
  "...##wwwwww##...",
  "..##www#wwww##..",
  ".##wwww#wwwww##.",
  ".#wwwww#wwwwww#.",
  ".#wwwww#wwwwww#.",
  "##wwwww#aaaaww##",
  "##wwwwwwwwwwww##",
  ".#wwwwwwwwwwww#.",
  ".#wwwwwwwwwwww#.",
  ".##wwwwwwwwww##.",
  "..##wwwwwwww##..",
  "...##wwwwww##...",
  "....########....",
  ".......##.......",
};

static const char * ART_LOCK[16] = {
  "................",
  "......####......",
  ".....#....#.....",
  "....#......#....",
  "....#......#....",
  "....#......#....",
  "..############..",
  "..#aaaaaaaaaa#..",
  "..#aaaaaaaaaa#..",
  "..#aaaa##aaaa#..",
  "..#aaaa##aaaa#..",
  "..#aaaaa#aaaa#..",
  "..#aaaaaaaaaa#..",
  "..#aaaaaaaaaa#..",
  "..############..",
  "................",
};

static const char * ART_HTMLG[16] = {
  "################",
  "#aaaaaaaaaaaa#w#",
  "################",
  "#wwwwwwwwwwwwww#",
  "#wwwwwwwwwwwwww#",
  "#www########www#",
  "#ww#gggggggg#ww#",
  "#ww#g#gggg#g#ww#",
  "#ww####gg#g##ww#",
  "#ww#g#gggggg#ww#",
  "#ww#gggggggg#ww#",
  "#www########www#",
  "#wwwwwwwwwwwwww#",
  "#wwwwwwwwwwwwww#",
  "################",
  "................",
};

static const char * ART_HTMLP[16] = {
  "################",
  "#aaaaaaaaaaaa#w#",
  "################",
  "#wwwwwwwwwwwwww#",
  "#wwwwwwwwwwwwww#",
  "#ww#wwwwww#wwww#",
  "#w#wwwwww#wwwww#",
  "##wwwwww#wwwwww#",
  "#w#wwwww#wwwwww#",
  "#ww#wwww#ww#www#",
  "#wwwwww#wwww#ww#",
  "#wwwwww#wwwww#w#",
  "#wwwwww#wwww#ww#",
  "#wwwwwwwwww#www#",
  "################",
  "................",
};

static const char * ART_CLOUD[16] = {
  "................",
  "................",
  "......####......",
  "....##wwww##....",
  "...#wwwwwwww#...",
  "..#wwwwwwwwww##.",
  ".##wwwwwwwwwwww#",
  "#wwwwwwaawwwwww#",
  "#wwwwwaaaawwwww#",
  "#wwwwaaaaaawwww#",
  "#wwwwwwaawwwwww#",
  ".##wwwwaawwwww#.",
  "...####aa#####..",
  ".......aa.......",
  "....aaaaaaaa....",
  "................",
};

static const char * ART_NIC[16] = {
  "................", ".##############.", ".#gggggggggggg#.", ".#g####ggwwwwg#.", ".#g#aa#ggw##wg#.", ".#g#aa#ggw##wg#.",
  ".#g####ggwwwwg#.", ".#gggggggggggg#.", ".#gg#g#g#g#ggg#.", ".##############.", "..a.a.a.a.a.a...", "..a.a.a.a.a.a...",
  "................", "...#...#...#....", "..#.#.#.#.#.#...", ".#...#...#...#.." };

static const char * ART_SCAN[16] = {
  "................", "......#####.....", "....##wwwww##...", "...#wwwww#www#..", "..#wwwww#wwwww#.", "..#wwww#wwwwww#.",
  ".#wwwww#waawww#.", ".#wwwwwwwaawww#.", ".#wwwwwwwwwwww#.", "..#wawwwwwwww#..", "..#wwwwwwwaw#...", "...#wwwwwwww#...",
  "....##wwwww##...", "......#####.....", "................", "................" };

static const char * ART_SD[16] = {
  "................", "...#########....", "...#a#a#a#a##...", "...#a#a#a#a#w#..",
  "...#wwwwwwwwww#.", "...#wwwwwwwwww#.", "...#wggggggggw#.", "...#wgwwwwwwgw#.",
  "...#wgw##wwwgw#.", "...#wgwwwwwwgw#.", "...#wggggggggw#.", "...#wwwwwwwwww#.",
  "...#wwwwwwwwww#.", "...############.", "................", "................" };

static const char * ART_GAMES[16] = {
  "................", "................", "................", "...##########...",
  "..#gggggggggg#..", ".#gg#gggggggag#.", ".#g###ggggagga#.", ".#gg#ggggggaag#.",
  ".#ggggg##gggggg#", ".#gggg#..#ggggg#", "..####....#####.", "................",
  "................", "................", "................", "................" };

static const char * ART_RUGGED[16] = {
  ".......##.......", "......#gg#......", ".....#gggg#.....", "....#gg##gg#....",
  "...#gg#aa#gg#...", "..#gg#aaaa#gg#..", ".#gg#aaaaaa#gg#.", "#ggg#aaaaaa#ggg#",
  "#ggg#aaaaaa#ggg#", "#gggg######gggg#", "#gggggggggggggg#", "#gggggg##gggggg#",
  "#gggggg##gggggg#", "#gggggg##gggggg#", "################", "................" };

struct DeskIcon {
  const char *  label;
  const char ** art;      // pixel art, or nullptr for a glyph tile
  const char *  glyph;
  void (*open)();
};

static void openNeat0()    { openNeat(nullptr); }
static void openTidy0()    { openTidy(nullptr); }
static void openBrowser0() { openBrowser(nullptr); }
static void openPictures0() { openPictures(nullptr); }

// roughly how much memory each app takes while open (measured on the board), in KB
static int appNeedKB(const char * label)
{
  static const struct { const char * n; int kb; } T_[] = {
    { "Task Manager", 18 }, { "Files", 14 }, { "Neat", 22 }, { "Tidy", 20 }, { "Doodle", 18 }, { "Browser", 30 },
    { "Rugged", 18 }, { "Pictures", 24 }, { "Music", 14 }, { "Nic Sweep", 12 }, { "Screenshot", 6 }, { "Clock", 10 }, { "Latch", 24 }, { "HTML Games", 8 }, { "HTML Programs", 8 }, { "Git-Store", 12 }, { "Settings", 16 }, { "Messages", 16 }, { "Ping", 14 }, { "Games", 6 }, { "Steady", 14 },
  };
  for (auto & e : T_) if (!strcmp(e.n, label)) return e.kb;
  return 10;
}
static bool appIsOpen(const char * label)
{
  for (uiWindow * w = gApp->rootWindow()->firstChild(); w; w = w->next())
    if (w->objectType().uiFrame && w->state().visible && ((uiFrame *)w)->title() && !strncmp(((uiFrame *)w)->title(), label, strlen(label))) return true;
  return false;
}

static const DeskIcon ICONS[] = {
  { "Calculator", ART_CALC,  nullptr, openCalculator },
  { "Neat",       ART_NEAT,  nullptr, openNeat0 },
  { "Tidy",       ART_TIDY,  nullptr, openTidy0 },
  { "Ready",      ART_READY, nullptr, openReady },
  { "Steady",     nullptr,   "$",     openSteady },
  { "Rugged",     ART_RUGGED, nullptr, openRugged },
  { "Browser",    nullptr,   "www",   openBrowser0 },
  { "Files",      ART_FILES, nullptr, openFiles },
  { "Settings",   ART_GEAR,  nullptr, openSettings },
  { "Task Manager", ART_SYS, nullptr, openSystem },
  { "Messages",   ART_MSG,   nullptr, openMessages },
  { "Games",      ART_GAMES, nullptr, openGames },
  { "Ping",       ART_PING,  nullptr, openPing },
  { "Doodle",     ART_DOODLE, nullptr, openDoodle },
  { "Pictures",   ART_PICS,  nullptr, openPictures0 },
  { "Music",      ART_MUSIC, nullptr, openMusic },
  { "Nic Sweep",  ART_NIC,   nullptr, openScan },
  { "Screenshot", ART_SHOT,  nullptr, openShot },
  { "Clock",      ART_CLOCK, nullptr, openClock },
  { "Latch",      ART_LOCK,  nullptr, openLatch },
  { "HTML Games", ART_HTMLG, nullptr, openHtmlGames },
  { "HTML Programs", ART_HTMLP, nullptr, openHtmlPrograms },
  { "Git-Store",  ART_CLOUD, nullptr, openGitStore },
  { "Switches",   ART_SD,    nullptr, openSwitches },
};
static const int NICONS = sizeof(ICONS) / sizeof(ICONS[0]);

static std::string s_bootNote;              // shown once on the desktop (update result)

#include "desk_panel.h"

class Desktop : public uiControl {
public:
  static const int BAR_H = 20;
  static const int CELL_W = 80, CELL_H = 72, X0 = 6, Y0 = 6, COLS = 4;   // 4 columns: 20+ icons fit

  Desktop(uiWindow * parent) : uiControl(parent, Point(0, 0), Size(SCREEN_W, SCREEN_H), true, 0) {
    windowStyle().borderSize = 0;
    windowStyle().focusedBorderSize = 0;
    setFocusIndex(-1);
  }

  void processEvent(uiEvent * event) override {
    uiControl::processEvent(event);
    switch (event->id) {
      case UIEVT_PAINT:
        beginPaint(event, Rect(0, 0, SCREEN_W - 1, SCREEN_H - 1));
        paint(Rect(event->params.rect.X1, event->params.rect.Y1, event->params.rect.X2, event->params.rect.Y2));
        break;
      case UIEVT_MOUSEBUTTONDOWN:
        if (event->params.mouse.changedButton == 1) {
          int mx = event->params.mouse.status.X, my = event->params.mouse.status.Y;
          if (my >= SCREEN_H - BAR_H && mx >= s_optBtn.X1 && mx <= s_optBtn.X2 && s_optBtn.X2 > 0) {
            optimizeRam();
            repaint(Rect(0, SCREEN_H - BAR_H, SCREEN_W - 1, SCREEN_H - 1));
            break;
          }
          if (my >= SCREEN_H - BAR_H && !s_minimized.empty()) {          // a minimized window's button
            Canvas cv(s_dc);
            int x1, x2; std::vector<Rect> slots;
            taskSlots(&cv, ramText().c_str(), x1, x2, slots);
            for (size_t i = 0; i < slots.size(); ++i)
              if (mx >= slots[i].X1 && mx <= slots[i].X2) { uiFrame * f = s_minimized[i]; bringToFront(f); break; }
            break;
          }
          if (!showingHint() && mx >= s_affBtn.X1 && mx <= s_affBtn.X2 && my >= s_affBtn.Y1 && my <= s_affBtn.Y2) {
            s_affShown = s_affShown % 3 + 1;                      // the affirmation >> button
            repaint(deskPanelRect());
            break;
          }
          int i = hit(mx, my);
          if (i >= 0) { m_pressed = i; repaintIcon(i); }
        }
        break;
      case UIEVT_MOUSEMOVE: {
        int mx = event->params.mouse.status.X, my = event->params.mouse.status.Y, h = -1;
        if (!showingHint() && s_graphRect.X2 > s_graphRect.X1 && mx >= s_graphRect.X1 && mx <= s_graphRect.X2 && my >= s_graphRect.Y1 && my <= s_graphRect.Y2 && s_tempsN > 1)
          h = ((mx - s_graphRect.X1) * (s_tempsN - 1) + (s_graphRect.width() / 2)) / imax(1, s_graphRect.width() - 1);
        if (h != s_graphHover) { s_graphHover = h; Rect g = s_graphRect; repaint(Rect(g.X1 - 2, g.Y1 - 2, g.X2 + 2, g.Y2 + 16)); }
        break;
      }
      case UIEVT_MOUSEBUTTONUP:
        if (event->params.mouse.changedButton == 1 && m_pressed >= 0) {
          int i = m_pressed;
          m_pressed = -1;
          repaintIcon(i);
          if (hit(event->params.mouse.status.X, event->params.mouse.status.Y) == i) {
            // open a moment later: the second click of a double-click then lands on the icon,
            // not on a button (like Save) in the window that just opened
            if (m_pendingOpen < 0) { m_pendingOpen = i; m_pendingAt = millis(); }
          }
        }
        break;
      case UIEVT_TIMER:
        if (m_pendingOpen >= 0 && millis() - m_pendingAt >= 350) {
          int i = m_pendingOpen;
          m_pendingOpen = -1;
          {
            Serial.printf("[TidyOS] open %s, free RAM %u (biggest %u)\n", ICONS[i].label, (unsigned)ramFree(), (unsigned)ramBiggest());
            int need = appNeedKB(ICONS[i].label);
            if (!appIsOpen(ICONS[i].label) && (int)(ramFree() / 1024) < need + 10) {
              char m[100];
              snprintf(m, sizeof(m), "%s needs ~%d KB, %u KB free. Close an app, or pick Black & white at start-up.",
                       ICONS[i].label, need, (unsigned)(ramFree() / 1024));
              gApp->messageBox("Not enough memory", m, "OK", nullptr, nullptr, uiMessageBoxIcon::Warning);
            } else
              ICONS[i].open();
          }
        }
        if (gOpenTaskMgr) { gOpenTaskMgr = false; openSystem(); }
        alarmTick();                                // Clock alarm + Print Screen
        deskDropTick();                             // files dropped on the page: where do they go?
#ifdef TIDYOS_HTML5                                 // (the monitor edition has no IRAM to spare for sleep code)
        if (gSleepReq) {
          gSleepReq = false;
          Serial.println("[TidyOS] deep sleep - press BOOT (or EN) to wake");
          ledSetOn(false);
          delay(400);                               // let the page get the last frame
          WiFi.disconnect(true);
          esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);   // the BOOT button wakes it
          esp_deep_sleep_start();
        }
#endif
        if (gRestartReq) {
          gRestartReq = false;
          Serial.println("[TidyOS] restart asked for from the browser");
          s_waitViewer = 0xB0B0;
          delay(300);
          ESP.restart();
        }
        if (!s_bootNote.empty() && (m_tick > 20 || screenViewer())) {     // show it once, when someone can see it
          std::string n = s_bootNote; s_bootNote.clear();
          gApp->messageBox("Update", n.c_str(), "OK", nullptr, nullptr, uiMessageBoxIcon::Info);
        }
        if (++m_tick % 10) break;                   // the rest once a second (timer: 100 ms)
        netTick();
        gHeapSnap = ramFree(); gMinHeapSnap = ramLowest();
        if (m_tick % 300 == 0) Serial.printf("[TidyOS] still running: up %us, free RAM %uK\n", (unsigned)(millis() / 1000), (unsigned)(gHeapSnap / 1024));
        repaint(Rect(0, SCREEN_H - BAR_H, SCREEN_W - 1, SCREEN_H - 1));
        {
          // refresh the welcome / pairing hint only when something changed
          std::string sig = inputStatus() + char('0' + inputKeyboards()) + char('0' + inputMice()) + (gSet.ssid.length() ? "w" : "-") + (netConnected() ? "c" : "-");
          int unread = msgUnreadTotal() + comsUnread();
          if (unread != m_unread) { m_unread = unread; for (int k = 0; k < NICONS; ++k) if (ICONS[k].open == openMessages) repaintIcon(k); }
          if (sig != m_hintSig) {
            m_hintSig = sig;
            repaint(Rect(CELL_W * COLS + 16, 0, SCREEN_W - 1, SCREEN_H - BAR_H - 1));   // hint or panel
            m_panelSig = deskPanelSig();
          } else if (!showingHint() && deskPanelSig() != m_panelSig) {
            m_panelSig = deskPanelSig();
            repaint(deskPanelRect());
          }
        }
        break;
      default:
        break;
    }
  }

private:
  int m_pressed = -1;
  int m_unread = 0;
  int m_tick = 0;
  std::string m_hintSig;

  Rect iconRect(int i) {
    int col = i % COLS, row = i / COLS;
    int x = X0 + col * CELL_W, y = Y0 + row * CELL_H;
    return Rect(x, y, x + CELL_W - 1, y + CELL_H - 1);
  }
  int hit(int x, int y) {
    for (int i = 0; i < NICONS; ++i) {
      Rect r = iconRect(i);
      if (x >= r.X1 && x <= r.X2 && y >= r.Y1 && y <= r.Y2) return i;
    }
    return -1;
  }
  void repaintIcon(int i) { repaint(iconRect(i)); }

  void drawIcon(Canvas * cv, int i) {
    Rect r = iconRect(i);
    int ix = r.X1 + (CELL_W - 36) / 2, iy = r.Y1 + 4;
    bool down = (i == m_pressed);
    if (down) {
      cv->setBrushColor(T.shadow);
      cv->fillRectangle(r.X1 + 4, r.Y1, r.X2 - 4, r.Y2 - 6);
    }
    const DeskIcon & ic = ICONS[i];
    if (ic.art) {
      for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) {
          char ch = ic.art[y][x];
          if (ch == '.') continue;
          cv->setBrushColor(ch == '#' ? T.text : ch == 'a' ? T.accent : ch == 'g' ? T.face : T.win);
          cv->fillRectangle(ix + 2 + x * 2, iy + 2 + y * 2, ix + 3 + x * 2, iy + 3 + y * 2);
        }
    } else {
      cv->setBrushColor(T.text);
      cv->fillRectangle(ix + 2, iy + 2, ix + 33, iy + 33);
      cv->setBrushColor(i == 4 ? T.good : T.accent);
      cv->fillRectangle(ix + 4, iy + 4, ix + 31, iy + 31);
      auto f = strlen(ic.glyph) > 1 ? &FONT_std_14 : &FONT_std_24;
      int w = cv->textExtent(f, ic.glyph);
      cv->setPenColor(T.accentText);
      cv->setBrushColor(i == 4 ? T.good : T.accent);
      cv->drawText(f, ix + 18 - w / 2, iy + 18 - f->height / 2, ic.glyph);
    }
    if (ic.open == openMessages) {
      int unread = msgUnreadTotal() + comsUnread();
      if (unread) {
        char nb[8];
        snprintf(nb, sizeof(nb), "%d", unread > 99 ? 99 : unread);
        cv->setBrushColor(T.warn == T.text ? T.accent : T.warn);
        cv->fillEllipse(ix + 32, iy + 4, 18, 18);
        cv->setPenColor(T.accentText);
        int nw = cv->textExtent(&FONT_std_12, nb);
        cv->drawText(&FONT_std_12, ix + 32 - nw / 2, iy - 2, nb);
      }
    }
    const fabgl::FontInfo * lf = &FONT_std_14;
    int w = cv->textExtent(lf, ic.label);
    if (w > CELL_W - 4) { lf = &FONT_std_12; w = cv->textExtent(lf, ic.label); }   // long names: smaller text
    cv->setPenColor(T.deskText);
    cv->setBrushColor(down ? T.shadow : T.desk);
    cv->drawText(lf, r.X1 + CELL_W / 2 - w / 2, iy + 40, ic.label);
  }

  bool showingHint() {
#ifdef TIDYOS_HTML5
    return !gSet.ssid.length() || !netConnected();
#else
    return inputKeyboards() == 0 || inputMice() == 0 || !gSet.ssid.length();
#endif
  }
  int m_panelSig = -1;
  int m_pendingOpen = -1;          // icon clicked, opening shortly
  uint32_t m_pendingAt = 0;

  // where the minimized windows' buttons go: after the RAM text, before the clock
  void taskSlots(Canvas * cv, const char * ramText, int & x1, int & x2, std::vector<Rect> & out) {
    int by = SCREEN_H - BAR_H;
    x1 = s_optBtn.X2 > 0 ? s_optBtn.X2 + 8 : s_ramX + cv->textExtent(&FONT_std_12, ramText) + 10;
    x2 = SCREEN_W - cv->textExtent(&FONT_std_14, "12:00 PM") - 16;
    int n = s_minimized.size();
    if (!n || x2 - x1 < 30) return;
    int w = imin(80, (x2 - x1) / n);
    for (int i = 0; i < n && w >= 24; ++i) out.push_back(Rect(x1 + i * w, by + 3, x1 + i * w + w - 3, SCREEN_H - 3));
  }
  std::string ramText() {
    char b[96];
    if (s_optUntil && (int32_t)(millis() - s_optUntil) < 0) return s_optMsg;
    std::string notice = msgLastNotice();
    if (!notice.empty()) snprintf(b, sizeof(b), "%s", notice.substr(0, 40).c_str());
#ifdef TIDYOS_HTML5
    else snprintf(b, sizeof(b), "RAM %uK", (unsigned)(ramFree() / 1024));     // (you're looking at it in the browser)
#else
    else snprintf(b, sizeof(b), "Keyboard: %s  Mouse: %s   RAM %uK", inputKeyboards() ? "on" : "--", inputMice() ? "on" : "--", (unsigned)(ramFree() / 1024));
#endif
    return b;
  }

  void paint(Rect const & clip) {
    auto cv = canvas();
    cv->setBrushColor(T.desk);
    cv->fillRectangle(clip.X1, clip.Y1, clip.X2, imin(clip.Y2, SCREEN_H - BAR_H - 1));
    for (int i = 0; i < NICONS; ++i) {
      Rect r = iconRect(i);
      if (r.X2 >= clip.X1 && r.X1 <= clip.X2 && r.Y2 >= clip.Y1 && r.Y1 <= clip.Y2)
        drawIcon(cv, i);
    }
    // welcome / pairing hint - or, once everything is set up, the panel on the right
    if (!showingHint()) { Rect p = deskPanelRect(); if (p.intersects(clip)) drawDeskPanel(cv); }
#ifdef TIDYOS_HTML5
    if (!gSet.ssid.length() || !netConnected()) {
      int x = CELL_W * COLS + 24, y = 150;
      cv->setPenColor(T.deskText);
      cv->setBrushColor(T.desk);
      cv->drawText(&FONT_std_22, x, y, "Welcome to TidyOS");
      y += 34;
      cv->drawText(&FONT_std_14, x, y, "This desktop runs on the ESP32 and");
      y += 20;
      cv->drawText(&FONT_std_14, x, y, "shows up in your web browser.");
      y += 28;
      cv->drawText(&FONT_std_14, x, y, gSet.ssid.length() ? "Joining your Wi-Fi..." : "Open Settings to join your home Wi-Fi.");
      y += 20;
      cv->drawText(&FONT_std_14, x, y, "Then open http://tidyos.local");
      y += 28;
      cv->setBrushColor(T.desk);
      cv->fillRectangle(x, y, SCREEN_W - 1, y + 16);
      std::string st = inputStatus();
      cv->drawText(&FONT_std_12, x, y, st.c_str());
    }
#else
    bool needKbd = inputKeyboards() == 0, needMouse = inputMice() == 0;
    if (needKbd || needMouse || !gSet.ssid.length()) {
      int x = CELL_W * COLS + 24, y = 150;
      cv->setPenColor(T.deskText);
      cv->setBrushColor(T.desk);
      cv->drawText(&FONT_std_22, x, y, "Welcome to TidyOS");
      y += 34;
      if (needKbd)
        cv->drawText(&FONT_std_14, x, y, "1. Put your keyboard in pairing mode.");
      else
        cv->drawText(&FONT_std_14, x, y, "1. Keyboard connected!");
      y += 20;
      if (needMouse)
        cv->drawText(&FONT_std_14, x, y, "2. Then put your mouse in pairing mode.");
      else
        cv->drawText(&FONT_std_14, x, y, "2. Mouse connected!");
      y += 20;
      cv->drawText(&FONT_std_14, x, y, gSet.ssid.length() ? "3. Wi-Fi is set up." : "3. Open Settings to join your Wi-Fi.");
      y += 28;
      std::string st = inputStatus();
      cv->setBrushColor(T.desk);
      cv->fillRectangle(x, y, SCREEN_W - 1, y + 16);
      cv->drawText(&FONT_std_14, x, y, st.c_str());
      y += 24;
      if (needKbd && needMouse)
        cv->drawText(&FONT_std_12, x, y, "No Bluetooth? Type in a serial monitor (115200).");
    }
#endif
    // status bar
    int by = SCREEN_H - BAR_H;
    if (clip.Y2 >= by) {
      cv->setBrushColor(T.face);
      cv->fillRectangle(0, by, SCREEN_W - 1, SCREEN_H - 1);
      cv->setPenColor(T.shadow);
      cv->drawLine(0, by, SCREEN_W - 1, by);
      cv->setPenColor(T.text);
      cv->setBrushColor(T.face);
      // groups, each centred on the bar, with a thin divider between them
      auto ty = [&](const fabgl::FontInfo * f) { return by + 1 + (BAR_H - 1 - (int)f->height) / 2; };
      auto divider = [&](int x) { cv->setPenColor(T.shadow); cv->drawLine(x, by + 4, x, SCREEN_H - 4); cv->setPenColor(T.text); };
      int x = 8;
      cv->drawText(&FONT_std_14, x, ty(&FONT_std_14), "TidyOS");
      x += cv->textExtent(&FONT_std_14, "TidyOS") + 6;
      std::string vid = std::string("v" TIDYOS_VERSION "  ") + boardId().substr(7);     // version and this board's number
      cv->drawText(&FONT_std_12, x, ty(&FONT_std_12), vid.c_str());
      x += cv->textExtent(&FONT_std_12, vid.c_str()) + 8;
      divider(x); x += 8;
      char b[96];
      std::string ns = netStatus();
      if (fileServerOn()) ns += " - sharing";
      snprintf(b, sizeof(b), "%s", ns.c_str());
      cv->drawText(&FONT_std_12, x, ty(&FONT_std_12), b);
      x += cv->textExtent(&FONT_std_12, b) + 8;
      divider(x); x += 8;
      s_ramX = x;
      strlcpy(b, ramText().c_str(), sizeof(b));
      cv->drawText(&FONT_std_12, x, ty(&FONT_std_12), b);
      {                                                   // Optimize button
        int ox = x + cv->textExtent(&FONT_std_12, b) + 8, ow = cv->textExtent(&FONT_std_12, "Optimize") + 10;
        s_optBtn = Rect(ox, by + 3, ox + ow, SCREEN_H - 3);
        cv->setBrushColor(T.win);
        cv->fillRectangle(s_optBtn.X1, s_optBtn.Y1, s_optBtn.X2, s_optBtn.Y2);
        cv->setPenColor(T.shadow);
        cv->drawRectangle(s_optBtn.X1, s_optBtn.Y1, s_optBtn.X2, s_optBtn.Y2);
        cv->setPenColor(T.text);
        cv->drawText(&FONT_std_12, ox + 5, ty(&FONT_std_12), "Optimize");
        cv->setBrushColor(T.face);
      }
      // minimized windows
      {
        int x1, x2; std::vector<Rect> slots;
        taskSlots(cv, b, x1, x2, slots);
        for (size_t i = 0; i < slots.size(); ++i) {
          Rect s = slots[i];
          cv->setBrushColor(T.win);
          cv->fillRectangle(s.X1, s.Y1, s.X2, s.Y2);
          cv->setPenColor(T.text);
          cv->drawRectangle(s.X1, s.Y1, s.X2, s.Y2);
          char t[40];
          strlcpy(t, s_minimized[i]->title() ? s_minimized[i]->title() : "?", sizeof(t));
          while (strlen(t) > 1 && cv->textExtent(&FONT_std_12, t) > s.width() - 6) t[strlen(t) - 1] = 0;
          cv->setBrushColor(T.win);
          cv->drawText(&FONT_std_12, s.X1 + 3, s.Y1 + 1, t);
        }
        cv->setBrushColor(T.face);
      }
      time_t now = time(nullptr);
      struct tm tmv;
      localtime_r(&now, &tmv);
      if (tmv.tm_year > 100) {
        int h = tmv.tm_hour % 12; if (h == 0) h = 12;
        snprintf(b, sizeof(b), "%d:%02d %s", h, tmv.tm_min, tmv.tm_hour < 12 ? "AM" : "PM");
      } else {
        snprintf(b, sizeof(b), "--:--");
      }
      int w = cv->textExtent(&FONT_std_14, b);
      cv->drawText(&FONT_std_14, SCREEN_W - w - 8, by + 3, b);
    }
  }
};

// ====================================================================== application

class TidyApp : public uiApp {
  void init() override {
    gApp = this;
    rootWindow()->frameStyle().backgroundColor = T.desk;
    auto desk = new Desktop(rootWindow());
    s_deskCtl = desk;
    setTimer(desk, 100);
    displayController()->setMouseCursor(CursorName::CursorPointerSimpleReduced);
    inputAttach(this, &s_keyboard, s_dc);
  }
};
static TidyApp s_app;

// ====================================================================== start-up

template <class C>
static BitmappedDisplayController * tryMode(size_t fbBytes, size_t margin = 60000)
{
  size_t freeMem = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  Serial.printf("[TidyOS] free %u, need %u\n", (unsigned)freeMem, (unsigned)(fbBytes + margin));
  if (freeMem < fbBytes + margin) return nullptr;
  C * c = new C;
#ifdef TIDYOS_HTML5
  c->setHeadless(true);     // no monitor: keep the picture in RAM, skip the VGA signal (~16 KB + CPU time)
#endif
  c->begin(VGA_RED1, VGA_RED0, VGA_GREEN1, VGA_GREEN0, VGA_BLUE1, VGA_BLUE0, VGA_HSYNC, VGA_VSYNC);
  c->setResolution(VGA_640x480_60Hz);
  if (c->getViewPortHeight() >= SCREEN_H && c->getViewPortWidth() >= SCREEN_W) return c;
  Serial.printf("[TidyOS] only got %d lines, trying fewer colours\n", c->getViewPortHeight());
  c->end();
  delete c;
  return nullptr;
}

static void startDisplay()
{
  // FabGL queues drawing commands; its default 1024-entry queue is ~15 KB of RAM. 256 is plenty
  // (when it fills, the app just waits a moment for drawing to catch up).
  BitmappedDisplayController::queueSize = 256;
  int want = gSet.colors;   // 0 = auto
#ifdef TIDYOS_HTML5
  if (want == 0) want = 4;  // HTML5: 4 colours unless asked for 8 - leaves ~38 KB more for Wi-Fi and apps
#endif
  BitmappedDisplayController * dc = nullptr;
  int got = 0;
  // 8 colours only when it still leaves plenty for Wi-Fi and apps (asked for explicitly = smaller margin)
  if (want == 2) { dc = tryMode<V2>(640 * 480 / 8, 0); got = 2; }      // black & white: half the screen memory
  if (!dc && (want == 0 || want >= 8))  { dc = tryMode<V8>(640 * 480 * 3 / 8, want >= 8 ? 60000 : 100000); got = 8; }
  if (!dc)                              { dc = tryMode<V4>(640 * 480 / 4);     got = 4; }
  if (!dc) {
    // last resort: 4 colours without the safety margin
    auto c = new V4;
#ifdef TIDYOS_HTML5
    c->setHeadless(true);
#endif
    c->begin(VGA_RED1, VGA_RED0, VGA_GREEN1, VGA_GREEN0, VGA_BLUE1, VGA_BLUE0, VGA_HSYNC, VGA_VSYNC);
    c->setResolution(VGA_640x480_60Hz);
    dc = c; got = 4;
  }
  s_dc = dc;
  themeSetup(dc, got);
  Serial.printf("[TidyOS] display 640x480, %d colours, free heap %u\n", got, (unsigned)ramFree());
}

const char * gSDWiring = "none";

// Same method as The Shelf on an ESP32 (Arduino SD library, tried at 10, 4 and 1 MHz):
// it reads cards that ESP-IDF's own SD-over-SPI driver rejects.
static SPIClass s_sdSpi(VSPI);

static bool mountSD(int miso, int mosi, int clk, int cs, const char * name)
{
  s_sdSpi.begin(clk, miso, mosi, cs);
  for (int pin : { miso, mosi, cs }) gpio_pullup_en((gpio_num_t)pin);      // SD lines need pull-ups
  for (uint32_t hz : { 10000000u, 4000000u, 1000000u }) {
    if (SD.begin(cs, s_sdSpi, hz, SD_MOUNT, 2)) {     // 2 open files at once: each one reserves a 4 KB buffer up front
      gSDWiring = name;
      Serial.printf("[TidyOS] SD card found on %s wiring at %u MHz (%llu MB)\n", name, (unsigned)(hz / 1000000), SD.cardSize() >> 20);
      return true;
    }
    SD.end();
  }
  s_sdSpi.end();
  Serial.printf("[TidyOS] no SD card on %s wiring\n", name);
  return false;
}

static bool mountAnySD()
{
#ifdef TIDYOS_HTML5
  return mountSD(SD_INLINE_MISO, SD_INLINE_MOSI, SD_INLINE_CLK, SD_INLINE_CS, "in-line (19/18/5/17)") ||
         mountSD(SD_MISO, SD_MOSI, SD_CLK, SD_CS, "classic (16/17/14/13)");
#else
  return mountSD(SD_MISO, SD_MOSI, SD_CLK, SD_CS, "16/17/14/13");
#endif
}

static void makeDirs();

bool sdMountForUpdate()
{
  if (!gSDMounted) gSDMounted = mountAnySD();
  return gSDMounted;
}

bool sdTurnOn()
{
  gSet.sdOn = true;
  settingsSave();
  if (gSDMounted) return true;
  gSDMounted = mountAnySD();
  if (gSDMounted) makeDirs();
  return gSDMounted;
}

void sdTurnOff()
{
  gSet.sdOn = false;
  settingsSave();
  if (gSDMounted) {
    SD.end();
    s_sdSpi.end();
    gSDMounted = false;
    gSDWiring = "none";
    Serial.println("[TidyOS] SD card turned off");
  }
  if (!gFlashMounted) gFlashMounted = FileBrowser::mountSPIFFS(true, FLASH_MOUNT, 2);
}

#ifdef TIDYOS_HTML5
// The screen memory keeps working (the browser reads it); only the pins stop being VGA outputs.
static void releaseVgaPins()
{
  for (gpio_num_t p : { VGA_RED1, VGA_RED0, VGA_GREEN1, VGA_GREEN0, VGA_BLUE1, VGA_BLUE0, VGA_HSYNC, VGA_VSYNC })
    gpio_reset_pin(p);
}
#endif

static void makeDirs()
{
  if (gSDMounted) {
    mkdir(SD_MOUNT "/tidyos", 0777);
    mkdir(SD_MOUNT "/Documents", 0777);
  }
}

// ---------------------------------------------------------------- boot logo
// big text: draws each font pixel as a scale x scale square (runs merged per row)
static void drawBig(Canvas & cv, FontInfo const * f, int x, int y, const char * t, int scale, RGB888 col)
{
  cv.setBrushColor(col);
  for (; *t; ++t) {
    uint8_t c = *t;
    int gw;
    const uint8_t * g;
    if (f->flags & FONTINFOFLAGS_VARWIDTH) { g = f->data + f->chptr[c]; gw = *g++; }
    else { gw = f->width; g = f->data + c * f->height * ((gw + 7) / 8); }
    int bpr = (gw + 7) / 8;
    for (int r = 0; r < f->height; ++r) {
      const uint8_t * row = g + r * bpr;
      int run = -1;
      for (int i = 0; i <= gw; ++i) {
        bool on = i < gw && (row[i >> 3] & (0x80 >> (i & 7)));
        if (on && run < 0) run = i;
        if (!on && run >= 0) {
          cv.fillRectangle(x + run * scale, y + r * scale, x + i * scale - 1, y + (r + 1) * scale - 1);
          run = -1;
        }
      }
    }
    x += gw * scale;
  }
}

static int bigWidth(FontInfo const * f, const char * t, int scale)
{
  int w = 0;
  for (; *t; ++t) w += (f->flags & FONTINFOFLAGS_VARWIDTH) ? f->data[f->chptr[(uint8_t)*t]] : f->width;
  return w * scale;
}

// one block of the logo "T" (same style as the Blocks game)
static void logoBlock(Canvas & cv, int x, int y, int s, RGB888 fill)
{
  cv.setBrushColor(T.text);
  cv.fillRectangle(x, y, x + s - 1, y + s - 1);
  cv.setBrushColor(fill);
  cv.fillRectangle(x + 3, y + 3, x + s - 4, y + s - 4);
  cv.setBrushColor(T.win);
  cv.fillRectangle(x + 3, y + 3, x + s - 4, y + 6);
  cv.fillRectangle(x + 3, y + 3, x + 6, y + s - 4);
}

static const int BAR_X = 170, BAR_Y = 300, BAR_W = 300, BAR_H = 14;

static void splashLogo()
{
  Canvas cv(s_dc);
  cv.setBrushColor(T.desk);
  cv.clear();
  // the logo: a "T" made of five tidy blocks
  int bs = 40, lx = 140, ly = 110;
  logoBlock(cv, lx,          ly,      bs, T.face);
  logoBlock(cv, lx + bs,     ly,      bs, T.face);
  logoBlock(cv, lx + bs * 2, ly,      bs, T.face);
  logoBlock(cv, lx + bs,     ly + bs, bs, T.face);
  logoBlock(cv, lx + bs,     ly + bs * 2, bs, T.face);
  // wordmark
  int tx = lx + bs * 3 + 30;
  drawBig(cv, &FONT_std_24, tx + 3, ly + 13, "TidyOS", 3, T.text);        // shadow
  drawBig(cv, &FONT_std_24, tx, ly + 10, "TidyOS", 3, T.win);
  cv.setPenColor(T.deskText);
  cv.setBrushColor(T.desk);
  cv.drawText(&FONT_std_16, tx + 6, ly + 92, "A tiny computer for the ESP32");
  // progress bar frame
  cv.setPenColor(T.win);
  cv.drawRectangle(BAR_X - 2, BAR_Y - 2, BAR_X + BAR_W + 1, BAR_Y + BAR_H + 1);
  cv.setPenColor(T.deskText);
  cv.drawText(&FONT_std_12, 8, 6, "TidyOS " TIDYOS_VERSION);
  cv.waitCompletion();
}

// The progress bar keeps creeping forward on its own while a step works (joining Wi-Fi,
// connecting to GitHub...), so it never looks stuck; each splash() sets the real value.
static SemaphoreHandle_t s_barMux = nullptr;
static volatile int  s_barShown = 0, s_barCeil = 0;
static volatile bool s_barRun = false, s_barAlive = false;

static void drawBar(Canvas & cv, int percent)
{
  cv.setBrushColor(T.desk);
  cv.fillRectangle(BAR_X, BAR_Y, BAR_X + BAR_W - 1, BAR_Y + BAR_H - 1);
  cv.setBrushColor(T.win);
  if (percent < 0) percent = 0;
  if (percent > 100) percent = 100;
  int w = BAR_W * percent / 100;
  if (w > 0) cv.fillRectangle(BAR_X, BAR_Y, BAR_X + w - 1, BAR_Y + BAR_H - 1);
}

static void barTask(void *)
{
  while (s_barRun) {
    vTaskDelay(pdMS_TO_TICKS(300));
    if (xSemaphoreTake(s_barMux, pdMS_TO_TICKS(100)) != pdTRUE) continue;
    if (s_barRun && s_barShown < s_barCeil) {
      s_barShown = s_barShown + 1;
      Canvas cv(s_dc);
      drawBar(cv, s_barShown);
      cv.waitCompletion();
    }
    xSemaphoreGive(s_barMux);
  }
  s_barAlive = false;
  vTaskDelete(nullptr);
}

static void barStart()
{
  if (!s_barMux) s_barMux = xSemaphoreCreateMutex();
  if (s_barAlive) return;
  s_barRun = s_barAlive = true;
  xTaskCreatePinnedToCore(barTask, "bar", 2048, nullptr, 1, nullptr, 1);
}

static void barStop()
{
  s_barRun = false;
  for (int i = 0; i < 20 && s_barAlive; ++i) delay(20);
}

// percent > 0: the bar moves up to it (never back) and then creeps on; 0 = stop creeping
// Like a classic Mac starting up: the icons of what's loading march in one by one along the
// bottom of the screen (wrapping to a second row), while the bar fills.
static int s_parade = 0;
static void paradeIcon(Canvas & cv, int i)
{
  const int PITCH = 36, PER_ROW = (SCREEN_W - 16) / PITCH;
  int ix = 10 + (i % PER_ROW) * PITCH, iy = SCREEN_H - 40 - (i / PER_ROW) * 36;
  const DeskIcon & ic = ICONS[i];
  if (ic.art) {
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 16; ++x) {
        char ch = ic.art[y][x];
        if (ch == '.') continue;
        cv.setBrushColor(ch == '#' ? T.text : ch == 'a' ? T.accent : ch == 'g' ? T.face : T.win);
        cv.fillRectangle(ix + x * 2, iy + y * 2, ix + 1 + x * 2, iy + 1 + y * 2);
      }
  } else {
    cv.setBrushColor(T.text);
    cv.fillRectangle(ix, iy, ix + 31, iy + 31);
    cv.setPenColor(T.win);
    cv.setBrushColor(T.text);
    if (ic.glyph) cv.drawText(&FONT_std_14, ix + 16 - cv.textExtent(&FONT_std_14, ic.glyph) / 2, iy + 8, ic.glyph);
  }
}

static void splash(const char * msg, int percent)
{
  if (s_barMux) xSemaphoreTake(s_barMux, portMAX_DELAY);
  Canvas cv(s_dc);
  cv.setBrushColor(T.desk);
  cv.fillRectangle(0, BAR_Y + 20, SCREEN_W - 1, BAR_Y + 40);
  // icons up to this step's share march in, and the bar slides up with them
  {
    int target = NICONS * (percent > 100 ? 100 : percent) / 100;
    if (target <= s_parade) target = imin(NICONS, s_parade + 1);      // at least one new icon per step
    int from = s_barShown, to = percent > from ? percent : from, n = target - s_parade;
    for (int k = 0; s_parade < target; ++k) {
      paradeIcon(cv, s_parade++);
      drawBar(cv, from + (to - from) * (k + 1) / imax(1, n));
      cv.waitCompletion();
      delay(percent >= 100 ? 60 : 110);
    }
    if (to > s_barShown) s_barShown = to;
  }
  int shown = s_barShown;
  if (percent > 0) {
    if (percent > 100) percent = 100;
    if (percent > shown) shown = percent;
    s_barShown = shown;
    s_barCeil  = percent >= 100 ? 100 : (shown + 15 > 97 ? 97 : shown + 15);
  } else {
    s_barCeil = shown;
  }
  drawBar(cv, shown);
  int tw = cv.textExtent(&FONT_std_14, msg);
  cv.setPenColor(T.deskText);
  cv.drawText(&FONT_std_14, BAR_X + BAR_W / 2 - tw / 2, BAR_Y + 22, msg);
  // the checklist: every step so far, ticked once the next one starts
  static char steps[8][48];
  static int nSteps = 0;
  if (strcmp(msg, "Ready") != 0 && (nSteps == 0 || strcmp(steps[(nSteps - 1) % 8], msg) != 0) && nSteps < 8)
    strlcpy(steps[nSteps++], msg, sizeof(steps[0]));
  bool allDone = percent >= 100;
  cv.setBrushColor(T.desk);
  cv.fillRectangle(0, BAR_Y + 44, SCREEN_W - 1, BAR_Y + 103);   // (the icons below stay)
  for (int i = 0; i < nSteps; ++i) {
    int x = i < 4 ? 120 : 360, y = BAR_Y + 46 + (i % 4) * 15;
    bool done = allDone || i < nSteps - 1;
    cv.setPenColor(T.deskText);
    if (done) { cv.drawLine(x, y + 6, x + 3, y + 9); cv.drawLine(x + 3, y + 9, x + 9, y + 2); cv.drawLine(x + 1, y + 6, x + 4, y + 9); }
    else cv.drawText(&FONT_std_12, x, y, "...");
    char t[48]; strlcpy(t, steps[i], sizeof(t));
    while (strlen(t) > 4 && cv.textExtent(&FONT_std_12, t) > 230) t[strlen(t) - 1] = 0;
    cv.drawText(&FONT_std_12, x + 16, y, t);
  }
  cv.waitCompletion();
  if (s_barMux) xSemaphoreGive(s_barMux);
}

RGB888 themePal(int i) { return PAL[i & 15]; }

// If memory runs out for a moment (e.g. a phone opening several connections at once),
// wait and retry instead of crashing. Gives up after 5 s of no memory at all.
#include <new>
static void lowMemoryWait()
{
  static uint32_t since = 0;
  uint32_t now = millis();
  if (!since || now - since > 6000) since = now;
  if (now - since > 5000) { since = 0; std::set_new_handler(nullptr); Serial.println("[TidyOS] out of memory"); return; }
  vTaskDelay(pdMS_TO_TICKS(25));
}

static void memLog(const char * step)
{
  Serial.printf("[TidyOS] mem after %-18s free %6u  biggest %6u  (32-bit-only extra %u)\n", step, (unsigned)ramFree(), (unsigned)ramBiggest(),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - ramFree()));
}

#ifdef TIDYOS_HTML5
// ---------------------------------------------------------------- start-up switches
// Shown in the browser before anything big starts, so memory-hungry parts can be left off
// for this boot. Boots by itself after a countdown, so a restart with nobody watching still
// comes up. Click anything (or press a key) to stop the countdown.
static const int BW_X = 130, BW_Y = 30, BW_W = 380, BW_H = 400;

static void bootRow(Canvas & cv, int i, const char * name, const char * note, bool on, const char * valText = nullptr)
{
  int y = BW_Y + 40 + i * 50;
  cv.setBrushColor(T.win);
  cv.fillRectangle(BW_X + 1, y - 4, BW_X + BW_W - 2, y + 44);
  cv.setPenColor(T.text);
  char b[48];
  snprintf(b, sizeof(b), "%d  %s", i + 1, name);
  cv.drawText(&FONT_std_16, BW_X + 16, y, b);
  cv.setPenColor(T.shadow);
  cv.drawText(&FONT_std_12, BW_X + 34, y + 21, note);
  int sx = BW_X + BW_W - 96;
  cv.setBrushColor(on ? T.accent : T.face);
  cv.fillRectangle(sx, y, sx + 76, y + 26);
  cv.setPenColor(T.shadow);
  cv.drawRectangle(sx, y, sx + 76, y + 26);
  cv.setPenColor(on ? T.accentText : T.text);
  const char * t = valText ? valText : on ? "ON" : "OFF";
  cv.drawText(&FONT_std_16, sx + 38 - cv.textExtent(&FONT_std_16, t) / 2, y + 5, t);
}

static void bootStatus(Canvas & cv, const char * msg)
{
  int y = BW_Y + BW_H - 28;
  cv.setBrushColor(T.win);
  cv.fillRectangle(BW_X + 1, y, BW_X + BW_W - 2, y + 18);
  cv.setPenColor(T.text);
  cv.drawText(&FONT_std_12, BW_X + BW_W / 2 - cv.textExtent(&FONT_std_12, msg) / 2, y + 2, msg);
}

static void bootScreenRow(Canvas & cv)
{
  int c = gSet.colors == 0 ? 4 : gSet.colors;
  bootRow(cv, 3, "Screen", c == 8 ? "8 colours: ~38 KB less free memory" : c == 2 ? "Black & white: ~37 KB more free memory" : "4 colours (restarts TidyOS to change)",
          c != 4, c == 8 ? "8 col" : c == 2 ? "B&W" : "4 col");
}

static void bootDraw(Canvas & cv)
{
  cv.setBrushColor(T.desk);
  cv.clear();
  cv.setBrushColor(T.shadow);
  cv.fillRectangle(BW_X + 4, BW_Y + 4, BW_X + BW_W + 3, BW_Y + BW_H + 3);
  cv.setBrushColor(T.win);
  cv.fillRectangle(BW_X, BW_Y, BW_X + BW_W - 1, BW_Y + BW_H - 1);
  cv.setPenColor(T.shadow);
  cv.drawRectangle(BW_X, BW_Y, BW_X + BW_W - 1, BW_Y + BW_H - 1);
  cv.setBrushColor(T.accent);
  cv.fillRectangle(BW_X + 1, BW_Y + 1, BW_X + BW_W - 2, BW_Y + 28);
  cv.setPenColor(T.accentText);
  cv.drawText(&FONT_std_16, BW_X + 10, BW_Y + 7, "Start-up switches - save memory");
  bootRow(cv, 0, "SD card", "~15 KB. Off: files go to internal storage", gSet.sdOn);
  bootRow(cv, 1, "Wireless mesh", "A few KB. Coms boards nearby", gSet.meshOn);
  bootRow(cv, 2, "Blue light", "~2 KB. Breathes when TidyOS is ready", gSet.ledOn);
  bootScreenRow(cv);
  bootRow(cv, 4, "Shelf messages", "~9 KB. Chat with your Shelves on Wi-Fi", gSet.shelfOn);
  // Boot button, and Update next to it
  int bx = BW_X + BW_W / 2 - 80, by = BW_Y + BW_H - 70;
  cv.setBrushColor(T.face);
  cv.fillRectangle(bx, by, bx + 160, by + 32);
  cv.setPenColor(T.shadow);
  cv.drawRectangle(bx, by, bx + 160, by + 32);
  cv.setPenColor(T.text);
  const char * t = "Boot TidyOS";
  cv.drawText(&FONT_std_16, bx + 80 - cv.textExtent(&FONT_std_16, t) / 2, by + 8, t);
  int ux = BW_X + BW_W - 102;
  cv.setBrushColor(T.face);
  cv.fillRectangle(ux, by + 4, ux + 96, by + 28);
  cv.setPenColor(T.shadow);
  cv.drawRectangle(ux, by + 4, ux + 96, by + 28);
  cv.setPenColor(T.text);
  cv.drawText(&FONT_std_14, ux + 48 - cv.textExtent(&FONT_std_14, "Update") / 2, by + 9, "Update");
  cv.setPenColor(T.deskText);
  cv.setBrushColor(T.desk);
  cv.drawText(&FONT_std_12, 8, SCREEN_H - 16, "TidyOS " TIDYOS_VERSION);
}

static RTC_NOINIT_ATTR uint32_t s_skipChooser;    // set before a restart the chooser itself asked for

static void bootChooser()
{
  if (s_skipChooser == 0x5EED) { s_skipChooser = 0; return; }
  const int startColors = gSet.colors;
  inputBootGrab(true);
  screenBegin();                         // the browser can show this window right away
  Canvas cv(s_dc);
  bootDraw(cv);
  bootStatus(cv, "Waiting for Wi-Fi...");
  cv.waitCompletion();
  const int COUNT = 12;
  uint32_t t0 = millis(), readyAt = 0;
  bool waitView = s_waitViewer == 0xB0B0;          // after the page's Restart: count down only once the browser is back
  s_waitViewer = 0;
  bool timer = true, boot = false;
  int shown = -2;
  while (!boot) {
    bool netUp = WiFi.status() == WL_CONNECTED || screenApOn();
    if (!readyAt && ((netUp && (!waitView || screenViewer())) || millis() - t0 > (waitView ? 60000 : 35000))) readyAt = millis();
    int left = readyAt ? COUNT - (int)((millis() - readyAt) / 1000) : COUNT;
    if (timer && readyAt && left <= 0) break;
    int toggled = -1;
    int x, y;
    if (inputBootClick(x, y)) {
      timer = false;
      for (int i = 0; i < 5; ++i) {
        int ry = BW_Y + 40 + i * 50;
        if (x >= BW_X && x < BW_X + BW_W && y >= ry - 4 && y < ry + 44) toggled = i;
      }
      int bx = BW_X + BW_W / 2 - 80, by = BW_Y + BW_H - 70;
      if (x >= bx && x <= bx + 160 && y >= by && y <= by + 32) boot = true;
      int ux = BW_X + BW_W - 102;
      if (x >= ux && x <= ux + 96 && y >= by + 4 && y <= by + 28) {     // Update: restart into update mode (black & white)
        if (updateLoaderPresent()) {
          bootStatus(cv, "Restarting to update (in black & white, for memory)...");
          cv.waitCompletion();
          delay(400);
          updateRequest();
        } else bootStatus(cv, "Install once from the web flasher to enable updates.");
      }
    }
    int k;
    while ((k = inputBootKey()) >= 0) {
      timer = false;
      if (k >= 0x1E && k <= 0x22) toggled = k - 0x1E;        // keys 1 to 5
      if (k == 0x28 || k == 0x58) boot = true;                // Enter
    }
    if (toggled == 3) {                          // 4 colours -> 8 colours -> black & white -> 4 colours
      int c = gSet.colors == 0 ? 4 : gSet.colors;
      gSet.colors = c == 4 ? 8 : c == 8 ? 2 : 0;
      settingsSave();
      bootScreenRow(cv);
      shown = -2;
      toggled = -1;
    }
    if (toggled == 4) {
      gSet.shelfOn = !gSet.shelfOn;
      settingsSave();
      bootRow(cv, 4, "Shelf messages", "~9 KB. Chat with your Shelves on Wi-Fi", gSet.shelfOn);
      shown = -2;
      toggled = -1;
    }
    if (toggled >= 0) {
      bool * v = toggled == 0 ? &gSet.sdOn : toggled == 1 ? &gSet.meshOn : &gSet.ledOn;
      *v = !*v;
      settingsSave();
      if (toggled == 2) ledSetOn(gSet.ledOn);
      bootRow(cv, toggled, toggled == 0 ? "SD card" : toggled == 1 ? "Wireless mesh" : "Blue light",
              toggled == 0 ? "~15 KB. Off: files go to internal storage" : toggled == 1 ? "A few KB. Coms boards nearby" : "~2 KB. Breathes when TidyOS is ready", *v);
      shown = -2;
    }
    int want = !timer ? -1 : readyAt ? left : 99;
    if (want != shown) {
      shown = want;
      char b[80];
      if (want == -1) snprintf(b, sizeof(b), "Click Boot TidyOS (or press Enter) when you're ready");
      else if (want == 99) snprintf(b, sizeof(b), "Waiting for Wi-Fi...   click anything to stop the timer");
      else snprintf(b, sizeof(b), "Booting in %d s   click anything to stop the timer", want);
      bootStatus(cv, b);
    }
    cv.waitCompletion();
    delay(50);
  }
  inputBootGrab(false);
  if (gSet.colors != startColors && (gSet.colors == 0 ? 4 : gSet.colors) != gColors) {   // screen mode changed here: set up at power-on
    s_skipChooser = 0x5EED;
    bootStatus(cv, "Restarting with the new screen...");
    cv.waitCompletion();
    delay(600);
    ESP.restart();
  }
  Serial.printf("[TidyOS] start-up switches: SD %s, mesh %s, light %s, shelf %s, colours %d\n", gSet.sdOn ? "on" : "off", gSet.meshOn ? "on" : "off", gSet.ledOn ? "on" : "off", gSet.shelfOn ? "on" : "off", gSet.colors);
}
#endif

// what happened to the last update (the installer leaves tidyos/update.log on the card)
static void checkUpdateNote()
{
  {                                              // the installer couldn't use the card: its note is in flash
    Preferences p;
    if (p.begin("tidyload", false)) {
      String n = p.getString("note", "");
      if (n.length()) {
        p.remove("note");
        Serial.printf("[TidyOS] update installer said: %s\n", n.c_str());
        if (n != "no update waiting") s_bootNote = std::string("The update wasn't installed: ") + n.substring(0, 70).c_str();
      }
      p.end();
    }
  }
  if (!gSDMounted) return;
  std::string l;
  if (readFileStr(SD_MOUNT "/tidyos/update.log", l, 200)) {
    remove(SD_MOUNT "/tidyos/update.log");
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
    Serial.printf("[TidyOS] update installer said: %s\n", l.c_str());
    if (l.find("installed") != std::string::npos) s_bootNote = "TidyOS was updated to " TIDYOS_VERSION ".";
    else s_bootNote = "The update wasn't installed: " + l.substr(0, 70);
  } else if (fileSize(SD_MOUNT "/tidyos/update.bin") > 0) {
    std::string ver;
    readFileStr(SD_MOUNT "/tidyos/update.ver", ver, 40);
    while (!ver.empty() && (ver.back() == '\n' || ver.back() == ' ')) ver.pop_back();
    auto vnum = [](const std::string & v) { int a = 0, b = 0, c = 0; sscanf(v.c_str(), "%d.%d.%d", &a, &b, &c); return a * 1000000 + b * 1000 + c; };
    if (ver.empty()) { }                  // a leftover from an earlier update (its note already shown): just tidy up
    else if (ver == TIDYOS_VERSION) s_bootNote = "TidyOS was updated to " TIDYOS_VERSION ".";   // it went in; the installer just couldn't tidy up
    else if (vnum(TIDYOS_VERSION) >= vnum(ver)) { }                         // already as new or newer (e.g. from the web flasher)
    else if (s_bootNote.empty())          // (the installer's own note from flash, if any, says more - keep that)
      s_bootNote = "An update was downloaded but the installer didn't run. Try again.";
    if (remove(SD_MOUNT "/tidyos/update.bin") != 0) {       // can't delete it? empty it, so it's never seen as waiting again
      FILE * z = fopen(SD_MOUNT "/tidyos/update.bin", "wb");
      if (z) fclose(z);
      Serial.println("[TidyOS] couldn't delete update.bin - emptied it");
    }
  }
  remove(SD_MOUNT "/tidyos/update.ver");
}

void setup()
{
#ifdef TIDYOS_HTML5
  esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);   // no Bluetooth in this edition: give its RAM to apps
#endif
  Serial.begin(115200);
  delay(200);
  crashBegin();     // did the last run crash? (Task Manager shows it)
  ledBegin();       // blue LED: slow blink while starting
  Serial.println("\n[TidyOS] " TIDYOS_VERSION " (" TIDYOS_EDITION " edition) starting");

  settingsLoad();
  ledSetOn(gSet.ledOn);   // ledBegin ran before the settings were read, so the switch looked "off" to it

  bool updMode = updateModeRequested();
  if (!updMode) updateRestoreColors();    // back from an update: the screen mode it ran in was only for the update
  if (updMode) {
    // update mode: screen + Wi-Fi only, so there's room for HTTPS
    Serial.println("[TidyOS] update mode");
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
    netBegin();
    startDisplay();
#ifdef TIDYOS_HTML5
    releaseVgaPins();
    screenBegin();
#endif
    splashLogo();
    barStart();
    updateRun(splash);
  }
  inputBegin();     // Bluetooth first: it needs a big block of RAM
  std::set_new_handler(lowMemoryWait);
  memLog("start");
  netBegin();       // Wi-Fi radio
  memLog("Wi-Fi");

#ifdef TIDYOS_HTML5
  startDisplay();
  releaseVgaPins();     // no monitor in this edition: free GPIO 19/18/5/... (the in-line SD wiring uses them)
  bootChooser();        // start-up switches (SD card, mesh, blue light) before they take any RAM
  if (gSet.sdOn) gSDMounted = mountAnySD();
#else
  if (gSet.sdOn) gSDMounted = mountAnySD();
  startDisplay();
#endif
  Serial.printf("[TidyOS] SD card %s\n", gSDMounted ? "mounted" : gSet.sdOn ? "not found - using internal flash" : "off (SD Card icon turns it on) - using internal flash");

  splashLogo();
  barStart();
  splash(gSDMounted ? "SD card found" : gSet.sdOn ? "No SD card - preparing internal storage..." : "Preparing internal storage...", 30);

  if (!gSDMounted) gFlashMounted = FileBrowser::mountSPIFFS(true, FLASH_MOUNT, 2);
  memLog("display + storage");
  checkUpdateNote();
  makeDirs();
  crashSave(dataPath("crash.txt"));
  splash("Loading the dictionary", 50);
  spellBegin();
  memLog("dictionary");
  splash("Looking for your Shelves", 70);
#ifdef TIDYOS_HTML5
  if (gSet.ssid.length() && msgWanted()) msgBegin();   // otherwise started once on home Wi-Fi (saves RAM for the setup hotspot)
#else
  if (msgWanted()) msgBegin();       // The Shelf messaging + Coms mesh
#endif
  memLog("Shelf + mesh");
  if (gSet.fileShare) fileServerSet(true);
  splash("Starting the web screen", 80);
  screenBegin();    // HTML5 edition: the desktop in a web browser
  memLog("web screen");
#ifdef TIDYOS_HTML5
  splash(gSet.ssid.length() ? "Joining Wi-Fi" : "Starting the TidyOS-Setup hotspot", 90);
#else
  splash(gSet.ssid.length() ? "Joining Wi-Fi and Bluetooth" : "Starting Bluetooth", 90);
#endif
  delay(400);
#ifdef TIDYOS_HTML5
  // keep the bar going until the page has looked up the weather and headlines, so the desktop opens filled in
  if (screenViewer() && WiFi.status() == WL_CONNECTED) {
    splash("Getting the weather and headlines", 94);
    screenMedia("w");
    uint32_t t0 = millis();
    while (millis() - t0 < 10000 && screenViewer() && !((s_wx.code >= 0 || s_wx.err[0]) && s_newsCount > 0)) delay(100);
  }
#endif
  splash("Ready", 100);
  delay(700);           // a moment to see it all come up
  barStop();
  ledReady();       // blue LED: breathing
}

void loop()
{
  s_app.run(s_dc, &s_keyboard, nullptr);
}

// ====================================================================== widget styling

void applyStyle(uiObject * o)
{
  auto & t = o->objectType();
  if (t.uiWindow) {
    auto w = (uiWindow*)o;
    w->windowStyle().borderColor        = T.shadow;
    w->windowStyle().activeBorderColor  = T.accent;
    w->windowStyle().focusedBorderColor = T.accent;
  }
  if (t.uiFrame) {
    auto f = (uiFrame*)o;
    f->frameStyle().backgroundColor                = T.win;
    f->frameStyle().titleBackgroundColor           = T.face;
    f->frameStyle().activeTitleBackgroundColor     = T.accent;
    f->frameStyle().titleColor                     = T.text;
    f->frameStyle().activeTitleColor               = T.accentText;
    f->frameStyle().titleFont                      = &FONT_std_14;
    f->frameStyle().buttonColor                    = T.text;
    f->frameStyle().activeButtonColor              = T.accentText;
    f->frameStyle().mouseOverBackgroundButtonColor = T.win;
    f->frameStyle().mouseOverButtonColor           = T.text;
    f->windowStyle().borderColor                   = T.face;
    f->windowStyle().borderSize                    = 2;
  }
  if (t.uiScrollableControl) {
    auto c = (uiScrollableControl*)o;
    c->scrollableControlStyle().scrollBarBackgroundColor          = T.face;
    c->scrollableControlStyle().scrollBarForegroundColor          = T.shadow;
    c->scrollableControlStyle().mouseOverScrollBarForegroundColor = T.accent;
  }
  if (t.uiButton) {
    auto b = (uiButton*)o;
    b->buttonStyle().backgroundColor          = T.face;
    b->buttonStyle().downBackgroundColor      = T.accent;
    b->buttonStyle().mouseOverBackgroundColor = T.win;
    b->buttonStyle().mouseDownBackgroundColor = T.accent;
    b->buttonStyle().mouseOverTextColor       = T.text;
    b->buttonStyle().textColor                = T.text;
    b->buttonStyle().downTextColor            = T.accentText;
  }
  if (t.uiTextEdit) {
    auto e = (uiTextEdit*)o;
    e->textEditStyle().backgroundColor          = T.win;
    e->textEditStyle().mouseOverBackgroundColor = T.win;
    e->textEditStyle().focusedBackgroundColor   = T.win;
    e->textEditStyle().textColor                = T.text;
    e->windowStyle().borderSize                 = 1;
  }
  if (t.uiLabel) {
    auto l = (uiLabel*)o;
    l->labelStyle().backgroundColor = T.win;
    l->labelStyle().textColor       = T.text;
  }
  if (t.uiCustomListBox) {
    auto l = (uiCustomListBox*)o;
    l->listBoxStyle().backgroundColor                = T.win;
    l->listBoxStyle().focusedBackgroundColor         = T.win;
    l->listBoxStyle().selectedBackgroundColor        = T.accent;
    l->listBoxStyle().focusedSelectedBackgroundColor = T.accent;
    l->listBoxStyle().textColor                      = T.text;
    l->listBoxStyle().selectedTextColor              = T.accentText;
    l->windowStyle().borderSize                      = 1;
  }
  if (t.uiCheckBox) {
    auto c = (uiCheckBox*)o;
    c->checkBoxStyle().backgroundColor          = T.win;
    c->checkBoxStyle().checkedBackgroundColor   = T.win;
    c->checkBoxStyle().mouseOverBackgroundColor = T.face;
    c->checkBoxStyle().foregroundColor          = T.text;
    c->checkBoxStyle().mouseOverForegroundColor = T.text;
  }
}
