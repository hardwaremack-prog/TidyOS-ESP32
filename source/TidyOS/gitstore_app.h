// ====================================================================== Git-Store
// Cloud storage for boards (handy without an SD card): each board gets its own folder,
// boards/TidyOS-XXXXXX, in a GitHub repo, through a small Cloudflare relay that holds the
// GitHub token and keeps each board to 5 MB. The browser page does the transfers, so the
// ESP32 never needs a TLS connection. (included at the end of apps.cpp)

static portMUX_TYPE s_storeMux = portMUX_INITIALIZER_UNLOCKED;
static char s_storeReply[1100];
static volatile int s_storeVer = 0;

void storeFromPage(const char * t)            // called on the screen server's task
{
  portENTER_CRITICAL(&s_storeMux);
  strlcpy(s_storeReply, t, sizeof(s_storeReply));
  s_storeVer++;
  portEXIT_CRITICAL(&s_storeMux);
}

std::string boardId()
{
  uint64_t mac = ESP.getEfuseMac();           // the factory MAC: bytes 3-5 make the board's number
  uint8_t * m = (uint8_t *)&mac;
  char b[16];
  snprintf(b, sizeof(b), "TidyOS-%02X%02X%02X", m[3], m[4], m[5]);
  return b;
}

class GitStoreApp : public AppFrame {
public:
  static AppFrame * inst;
  uiListBox * list;
  uiLabel * status, * usage, * idLbl;
  uiProgressBar * bar;
  uiTextEdit * relay;
  std::vector<std::pair<std::string, long>> files;
  int seenVer = 0;
  char busy = 0;                              // the op we're waiting for ('L', 'U', 'D', 'X')
  uint32_t busySince = 0;

  GitStoreApp() : AppFrame("Git-Store", nextPos(520, 400), Size(520, 400), &inst) {
    idLbl = label(("This board's folder: " + boardId()).c_str(), cx, cy, 380, 18);
    usage = label("", cx, cy + 22, 250, 16);
    usage->labelStyle().textFont = &FONT_std_12;
    bar = S(new uiProgressBar(this, Point(cx + 256, cy + 24), Size(244, 10)));
    bar->progressBarProps().showPercentage = false;
    bar->progressBarStyle().backgroundColor = T.win;
    bar->progressBarStyle().foregroundColor = T.accent;
    list = S(new uiListBox(this, Point(cx, cy + 42), Size(340, 250)));
    list->items().takeStrings();
    int bx = cx + 350;
    button("Refresh", bx, cy + 42, 150, 28)->onClick = [this]() { refresh(); };
    button("Upload a file...", bx, cy + 76, 150, 28)->onClick = [this]() { upload(); };
    button("Download", bx, cy + 110, 150, 28)->onClick = [this]() { download(); };
    button("Delete", bx, cy + 144, 150, 28)->onClick = [this]() { del(); };
    label("Downloads go to Documents.", bx, cy + 180, 150, 14)->labelStyle().textFont = &FONT_std_12;
    label("Up to 2 MB a file, 5 MB in all.", bx, cy + 196, 150, 14)->labelStyle().textFont = &FONT_std_12;
    label("Relay", cx, cy + 302, 44, 18);
    relay = edit(url().c_str(), cx + 46, cy + 298, 340);
    button("Save", cx + 392, cy + 298, 54, 24)->onClick = [this]() {
      std::string u = relay->text() ? relay->text() : "";
      while (!u.empty() && (u.back() == '/' || u.back() == ' ')) u.pop_back();
      gSet.storeUrl = u.c_str(); settingsSave();
      say("Saved."); refresh();
    };
    status = label("", cx, cy + 330, 500, 18);
    status->labelStyle().textFont = &FONT_std_12;
    onTimer = [this](uiTimerHandle) { poll(); };
    startTimer(250);
    seenVer = s_storeVer;
    refresh();
  }

  void say(const std::string & m) { status->setText(m.c_str()); status->repaint(); }
  std::string url() { return gSet.storeUrl.length() ? std::string(gSet.storeUrl.c_str()) : std::string(TIDYOS_STORE_URL); }
  bool ready() {
#ifndef TIDYOS_HTML5
    say("Git-Store works in the browser edition.");
    return false;
#endif
    if (busy) { say("Still working on the last one..."); return false; }
    if (url().compare(0, 8, "https://") != 0) { say("Type the relay address (https://...workers.dev) and press Save."); return false; }
    if (WiFi.status() != WL_CONNECTED) { say("Needs your home Wi-Fi."); return false; }
    if (!screenViewer()) { say("Needs the TidyOS page open in a web browser (it does the transfers)."); return false; }
    return true;
  }
  void go(char op, const std::string & args, const char * msg) {
    fileServerMedia(true);
    busy = op; busySince = millis();
    screenMedia(std::string("g ") + op + "\t" + url() + "\t" + boardId() + args);
    say(msg);
  }
  void refresh() { if (ready()) go('L', "", "Looking in the store..."); }

