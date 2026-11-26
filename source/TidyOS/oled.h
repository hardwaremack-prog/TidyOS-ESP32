// ====================================================================== little OLED (HTML5 edition)
// A 128x64 SSD1306 on GPIO 21 (SDA) / 22 (SCL), or plugged straight onto 27 · 14 · 12 · GND (as on
// The Shelf for ESP32), is found at start-up and then shows a slow carousel of pages, drawn the way The
// Shelf draws its screen (its fonts and layouts, with U8g2): status, watch face, the song playing, the
// sunlit earth, news, today's affirmation, big clock, weather, the last 4 days of temperatures, world
// times, the color of the day, CPU temperature, new messages and a stopwatch.
// The two-colour modules' yellow band is the bottom 16 rows here.
// One small task (core 0) draws the pages and sends them over I2C, ~20 frames a second while something
// moves; the desktop only hands it the data once a second, so nothing waits on the screen.
// The BOOT button, as on TidyOS Pro: tap once = hold this page (tap again to let them turn), tap twice =
// next page; on the stopwatch page hold 1 s = start / stop, 3 s = reset. After 5 minutes without a press
// the screen goes off (not while music plays; a new message wakes it); a press wakes it. Switches -> OLED sleep.
// (included from core.cpp, after desk_panel.h)
#ifdef TIDYOS_HTML5
#include <Wire.h>
#include <U8g2lib.h>
#include <sys/time.h>
#include <esp_wifi.h>
#include "world_map_oled.h"
#include "oled_colors.h"

std::string musicNowPlaying();                // media_apps.h: the song playing ("" = none)

