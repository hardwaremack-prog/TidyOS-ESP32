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

class ClockApp : public AppFrame {
public:
  static AppFrame * inst;
  uiLabel * timeLbl, * dateLbl, * alarmLbl, * swLbl, * lapsLbl;
  uiButton * alarmBtn, * swBtn;
  bool swRunning = false;
  uint32_t swStart = 0, swAcc = 0;          // ms
  std::vector<uint32_t> laps;
  std::string lastTime;

  ClockApp() : AppFrame("Clock", nextPos(440, 360), Size(440, 360), &inst) {
    timeLbl = label("", cx, cy, 420, 30);
    timeLbl->labelStyle().textFont = &FONT_std_24;
    dateLbl = label("", cx, cy + 32, 420, 18);
    int y = cy + 62;
    label("Alarm", cx, y, 60, 18)->labelStyle().textFont = &FONT_std_16;
    alarmLbl = label("", cx + 64, y - 2, 140, 22);
    alarmLbl->labelStyle().textFont = &FONT_std_18;
    alarmBtn = button("", cx + 214, y - 2, 96, 24);
    alarmBtn->onClick = [this]() { gSet.alarmOn = !gSet.alarmOn; settingsSave(); showAlarm(); };
    button("Test sound", cx + 316, y - 2, 96, 24)->onClick = []() { screenMedia("b 2"); };
    y += 28;
    button("Hour -", cx + 64, y, 60, 24)->onClick = [this]() { moveAlarm(-60); };
    button("Hour +", cx + 128, y, 60, 24)->onClick = [this]() { moveAlarm(60); };
    button("Min -", cx + 196, y, 54, 24)->onClick = [this]() { moveAlarm(-5); };
    button("Min +", cx + 254, y, 54, 24)->onClick = [this]() { moveAlarm(5); };
    button("-1", cx + 316, y, 46, 24)->onClick = [this]() { moveAlarm(-1); };
    button("+1", cx + 366, y, 46, 24)->onClick = [this]() { moveAlarm(1); };
    y += 30;
#ifdef TIDYOS_HTML5
    label("Rings with beeps in the web browser and a message on screen.", cx + 64, y, 360, 16)->labelStyle().textFont = &FONT_std_12;
#else
    label("Rings with a message on screen.", cx + 64, y, 360, 16)->labelStyle().textFont = &FONT_std_12;
#endif
    y += 30;
    label("Stopwatch", cx, y + 4, 90, 18)->labelStyle().textFont = &FONT_std_16;
    swLbl = label("0:00.0", cx + 96, y, 160, 30);
    swLbl->labelStyle().textFont = &FONT_std_24;
    y += 36;
    swBtn = button("Start", cx, y, 80, 26);
    swBtn->onClick = [this]() { startStop(); };
    button("Lap", cx + 86, y, 70, 26)->onClick = [this]() { lap(); };
    button("Reset", cx + 162, y, 70, 26)->onClick = [this]() { reset(); };
    lapsLbl = label("", cx, y + 32, 420, 100);
    lapsLbl->labelStyle().textFont = &FONT_std_14;
    lapsLbl->labelStyle().textAlign = uiHAlign::Left;
    showAlarm();
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
    if (laps.size() > 5) laps.erase(laps.begin());
    std::string s;
    for (int i = laps.size() - 1; i >= 0; --i) {
      uint32_t prev = i > 0 ? laps[i - 1] : 0;
      char b[64];
      snprintf(b, sizeof(b), "Lap at %s   (+%s)\n", fmtSw(laps[i]).c_str(), fmtSw(laps[i] - (i > 0 ? prev : 0)).c_str());
      s += b;
    }
    lapsLbl->setText(s.c_str()); lapsLbl->repaint();
  }
  void reset() {
    swRunning = false; swAcc = 0; laps.clear();
    swBtn->setText("Start"); swBtn->repaint();
    lapsLbl->setText(""); lapsLbl->repaint();
    showSw();
  }
  void showSw() { swLbl->setText(fmtSw(swNow()).c_str()); swLbl->repaint(); }
  void moveAlarm(int d) { gSet.alarmMin = ((gSet.alarmMin + d) % 1440 + 1440) % 1440; settingsSave(); showAlarm(); }
  void showAlarm() {
    alarmLbl->setText(hhmm(gSet.alarmMin).c_str()); alarmLbl->repaint();
    alarmBtn->setText(gSet.alarmOn ? "Alarm: on" : "Alarm: off"); alarmBtn->repaint();
  }
  void tick() {
    if (swRunning) showSw();
    time_t now = time(nullptr);
    std::string ts, ds;
    if (now < 1600000000) { ts = "--:--"; ds = "The clock sets itself once Wi-Fi or the browser connects."; }
    else {
      struct tm t; localtime_r(&now, &t);
      char b[48];
      snprintf(b, sizeof(b), "%d:%02d:%02d %s", t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12, t.tm_min, t.tm_sec, t.tm_hour < 12 ? "AM" : "PM");
      ts = b;
      static const char * DAY[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
      static const char * MON[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
      snprintf(b, sizeof(b), "%s, %s %d, %d", DAY[t.tm_wday % 7], MON[t.tm_mon % 12], t.tm_mday, t.tm_year + 1900);
      ds = b;
    }
    if (ts != lastTime) {
      lastTime = ts;
      timeLbl->setText(ts.c_str()); timeLbl->repaint();
      dateLbl->setText(ds.c_str()); dateLbl->repaint();
    }
  }
};
AppFrame * ClockApp::inst = nullptr;

void openClock()
{
  if (!ClockApp::inst) new ClockApp();
  ClockApp::inst->open();
}
