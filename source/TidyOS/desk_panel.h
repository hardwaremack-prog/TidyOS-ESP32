// TidyOS desktop: the panel on the right - time, date, weather, world times, a daily affirmation,
// and a world map shaded for day and night. (included in core.cpp, before the Desktop)
#include "worldmap.h"
#include "tzcalc.h"
#include "net.h"

// weather arrives from the web page (it looks it up: ip-api.com for the place, Open-Meteo for the forecast)
static struct { int temp = 0, code = -1, hi = 0, lo = 0; float lat = 0, lon = 0; bool haveLoc = false; char city[32] = ""; char err[48] = ""; } s_wx;
static volatile int s_wxVer = 0;

// from the page: "41|3|45|33|42.56|-82.63|Harsons Island", or "E|why|lat|lon"
void deskWeatherFromPage(const char * t)
{
  if (t[0] == 'E' && t[1] == '|') {
    const char * why = t + 2, * bar = strchr(why, '|');
    size_t n = bar ? (size_t)(bar - why) : strlen(why);
    snprintf(s_wx.err, sizeof(s_wx.err), "%.*s", (int)n, why);
    float la, lo;
    if (bar && sscanf(bar + 1, "%f|%f", &la, &lo) == 2) { s_wx.lat = la; s_wx.lon = lo; s_wx.haveLoc = true; }
    s_wxVer++;
    return;
  }
  int temp, code, hi, lo, n = 0;
  float la, lon;
  if (sscanf(t, "%d|%d|%d|%d|%f|%f|%n", &temp, &code, &hi, &lo, &la, &lon, &n) < 6) return;
  s_wx.temp = temp; s_wx.code = code; s_wx.hi = hi; s_wx.lo = lo;
  s_wx.lat = la; s_wx.lon = lon; s_wx.haveLoc = true;
  strlcpy(s_wx.city, n ? t + n : "", sizeof(s_wx.city));
  s_wx.err[0] = 0;
  s_wxVer++;
}

// news headlines, also from the page (NPR, or BBC News as a fallback)
static char s_news[6][120];
static int s_newsCount = 0;

// temperatures every 3 hours over the last 4 days, for the little graph
static int8_t s_temps[40];
static int s_tempsN = 0;
static int s_cityT[3]; static bool s_cityHave = false;   // Moscow, Los Angeles, Tokyo (F)
void deskCityTempsFromPage(const char * t)
{
  s_cityHave = sscanf(t, "%d,%d,%d", &s_cityT[0], &s_cityT[1], &s_cityT[2]) == 3;
  s_wxVer++;
}
void deskTempsFromPage(const char * t)
{
  int n = 0;
  while (*t && n < 40) {
    char * e; long v = strtol(t, &e, 10);
    if (e == t) break;
    s_temps[n++] = (int8_t)(v < -99 ? -99 : v > 127 ? 127 : v);
    t = *e == ',' ? e + 1 : e;
  }
  s_tempsN = n;
  s_wxVer++;
}

void deskNewsFromPage(const char * t)
{
  s_newsCount = 0;
  while (*t && s_newsCount < 6) {
    const char * e = strchr(t, '\n');
    size_t n = e ? (size_t)(e - t) : strlen(t);
    snprintf(s_news[s_newsCount++], sizeof(s_news[0]), "%.*s", (int)n, t);
    t = e ? e + 1 : t + n;
  }
  s_wxVer++;
}

