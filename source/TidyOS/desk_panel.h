// TidyOS desktop: the panel on the right - time, date, weather, world times, a daily affirmation,
// and a world map shaded for day and night. (included in core.cpp, before the Desktop)
#include "worldmap.h"

// weather arrives from the web page (it looks it up: ip-api.com for the place, Open-Meteo for the forecast)
static struct { int temp = 0, code = -1, hi = 0, lo = 0; char city[40] = ""; uint32_t at = 0; } s_wx;
static volatile int s_wxVer = 0;

void deskWeatherFromPage(const char * t)     // "41|3|45|33|Harsons Island"
{
  int temp, code, hi, lo, n = 0;
  if (sscanf(t, "%d|%d|%d|%d|%n", &temp, &code, &hi, &lo, &n) < 4) return;
  s_wx.temp = temp; s_wx.code = code; s_wx.hi = hi; s_wx.lo = lo;
  strlcpy(s_wx.city, n ? t + n : "", sizeof(s_wx.city));
  s_wx.at = millis();
  s_wxVer++;
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
  const char * old = getenv("TZ");
  std::string keep = old ? old : "";
  setenv("TZ", tz, 1); tzset();
  localtime_r(&now, &out);
  if (keep.empty()) unsetenv("TZ"); else setenv("TZ", keep.c_str(), 1);
  tzset();
}

static const int PANEL_X = 344, PANEL_Y = 8, PANEL_W = 290, PANEL_H = 440;
static Rect deskPanelRect() { return Rect(PANEL_X, PANEL_Y, PANEL_X + PANEL_W - 1, PANEL_Y + PANEL_H - 1); }

// what the panel shows changes once a minute (or when weather arrives)
static int deskPanelSig()
{
  time_t now = time(nullptr);
  return (int)(now / 60) * 7 + s_wxVer;
}

static void drawWorldMap(Canvas * cv, int x0, int y0, time_t now)
{
  // where the sun is overhead: declination from the day of the year, longitude from the UTC time
  struct tm u; gmtime_r(&now, &u);
  float doy = u.tm_yday + (u.tm_hour + u.tm_min / 60.0f) / 24.0f;
  float decl = -23.44f * cosf(2 * PI / 365.0f * (doy + 10)) * PI / 180;
  float sunLon = -15.0f * (u.tm_hour + u.tm_min / 60.0f - 12) * PI / 180;
  float sd = sinf(decl), cd = cosf(decl);
  const int W = 144, H = 72, S = 2;
  for (int y = 0; y < H; ++y) {
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
  cv->drawRectangle(x0 - 1, y0 - 1, x0 + W * S, y0 + H * S);
  // the sun's spot
  int sx = x0 + (int)((sunLon * 180 / PI + 180) / 360 * W * S), sy = y0 + (int)((90 - decl * 180 / PI) / 180 * H * S);
  while (sx < x0) sx += W * S;
  while (sx >= x0 + W * S) sx -= W * S;
  cv->setBrushColor(T.win);
  cv->fillEllipse(sx, sy, 9, 9);
  cv->setPenColor(T.text);
  cv->drawEllipse(sx, sy, 9, 9);
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
  y += 30;
  snprintf(b, sizeof(b), "%s, %s %d, %d", DAY[t.tm_wday % 7], MON[t.tm_mon % 12], t.tm_mday, t.tm_year + 1900);
  cv->drawText(&FONT_std_14, x, y, b);
  y += 24;
  // weather
  cv->drawLine(x, y - 4, r.X2 - 10, y - 4);
  if (s_wx.code >= 0) {
    snprintf(b, sizeof(b), "%d\xB0" "F  %s", s_wx.temp, wxText(s_wx.code));
    cv->drawText(&FONT_std_18, x, y, b);
    y += 24;
    snprintf(b, sizeof(b), "High %d\xB0  Low %d\xB0   %s", s_wx.hi, s_wx.lo, s_wx.city);
    cv->drawText(&FONT_std_12, x, y, b);
    y += 18;
  } else {
#ifdef TIDYOS_HTML5
    cv->drawText(&FONT_std_14, x, y, "Weather: looking it up...");
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
    cv->drawText(&FONT_std_14, r.X2 - 12 - tw, y, b);
    y += 19;
  }
  // affirmation of the day, word-wrapped
  y += 4;
  cv->drawLine(x, y - 4, r.X2 - 10, y - 4);
  std::string a = AFFIRM[(t.tm_yday + t.tm_year) % NAFFIRM];
  int maxW = PANEL_W - 22, lines = 0;
  while (!a.empty() && lines < 3) {
    size_t cut = a.size();
    while (cv->textExtent(&FONT_std_14, a.substr(0, cut).c_str()) > maxW) { size_t sp = a.rfind(' ', cut - 1); if (sp == std::string::npos || sp == 0) break; cut = sp; }
    cv->drawText(&FONT_std_14, x, y, a.substr(0, cut).c_str());
    a = cut < a.size() ? a.substr(cut + 1) : "";
    y += 18; ++lines;
  }
  // the map sits at the bottom
  drawWorldMap(cv, r.X1 + (PANEL_W - 288) / 2, r.Y2 - 150, now);
}