  static std::string cleanName(std::string n) {
    for (auto & c : n) if (!(isalnum((unsigned char)c) || c == ' ' || c == '.' || c == '_' || c == '-' || c == '(' || c == ')')) c = '_';
    if (n.size() > 80) n = n.substr(n.size() - 80);
    if (!n.empty() && n[0] == '.') n[0] = '_';
    return n;
  }
  void upload() {
    if (!ready()) return;
    std::string p = askFile("Upload to Git-Store", "Upload", "", documentsDir());
    if (p.empty()) return;
    long sz = fileSize(p.c_str());
    if (sz < 0) { say("Couldn't read that file."); return; }
    if (sz > 2L * 1024 * 1024) { say("That file is over 2 MB."); return; }
    go('U', "\t" + fileServerMediaPath(p) + "\t" + cleanName(baseName(p)), ("Uploading " + baseName(p) + "...").c_str());
  }
  int picked() { int i = list->firstSelectedItem(); return i >= 0 && i < (int)files.size() ? i : -1; }
  void download() {
    int i = picked();
    if (i < 0) { say("Pick a file in the list first."); return; }
    if (!ready()) return;
    std::string dest = joinPath(documentsDir().c_str(), files[i].first.c_str());
    if (fileSize(dest.c_str()) >= 0 &&
        gApp->messageBox("Git-Store", ("Replace " + files[i].first + " in Documents?").c_str(), "Replace", "Cancel", nullptr, uiMessageBoxIcon::Question) != uiMessageBoxResult::Button1) return;
    go('D', "\t" + files[i].first + "\t" + fileServerMediaPath(dest), ("Downloading " + files[i].first + "...").c_str());
  }
  void del() {
    int i = picked();
    if (i < 0) { say("Pick a file in the list first."); return; }
    if (!ready()) return;
    if (gApp->messageBox("Git-Store", ("Delete " + files[i].first + " from the store?").c_str(), "Delete", "Cancel", nullptr, uiMessageBoxIcon::Warning) != uiMessageBoxResult::Button1) return;
    go('X', "\t" + files[i].first, "Deleting...");
  }
  static std::string kb(long n) { char b[24]; if (n >= 1048576) snprintf(b, sizeof(b), "%.2f MB", n / 1048576.0); else snprintf(b, sizeof(b), "%.1f KB", n / 1024.0); return b; }

  void poll() {
    if (busy && millis() - busySince > 60000) { busy = 0; say("No answer from the page - is the browser still open?"); }
    if (s_storeVer == seenVer) return;
    char r[1100];
    portENTER_CRITICAL(&s_storeMux);
    strlcpy(r, s_storeReply, sizeof(r));
    seenVer = s_storeVer;
    portEXIT_CRITICAL(&s_storeMux);
    char op = r[0];
    busy = 0;
    const char * t = strchr(r, '\t');
    if (!t) return;
    ++t;
    bool ok = strncmp(t, "ok", 2) == 0;
    if (!ok) { std::string e = t; size_t nl = e.find('\n'); say("Didn't work: " + e.substr(0, nl)); return; }
    if (op == 'L') {
      long used = 0, mx = 5L * 1024 * 1024;
      sscanf(t, "ok\t%ld\t%ld", &used, &mx);
      files.clear();
      list->items().clear();
      const char * line = strchr(t, '\n');
      while (line) {
        ++line;
        const char * end = strchr(line, '\n');
        std::string row = end ? std::string(line, end) : std::string(line);
        size_t tab = row.find('\t');
        if (tab != std::string::npos) {
          files.push_back({ row.substr(0, tab), atol(row.c_str() + tab + 1) });
          std::string shown = row.substr(0, tab) + "   (" + kb(atol(row.c_str() + tab + 1)) + ")";
          list->items().append(shown.c_str());
        }
        line = end;
      }
      list->repaint();
      usage->setText(("Used " + kb(used) + " of " + kb(mx)).c_str()); usage->repaint();
      bar->setPercentage(mx > 0 ? (int)(used * 100 / mx) : 0);
      say(files.empty() ? "The store is empty. Upload a file to start." : "Up to date.");
    } else if (op == 'U') { say("Uploaded."); refresh(); }
    else if (op == 'D') { say("Downloaded to Documents."); }
    else if (op == 'X') { say("Deleted."); refresh(); }
  }
};
AppFrame * GitStoreApp::inst = nullptr;

void openGitStore()
{
  if (!GitStoreApp::inst) new GitStoreApp();
  GitStoreApp::inst->open();
}