// files dropped on the page: the page saves them in SD:/Incoming, then the desktop asks where each one goes
static portMUX_TYPE s_dropMux = portMUX_INITIALIZER_UNLOCKED;
static char s_dropQ[8][100];
static volatile int s_dropW = 0, s_dropR = 0;
bool gitStoreDropAt(int x, int y);          // gitstore_app.h
void deskDropStart(int x, int y)            // the page has files to send (screen server task); x, y = where they were dropped
{
  if (gitStoreDropAt(x, y)) return;         // onto Git-Store's box: straight to the cloud
  if (!gSDMounted) { screenMedia("u nosd"); return; }
  mkdir(SD_MOUNT "/Incoming", 0777);
  fileServerMedia(true);
  screenMedia("u go");
}
void deskDropped(const char * name)         // one has arrived in SD:/Incoming
{
  portENTER_CRITICAL(&s_dropMux);
  if (s_dropW - s_dropR < 8) { strlcpy(s_dropQ[s_dropW % 8], name, sizeof(s_dropQ[0])); s_dropW++; }
  portEXIT_CRITICAL(&s_dropMux);
}
static void deskDropTick()                  // on the desktop's timer: ask where each dropped file goes
{
  static bool asking = false;
  if (asking || s_dropR == s_dropW) return;
  char name[100];
  portENTER_CRITICAL(&s_dropMux);
  strlcpy(name, s_dropQ[s_dropR % 8], sizeof(name));
  s_dropR++;
  portEXIT_CRITICAL(&s_dropMux);
  asking = true;
  std::string from = std::string(SD_MOUNT "/Incoming/") + name;
  std::string to = askFile((std::string("Where should ") + name + " go?").c_str(), "Put it here", name, SD_MOUNT);
  if (!to.empty() && to != from) {
    if (fileSize(to.c_str()) >= 0 &&
        gApp->messageBox("Dropped file", "A file with that name is already there. Replace it?", "Replace", "Keep both", nullptr, uiMessageBoxIcon::Question) == uiMessageBoxResult::Button1)
      remove(to.c_str());
    if (fileSize(to.c_str()) >= 0) {                      // keep both: add a number
      std::string base = to, ext;
      size_t dot = to.rfind('.'), sl = to.rfind('/');
      if (dot != std::string::npos && dot > sl) { base = to.substr(0, dot); ext = to.substr(dot); }
      for (int n = 2; n < 100 && fileSize(to.c_str()) >= 0; ++n) to = base + " (" + std::to_string(n) + ")" + ext;
    }
    if (rename(from.c_str(), to.c_str()) != 0)
      gApp->messageBox("Dropped file", "Couldn't move it - it's still in the Incoming folder.", "OK", nullptr, nullptr, uiMessageBoxIcon::Warning);
  } else if (to.empty()) {
    gApp->messageBox("Dropped file", (std::string(name) + " stays in the Incoming folder on the SD card.").c_str(), "OK", nullptr, nullptr, uiMessageBoxIcon::Info);
  }
  asking = false;
}

static const char * wxText(int c)
{
  if (c == 0) return "Clear";
  if (c == 1) return "Mostly clear";
  if (c == 2) return "Partly cloudy";
  if (c == 3) return "Cloudy";
  if (c == 45 || c == 48) return "Fog";
  if (c >= 51 && c <= 57) return "Drizzle";
  if (c >= 61 && c <= 67) return "Rain";
  if (c >= 71 && c <= 77) return "Snow";
  if (c >= 80 && c <= 82) return "Showers";
  if (c == 85 || c == 86) return "Snow showers";
  if (c >= 95) return "Thunderstorms";
  return "";
}

static const char * AFFIRM[] = {
  "I can do hard things, one step at a time.", "Today I choose patience with myself and others.",
  "Small progress is still progress.", "I am allowed to rest and start again.",
  "My curiosity makes the world bigger.", "I build things that help the people I love.",
  "Mistakes are how I learn.", "I have handled tough days before. I can handle this one.",
  "I bring something good to every room I walk into.", "I am proud of how far I have come.",
  "Kindness costs nothing and changes everything.", "I focus on what I can control.",
  "Every problem is a puzzle waiting for me.", "I am becoming the person I want to be.",
  "Good things are on their way to me.", "I listen, I learn, and I grow.",
  "Today is a fresh start.", "I make time for what matters most.",
  "My work has value, and so do I.", "I can ask for help - that is strength too.",
  "I keep my promises to myself.", "A calm mind finds the answer.",
  "I celebrate the wins, big and small.", "I am grateful for the people around me.",
  "Done is better than perfect.", "I am creative, capable and resourceful.",
  "I choose to see what is going right.", "Each day I get a little better.",
  "I can fix it, build it, or figure it out.", "My best is enough.",
  "I welcome new ideas.", "I take care of my body and my mind.",
  "I am patient with things that take time.", "I share what I know.",
  "Today I will make someone smile.", "I trust myself to make good choices.",
};
static const int NAFFIRM = sizeof(AFFIRM) / sizeof(AFFIRM[0]);

static void zoneTime(const char * tz, time_t now, struct tm & out)
{
  tzLocal(tz, now, out);                           // (no setenv: it leaked memory every call)
}

// the >> button: show the next affirmations too (up to 3); back to one each new day
static int s_affShown = 1, s_affDay = -1;
static Rect s_affBtn;

