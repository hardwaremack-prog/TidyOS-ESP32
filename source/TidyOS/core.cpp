// TidyOS core: display start-up, theme, settings, storage, desktop, main app
#include <Preferences.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include "core.h"
#include "input.h"
#include "net.h"
#include "style.h"

using namespace fabgl;

Theme    T;
int      gColors      = 4;
bool     gSDMounted   = false;
bool     gFlashMounted= false;
Settings gSet;
uiApp *  gApp = nullptr;

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
struct V4  : public VGA4Controller  { void lut() { updateRGB2PaletteLUT(); } };

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

  T.win        = PAL[1];
  T.text       = PAL[0];
  T.face       = PAL[2];
  T.accent     = PAL[3];
  T.accentText = PAL[1];
  if (colors == 4) {
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

void bringToFront(uiFrame * f)
{
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

struct DeskIcon {
  const char *  label;
  const char ** art;      // pixel art, or nullptr for a glyph tile
  const char *  glyph;
  void (*open)();
};

static void openNeat0()    { openNeat(nullptr); }
static void openTidy0()    { openTidy(nullptr); }
static void openBrowser0() { openBrowser(nullptr); }

static const DeskIcon ICONS[] = {
  { "Calculator", ART_CALC,  nullptr, openCalculator },
  { "Neat",       ART_NEAT,  nullptr, openNeat0 },
  { "Tidy",       ART_TIDY,  nullptr, openTidy0 },
  { "Ready",      ART_READY, nullptr, openReady },
  { "Steady",     nullptr,   "$",     openSteady },
  { "Browser",    nullptr,   "www",   openBrowser0 },
  { "Files",      ART_FILES, nullptr, openFiles },
  { "Settings",   ART_GEAR,  nullptr, openSettings },
  { "System",     ART_SYS,   nullptr, openSystem },
};
static const int NICONS = sizeof(ICONS) / sizeof(ICONS[0]);

class Desktop : public uiControl {
public:
  static const int BAR_H = 20;
  static const int CELL_W = 84, CELL_H = 80, X0 = 8, Y0 = 10;

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
          int i = hit(event->params.mouse.status.X, event->params.mouse.status.Y);
          if (i >= 0) { m_pressed = i; repaintIcon(i); }
        }
        break;
      case UIEVT_MOUSEBUTTONUP:
        if (event->params.mouse.changedButton == 1 && m_pressed >= 0) {
          int i = m_pressed;
          m_pressed = -1;
          repaintIcon(i);
          if (hit(event->params.mouse.status.X, event->params.mouse.status.Y) == i)
            ICONS[i].open();
        }
        break;
      case UIEVT_TIMER:
        repaint(Rect(0, SCREEN_H - BAR_H, SCREEN_W - 1, SCREEN_H - 1));
        {
          // refresh the welcome / pairing hint only when something changed
          std::string sig = inputStatus() + char('0' + inputKeyboards()) + char('0' + inputMice()) + (gSet.ssid.length() ? "w" : "-");
          if (sig != m_hintSig) {
            m_hintSig = sig;
            repaint(Rect(CELL_W * 2 + 20, 140, SCREEN_W - 1, 330));
          }
        }
        break;
      default:
        break;
    }
  }

