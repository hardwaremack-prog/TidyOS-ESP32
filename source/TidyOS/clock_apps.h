#include "tzcalc.h"
// ====================================================================== Screenshot and Clock (alarm + stopwatch)
// (included at the end of apps.cpp)

bool screenSaveBMP(const char * path);

static std::string shotFolder()
{
  if (gSDMounted) {
    std::string d = SD_MOUNT "/Pictures";
    mkdir(d.c_str(), 0777);
    DIR * t = opendir(d.c_str());
    if (t) { closedir(t); return d; }
  }
  return documentsDir();
}

// saves the screen now; returns the file's path, or "" if it couldn't be written
static std::string takeScreenshot()
{
  char name[48];
  time_t now = time(nullptr); struct tm t; localtime_r(&now, &t);
  if (t.tm_year > 100) snprintf(name, sizeof(name), "screenshot-%04d-%02d-%02d-%02d%02d%02d.bmp", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  else snprintf(name, sizeof(name), "screenshot-%lu.bmp", (unsigned long)(millis() / 1000));
  std::string p = joinPath(shotFolder().c_str(), name);
  return screenSaveBMP(p.c_str()) ? p : "";
}

class ShotApp : public AppFrame {
public:
  static AppFrame * inst;
  uiLabel * status;
  uiButton * delayBtn;
  int delay = 0;                 // seconds before taking it
  int countdown = -1;
  std::string last;

  ShotApp() : AppFrame("Screenshot", nextPos(360, 200), Size(360, 200), &inst) {
    label("Saves the whole screen as a picture in Pictures.", cx, cy, 340, 18)->labelStyle().textFont = &FONT_std_12;
    button("Take screenshot", cx, cy + 26, 150, 30)->onClick = [this]() { start(); };
    delayBtn = button("Delay: none", cx + 158, cy + 26, 110, 30);
    delayBtn->onClick = [this]() {
      delay = delay == 0 ? 3 : delay == 3 ? 5 : delay == 5 ? 10 : 0;
      char b[24]; snprintf(b, sizeof(b), delay ? "Delay: %d s" : "Delay: none", delay);
      delayBtn->setText(b); delayBtn->repaint();
    };
    button("Open it", cx, cy + 64, 90, 26)->onClick = [this]() { if (!last.empty()) openPictures(last.c_str()); };
    button("Pictures folder", cx + 96, cy + 64, 130, 26)->onClick = [this]() { openFilesAt(shotFolder().c_str()); };
    status = label("Tip: the Print Screen key takes one any time.", cx, cy + 100, 340, 36);
    status->labelStyle().textFont = &FONT_std_12;
    onTimer = [this](uiTimerHandle) { tick(); };
  }
  void start() {
    if (countdown >= 0) return;
    countdown = delay * 2 + 2;                        // half-second steps; this window hides first
    gApp->showWindow(this, false);
    startTimer(500);
  }
  void tick() {
    if (countdown < 0) return;
    if (--countdown > 0) return;
    if (m_timer) { gApp->killTimer(m_timer); m_timer = nullptr; }
    countdown = -1;
    last = takeScreenshot();
    gApp->showWindow(this, true);
    bringToFront(this);
    status->setText(last.empty() ? "Couldn't save it - is the SD card in?" : ("Saved " + baseName(last)).c_str());
    status->repaint();
  }
};
AppFrame * ShotApp::inst = nullptr;

void openShot()
{
  if (!ShotApp::inst) new ShotApp();
  ShotApp::inst->open();
}

volatile bool gShotReq = false;           // Print Screen key

static void shotKeyCheck()
{
  if (!gShotReq) return;
  gShotReq = false;
  std::string p = takeScreenshot();
  gApp->messageBox("Screenshot", p.empty() ? "Couldn't save the screenshot - is the SD card in?" : ("Saved " + baseName(p) + " in Pictures.").c_str(),
                   "OK", nullptr, nullptr, p.empty() ? uiMessageBoxIcon::Error : uiMessageBoxIcon::Info);
}

// ---------------------------------------------------------------------- alarm (rings even with the Clock closed)

static std::string hhmm(int minutes)
{
  int h = minutes / 60, m = minutes % 60;
  char b[16]; snprintf(b, sizeof(b), "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM");
  return b;
}

static bool   s_ringing = false;
static long   s_rungKey = -1;
static time_t s_snoozeAt = 0;

static void ringAlarm()
{
  s_ringing = true;
  Serial.printf("[TidyOS] alarm ringing (browser %s)\n", screenViewer() ? "connected" : "not connected");
  screenMedia("b 1");
  time_t now = time(nullptr); struct tm t; localtime_r(&now, &t);
  std::string msg = "Alarm - it's " + hhmm(t.tm_hour * 60 + t.tm_min) + ".";
  auto r = gApp->messageBox("Alarm", msg.c_str(), "Stop", "Snooze 5 min", nullptr, uiMessageBoxIcon::Warning);
  screenMedia("b 0");
  s_snoozeAt = r == uiMessageBoxResult::Button2 ? time(nullptr) + 300 : 0;
  s_ringing = false;
}

void alarmTick()                          // the desktop calls this a few times a second
{
  shotKeyCheck();
  if (s_ringing) return;
  time_t now = time(nullptr);
  if (now < 1600000000) return;           // clock not set yet
  if (s_snoozeAt && now >= s_snoozeAt) { s_snoozeAt = 0; ringAlarm(); return; }
  if (!gSet.alarmOn) return;
  struct tm t; localtime_r(&now, &t);
  int m = t.tm_hour * 60 + t.tm_min;
  long key = (long)(t.tm_year % 100) * 1000000L + t.tm_yday * 1440L + m;
  if (m == gSet.alarmMin && key != s_rungKey) { s_rungKey = key; ringAlarm(); }
}

// world time: POSIX time-zone rules, so summer time switches by itself
static const struct { const char * name; const char * tz; } CITIES[] = {
  { "London", "GMT0BST,M3.5.0/1,M10.5.0" },        { "Paris", "CET-1CEST,M3.5.0,M10.5.0/3" },
  { "Tokyo", "JST-9" },                            { "Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
  { "Los Angeles", "PST8PDT,M3.2.0,M11.1.0" },     { "Denver", "MST7MDT,M3.2.0,M11.1.0" },
  { "Chicago", "CST6CDT,M3.2.0,M11.1.0" },         { "New York", "EST5EDT,M3.2.0,M11.1.0" },
  { "Honolulu", "HST10" },                         { "Anchorage", "AKST9AKDT,M3.2.0,M11.1.0" },
  { "Rio de Janeiro", "<-03>3" },                  { "Moscow", "MSK-3" },
  { "Dubai", "<+04>-4" },                          { "New Delhi", "IST-5:30" },
  { "Beijing", "CST-8" },                          { "UTC", "UTC0" },
};
static const int NCITIES = sizeof(CITIES) / sizeof(CITIES[0]);

static bool cityTime(int i, time_t now, struct tm & out)
{
  tzLocal(CITIES[i % NCITIES].tz, now, out);      // (no setenv: it leaked memory every call)
  return true;
}

// an analog watch face: fluted bezel, baton markers, date window at 3, sweep second hand
class WatchFace : public uiControl {
public:
  struct tm t;
  bool valid = false;
  WatchFace(uiWindow * parent, Point pos, int size) : uiControl(parent, pos, Size(size, size), true, 0) {
    windowStyle().borderSize = 0;
    memset(&t, 0, sizeof(t));
  }
  void processEvent(uiEvent * e) override {
    uiControl::processEvent(e);
    if (e->id == UIEVT_PAINT) { beginPaint(e, clientRect(uiOrigin::Window)); paint(); }
  }
  static void hand(Canvas * cv, int x, int y, float turns, float len, float tail, int width) {
    float a = turns * 2 * PI;
    float sx = sinf(a), sy = -cosf(a);
    cv->setPenWidth(width);
    cv->drawLine(x - (int)(sx * tail), y - (int)(sy * tail), x + (int)(sx * len), y + (int)(sy * len));
  }
  void paint() {
    Rect r = clientRect(uiOrigin::Window);
    auto cv = canvas();
    int D = r.width(), x = r.X1 + D / 2, y = r.Y1 + D / 2, R = D / 2 - 1;
    cv->setBrushColor(T.win);
    cv->fillRectangle(r.X1, r.Y1, r.X2, r.Y2);
    // bezel with a fluted (ridged) edge
    cv->setBrushColor(T.face);
    cv->fillEllipse(x, y, R * 2, R * 2);
    cv->setPenColor(T.text);
    cv->setPenWidth(1);
    cv->drawEllipse(x, y, R * 2, R * 2);
    for (int i = 0; i < 72; ++i) {
      float a = i * 2 * PI / 72;
      cv->drawLine(x + (int)(sinf(a) * (R - 8)), y - (int)(cosf(a) * (R - 8)), x + (int)(sinf(a) * (R - 1)), y - (int)(cosf(a) * (R - 1)));
    }
    // dial
    int dr = R - 10;
    cv->setBrushColor(T.accent);
    cv->fillEllipse(x, y, dr * 2, dr * 2);
    cv->setPenColor(T.text);
    cv->drawEllipse(x, y, dr * 2, dr * 2);
    // minute track and hour batons
    cv->setPenColor(T.accentText);
    for (int i = 0; i < 60; ++i) {
      float a = i * 2 * PI / 60, sx = sinf(a), sy = -cosf(a);
      if (i % 5) { cv->setPenWidth(1); cv->drawLine(x + (int)(sx * (dr - 4)), y + (int)(sy * (dr - 4)), x + (int)(sx * (dr - 2)), y + (int)(sy * (dr - 2))); continue; }
      if (i == 0) {                                             // a triangle at 12
        Point tri[3] = { Point(x, y - dr + 16), Point(x - 6, y - dr + 4), Point(x + 6, y - dr + 4) };
        cv->setBrushColor(T.accentText);
        cv->fillPath(tri, 3);
        continue;
      }
      if (i == 15) continue;                                    // the date window sits at 3
      cv->setPenWidth(i % 15 == 0 ? 5 : 3);
      cv->drawLine(x + (int)(sx * (dr - 16)), y + (int)(sy * (dr - 16)), x + (int)(sx * (dr - 4)), y + (int)(sy * (dr - 4)));
    }
    cv->setPenWidth(1);
    // date window
    if (valid) {
      char b[4]; snprintf(b, sizeof(b), "%d", t.tm_mday);
      int wx = x + dr - 30, wy = y - 7;
      cv->setBrushColor(T.win);
      cv->fillRectangle(wx, wy, wx + 20, wy + 14);
      cv->setPenColor(T.text);
      cv->drawRectangle(wx, wy, wx + 20, wy + 14);
      cv->setBrushColor(T.win);
      int tw = cv->textExtent(&FONT_std_12, b);
      cv->drawText(&FONT_std_12, wx + 11 - tw / 2, wy, b);
    }
    // name plate
    cv->setPenColor(T.accentText);
    cv->setBrushColor(T.accent);
    int nw = cv->textExtent(&FONT_std_12, "TidyOS");
    cv->drawText(&FONT_std_12, x - nw / 2, y - dr / 2 - 4, "TidyOS");
    nw = cv->textExtent(&FONT_std_12, "CHRONOMETER");
    cv->drawText(&FONT_std_12, x - nw / 2, y + dr / 3, "CHRONOMETER");
    if (!valid) return;
    float sec = t.tm_sec, min = t.tm_min + sec / 60, hr = (t.tm_hour % 12) + min / 60;
    // hands: an outline in the bezel colour keeps them readable on the dial
    cv->setPenColor(T.text);
    hand(cv, x, y, hr / 12, dr * 0.52f, 8, 7);
    hand(cv, x, y, min / 60, dr * 0.80f, 8, 5);
    cv->setPenColor(T.accentText);
    hand(cv, x, y, hr / 12, dr * 0.50f, 6, 4);
    hand(cv, x, y, min / 60, dr * 0.78f, 6, 2);
    cv->setPenColor(T.face);
    hand(cv, x, y, sec / 60, dr * 0.88f, 16, 1);
    cv->setPenWidth(1);
    cv->setBrushColor(T.face);
    cv->fillEllipse(x, y, 9, 9);
    cv->setPenColor(T.text);
    cv->drawEllipse(x, y, 9, 9);
  }
};

class ClockApp : public AppFrame {
public:
  static AppFrame * inst;
  WatchFace * face;
  uiLabel * timeLbl, * dateLbl, * worldLbl, * alarmLbl, * swLbl, * lapsLbl[3], * presetHint;
  uiButton * alarmBtn, * swBtn, * storeBtn, * presetBtn[4];
  bool swRunning = false, storing = false;
  uint32_t swStart = 0, swAcc = 0;          // ms
  std::vector<uint32_t> laps;
  int lastSec = -1;

  ClockApp() : AppFrame("Clock", Point(40, 2), Size(560, 454), &inst) {
    face = new WatchFace(this, Point(cx, cy), 168);
    int rx = cx + 180;
    timeLbl = label("", rx, cy + 6, 360, 30);
    timeLbl->labelStyle().textFont = &FONT_std_24;
    dateLbl = label("", rx, cy + 40, 360, 18);
    label("World time", rx, cy + 74, 120, 16)->labelStyle().textFont = &FONT_std_12;
    worldLbl = label("", rx, cy + 90, 250, 20);
    worldLbl->labelStyle().textFont = &FONT_std_16;
    button("Next city", rx + 256, cy + 88, 90, 24)->onClick = [this]() { gSet.worldCity = (gSet.worldCity + 1) % NCITIES; settingsSave(); lastSec = -1; tick(); };
    button("Test sound", rx, cy + 128, 96, 24)->onClick = []() { screenMedia("b 2"); };
    int y = cy + 178;
    label("Alarm", cx, y + 2, 60, 18)->labelStyle().textFont = &FONT_std_16;
    alarmLbl = label("", cx + 64, y, 120, 22);
    alarmLbl->labelStyle().textFont = &FONT_std_18;
    alarmBtn = button("", cx + 190, y, 96, 24);
    alarmBtn->onClick = [this]() { gSet.alarmOn = !gSet.alarmOn; settingsSave(); showAlarm(); };
    y += 30;
    button("Hour -", cx + 64, y, 60, 24)->onClick = [this]() { moveAlarm(-60); };
    button("Hour +", cx + 128, y, 60, 24)->onClick = [this]() { moveAlarm(60); };
    button("Min -", cx + 196, y, 54, 24)->onClick = [this]() { moveAlarm(-5); };
    button("Min +", cx + 254, y, 54, 24)->onClick = [this]() { moveAlarm(5); };
    button("-1", cx + 316, y, 40, 24)->onClick = [this]() { moveAlarm(-1); };
    button("+1", cx + 360, y, 40, 24)->onClick = [this]() { moveAlarm(1); };
    y += 30;
    label("Presets", cx, y + 4, 60, 18)->labelStyle().textFont = &FONT_std_14;
    for (int i = 0; i < 4; ++i) {
      presetBtn[i] = button("", cx + 64 + i * 84, y, 80, 24);
      presetBtn[i]->onClick = [this, i]() { usePreset(i); };
    }
    storeBtn = button("Save to...", cx + 404, y, 84, 24);
    storeBtn->onClick = [this]() { storing = !storing; showPresets(); };
    presetHint = label("", cx + 64, y + 26, 440, 16);
    presetHint->labelStyle().textFont = &FONT_std_12;
    y += 48;
    label("Stopwatch", cx, y + 6, 90, 18)->labelStyle().textFont = &FONT_std_16;
    swLbl = label("0:00.0", cx + 96, y, 150, 30);
    swLbl->labelStyle().textFont = &FONT_std_24;
    swBtn = button("Start", cx + 256, y + 2, 70, 26);
    swBtn->onClick = [this]() { startStop(); };
    button("Lap", cx + 332, y + 2, 60, 26)->onClick = [this]() { lap(); };
    button("Reset", cx + 398, y + 2, 64, 26)->onClick = [this]() { reset(); };
    for (int i = 0; i < 3; ++i) { lapsLbl[i] = label("", cx + 96, y + 34 + i * 15, 340, 15); lapsLbl[i]->labelStyle().textFont = &FONT_std_12; }
    showAlarm();
    showPresets();
    onTimer = [this](uiTimerHandle) { tick(); };
    startTimer(100);
    tick();
  }
  static std::string fmtSw(uint32_t ms) {
    char b[24];
    uint32_t t = ms / 100;
    if (t >= 36000) snprintf(b, sizeof(b), "%u:%02u:%02u.%u", t / 36000, t / 600 % 60, t / 10 % 60, t % 10);
    else snprintf(b, sizeof(b), "%u:%02u.%u", t / 600, t / 10 % 60, t % 10);
    return b;
  }
  uint32_t swNow() { return swAcc + (swRunning ? millis() - swStart : 0); }
  void startStop() {
    if (swRunning) { swAcc += millis() - swStart; swRunning = false; }
    else { swStart = millis(); swRunning = true; }
    swBtn->setText(swRunning ? "Stop" : "Start"); swBtn->repaint();
    showSw();
  }
  void lap() {
    if (!swRunning) return;
    laps.push_back(swNow());
    if (laps.size() > 3) laps.erase(laps.begin());
    showLaps();
  }
  void showLaps() {
    for (int k = 0; k < 3; ++k) {
      int i = (int)laps.size() - 1 - k;
      char b[64] = "";
      if (i >= 0) snprintf(b, sizeof(b), "Lap at %s   (+%s)", fmtSw(laps[i]).c_str(), fmtSw(laps[i] - (i > 0 ? laps[i - 1] : 0)).c_str());
      lapsLbl[k]->setText(b); lapsLbl[k]->repaint();
    }
  }
  void reset() {
    swRunning = false; swAcc = 0; laps.clear();
    swBtn->setText("Start"); swBtn->repaint();
    showLaps();
    showSw();
  }
  void showSw() { swLbl->setText(fmtSw(swNow()).c_str()); swLbl->repaint(); }
  void moveAlarm(int d) { gSet.alarmMin = ((gSet.alarmMin + d) % 1440 + 1440) % 1440; settingsSave(); showAlarm(); }
  void showAlarm() {
    alarmLbl->setText(hhmm(gSet.alarmMin).c_str()); alarmLbl->repaint();
    alarmBtn->setText(gSet.alarmOn ? "Alarm: on" : "Alarm: off"); alarmBtn->repaint();
  }
  void usePreset(int i) {
    if (storing) {                                  // keep the alarm time in this slot
      gSet.alarmPresets[i] = gSet.alarmMin;
      storing = false;
      settingsSave();
      showPresets();
      return;
    }
    gSet.alarmMin = gSet.alarmPresets[i];
    gSet.alarmOn = true;
    settingsSave();
    showAlarm();
  }
  void showPresets() {
    for (int i = 0; i < 4; ++i) { presetBtn[i]->setText(hhmm(gSet.alarmPresets[i]).c_str()); presetBtn[i]->repaint(); }
    storeBtn->setText(storing ? "Cancel" : "Save to..."); storeBtn->repaint();
    presetHint->setText(storing ? "Click a preset to keep the alarm time above in it." : "Click a preset to set the alarm to it and turn it on.");
    presetHint->repaint();
  }
  void tick() {
    if (swRunning) showSw();
    time_t now = time(nullptr);
    if (now < 1600000000) {
      if (lastSec != -2) {
        lastSec = -2;
        timeLbl->setText("--:--"); timeLbl->repaint();
        dateLbl->setText("Waiting for the clock to be set..."); dateLbl->repaint();
        face->valid = false; face->repaint();
      }
      return;
    }
    struct tm t; localtime_r(&now, &t);
    if (t.tm_sec == lastSec) return;
    lastSec = t.tm_sec;
    char b[64];
    snprintf(b, sizeof(b), "%d:%02d:%02d %s", t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12, t.tm_min, t.tm_sec, t.tm_hour < 12 ? "AM" : "PM");
    timeLbl->setText(b); timeLbl->repaint();
    static const char * DAY[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char * MON[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
    snprintf(b, sizeof(b), "%s, %s %d, %d", DAY[t.tm_wday % 7], MON[t.tm_mon % 12], t.tm_mday, t.tm_year + 1900);
    dateLbl->setText(b); dateLbl->repaint();
    face->t = t; face->valid = true; face->repaint();
    struct tm w;
    cityTime(gSet.worldCity, now, w);
    snprintf(b, sizeof(b), "%s  %d:%02d %s %.3s", CITIES[gSet.worldCity % NCITIES].name, w.tm_hour % 12 == 0 ? 12 : w.tm_hour % 12, w.tm_min,
             w.tm_hour < 12 ? "AM" : "PM", DAY[w.tm_wday % 7]);
    worldLbl->setText(b); worldLbl->repaint();
  }
};
AppFrame * ClockApp::inst = nullptr;

void openClock()
{
  if (!ClockApp::inst) new ClockApp();
  ClockApp::inst->open();
}