static const int PANEL_X = 344, PANEL_Y = 8, PANEL_W = 290, PANEL_H = 440;
static Rect deskPanelRect() { return Rect(PANEL_X, PANEL_Y, PANEL_X + PANEL_W - 1, PANEL_Y + PANEL_H - 1); }

#ifdef TIDYOS_HTML5
// the music buttons beside the time (MP3 play / pause, volume - / +, next), and what one just did, shown in the
// drop box for a few seconds
static Rect s_mp3Btn, s_volDnBtn, s_volUpBtn, s_nextBtn;
static char s_panNote1[24] = "", s_panNote2[100] = "";
static uint32_t s_panNoteAt = 0;
static bool panNoteOn() { return s_panNoteAt && millis() - s_panNoteAt < 4000; }
static void deskNote(const char * a, const std::string & b) { strlcpy(s_panNote1, a, sizeof(s_panNote1)); strlcpy(s_panNote2, b.c_str(), sizeof(s_panNote2)); s_panNoteAt = millis() | 1; }
bool musicQuickPlaying();
std::string musicQuickToggle(bool midi);
std::string musicQuickVol(int d);
#endif

// what the panel shows changes once a minute (or when weather arrives, a music button is pressed...)
static int deskPanelSig()
{
  time_t now = time(nullptr);
  int extra = 0;
#ifdef TIDYOS_HTML5
  extra = (panNoteOn() ? 1 : 0) * 3 + (musicQuickPlaying() ? 1 : 0) * 5 + (int)(s_panNoteAt & 0x3FF) * 11;
#endif
  return (int)(now / 60) * 7 + s_wxVer + s_affShown * 100003 + extra * 1009;
}

static void drawWorldMap(Canvas * cv, int x0, int y0, time_t now)
{
  // where the sun is overhead: declination from the day of the year, longitude from the UTC time
  struct tm u; gmtime_r(&now, &u);
  float doy = u.tm_yday + (u.tm_hour + u.tm_min / 60.0f) / 24.0f;
  float decl = -23.44f * cosf(2 * PI / 365.0f * (doy + 10)) * PI / 180;
  float sunLon = -15.0f * (u.tm_hour + u.tm_min / 60.0f - 12) * PI / 180;
  float sd = sinf(decl), cd = cosf(decl);
  const int W = 144, H = 72, S = 2, Y0 = 4, Y1 = 64;     // rows 4..63: 80N to 70S (the poles have little land)
  y0 -= Y0 * S;
  for (int y = Y0; y < Y1; ++y) {
    float lat = (90 - (y + 0.5f) * 180 / H) * PI / 180;
    float sl = sinf(lat), cl = cosf(lat);
    int runX = 0; RGB888 runC = T.win;
    for (int x = 0; x <= W; ++x) {
      RGB888 c = runC;
      if (x < W) {
        float lon = (-180 + (x + 0.5f) * 360 / W) * PI / 180;
        float h = sl * sd + cl * cd * cosf(lon - sunLon);           // sine of the sun's height
        bool land = WORLD_LAND[y * 18 + (x >> 3)] & (0x80 >> (x & 7));
        bool day = h > 0.05f || (h > -0.1f && ((x + y) & 1));        // a dithered dusk band
        c = land ? (day ? T.face : T.text) : (day ? T.win : T.accent);
      }
      if (x == 0) { runC = c; continue; }
      if (x == W || !(c == runC)) {
        cv->setBrushColor(runC);
        cv->fillRectangle(x0 + runX * S, y0 + y * S, x0 + x * S - 1, y0 + y * S + S - 1);
        runX = x; runC = c;
      }
    }
  }
  cv->setPenColor(T.text);
  cv->drawRectangle(x0 - 1, y0 + Y0 * S - 1, x0 + W * S, y0 + Y1 * S);
  // you are here (from the same lookup as the weather)
  if (s_wx.haveLoc) {
    int px = x0 + (int)((s_wx.lon + 180) / 360 * W * S), py = y0 + (int)((90 - s_wx.lat) / 180 * H * S);
    cv->setBrushColor(T.win);
    cv->fillEllipse(px, py, 11, 11);
    cv->setPenColor(T.text);
    cv->drawEllipse(px, py, 11, 11);
    cv->setBrushColor(T.text);
    cv->fillEllipse(px, py, 5, 5);
    cv->drawLine(px - 9, py, px - 6, py); cv->drawLine(px + 6, py, px + 9, py);
    cv->drawLine(px, py - 9, px, py - 6); cv->drawLine(px, py + 6, px, py + 9);
  }
}