private:
  int m_pressed = -1;
  std::string m_hintSig;

  Rect iconRect(int i) {
    int col = i % 2, row = i / 2;
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
    int w = cv->textExtent(&FONT_std_14, ic.label);
    cv->setPenColor(T.deskText);
    cv->setBrushColor(down ? T.shadow : T.desk);
    cv->drawText(&FONT_std_14, r.X1 + CELL_W / 2 - w / 2, iy + 40, ic.label);
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
    // welcome / pairing hint
    bool needKbd = inputKeyboards() == 0, needMouse = inputMice() == 0;
    if (needKbd || needMouse || !gSet.ssid.length()) {
      int x = CELL_W * 2 + 40, y = 150;
      cv->setPenColor(T.deskText);
      cv->setBrushColor(T.desk);
      cv->drawText(&FONT_std_24, x, y, "Welcome to TidyOS");
      y += 34;
      if (needKbd)
        cv->drawText(&FONT_std_14, x, y, "1. Put your Bluetooth keyboard in pairing mode.");
      else
        cv->drawText(&FONT_std_14, x, y, "1. Keyboard connected!");
      y += 20;
      if (needMouse)
        cv->drawText(&FONT_std_14, x, y, "2. Then put your Bluetooth mouse in pairing mode.");
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
        cv->drawText(&FONT_std_12, x, y, "No Bluetooth? Type in the Arduino Serial Monitor (115200).");
    }
    // status bar
    int by = SCREEN_H - BAR_H;
    if (clip.Y2 >= by) {
      cv->setBrushColor(T.face);
      cv->fillRectangle(0, by, SCREEN_W - 1, SCREEN_H - 1);
      cv->setPenColor(T.shadow);
      cv->drawLine(0, by, SCREEN_W - 1, by);
      cv->setPenColor(T.text);
      cv->setBrushColor(T.face);
      cv->drawText(&FONT_std_14, 6, by + 3, "TidyOS");
      char b[96];
      std::string ns = netStatus();
      snprintf(b, sizeof(b), "%s", ns.c_str());
      cv->drawText(&FONT_std_12, 70, by + 4, b);
      snprintf(b, sizeof(b), "Keyboard:%s  Mouse:%s  RAM %uK", inputKeyboards() ? "on" : "--", inputMice() ? "on" : "--",
               (unsigned)(ESP.getFreeHeap() / 1024));
      cv->drawText(&FONT_std_12, 330, by + 4, b);
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
    setTimer(desk, 1000);
    inputAttach(this, &s_keyboard, s_dc);
    displayController()->setMouseCursor(CursorName::CursorPointerSimpleReduced);
  }
};
static TidyApp s_app;

// ====================================================================== start-up

template <class C>
static BitmappedDisplayController * tryMode(size_t fbBytes)
{
  size_t freeMem = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  Serial.printf("[TidyOS] free %u, need %u\n", (unsigned)freeMem, (unsigned)(fbBytes + 60000));
  if (freeMem < fbBytes + 60000) return nullptr;
  C * c = new C;
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
  int want = gSet.colors;   // 0 = auto
  BitmappedDisplayController * dc = nullptr;
  int got = 0;
  if (!dc && (want == 0 || want >= 8))  { dc = tryMode<V8>(640 * 480 * 3 / 8); got = 8; }
  if (!dc)                              { dc = tryMode<V4>(640 * 480 / 4);     got = 4; }
  if (!dc) {
    // last resort: 4 colours without the safety margin
    auto c = new V4;
    c->begin(VGA_RED1, VGA_RED0, VGA_GREEN1, VGA_GREEN0, VGA_BLUE1, VGA_BLUE0, VGA_HSYNC, VGA_VSYNC);
    c->setResolution(VGA_640x480_60Hz);
    dc = c; got = 4;
  }
  s_dc = dc;
  themeSetup(dc, got);
  Serial.printf("[TidyOS] display 640x480, %d colours, free heap %u\n", got, (unsigned)ESP.getFreeHeap());
}

static void makeDirs()
{
  if (gSDMounted) {
    mkdir(SD_MOUNT "/tidyos", 0777);
    mkdir(SD_MOUNT "/Documents", 0777);
  }
}

static void splash(const char * msg)
{
  Canvas cv(s_dc);
  cv.setBrushColor(T.desk);
  cv.clear();
  cv.setPenColor(T.deskText);
  cv.drawText(&FONT_std_24, 240, 200, "TidyOS");
  cv.drawText(&FONT_std_14, 240, 236, msg);
  cv.waitCompletion();
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[TidyOS] " TIDYOS_VERSION " starting");

  settingsLoad();
  inputBegin();     // Bluetooth first: it needs a big block of RAM
  netBegin();       // Wi-Fi radio

  gSDMounted = FileBrowser::mountSDCard(false, SD_MOUNT, 6, 16 * 1024, SD_MISO, SD_MOSI, SD_CLK, SD_CS);
  Serial.printf("[TidyOS] SD card %s\n", gSDMounted ? "mounted" : "not found - using internal flash");

  startDisplay();

  if (!gSDMounted) {
    splash("No SD card - preparing internal storage...");
    gFlashMounted = FileBrowser::mountSPIFFS(true, FLASH_MOUNT, 4);
  }
  makeDirs();
  splash("Starting...");
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