namespace oled {

// U8g2 draws into its own 1 KB page buffer and has no wiring of its own: the send task copies the
// buffer out and sends it (the SSD1306's memory layout, same as before).
class U8G2_MEM : public U8G2 {
public:
  U8G2_MEM() : U8G2() { u8g2_Setup_ssd1306_128x64_noname_f(&u8g2, U8G2_R0, u8x8_byte_empty, u8x8_dummy_cb); }
};
static U8G2_MEM g;

static bool     s_on = false;
static bool     s_inline = false;            // plugged straight onto 27 · 14 · 12 · GND, powered from GPIO 12
static uint8_t  s_addr = 0x3C;
static uint8_t  s_tx[1024];                   // the picture being sent
static volatile bool s_dirty = false;         // start-up: a new splash picture is in s_tx
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_wakeAt = 0;        // last BOOT press (or start-up): the screen sleeps 5 minutes after it
static volatile bool s_asleep = false, s_wakeReq = false, s_sleepReq = false;   // (only the send task talks to the screen)

// ---- what the desktop hands over once a second (the send task only reads these)
static char s_song[96] = "";
static volatile int s_unread = 0;
static volatile bool s_home = false, s_ap = false, s_view = false, s_sd = false;
static char s_ssid[33] = "", s_ip[16] = "", s_host[32] = "", s_apName[24] = "";
static volatile int s_rssi = 0, s_apJoined = 0;
static volatile uint32_t s_ramK = 0;
static volatile int64_t s_stTotal = 0, s_stUsed = 0;

// ---- the carousel
enum { P_STATUS, P_WATCH, P_NOW, P_EARTH, P_NEWS, P_AFFIRM, P_CLOCK, P_WEATHER, P_WXGRAPH, P_WORLD, P_COLOR, P_TEMP, P_MSG, P_SW, P_COUNT };
static volatile int s_page = -1;              // -1: still starting (the splash)
static uint32_t s_pageAt = 0, s_pageLen = 10000;
static volatile uint8_t s_evNext = 0, s_evHold = 0, s_evSw = 0;
static volatile bool s_held = false;
static uint32_t s_holdNoteAt = 0;
static bool s_swRun = false; static uint32_t s_swStart = 0, s_swAcc = 0;
static uint32_t swMs() { return s_swAcc + (s_swRun ? millis() - s_swStart : 0); }
static bool s_moving = false;                 // this frame moves (redraw ~20 times a second)

static void cmds(std::initializer_list<uint8_t> list)
{
  Wire.beginTransmission(s_addr);
  Wire.write(0x00);
  for (uint8_t c : list) Wire.write(c);
  Wire.endTransmission();
}
static void sendBuf()
{
  cmds({ 0x21, 0, 127, 0x22, 0, 7 });          // whole screen
  for (int i = 0; i < 1024; i += 32) {
    Wire.beginTransmission(s_addr);
    Wire.write(0x40);
    Wire.write(s_tx + i, 32);
    Wire.endTransmission();
  }
}

// ---- helpers (as on The Shelf)
static const char * const WDAY[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char * const MON[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static bool clockOK() { return time(nullptr) > 1700000000; }
static void localNow(struct tm * t) { time_t n = time(nullptr); localtime_r(&n, t); }
static int imx(int a, int b) { return a > b ? a : b; }

static const int SCROLL_PXS = 30;             // pixels a second
// one pass: wait 1.5 s, slide until the end shows, wait 1.5 s
static uint32_t passMs(int tw) { return tw <= 128 ? 0 : 1500 + (uint32_t)(tw - 128) * 1000 / SCROLL_PXS + 1500; }
static void drawPass(int y, const char * t, uint32_t el)
{
  int tw = g.getStrWidth(t);
  if (tw <= 128) { g.drawStr((128 - tw) / 2, y, t); return; }
  s_moving = true;
  int off = el < 1500 ? 0 : (int)std::min<uint32_t>((el - 1500) * SCROLL_PXS / 1000, (uint32_t)(tw - 128));
  g.drawStr(-off, y, t);
}
static void drawLoop(int y, const char * t, uint32_t el)     // round and round (the song name)
{
  int tw = g.getStrWidth(t);
  if (tw <= 128) { g.drawStr((128 - tw) / 2, y, t); return; }
  s_moving = true;
  int span = tw + 40, off = el < 1500 ? 0 : (int)(((el - 1500) * SCROLL_PXS / 1000) % span);
  g.drawStr(-off, y, t);
  g.drawStr(span - off, y, t);
}
static void header(const char * t)
{
  g.setFont(u8g2_font_5x7_tr);
  g.drawStr((128 - g.getStrWidth(t)) / 2, 7, t);
  g.drawHLine(0, 10, 128);
}
static void centre(int y, const char * t) { g.drawStr((128 - g.getStrWidth(t)) / 2, y, t); }
static void fitStr(int x, int y, const char * t, int maxW = 128)
{
  char b[100]; strlcpy(b, t, sizeof(b));
  size_t n = strlen(b);
  while (n > 1 && g.getStrWidth(b) > maxW - x) b[--n] = 0;
  g.drawStr(x, y, b);
}
// a degree sign after text that ends at x (font height h)
static int degAt(int x, int yTop, int r) { g.drawCircle(x + r + 1, yTop + r, r); return x + 2 * r + 3; }
// plain letters for the screen's fonts: curly quotes and dashes made plain, the rest dropped
static void plain(char * out, size_t max, const char * in)
{
  size_t o = 0;
  for (const unsigned char * p = (const unsigned char *)in; *p && o < max - 1; ++p) {
    if (*p < 0x80) out[o++] = *p;
    else if (p[0] == 0xE2 && p[1] == 0x80 && p[2]) { out[o++] = (p[2] == 0x98 || p[2] == 0x99) ? '\'' : (p[2] == 0x9C || p[2] == 0x9D) ? '"' : '-'; p += 2; }
    else if (*p == 0xC2 && p[1] == 0xA0) { out[o++] = ' '; ++p; }
    else if ((*p & 0xC0) == 0xC0) { out[o++] = ' '; while ((p[1] & 0xC0) == 0x80) ++p; }
  }
  out[o] = 0;
}

// ---- pages
static void drawStatusPage()
{
  char b[64];
  g.setFont(u8g2_font_helvB08_tr); g.drawStr(0, 9, "TIDYOS");
  int x = g.getStrWidth("TIDYOS") + 4;
  g.setFont(u8g2_font_5x7_tr); g.drawStr(x, 9, TIDYOS_VERSION);
  const char * st = s_sd ? "SD" : "FLASH";
  g.drawStr(128 - g.getStrWidth(st), 9, st);
  g.drawHLine(0, 12, 128);
  g.setFont(u8g2_font_6x10_tr);
  const char * bottom = "";
  char bot[40];
  if (s_home) {
    snprintf(b, sizeof(b), "%s.local", s_host);
    if (g.getStrWidth(b) > 128) g.setFont(u8g2_font_5x7_tr);
    g.drawStr(0, 24, b);
    g.setFont(u8g2_font_6x10_tr);
    if (s_ap) snprintf(b, sizeof(b), "Hotspot too: %d joined", s_apJoined);
    else snprintf(b, sizeof(b), "Wi-Fi %d dBm", s_rssi);
    fitStr(0, 35, b);
    snprintf(bot, sizeof(bot), "IP %s", s_ip); bottom = bot;
  } else if (s_ap) {
    snprintf(b, sizeof(b), "Join %s", s_apName);
    if (g.getStrWidth(b) > 128) g.setFont(u8g2_font_5x7_tr);
    g.drawStr(0, 24, b);
    g.setFont(u8g2_font_6x10_tr);
    g.drawStr(0, 35, "Password: tidyos123");
    bottom = "Open 10.1.1.250";
  } else {
    g.drawStr(0, 24, "Joining Wi-Fi...");
    fitStr(0, 35, s_ssid);
    bottom = "TidyOS";
  }
  if (s_stTotal > 0) {                          // storage: a bar and what's free
    int w = (int)((s_stUsed * 62) / s_stTotal);
    g.drawFrame(0, 39, 64, 7); g.drawBox(1, 40, w > 62 ? 62 : w, 5);
    g.setFont(u8g2_font_5x7_tr);
    int64_t fr = s_stTotal - s_stUsed;
    if (fr >= 1073741824LL) snprintf(b, sizeof(b), "%.1fG free", fr / 1073741824.0);
    else if (fr >= 1048576) snprintf(b, sizeof(b), "%dM free", (int)(fr / 1048576));
    else snprintf(b, sizeof(b), "%dK free", (int)(fr / 1024));
    g.drawStr(128 - g.getStrWidth(b), 46, b);
  }
  g.setFont(u8g2_font_4x6_tr);                  // what BOOT does, and the address, in the bottom (yellow) strip
  snprintf(b, sizeof(b), "BOOT 1x hold  2x next   RAM %uK", (unsigned)s_ramK);
  centre(54, b);
  g.setFont(u8g2_font_6x10_tr);
  centre(63, bottom);
}

static void hand(int cx, int cy, float a, int len, float w)
{
  float s = sinf(a), c = cosf(a);
  int tx = cx + (int)lroundf(s * len), ty = cy - (int)lroundf(c * len);
  int px = (int)lroundf(c * w), py = (int)lroundf(s * w);
  g.drawTriangle(cx + px, cy + py, cx - px, cy - py, tx, ty);
  g.drawLine(cx, cy, tx, ty);
}
static void drawWatchPage()
{
  struct timeval tv; gettimeofday(&tv, nullptr);     // the second and its fraction from the same clock reading
  time_t lt = tv.tv_sec;
  struct tm t; localtime_r(&lt, &t);
  const int cx = 31, cy = 32;
  g.drawCircle(cx, cy, 31); g.drawCircle(cx, cy, 30);
  for (int i = 0; i < 60; i++) {
    float a = i * (float)M_PI / 30, s = sinf(a), c = cosf(a);
    if (i % 5 == 0) {
      int r0 = i % 15 == 0 ? 22 : 24;
      for (int k = -1; k <= (i % 15 == 0 ? 1 : 0); k++) {   // quarter marks a bit bolder
        float ka = a + k * 0.025f;
        g.drawLine(cx + (int)lroundf(sinf(ka) * r0), cy - (int)lroundf(cosf(ka) * r0),
                   cx + (int)lroundf(sinf(ka) * 28), cy - (int)lroundf(cosf(ka) * 28));
      }
    } else g.drawPixel(cx + (int)lroundf(s * 27), cy - (int)lroundf(c * 27));
  }
  float sec = t.tm_sec + tv.tv_usec / 1e6f;
  float mn = t.tm_min + t.tm_sec / 60.0f, hr = (t.tm_hour % 12) + mn / 60.0f;
  hand(cx, cy, hr * (float)M_PI / 6, 15, 2.5f);
  hand(cx, cy, mn * (float)M_PI / 30, 23, 2.0f);
  float sa = sec * (float)M_PI / 30;                  // a sweeping second hand with a short tail
  g.drawLine(cx - (int)lroundf(sinf(sa) * 6), cy + (int)lroundf(cosf(sa) * 6),
             cx + (int)lroundf(sinf(sa) * 26), cy - (int)lroundf(cosf(sa) * 26));
  g.drawDisc(cx, cy, 2); g.setDrawColor(0); g.drawPixel(cx, cy); g.setDrawColor(1);
  s_moving = true;
  char b[24], wd[4];                                  // beside it: the date
  snprintf(wd, sizeof(wd), "%.3s", WDAY[t.tm_wday % 7]);
  for (char * q = wd; *q; ++q) *q = toupper(*q);
  g.setFont(u8g2_font_helvB08_tr);
  g.drawStr(96 - g.getStrWidth(wd) / 2, 12, wd);
  g.setFont(u8g2_font_helvB18_tr);
  snprintf(b, sizeof(b), "%d", t.tm_mday);
  g.drawStr(96 - g.getStrWidth(b) / 2, 35, b);
  g.setFont(u8g2_font_5x7_tr);
  snprintf(b, sizeof(b), "%s %d", MON[t.tm_mon % 12], t.tm_year + 1900);
  g.drawStr(96 - g.getStrWidth(b) / 2, 45, b);
  g.setFont(u8g2_font_6x10_tr);
  snprintf(b, sizeof(b), "%d:%02d %s", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  g.drawStr(96 - g.getStrWidth(b) / 2, 60, b);
}

static void drawNowPage(uint32_t el)
{
  g.setFont(u8g2_font_5x7_tr);
  g.drawDisc(3, 7, 2); g.drawVLine(5, 0, 8); g.drawLine(5, 0, 9, 2);    // a music note
  g.drawStr(13, 7, "NOW PLAYING");
  g.drawHLine(0, 10, 128);
  char n[96]; plain(n, sizeof(n), s_song);
  char * dot = strrchr(n, '.'); if (dot && strlen(dot) <= 5) *dot = 0;
  for (char * q = n; *q; ++q) if (*q == '_') *q = ' ';
  g.setFont(u8g2_font_helvB12_tr);
  drawLoop(34, n, el);
  s_moving = true;                                    // bouncing bars along the bottom
  uint32_t f = millis() / 110;
  for (int i = 0; i < 16; i++) {
    uint32_t h = (f * 2654435761u + i * 40503u) ^ (f >> 3) * 977u;
    int bh = 2 + (int)((h >> 7) % 15);
    g.drawBox(i * 8 + 1, 64 - bh, 6, bh);
  }
}

static void drawClockPage()
{
  struct tm t; localNow(&t);
  char b[24];
  int h = t.tm_hour % 12 ? t.tm_hour % 12 : 12;
  bool colon = millis() % 1000 < 600;
  snprintf(b, sizeof(b), colon ? "%d:%02d" : "%d %02d", h, t.tm_min);
  g.setFont(u8g2_font_logisoso32_tn);
  int tw = g.getStrWidth(b);
  g.setFont(u8g2_font_helvB08_tr);
  const char * ap = t.tm_hour < 12 ? "AM" : "PM";
  int aw = g.getStrWidth(ap), x = (128 - tw - aw - 3) / 2;
  g.setFont(u8g2_font_logisoso32_tn);
  g.drawStr(x, 38, b);
  g.setFont(u8g2_font_helvB08_tr);
  g.drawStr(x + tw + 3, 38, ap);
  snprintf(b, sizeof(b), ":%02d", t.tm_sec);
  g.drawStr(x + tw + 3, 22, b);
  snprintf(b, sizeof(b), "%s, %s %d", WDAY[t.tm_wday % 7], MON[t.tm_mon % 12], t.tm_mday);
  centre(58, b);
  s_moving = true;                                   // keep the seconds ticking
}

static uint32_t dayNumber() { return (uint32_t)(time(nullptr) / 86400); }
static const char * todaysAffirmation()
{
  struct tm t; localNow(&t);
  return AFFIRM[(t.tm_yday + t.tm_year) % NAFFIRM];       // the same one as on the desktop
}
static int todaysColor() { return (int)(((uint64_t)dayNumber() * 7) % (sizeof DAILY_COLOR_NAMES / sizeof DAILY_COLOR_NAMES[0])); }

static void drawAffirmPage(uint32_t el)
{
  header("TODAY'S AFFIRMATION");
  for (int sx : { 6, 121 }) { g.drawHLine(sx - 2, 4, 5); g.drawVLine(sx, 2, 5); }   // little sparkles
  char a[160]; plain(a, sizeof(a), todaysAffirmation());
  g.setFont(u8g2_font_helvB12_tr);
  drawPass(38, a, el);
  struct tm t; localNow(&t);
  char b[24]; snprintf(b, sizeof(b), "%s, %s %d", WDAY[t.tm_wday % 7], MON[t.tm_mon % 12], t.tm_mday);
  g.setFont(u8g2_font_5x7_tr);
  centre(62, b);
}

static void drawColorPage(uint32_t el)
{
  header("COLOR OF THE DAY");
  int c = todaysColor();
  g.setFont(u8g2_font_helvB14_tr);
  if (g.getStrWidth(DAILY_COLOR_NAMES[c]) > 112) g.setFont(u8g2_font_helvB10_tr);
  int tw = g.getStrWidth(DAILY_COLOR_NAMES[c]), x = (128 - tw - 12) / 2;
  g.drawDisc(x + 4, 29, 4);                         // a paint dot beside the name
  g.drawStr(x + 12, 35, DAILY_COLOR_NAMES[c]);
  g.setFont(u8g2_font_6x10_tr);
  drawPass(61, DAILY_COLOR_MEANINGS[c], el);         // in the bottom (yellow) strip
}

static void drawTempPage()
{
  header("CPU TEMPERATURE");
  float c = temperatureRead();
  g.drawRFrame(8, 13, 9, 26, 4); g.drawCircle(12, 41, 6); g.drawDisc(12, 41, 4);   // a thermometer
  g.setDrawColor(0); g.drawBox(9, 35, 7, 4); g.setDrawColor(1);
  int lvl = (int)((c - 20) * 20 / 70); lvl = lvl < 2 ? 2 : lvl > 20 ? 20 : lvl;
  g.drawBox(10, 37 - lvl, 5, lvl + 3);
  char b[40];
  snprintf(b, sizeof(b), "%.0f", c);
  g.setFont(u8g2_font_helvB18_tr);
  g.drawStr(30, 36, b);
  int x = degAt(30 + g.getStrWidth(b), 17, 2);
  g.drawStr(x, 36, "C");
  snprintf(b, sizeof(b), "%.0f", c * 9 / 5 + 32);
  g.setFont(u8g2_font_helvB10_tr);
  int w = g.getStrWidth(b) + 7 + g.getStrWidth("F");
  g.drawStr(128 - w, 36, b);
  degAt(128 - w + g.getStrWidth(b), 26, 1);
  g.drawStr(128 - g.getStrWidth("F"), 36, "F");
  uint32_t up = millis() / 60000;
  if (up >= 1440) snprintf(b, sizeof(b), "Up %ud %uh", (unsigned)(up / 1440), (unsigned)(up % 1440 / 60));
  else snprintf(b, sizeof(b), "Up %uh %um", (unsigned)(up / 60), (unsigned)(up % 60));
  g.setFont(u8g2_font_5x7_tr);
  centre(55, b);
  snprintf(b, sizeof(b), "%u KB free - %u MHz", (unsigned)s_ramK, (unsigned)getCpuFrequencyMhz());
  centre(63, b);
}

// the sunlit earth: day and night on the world map as they are right now; the sun over the spot where it's
// straight overhead; where the board is (its weather location) blinks
static void sunPosition(time_t t, float & decl, float & subLon)
{
  struct tm gm; gmtime_r(&t, &gm);
  float hours = gm.tm_hour + gm.tm_min / 60.0f + gm.tm_sec / 3600.0f;
  float y = 2 * (float)M_PI / 365 * (gm.tm_yday + (hours - 12) / 24);      // NOAA's approximation
  decl = 0.006918f - 0.399912f * cosf(y) + 0.070257f * sinf(y) - 0.006758f * cosf(2 * y) + 0.000907f * sinf(2 * y)
       - 0.002697f * cosf(3 * y) + 0.00148f * sinf(3 * y);
  float eqt = 229.18f * (0.000075f + 0.001868f * cosf(y) - 0.032077f * sinf(y) - 0.014615f * cosf(2 * y) - 0.040849f * sinf(2 * y));
  subLon = -15.0f * (hours - 12 + eqt / 60);
  while (subLon < -180) subLon += 360;
  while (subLon > 180) subLon -= 360;
}
static void drawEarthPage()
{
  float decl, subLon; sunPosition(time(nullptr), decl, subLon);
  float sd = sinf(decl), cd = cosf(decl);
  static float cosCol[128];
  for (int x = 0; x < 128; x++) cosCol[x] = cosf(((-180 + (x + 0.5f) * 360 / 128) - subLon) * (float)M_PI / 180);
  for (int y = 0; y < 64; y++) {
    float lat = (WORLD_TOP - (y + 0.5f) * (WORLD_TOP - WORLD_BOTTOM) / 64) * (float)M_PI / 180, a = sinf(lat) * sd, b = cosf(lat) * cd;
    for (int x = 0; x < 128; x++) {
      float z = a + b * cosCol[x];
      bool land = (WORLD_MAP[y][x >> 3] >> (7 - (x & 7))) & 1;
      bool on;
      if (land) on = z > 0.05f ? true : z > -0.10f ? ((x + y) & 1) : (!(x & 1) && !(y & 1));   // day, twilight, night
      else on = fabsf(z) < 0.025f && ((x + y) % 3 == 0);                                  // the day/night line over the sea
      if (on) g.drawPixel(x, y);
    }
  }
  int sx = (int)lroundf((subLon + 180) * 128 / 360), sy = (int)lroundf((WORLD_TOP - decl * 180 / (float)M_PI) * 64 / (WORLD_TOP - WORLD_BOTTOM));
  g.setDrawColor(0); g.drawDisc(sx, sy, 6); g.setDrawColor(1);
  g.drawDisc(sx, sy, 2);
  for (int k = 0; k < 8; k++) {
    float a = k * (float)M_PI / 4, c = cosf(a), d = sinf(a);
    g.drawLine(sx + (int)lroundf(c * 4), sy + (int)lroundf(d * 4), sx + (int)lroundf(c * 6), sy + (int)lroundf(d * 6));
  }
  if (s_wx.haveLoc) {                                   // here: flashes between a ring and a spot
    int hx = (int)lroundf((s_wx.lon + 180) * 128 / 360), hy = (int)lroundf((WORLD_TOP - s_wx.lat) * 64 / (WORLD_TOP - WORLD_BOTTOM));
    g.setDrawColor(0); g.drawDisc(hx, hy, 4); g.setDrawColor(1);
    if ((millis() / 400) % 2) g.drawCircle(hx, hy, 3); else g.drawDisc(hx, hy, 2);
    g.drawPixel(hx, hy);
  }
  struct tm t; localNow(&t);
  char b[16]; snprintf(b, sizeof(b), "%d:%02d %s", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  g.setFont(u8g2_font_5x7_tr);
  int w = g.getStrWidth(b);
  g.setDrawColor(0); g.drawBox(0, 55, w + 3, 9); g.setDrawColor(1);
  g.drawStr(1, 63, b);
  s_moving = true;
}

static void drawWeatherPage()
{
  char b[64];
  header(s_wx.city[0] ? "WEATHER" : "WEATHER");
  snprintf(b, sizeof(b), "%d", s_wx.temp);
  g.setFont(u8g2_font_helvB18_tr);
  g.drawStr(0, 38, b);
  int x = degAt(g.getStrWidth(b), 19, 2);
  g.drawStr(x, 38, "F");
  x += g.getStrWidth("F") + 6;
  g.setFont(u8g2_font_6x10_tr);
  char c[40]; plain(c, sizeof(c), wxText(s_wx.code));
  // the conditions, over two lines if needed
  const char * sp = nullptr;
  if (g.getStrWidth(c) > 128 - x) { for (const char * q = c; *q; ++q) if (*q == ' ' && (q - c) * 6 <= 128 - x) sp = q; }
  if (sp) { std::string a(c, sp - c); g.drawStr(x, 24, a.c_str()); fitStr(x, 35, sp + 1); }
  else fitStr(x, 30, c);
  g.setFont(u8g2_font_5x7_tr);
  snprintf(b, sizeof(b), "High %d  Low %d", s_wx.hi, s_wx.lo);
  g.drawStr(x, 46, b);
  g.setFont(u8g2_font_6x10_tr);
  char city[40]; plain(city, sizeof(city), s_wx.city[0] ? s_wx.city : "Weather");
  centre(62, city);
}

// the last 4 days of outside temperatures (every 3 hours), with the freezing line, as on The Shelf
static void drawWxGraphPage()
{
  int n = s_tempsN;
  float lo = s_temps[0], hi = s_temps[0];
  for (int i = 1; i < n; i++) { lo = fminf(lo, s_temps[i]); hi = fmaxf(hi, s_temps[i]); }
  float dlo = lo, dhi = hi;
  if (lo < 45 && hi > 20) { dlo = fminf(dlo, 32.0f); dhi = fmaxf(dhi, 32.0f); }   // keep freezing on the graph when it's near
  if (dhi - dlo < 10) { float m = (dhi + dlo) / 2; dlo = m - 5; dhi = m + 5; }
  const int X0 = 16, X1 = 127, Y0 = 12, Y1 = 46;
  auto Y = [&](float f) { return Y1 - (int)lroundf((f - dlo) * (Y1 - Y0) / (dhi - dlo)); };
  auto X = [&](int i) { return X0 + (int)lroundf((float)i * (X1 - X0) / (n - 1)); };
  char b[48];
  g.setFont(u8g2_font_5x7_tr);
  g.drawStr(0, 7, "OUTSIDE - 4 DAYS");
  snprintf(b, sizeof(b), "%d", s_wx.code >= 0 ? s_wx.temp : (int)s_temps[n - 1]);
  int w = g.getStrWidth(b) + 4 + g.getStrWidth("F now");
  g.drawStr(128 - w, 7, b);
  degAt(128 - w + g.getStrWidth(b) - 1, 1, 1);
  g.drawStr(128 - g.getStrWidth("F now"), 7, "F now");
  g.setFont(u8g2_font_4x6_tr);
  snprintf(b, sizeof(b), "%.0f", dhi); g.drawStr(0, Y0 + 5, b);
  snprintf(b, sizeof(b), "%.0f", dlo); g.drawStr(0, Y1, b);
  if (32 >= dlo && 32 <= dhi) {                        // the freezing line, dashed
    int fy = Y(32);
    for (int x = X0; x <= X1; x += 4) g.drawHLine(x, fy, 2);
    if (fy - 3 > Y0 + 6 && fy + 3 < Y1 - 5) g.drawStr(0, fy + 3, "32");
  }
  // midnights: a dotted line each day, and the day's name underneath (points are 3 hours apart, the last one now)
  time_t now = time(nullptr), start = now - (time_t)(n - 1) * 10800;
  struct tm d; localtime_r(&start, &d);
  time_t mid = start - (d.tm_hour * 3600 + d.tm_min * 60 + d.tm_sec) + 86400;   // the first local midnight after the start
  int prevX = X(0);
  g.setFont(u8g2_font_5x7_tr);
  for (;; mid += 86400) {
    float fi = (float)(mid - start) / 10800;
    int mx = fi < n - 1 ? X0 + (int)lroundf(fi * (X1 - X0) / (n - 1)) : X(n - 1);
    time_t dayT = mid - 43200; localtime_r(&dayT, &d);     // the day that ends at this midnight
    char nm[4]; snprintf(nm, sizeof(nm), "%.3s", WDAY[d.tm_wday % 7]);
    if (mx - prevX > 16) g.drawStr((prevX + mx) / 2 - g.getStrWidth(nm) / 2, 56, nm);
    if (fi >= n - 1) break;
    for (int y = Y0; y <= Y1; y += 3) g.drawPixel(mx, y);
    prevX = mx;
  }
  for (int i = 1; i < n; i++) g.drawLine(X(i - 1), Y(s_temps[i - 1]), X(i), Y(s_temps[i]));
  g.drawDisc(X(n - 1), Y(s_temps[n - 1]), 1);
  g.setFont(u8g2_font_4x6_tr);
  snprintf(b, sizeof(b), "lo %.0f  hi %.0f  - - freezing", lo, hi);
  centre(63, b);
}

// news: one headline at a time, each sliding along once when it's too long, then the next
static uint32_t headlineMs(const char * t) { g.setFont(u8g2_font_helvB12_tr); return imx(4500, passMs(g.getStrWidth(t))); }
static char s_nh[6][120]; static int s_nhN = 0;
static uint32_t newsDuration()
{
  s_nhN = s_newsCount;                                 // a snapshot for the whole page
  for (int i = 0; i < s_nhN; ++i) plain(s_nh[i], sizeof(s_nh[0]), s_news[i]);
  uint32_t t = 0; for (int i = 0; i < s_nhN; i++) t += headlineMs(s_nh[i]);
  return t ? t : 6000;
}
static void drawNewsPage(uint32_t el)
{
  if (!s_nhN) return;
  int i = 0; uint32_t start = 0;
  while (i < s_nhN - 1 && el - start >= headlineMs(s_nh[i])) { start += headlineMs(s_nh[i]); i++; }
  char b[32]; snprintf(b, sizeof(b), "NEWS - %d OF %d", i + 1, s_nhN);
  header(b);
  g.setFont(u8g2_font_helvB12_tr);
  drawPass(36, s_nh[i], el - start);
  s_moving = true;
  for (int k = 0; k < s_nhN; k++) { int x = 128 - (s_nhN - k) * 8 + 3; if (k == i) g.drawDisc(x, 58, 2); else g.drawCircle(x, 58, 2); }
  g.setFont(u8g2_font_5x7_tr);
  struct tm t; localNow(&t);
  snprintf(b, sizeof(b), "%d:%02d %s", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  g.drawStr(0, 62, b);
}

static void drawWorldPage()
{
  static const char * Z[3][2] = { { "Moscow", "MSK-3" }, { "Los Angeles", "PST8PDT,M3.2.0,M11.1.0" }, { "Tokyo", "JST-9" } };
  header("WORLD TIME");
  time_t now = time(nullptr);
  g.setFont(u8g2_font_6x10_tr);
  for (int i = 0; i < 3; ++i) {
    struct tm w; tzLocal(Z[i][1], now, w);
    char b[24];
    int y = 24 + i * 13;
    g.drawStr(0, y, Z[i][0]);
    snprintf(b, sizeof(b), "%d:%02d%s", w.tm_hour % 12 == 0 ? 12 : w.tm_hour % 12, w.tm_min, w.tm_hour < 12 ? "a" : "p");
    g.drawStr(74, y, b);
    if (s_cityHave) { snprintf(b, sizeof(b), "%d", s_cityT[i]); int x = 128 - 5 - g.getStrWidth(b); g.drawStr(x, y, b); degAt(x + g.getStrWidth(b) - 1, y - 8, 1); }
  }
  struct tm t; localNow(&t);
  char b[24]; snprintf(b, sizeof(b), "Here %d:%02d %s", t.tm_hour % 12 ? t.tm_hour % 12 : 12, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
  g.setFont(u8g2_font_6x10_tr);
  centre(63, b);
}

static void drawMsgPage()
{
  header("MESSAGES");
  char b[24];
  snprintf(b, sizeof(b), "%d", s_unread);
  g.setFont(u8g2_font_logisoso32_tn);
  int w = g.getStrWidth(b);
  g.setFont(u8g2_font_helvB12_tr);
  int w2 = g.getStrWidth(" new"), x = (128 - w - w2) / 2;
  g.setFont(u8g2_font_logisoso32_tn);
  g.drawStr(x, 46, b);
  g.setFont(u8g2_font_helvB12_tr);
  g.drawStr(x + w, 46, " new");
  g.setFont(u8g2_font_5x7_tr);
  centre(62, "Open Messages to read");
}

static void drawStopwatchPage()
{
  char b[24];
  uint32_t ms = swMs(), sec = ms / 1000, h = sec / 3600, m = (sec / 60) % 60;
  if (h) snprintf(b, sizeof(b), "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)(sec % 60));
  else snprintf(b, sizeof(b), "%02u:%02u.%u", (unsigned)m, (unsigned)(sec % 60), (unsigned)(ms / 100 % 10));
  header(s_swRun ? "STOPWATCH - RUNNING" : ms ? "STOPWATCH - STOPPED" : "STOPWATCH");
  g.setFont(u8g2_font_logisoso22_tn);
  centre(40, b);
  g.setFont(u8g2_font_5x7_tr);
  centre(62, s_swRun ? "Hold BOOT 1 s to stop" : ms ? "BOOT 1 s: go on   3 s: reset" : "Hold BOOT 1 s to start");
  if (s_swRun) s_moving = true;
}

// ---- the carousel
static bool pageReady(int p)
{
  switch (p) {
    case P_NOW: return s_song[0] != 0;
    case P_WATCH: case P_CLOCK: case P_AFFIRM: case P_COLOR: case P_EARTH: case P_WORLD: return clockOK();
    case P_WEATHER: return s_wx.code >= 0;
    case P_WXGRAPH: return s_tempsN > 1 && clockOK();
    case P_NEWS: return s_newsCount > 0;
    case P_MSG: return s_unread > 0;
    default: return true;
  }
}
static uint32_t pageLength(int p)
{
  switch (p) {
    case P_STATUS: return 10000;
    case P_WATCH: return 9000;
    case P_EARTH: return 9000;
    case P_WXGRAPH: return 9000;
    case P_NEWS: return newsDuration();
    case P_NOW: return 10000;
    case P_AFFIRM: { char a[160]; plain(a, sizeof(a), todaysAffirmation()); g.setFont(u8g2_font_helvB12_tr); return imx(7000, passMs(g.getStrWidth(a))); }
    case P_COLOR: g.setFont(u8g2_font_6x10_tr); return imx(7000, passMs(g.getStrWidth(DAILY_COLOR_MEANINGS[todaysColor()])));
    default: return 6000;
  }
}
static void goPage(int p) { s_page = p; s_pageAt = millis(); s_pageLen = pageLength(p); }
static void nextPage() { int p = s_page < 0 ? 0 : s_page; for (int k = 0; k < P_COUNT; ++k) { p = (p + 1) % P_COUNT; if (pageReady(p)) break; } goPage(p); }

static void drawFrame()
{
  uint32_t el = millis() - s_pageAt;
  g.clearBuffer();
  g.setDrawColor(1); g.setFontMode(0);
  s_moving = false;
  switch (s_page) {
    case P_STATUS: drawStatusPage(); break;
    case P_WATCH: drawWatchPage(); break;
    case P_NOW: drawNowPage(el); break;
    case P_EARTH: drawEarthPage(); break;
    case P_NEWS: drawNewsPage(el); break;
    case P_AFFIRM: drawAffirmPage(el); break;
    case P_CLOCK: drawClockPage(); break;
    case P_WEATHER: drawWeatherPage(); break;
    case P_WXGRAPH: drawWxGraphPage(); break;
    case P_WORLD: drawWorldPage(); break;
    case P_COLOR: drawColorPage(el); break;
    case P_TEMP: drawTempPage(); break;
    case P_MSG: drawMsgPage(); break;
    case P_SW: drawStopwatchPage(); break;
  }
  // "HOLD" in the corner while the pages are held, and a note for 1.5 s after a tap
  if (s_held || (s_holdNoteAt && millis() - s_holdNoteAt < 1500)) {
    g.setFont(u8g2_font_5x7_tr);
    const char * t = s_held ? "HOLD" : "TURNING";
    int w = g.getStrWidth(t) + 4;
    g.setDrawColor(1); g.drawBox(128 - w, 0, w, 9);
    g.setDrawColor(0); g.drawStr(128 - w + 2, 7, t); g.setDrawColor(1);
    if (!s_held) s_moving = true;
  }
  portENTER_CRITICAL(&s_mux);
  memcpy(s_tx, g.getBufferPtr(), 1024);
  portEXIT_CRITICAL(&s_mux);
}

static void sendTask(void *)
{
  bool wasDown = false, swallow = false;
  uint32_t downAt = 0, tapAt = 0, lastFrame = 0, lastSec = 0;
  for (;;) {
    // the BOOT button (only read here, never driven)
    bool down = digitalRead(0) == LOW;
    uint32_t t = millis();
    if (down && !wasDown) {
      downAt = t; s_wakeAt = t;
      if (s_asleep) { s_wakeReq = true; swallow = true; }   // a press on a sleeping screen only wakes it
    }
    if (!down && wasDown) {
      uint32_t held = t - downAt;
      if (swallow) swallow = false;
      else if (held >= 3000) { if (s_page == P_SW) s_evSw = 2; }       // stopwatch: reset
      else if (held >= 1000) { if (s_page == P_SW) s_evSw = 1; }       // stopwatch: start / stop
      else if (tapAt && t - tapAt < 450) { tapAt = 0; s_evNext = 1; }  // two taps: next page
      else tapAt = t;
    }
    if (tapAt && !down && t - tapAt >= 450) { tapAt = 0; s_evHold = 1; }   // one tap: hold / let go
    wasDown = down;
    if (s_sleepReq) { s_sleepReq = false; cmds({ 0xAE }); }
    if (s_wakeReq) { s_wakeReq = false; s_asleep = false; cmds({ 0xAF }); lastFrame = 0; }
    if (s_asleep) { vTaskDelay(pdMS_TO_TICKS(40)); continue; }

    if (s_page < 0) {                                // still starting: the splash pictures
      if (s_dirty) { s_dirty = false; sendBuf(); }
      vTaskDelay(pdMS_TO_TICKS(30));
      continue;
    }
    bool redraw = false;
    if (s_evNext) { s_evNext = 0; nextPage(); redraw = true; }
    if (s_evHold) { s_evHold = 0; s_held = !s_held; s_holdNoteAt = millis() | 1; if (!s_held) s_pageAt = millis(); redraw = true; }
    if (s_evSw) {
      if (s_evSw == 2) { s_swRun = false; s_swAcc = 0; }
      else if (s_swRun) { s_swAcc += millis() - s_swStart; s_swRun = false; }
      else { s_swStart = millis(); s_swRun = true; }
      s_evSw = 0; redraw = true;
    }
    if (!s_held && millis() - s_pageAt >= s_pageLen) { nextPage(); redraw = true; }
    if (!pageReady(s_page) && s_page != P_STATUS) { nextPage(); redraw = true; }
    uint32_t now = millis();
    if (redraw || (s_moving && now - lastFrame >= 50) || now - lastSec >= 1000) {
      lastFrame = now;
      if (now - lastSec >= 1000) lastSec = now;
      drawFrame();
      sendBuf();
    }
    vTaskDelay(pdMS_TO_TICKS(s_moving ? 10 : 40));
  }
}

}  // namespace oled

// found it? (called once at start-up)
void oledBegin()
{
  using namespace oled;
  auto probe = [](int sda, int scl) {
    Wire.begin(sda, scl, 400000);
    for (uint8_t a : { (uint8_t)0x3C, (uint8_t)0x3D }) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) { s_addr = a; s_on = true; return true; }
    }
    Wire.end();
    return false;
  };
  if (!probe(21, 22)) {
    // In line, as on The Shelf: the screen plugged straight onto 27 · 14 · 12 · GND (turned around),
    // powered from GPIO 12 (a screen draws ~10-20 mA, well within what a pin gives). It's turned the
    // same way as on 21/22, so the picture needs no change.
    gpio_set_drive_capability(GPIO_NUM_12, GPIO_DRIVE_CAP_3);
    pinMode(12, OUTPUT); digitalWrite(12, HIGH);
    delay(120);
    if (probe(27, 14)) { s_inline = true; Serial.println("[TidyOS] OLED screen found in line (SDA 27, SCL 14, power 12)"); }
    else { digitalWrite(12, LOW); pinMode(12, INPUT); }
  }
  if (!s_on) { Serial.println("[TidyOS] no OLED screen (looked on 21/22 and in line on 27/14)"); return; }
  Wire.setClock(700000);                       // a whole picture in ~15 ms, so moving pages stay smooth
  // SSD1306 128x64, turned around (A0 / C0), charge pump on
  cmds({ 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00, 0xA0, 0xC0,
         0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0x2E, 0xAF });
  g.clearBuffer();
  g.setFont(u8g2_font_helvB12_tr); centre(24, "TidyOS");
  g.setFont(u8g2_font_5x7_tr); centre(40, "starting...");
  memcpy(s_tx, g.getBufferPtr(), 1024);
  sendBuf();
  pinMode(0, INPUT_PULLUP);
  s_wakeAt = millis();
  xTaskCreatePinnedToCore(sendTask, "oled", 4096, nullptr, 1, nullptr, 0);
  Serial.printf("[TidyOS] OLED screen found at 0x%02X\n", s_addr);
}

// before deep sleep: power an in-line screen down (GPIO 12 must be low when the board starts again)
void oledOff()
{
  using namespace oled;
  if (!s_on) return;
  cmds({ 0xAE });
  if (s_inline) { digitalWrite(12, LOW); pinMode(12, INPUT_PULLDOWN); }
}

// start-up progress (drawn here, sent by the screen's task)
void oledSplash(const char * msg, int percent)
{
  using namespace oled;
  if (!s_on || s_page >= 0) return;
  g.clearBuffer();
  g.setFont(u8g2_font_helvB12_tr); centre(16, "TidyOS");
  g.setFont(u8g2_font_5x7_tr);
  char m[100]; plain(m, sizeof(m), msg);
  // the message over up to two lines
  const char * sp = nullptr;
  if (g.getStrWidth(m) > 128) for (const char * q = m; *q; ++q) if (*q == ' ' && (q - m) * 5 <= 126) sp = q;
  if (sp) { std::string a(m, sp - m); centre(30, a.c_str()); fitStr(imx(0, (128 - g.getStrWidth(sp + 1)) / 2), 39, sp + 1); }
  else centre(34, m);
  if (percent > 100) percent = 100;
  g.drawFrame(0, 52, 128, 10);
  if (percent > 0) {
    g.drawBox(2, 54, (124 * percent) / 100, 6);
    char p[8]; snprintf(p, sizeof(p), "%d%%", percent);
    g.drawStr(127 - g.getStrWidth(p), 48, p);       // the number, so bar and figure agree
  }
  portENTER_CRITICAL(&s_mux);
  memcpy(s_tx, g.getBufferPtr(), 1024);
  portEXIT_CRITICAL(&s_mux);
  s_dirty = true;
}

// once a second from the desktop: hand the screen what it shows, and put it to sleep after 5 minutes
void oledTick()
{
  using namespace oled;
  if (!s_on) return;
  std::string song = musicNowPlaying();
  int unread = msgStarted() ? msgUnreadTotal() : 0;
  if (song != s_song) strlcpy(s_song, song.c_str(), sizeof(s_song));
  s_unread = unread;
  s_home = WiFi.status() == WL_CONNECTED;
  s_ap = screenApOn();
  s_view = screenViewer();
  s_sd = gSDMounted;
  s_ramK = ramFree() / 1024;
  if (s_home) {
    s_rssi = WiFi.RSSI();
    IPAddress ip = WiFi.localIP();
    snprintf(s_ip, sizeof(s_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) strlcpy(s_ssid, (const char *)ap.ssid, sizeof(s_ssid));
  } else strlcpy(s_ssid, gSet.ssid.c_str(), sizeof(s_ssid));
  if (s_ap) s_apJoined = WiFi.softAPgetStationNum();
  if (!s_host[0]) { strlcpy(s_host, boardHost().c_str(), sizeof(s_host)); strlcpy(s_apName, boardId().c_str(), sizeof(s_apName)); }
  static uint32_t lastSt = 0;
  if (!lastSt || millis() - lastSt > 15000) {         // storage: every 15 s is plenty
    lastSt = millis() | 1;
    int64_t total = 0, used = 0;
    if (gSDMounted ? fabgl::FileBrowser::getFSInfo(fabgl::DriveType::SDCard, 0, &total, &used)
                   : (gFlashMounted && fabgl::FileBrowser::getFSInfo(fabgl::DriveType::SPIFFS, 0, &total, &used))) { s_stTotal = total; s_stUsed = used; }
  }
  // asleep after 5 minutes without a BOOT press (not while music plays; a new message wakes it)
  static int lastUnread = 0;
  if (unread > lastUnread || !song.empty()) s_wakeAt = millis();
  lastUnread = unread;
  if (gSet.oledSleep && !s_asleep && millis() - s_wakeAt > 300000) { s_asleep = true; s_sleepReq = true; }
  if (s_asleep && (!gSet.oledSleep || millis() - s_wakeAt < 300000)) s_wakeReq = true;
  if (s_page < 0) goPage(P_STATUS);                   // start-up done: the pages begin
}

void oledPoll() {}                                    // (the screen's own task does it all now)
#else
void oledBegin() {}
void oledOff() {}
void oledSplash(const char *, int) {}
void oledTick() {}
void oledPoll() {}
#endif