// the last 4 days of temperatures, with the freezing line at 32 F (red where the screen has red)
static Rect s_graphRect;        // where the graph is (for the mouse)
static int s_graphHover = -1;   // the point under the mouse
static void drawTempGraph(Canvas * cv, int gx, int gy, int gw, int gh)
{
  s_graphRect = Rect(gx, gy, gx + gw, gy + gh);
  int lo = 999, hi = -999;
  for (int i = 0; i < s_tempsN; ++i) { lo = imin(lo, s_temps[i]); hi = imax(hi, s_temps[i]); }
  if (lo < 45 && hi > 20) { lo = imin(lo, 30); hi = imax(hi, 34); }     // keep the freezing line in view when it matters
  if (hi - lo < 6) { hi += 3; lo -= 3; }
  cv->setPenColor(T.shadow);
  cv->drawRectangle(gx, gy, gx + gw, gy + gh);
  auto Y = [&](int v) { return gy + gh - 2 - (v - lo) * (gh - 4) / (hi - lo); };
  // midnight lines, and each day's letter underneath (points are 3 hours apart, the last one is this hour)
  time_t now = time(nullptr);
  time_t tLast = now - now % 3600, t0 = tLast - (time_t)(s_tempsN - 1) * 3 * 3600;
  auto X = [&](time_t t) { return gx + 1 + (int)((t - t0) * (gw - 2) / (tLast - t0)); };
  struct tm m; localtime_r(&t0, &m);
  m.tm_hour = m.tm_min = m.tm_sec = 0; m.tm_mday += 1; m.tm_isdst = -1;
  time_t mid = mktime(&m);
  time_t segStart = t0;
  static const char * DL = "SMTWTFS";
  cv->setBrushColor(T.win);
  while (true) {
    time_t segEnd = mid <= tLast ? mid : tLast;
    if (mid <= tLast) { int x = X(mid); cv->setPenColor(T.shadow); for (int k = gy + 2; k < gy + gh; k += 4) cv->setPixel(x, k); }
    int x1 = X(segStart), x2 = X(segEnd);
    if (x2 - x1 >= 8) {
      time_t midT = segStart + (segEnd - segStart) / 2;
      struct tm d; localtime_r(&midT, &d);
      char s[2] = { DL[d.tm_wday % 7], 0 };
      cv->setPenColor(T.text);
      cv->drawText(&FONT_std_12, (x1 + x2) / 2 - 3, gy + gh + 1, s);
    }
    if (mid > tLast) break;
    segStart = mid;
    struct tm n; localtime_r(&mid, &n); n.tm_mday += 1; n.tm_hour = 0; n.tm_isdst = -1;
    mid = mktime(&n);
  }
  if (32 >= lo && 32 <= hi) {
    RGB888 red = gColors >= 8 ? themePal(6) : T.text;
    cv->setPenColor(red);
    int y = Y(32);
    for (int k = gx + 1; k < gx + gw; k += (gColors >= 8 ? 1 : 3)) cv->setPixel(k, y);
    if (gColors >= 8) for (int k = gx + 1; k < gx + gw; ++k) cv->setPixel(k, y + 1);
    // "freezing ->" to the left of the graph, pointing at the line
    cv->setPenColor(red);
#ifdef TIDYOS_HTML5                                  // (the VGA edition has no room left for the small font)
    int fw = cv->textExtent(&FONT_5x7, "freezing");
    cv->setBrushColor(T.win);
    cv->drawText(&FONT_5x7, gx - 13 - fw, y - 3, "freezing");
#endif
    cv->drawLine(gx - 11, y, gx - 2, y);
    cv->drawLine(gx - 5, y - 3, gx - 2, y);
    cv->drawLine(gx - 5, y + 3, gx - 2, y);
  }
  cv->setPenColor(T.accent);
  cv->setPenWidth(2);
  for (int i = 1; i < s_tempsN; ++i)
    cv->drawLine(gx + 1 + (i - 1) * (gw - 2) / (s_tempsN - 1), Y(s_temps[i - 1]), gx + 1 + i * (gw - 2) / (s_tempsN - 1), Y(s_temps[i]));
  cv->setPenWidth(1);
  // the mouse over the graph: that point's time and temperature
  if (s_graphHover >= 0 && s_graphHover < s_tempsN) {
    int i = s_graphHover, px = gx + 1 + i * (gw - 2) / (s_tempsN - 1), py = Y(s_temps[i]);
    cv->setPenColor(T.text);
    for (int k = gy + 1; k < gy + gh; k += 2) cv->setPixel(px, k);
    cv->setBrushColor(T.text);
    cv->fillEllipse(px, py, 5, 5);
    time_t when = tLast - (time_t)(s_tempsN - 1 - i) * 3 * 3600;
    struct tm w; localtime_r(&when, &w);
    static const char * DN[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    char b[32];
    snprintf(b, sizeof(b), "%s %d%s %d\xB0", DN[w.tm_wday % 7], w.tm_hour % 12 == 0 ? 12 : w.tm_hour % 12, w.tm_hour < 12 ? "am" : "pm", s_temps[i]);
    int tw = cv->textExtent(&FONT_std_12, b);
    int lx = imax(gx + 1, imin(px - tw / 2, gx + gw - tw - 3));
    cv->setBrushColor(T.win);
    cv->fillRectangle(lx - 2, gy + 1, lx + tw + 1, gy + 13);
    cv->setPenColor(T.text);
    cv->drawRectangle(lx - 2, gy + 1, lx + tw + 1, gy + 13);
    cv->drawText(&FONT_std_12, lx, gy + 1, b);
  }
}


// ---- the news line: one headline at a time. A short one stays 5 s; a long one waits 1.5 s, slides along
// (2 pixels every 100 ms) until its end shows, waits 1.5 s, then the next headline takes its turn.
static Rect s_tickRect(0, 0, -1, -1);
static int s_tickIdx = 0, s_tickPos = 0, s_tickPx = 0, s_tickPhase = 0;   // phase 0 wait, 1 slide, 2 wait at the end
static uint32_t s_tickAt = 0;
static Rect newsTickerRect() { return s_tickRect; }
static int tickTextX(Canvas * cv) { return s_tickRect.X1 + 9 + cv->textExtent(&FONT_std_12, "NEWS") + 12; }
static void drawNewsTicker(Canvas * cv)
{
  Rect k = s_tickRect;
  if (k.X2 <= k.X1) return;
  const fabgl::FontInfo * f = &FONT_std_12;
  cv->setBrushColor(T.win);
  cv->fillRectangle(k.X1, k.Y1, k.X2, k.Y2);
  if (!s_newsCount) return;
  if (s_tickIdx >= s_newsCount) { s_tickIdx = 0; s_tickPos = s_tickPx = 0; }
  const char * t = s_news[s_tickIdx];
  int n = strlen(t), xs = tickTextX(cv), xe = k.X2 - 8, y = k.Y1 + 2;
  if (s_tickPos > n) s_tickPos = n;
  std::string vis;                                  // from the left-edge letter on, until the right edge
  for (int i = s_tickPos; i < n; ++i) {
    vis += t[i];
    if (xs - s_tickPx + cv->textExtent(f, vis.c_str()) > xe) { vis.pop_back(); break; }
  }
  cv->setPenColor(T.text); cv->setBrushColor(T.win);
  cv->drawText(f, xs - s_tickPx, y, vis.c_str());
  cv->setBrushColor(T.win);                         // tidy edges: the letter sliding out on the left
  cv->fillRectangle(k.X1, k.Y1, xs - 1, k.Y2);
  cv->fillRectangle(xe + 1, k.Y1, k.X2, k.Y2);
  int x0 = k.X1 + 9, tw = cv->textExtent(f, "NEWS");          // the NEWS tag, with which headline this is
  cv->setBrushColor(T.accent); cv->setPenColor(T.accentText);
  cv->fillRectangle(x0 - 3, k.Y1 + 1, x0 + tw + 2, k.Y2 - 1);
  cv->drawText(f, x0, k.Y1 + 2, "NEWS");
  cv->setBrushColor(T.win); cv->setPenColor(T.text);
  for (int i = 0; i < s_newsCount; ++i) {            // little dots under the tag: which one
    int dx = x0 + i * 6 - 1;
    if (i == s_tickIdx) cv->fillRectangle(dx, k.Y2 - 1, dx + 3, k.Y2); else cv->drawLine(dx, k.Y2, dx + 3, k.Y2);
  }
}
// every 100 ms; true when the line changed and needs redrawing
static bool newsTickerStep(Canvas * cv)
{
  if (!s_newsCount || s_tickRect.X2 <= s_tickRect.X1) return false;
  if (s_tickIdx >= s_newsCount) { s_tickIdx = 0; s_tickPos = s_tickPx = 0; s_tickPhase = 0; s_tickAt = millis(); return true; }
  const fabgl::FontInfo * f = &FONT_std_12;
  const char * t = s_news[s_tickIdx];
  int avail = s_tickRect.X2 - 8 - tickTextX(cv);
  int rest = cv->textExtent(f, t + s_tickPos) - s_tickPx;           // what's still to come, from the left edge
  uint32_t el = millis() - s_tickAt;
  auto next = [&]() { s_tickIdx = (s_tickIdx + 1) % s_newsCount; s_tickPos = s_tickPx = 0; s_tickPhase = 0; s_tickAt = millis(); };
  if (s_tickPhase == 0) {
    if (rest <= avail) { if (el >= 5000) { next(); return true; } return false; }   // fits: 5 s, then the next
    if (el >= 1500) { s_tickPhase = 1; }
    return false;
  }
  if (s_tickPhase == 1) {
    s_tickPx += 2;
    for (;;) { char c[2] = { t[s_tickPos], 0 }; int w = cv->textExtent(f, c); if (!t[s_tickPos] || s_tickPx < w) break; s_tickPx -= w; ++s_tickPos; }
    if (cv->textExtent(f, t + s_tickPos) - s_tickPx <= avail) { s_tickPhase = 2; s_tickAt = millis(); }
    return true;
  }
  if (el >= 1500) { next(); return true; }
  return false;
}

static void drawDeskPanel(Canvas * cv)
{
  Rect r = deskPanelRect();
  cv->setBrushColor(T.win);
  cv->fillRectangle(r.X1, r.Y1, r.X2, r.Y2);
  cv->setPenColor(T.text);
  cv->drawRectangle(r.X1, r.Y1, r.X2, r.Y2);
  int x = r.X1 + 10, y = r.Y1 + 6;
  char b[96];
  time_t now = time(nullptr);
  cv->setPenColor(T.text); cv->setBrushColor(T.win);
  if (now < 1600000000) {
    cv->drawText(&FONT_std_16, x, y + 6, "Waiting for the clock...");
    return;
  }
  struct tm t; localtime_r(&now, &t);
  static const char * DAY[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
  static const char * MON[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
  snprintf(b, sizeof(b), "%d:%02d %s", t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  cv->drawText(&FONT_std_24, x, y, b);
  const int clockW = cv->textExtent(&FONT_std_24, b) + 2;    // (the music buttons sit evenly after it)
  y += 30;
  snprintf(b, sizeof(b), "%s, %s %d, %d", DAY[t.tm_wday % 7], MON[t.tm_mon % 12], t.tm_mday, t.tm_year + 1900);
  cv->drawText(&FONT_std_14, x, y, b);
#ifdef TIDYOS_HTML5
  {                                                          // MP3, volume - / +, next: evenly between the time and the drop box
    int left = x + clockW, right = r.X2 - 106;
    const int D = 17, mw = 29, by = r.Y1 + 10;               // round buttons (MP3 a pill), 17 px: an odd size draws a round circle
    int g = imax(2, (right - left - (mw + 3 * D)) / 5);
    int bx = left + g + imax(0, (right - left - (mw + 3 * D) - 5 * g) / 2);
    s_mp3Btn   = Rect(bx, by, bx + mw - 1, by + D - 1);
    s_volDnBtn = Rect(s_mp3Btn.X2 + 1 + g, by, s_mp3Btn.X2 + g + D, by + D - 1);
    s_volUpBtn = Rect(s_volDnBtn.X2 + 1 + g, by, s_volDnBtn.X2 + g + D, by + D - 1);
    s_nextBtn  = Rect(s_volUpBtn.X2 + 1 + g, by, s_volUpBtn.X2 + g + D, by + D - 1);
    bool on = musicQuickPlaying();
    auto btn = [&](const Rect & q, const char * t, bool hot) {
      int w = q.width(), h = q.height(), cy = q.Y1 + h / 2;
      RGB888 fill = hot ? T.accent : T.face;
      cv->setPenColor(T.shadow); cv->setBrushColor(fill);
      cv->fillEllipse(q.X1 + h / 2, cy, h, h); cv->drawEllipse(q.X1 + h / 2, cy, h, h);
      if (w > h) {                                           // a pill: two half circles joined by a box
        cv->fillEllipse(q.X2 - h / 2, cy, h, h); cv->drawEllipse(q.X2 - h / 2, cy, h, h);
        cv->fillRectangle(q.X1 + h / 2, q.Y1, q.X2 - h / 2, q.Y2);
        cv->drawLine(q.X1 + h / 2, q.Y1, q.X2 - h / 2, q.Y1);
        cv->drawLine(q.X1 + h / 2, q.Y2, q.X2 - h / 2, q.Y2);
      }
      cv->setPenColor(hot ? T.accentText : T.text);
      cv->drawText(&FONT_std_12, (q.X1 + q.X2 + 1) / 2 - cv->textExtent(&FONT_std_12, t) / 2, cy - 6, t);
    };
    if (s_mp3Btn.X1 > left) {                                // (room for them beside the time)
      btn(s_mp3Btn, "MP3", on);
      btn(s_volDnBtn, "-", false);
      btn(s_volUpBtn, "+", false);
      btn(s_nextBtn, "N", false);
    } else s_mp3Btn = s_volDnBtn = s_volUpBtn = s_nextBtn = Rect(0, 0, -1, -1);
    cv->setBrushColor(T.win);
  }
  {                                                          // the drop box, right of the time
    int bx1 = r.X2 - 106, by1 = r.Y1 + 6, bx2 = r.X2 - 8, by2 = r.Y1 + 34;
    cv->setPenColor(T.text);
    for (int k = bx1; k < bx2; k += 6) { cv->drawLine(k, by1, imin(k + 3, bx2), by1); cv->drawLine(k, by2, imin(k + 3, bx2), by2); }
    for (int k = by1; k < by2; k += 6) { cv->drawLine(bx1, k, bx1, imin(k + 3, by2)); cv->drawLine(bx2, k, bx2, imin(k + 3, by2)); }
    if (panNoteOn()) {                                       // what a music button just did
      char l2[100]; strlcpy(l2, s_panNote2, sizeof(l2));
      while (strlen(l2) > 3 && cv->textExtent(&FONT_std_12, l2) > bx2 - bx1 - 10) { l2[strlen(l2) - 4] = 0; strlcat(l2, "...", sizeof(l2)); }
      cv->setPenColor(T.accent);
      cv->drawText(&FONT_std_12, bx1 + 6, by1 + 1, s_panNote1);
      cv->setPenColor(T.text);
      cv->drawText(&FONT_std_12, bx1 + 6, by1 + 14, l2);
    } else {
      cv->drawText(&FONT_std_12, bx1 + 6, by1 + 1, "Drop files here");
      cv->drawText(&FONT_std_12, bx1 + 6, by1 + 14, gSDMounted ? "for (SD) card" : "(needs SD card)");
    }
  }
#endif
  y += 24;
  // weather
  cv->drawLine(x, y - 4, r.X2 - 10, y - 4);
  if (s_wx.code >= 0) {
    snprintf(b, sizeof(b), "%d\xB0" "F  %s", s_wx.temp, wxText(s_wx.code));
    const fabgl::FontInfo * tf = (s_tempsN > 1 && cv->textExtent(&FONT_std_18, b) > 116) ? &FONT_std_14 : &FONT_std_18;
    while (s_tempsN > 1 && strlen(b) > 6 && cv->textExtent(tf, b) > 118) b[strlen(b) - 1] = 0;
    cv->drawText(tf, x, y, b);
    y += 24;
    snprintf(b, sizeof(b), "High %d\xB0  Low %d\xB0   %s", s_wx.hi, s_wx.lo, s_wx.city);
    while (strlen(b) > 10 && cv->textExtent(&FONT_std_12, b) > (s_tempsN > 1 ? 116 : PANEL_W - 22)) b[strlen(b) - 1] = 0;
    cv->drawText(&FONT_std_12, x, y, b);
    if (s_tempsN > 1) drawTempGraph(cv, r.X2 - 112, y - 23, 102, 26);
    y += 18;
  } else if (s_wx.err[0]) {
    cv->drawText(&FONT_std_14, x, y, "Weather unavailable");
    cv->drawText(&FONT_std_12, x, y + 20, s_wx.err);
    y += 42;
  } else {
#ifdef TIDYOS_HTML5
    if (!netConnected() && !screenViewer()) {   // on the hotspot, no browser: nothing to look things up with
      cv->drawText(&FONT_std_14, x, y, "No internet here yet");
      cv->drawText(&FONT_std_12, x, y + 18, "Weather and headlines come through the web browser");
      cv->drawText(&FONT_std_12, x, y + 31, "showing this desktop, or join Wi-Fi in Settings.");
      y += 4;
    } else
    cv->drawText(&FONT_std_14, x, y, screenViewer() ? "Weather: looking it up..." : "Weather: shows with the browser open");
#else
    cv->drawText(&FONT_std_14, x, y, "Weather shows in the browser edition.");
#endif
    y += 42;
  }
  // world times
  cv->drawLine(x, y - 2, r.X2 - 10, y - 2);
  y += 4;
  static const char * Z[3][2] = { { "Moscow", "MSK-3" }, { "Los Angeles", "PST8PDT,M3.2.0,M11.1.0" }, { "Tokyo", "JST-9" } };
  for (int i = 0; i < 3; ++i) {
    struct tm w; zoneTime(Z[i][1], now, w);
    cv->drawText(&FONT_std_14, x, y, Z[i][0]);
    snprintf(b, sizeof(b), "%d:%02d %s  %.3s", w.tm_hour % 12 == 0 ? 12 : w.tm_hour % 12, w.tm_min, w.tm_hour < 12 ? "AM" : "PM", DAY[w.tm_wday % 7]);
    int tw = cv->textExtent(&FONT_std_14, b);
    int tempW = cv->textExtent(&FONT_std_14, "-00\xB0");               // the temperature column on the right
    cv->drawText(&FONT_std_14, r.X2 - 12 - tempW - 12 - tw, y, b);
    if (s_cityHave) {
      char tb[12]; snprintf(tb, sizeof(tb), "%d\xB0", s_cityT[i]);
      cv->drawText(&FONT_std_14, r.X2 - 12 - cv->textExtent(&FONT_std_14, tb), y, tb);
    }
    y += 19;
  }
  // affirmation of the day, word-wrapped
  y += 4;
  cv->drawLine(x, y - 4, r.X2 - 10, y - 4);
  if (s_affDay != t.tm_yday) { s_affDay = t.tm_yday; s_affShown = 1; }
  s_affBtn = Rect(r.X2 - 40, y - 1, r.X2 - 10, y + 15);                     // the >> button
  cv->setBrushColor(T.face);
  cv->fillRectangle(s_affBtn.X1, s_affBtn.Y1, s_affBtn.X2, s_affBtn.Y2);
  cv->setPenColor(T.text);
  cv->drawRectangle(s_affBtn.X1, s_affBtn.Y1, s_affBtn.X2, s_affBtn.Y2);
  cv->setBrushColor(T.face);
  cv->drawText(&FONT_std_12, s_affBtn.X1 + 8, s_affBtn.Y1 + 1, ">>");
  cv->setBrushColor(T.win);
  int base = (t.tm_yday + t.tm_year) % NAFFIRM;
  for (int k = 0; k < s_affShown; ++k) {
    std::string a = AFFIRM[(base + k) % NAFFIRM];
    const fabgl::FontInfo * af = s_affShown == 1 ? &FONT_std_14 : &FONT_std_12;
    int lh = s_affShown == 1 ? 18 : 14;
    int maxW = PANEL_W - 22 - (k == 0 ? 36 : 0), lines = 0, maxLines = s_affShown == 1 ? 3 : 2;
    while (!a.empty() && lines < maxLines) {
      size_t cut = a.size();
      while (cv->textExtent(af, a.substr(0, cut).c_str()) > maxW) { size_t sp = a.rfind(' ', cut - 1); if (sp == std::string::npos || sp == 0) break; cut = sp; }
      cv->drawText(af, x, y, a.substr(0, cut).c_str());
      a = cut < a.size() ? a.substr(cut + 1) : "";
      y += lh; ++lines; maxW = PANEL_W - 22;
    }
    y += 3;
  }
  // news: one line, the headlines taking turns (the desktop's timer slides it along and redraws just that strip)
  if (s_newsCount) {
    cv->drawLine(x, y - 2, r.X2 - 10, y - 2);
    s_tickRect = Rect(r.X1 + 1, y + 1, r.X2 - 1, y + 17);
    drawNewsTicker(cv);
  } else s_tickRect = Rect(0, 0, -1, -1);
  // the map sits at the bottom
  drawWorldMap(cv, r.X1 + (PANEL_W - 288) / 2, r.Y2 - 124, now);
}
