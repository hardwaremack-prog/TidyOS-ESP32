// ====================================================================== little OLED (HTML5 edition)
// A 128x64 SSD1306 on GPIO 21 (SDA) / 22 (SCL) - the same wiring as The Shelf on ESP32 - is found
// at start-up and then shows a few screens in turn: how to get in, the clock, the weather, world
// times, the system, the song playing and new messages. The two-colour modules' yellow band is the
// bottom 16 rows here (the module sits upside down on the header, so the picture is turned around).
// The picture is drawn on the UI side; a small task sends it over I2C (~25 ms), so nothing waits.
// (included from core.cpp, after desk_panel.h)
#ifdef TIDYOS_HTML5
#include <Wire.h>

std::string musicNowPlaying();                // media_apps.h: the song playing ("" = none)

namespace oled {

static bool     s_on = false;
static bool     s_inline = false;            // plugged straight onto 27 · 14 · 12 · GND, powered from GPIO 12
static uint8_t  s_addr = 0x3C;
static uint8_t  s_buf[1024];                  // drawn here (UI side)
static uint8_t  s_tx[1024];                   // being sent (I2C task)
static volatile bool s_dirty = false;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int      s_page = 0, s_pageAge = 0;

static void cmds(std::initializer_list<uint8_t> list)
{
  Wire.beginTransmission(s_addr);
  Wire.write(0x00);
  for (uint8_t c : list) Wire.write(c);
  Wire.endTransmission();
}

static void sendTask(void *)
{
  for (;;) {
    if (!s_dirty) { vTaskDelay(pdMS_TO_TICKS(40)); continue; }
    portENTER_CRITICAL(&s_mux);
    memcpy(s_tx, s_buf, sizeof(s_tx));
    s_dirty = false;
    portEXIT_CRITICAL(&s_mux);
    cmds({ 0x21, 0, 127, 0x22, 0, 7 });       // whole screen
    for (int i = 0; i < 1024; i += 32) {
      Wire.beginTransmission(s_addr);
      Wire.write(0x40);
      Wire.write(s_tx + i, 32);
      Wire.endTransmission();
    }
  }
}

// ---- drawing into s_buf
static void clear() { memset(s_buf, 0, sizeof(s_buf)); }
static inline void px(int x, int y) { if ((unsigned)x < 128 && (unsigned)y < 64) s_buf[x + (y >> 3) * 128] |= 1 << (y & 7); }
static void hline(int x0, int x1, int y) { for (int x = x0; x <= x1; ++x) px(x, y); }
static void box(int x0, int y0, int x1, int y1) { for (int y = y0; y <= y1; ++y) hline(x0, x1, y); }

static int glyphW(const fabgl::FontInfo * f, uint8_t c)
{
  return f->chptr ? f->data[f->chptr[c]] : f->width;
}
static int textW(const fabgl::FontInfo * f, const char * s) { int w = 0; while (*s) w += glyphW(f, (uint8_t)*s++); return w; }
static int text(const fabgl::FontInfo * f, int x, int y, const char * s)
{
  for (; *s; ++s) {
    uint8_t c = (uint8_t)*s;
    int w; const uint8_t * rows;
    if (f->chptr) { const uint8_t * g = f->data + f->chptr[c]; w = g[0]; rows = g + 1; }
    else { w = f->width; rows = f->data + c * f->height * ((w + 7) / 8); }
    int bpr = (w + 7) / 8;
    for (int r = 0; r < f->height; ++r)
      for (int i = 0; i < w; ++i)
        if (rows[r * bpr + i / 8] & (0x80 >> (i & 7))) px(x + i, y + r);
    x += w;
    if (x > 128) break;
  }
  return x;
}
static void center(const fabgl::FontInfo * f, int y, const char * s) { text(f, (128 - textW(f, s)) / 2, y, s); }
static void fit(const fabgl::FontInfo * f, int x, int y, const char * s, int maxW = 128)     // cut to fit
{
  char b[64]; strlcpy(b, s, sizeof(b));
  while (strlen(b) > 1 && textW(f, b) > maxW - x) b[strlen(b) - 1] = 0;
  text(f, x, y, b);
}
// the yellow band at the bottom: one line of small text
static void band(const char * s) { fit(&FONT_std_12, 2, 51, s, 126); }

static void push() { portENTER_CRITICAL(&s_mux); s_dirty = true; portEXIT_CRITICAL(&s_mux); }

// ---- the screens
static void hm(const struct tm & t, char * b, size_t n, bool ampm = true)
{
  int h = t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12;
  snprintf(b, n, ampm ? "%d:%02d %s" : "%d:%02d", h, t.tm_min, t.tm_hour < 12 ? "AM" : "PM");
}

static void pageAccess()
{
  char b[48];
  bool home = WiFi.status() == WL_CONNECTED;
  if (screenApOn()) {
    std::string id = boardId();
    snprintf(b, sizeof(b), "Wi-Fi %s", id.c_str());  fit(&FONT_std_12, 0, 0, b);
    text(&FONT_std_12, 0, 15, "Password tidyos123");
    text(&FONT_std_14, 0, 31, "10.1.1.250");
    band(home ? "Hotspot - also on home Wi-Fi" : "How to get in: hotspot");
  } else if (home) {
    snprintf(b, sizeof(b), "%s", WiFi.SSID().c_str()); fit(&FONT_std_12, 0, 0, b);
    IPAddress ip = WiFi.localIP();
    snprintf(b, sizeof(b), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    text(&FONT_std_16, 0, 14, b);
    std::string h = boardHost() + ".local";
    fit(&FONT_std_12, 0, 33, h.c_str());
    band("How to get in: home Wi-Fi");
  } else {
    text(&FONT_std_14, 0, 4, "Joining Wi-Fi...");
    band("How to get in");
  }
}

static void pageClock(time_t now)
{
  struct tm t; localtime_r(&now, &t);
  char b[24];
  hm(t, b, sizeof(b), false);
  int w = textW(&FONT_std_24, b), w2 = textW(&FONT_std_12, t.tm_hour < 12 ? "AM" : "PM");
  int x = (128 - w - w2 - 3) / 2;
  text(&FONT_std_24, x, 2, b);
  text(&FONT_std_12, x + w + 3, 12, t.tm_hour < 12 ? "AM" : "PM");
  static const char * DN[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
  static const char * MN[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
  char d[40]; snprintf(d, sizeof(d), "%s %s %d", DN[t.tm_wday % 7], MN[t.tm_mon % 12], t.tm_mday);
  center(&FONT_std_12, 33, d);
  if (s_wx.code >= 0) { snprintf(d, sizeof(d), "%d\xB0" "F  %s", s_wx.temp, wxText(s_wx.code)); band(d); }
  else band(netClockSet() ? "TidyOS" : "Clock not set yet");
}

static void pageWeather()
{
  char b[48];
  snprintf(b, sizeof(b), "%d\xB0" "F", s_wx.temp);
  text(&FONT_std_24, 0, 2, b);
  snprintf(b, sizeof(b), "Hi %d\xB0", s_wx.hi);   text(&FONT_std_12, 84, 3, b);
  snprintf(b, sizeof(b), "Lo %d\xB0", s_wx.lo);   text(&FONT_std_12, 84, 17, b);
  fit(&FONT_std_14, 0, 32, wxText(s_wx.code));
  band(s_wx.city[0] ? s_wx.city : "Weather");
}

static void pageWorld(time_t now)
{
  static const char * Z[3][2] = { { "Moscow", "MSK-3" }, { "L.A.", "PST8PDT,M3.2.0,M11.1.0" }, { "Tokyo", "JST-9" } };
  for (int i = 0; i < 3; ++i) {
    struct tm w; tzLocal(Z[i][1], now, w);
    char b[24];
    int y = i * 16;
    text(&FONT_std_12, 0, y, Z[i][0]);
    int h = w.tm_hour % 12 == 0 ? 12 : w.tm_hour % 12;
    snprintf(b, sizeof(b), "%d:%02d%s", h, w.tm_min, w.tm_hour < 12 ? "a" : "p");
    text(&FONT_std_12, 50, y, b);
    if (s_cityHave) { snprintf(b, sizeof(b), "%d\xB0", s_cityT[i]); text(&FONT_std_12, 127 - textW(&FONT_std_12, b), y, b); }
  }
  band("World time");
}

static void pageSystem()
{
  char b[48];
  snprintf(b, sizeof(b), "RAM %uK free", (unsigned)(ramFree() / 1024)); text(&FONT_std_12, 0, 0, b);
  if (WiFi.status() == WL_CONNECTED) snprintf(b, sizeof(b), "Wi-Fi %d dBm", (int)WiFi.RSSI());
  else if (screenApOn()) snprintf(b, sizeof(b), "Hotspot: %d joined", (int)WiFi.softAPgetStationNum());
  else snprintf(b, sizeof(b), "No Wi-Fi");
  text(&FONT_std_12, 0, 15, b);
  uint32_t s = millis() / 1000;
  snprintf(b, sizeof(b), "Up %uh %02um  %s", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), screenViewer() ? "browser on" : "no browser");
  fit(&FONT_std_12, 0, 30, b);
  snprintf(b, sizeof(b), "TidyOS %s  %s", TIDYOS_VERSION, boardId().substr(7).c_str());
  band(b);
}

static void pageMusic(const std::string & song)
{
  text(&FONT_std_12, 0, 0, "Now playing");
  // the song name over two lines
  std::string a = song, b;
  while (a.size() > 1 && textW(&FONT_std_14, a.c_str()) > 128) { b.insert(b.begin(), a.back()); a.pop_back(); }
  if (!b.empty()) { size_t sp = a.rfind(' '); if (sp != std::string::npos && sp > a.size() / 2) { b = a.substr(sp + 1) + b; a.resize(sp); } }
  text(&FONT_std_14, 0, 15, a.c_str());
  fit(&FONT_std_14, 0, 31, b.c_str());
  band("Music");
}

static void pageMessages(int n)
{
  char b[32];
  text(&FONT_std_14, 0, 4, "New messages");
  snprintf(b, sizeof(b), "%d unread", n);
  text(&FONT_std_24, 0, 20, b);
  band("Open Messages to read them");
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
  // SSD1306 128x64, turned around (A0 / C0), charge pump on
  cmds({ 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40, 0x8D, 0x14, 0x20, 0x00, 0xA0, 0xC0,
         0xDA, 0x12, 0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0x2E, 0xAF });
  clear();
  center(&FONT_std_16, 14, "TidyOS");
  center(&FONT_std_12, 34, "starting...");
  push();
  xTaskCreatePinnedToCore(sendTask, "oled", 2560, nullptr, 1, nullptr, 0);
  Serial.printf("[TidyOS] OLED screen found at 0x%02X\n", s_addr);
}

// start-up progress
// before deep sleep: power an in-line screen down (GPIO 12 must be low when the board starts again)
void oledOff()
{
  using namespace oled;
  if (!s_on) return;
  cmds({ 0xAE });
  if (s_inline) { digitalWrite(12, LOW); pinMode(12, INPUT_PULLDOWN); }
}

void oledSplash(const char * msg, int percent)
{
  using namespace oled;
  if (!s_on) return;
  clear();
  center(&FONT_std_16, 4, "TidyOS");
  fit(&FONT_std_12, 0, 26, msg);
  hline(0, 127, 52); hline(0, 127, 61);
  for (int y = 52; y <= 61; ++y) { px(0, y); px(127, y); }
  if (percent > 0) box(2, 54, 2 + (123 * (percent > 100 ? 100 : percent)) / 100, 59);
  push();
}

// once a second from the desktop: the next screen every 5 s
void oledTick()
{
  using namespace oled;
  if (!s_on) return;
  time_t now = time(nullptr);
  std::string song = musicNowPlaying();
  int unread = msgStarted() ? msgUnreadTotal() : 0;
  // the screens to go round: 0 access, 1 clock, 2 weather, 3 world, 4 system, 5 music, 6 messages
  bool have[7] = { true, true, s_wx.code >= 0, true, true, !song.empty(), unread > 0 };
  if (++s_pageAge >= 5) {
    s_pageAge = 0;
    for (int k = 0; k < 7; ++k) { s_page = (s_page + 1) % 7; if (have[s_page]) break; }
  }
  if (!have[s_page]) s_page = 1;
  clear();
  switch (s_page) {
    case 0: pageAccess(); break;
    case 1: pageClock(now); break;
    case 2: pageWeather(); break;
    case 3: pageWorld(now); break;
    case 4: pageSystem(); break;
    case 5: pageMusic(song); break;
    default: pageMessages(unread); break;
  }
  // which screen of how many: little dots up the right of the band
  push();
}
#else
void oledBegin() {}
void oledOff() {}
void oledSplash(const char *, int) {}
void oledTick() {}
#endif
