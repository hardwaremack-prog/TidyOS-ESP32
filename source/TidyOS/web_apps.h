// ====================================================================== HTML Games / HTML Programs
// Web games and programs kept on the SD card play in their own browser tab, served by the file server.
// (included at the end of apps.cpp)

class WebShelfApp : public AppFrame {
public:
  AppFrame ** instRef;
  std::string folder, title;
  std::vector<std::string> paths;       // what each list row opens
  uiListBox * list;
  uiLabel * status;

  WebShelfApp(const char * t, const char * sub, AppFrame ** ir) : AppFrame(t, nextPos(460, 380), Size(460, 380), ir), instRef(ir), folder(sub), title(t) {
    list = S(new uiListBox(this, Point(cx, cy), Size(300, 300)));
    list->items().takeStrings();          // keep copies: the names are built on the fly
    list->onDblClick = [this]() { play(); };
    int bx = cx + 310;
    button("Play", bx, cy, 130, 34)->onClick = [this]() { play(); };
    button("Refresh", bx, cy + 42, 130, 26)->onClick = [this]() { fill(); };
    button("Show folder", bx, cy + 74, 130, 26)->onClick = [this]() { openFilesAt(dir().c_str()); };
    static const char * HELP[5] = { "Put a .html file, or a", "folder with an index.html", "in it, into this folder on", "the SD card. Play opens", "it in a new browser tab." };
    for (int i = 0; i < 5; ++i) label(HELP[i], bx, cy + 110 + i * 15, 134, 15)->labelStyle().textFont = &FONT_std_12;
    status = label("", cx, cy + 308, 440, 18);
    status->labelStyle().textFont = &FONT_std_12;
    fill();
  }
  std::string dir() { return std::string(SD_MOUNT "/") + folder; }
  void say(const std::string & m) { status->setText(m.c_str()); status->repaint(); }
  void fill() {
    paths.clear();
    list->items().clear();
    if (!gSDMounted) { list->repaint(); say("Needs the SD card (turn it on in Switches)."); return; }
    mkdir(dir().c_str(), 0777);
    DIR * d = opendir(dir().c_str());
    if (!d) { list->repaint(); say("Couldn't open the folder."); return; }
    std::vector<std::pair<std::string, std::string>> found;   // name, path
    struct dirent * e;
    while ((e = readdir(d)) != nullptr) {
      if (e->d_name[0] == '.') continue;
      std::string p = joinPath(dir().c_str(), e->d_name);
      const char * x = fileExt(e->d_name);
      if (!strcmp(x, "html") || !strcmp(x, "htm")) {
        std::string n = e->d_name; n = n.substr(0, n.rfind('.'));
        found.push_back({ n, p });
      } else if (e->d_type == DT_DIR) {
        std::string idx = joinPath(p.c_str(), "index.html");
        FILE * f = fopen(idx.c_str(), "rb");
        if (f) { fclose(f); found.push_back({ std::string(e->d_name), idx }); }
      }
    }
    closedir(d);
    std::sort(found.begin(), found.end(), [](const std::pair<std::string, std::string> & a, const std::pair<std::string, std::string> & b) { return strcasecmp(a.first.c_str(), b.first.c_str()) < 0; });
    for (auto & f : found) { list->items().append(f.first.c_str()); paths.push_back(f.second); }
    list->repaint();
    char b[96];
    if (found.empty()) snprintf(b, sizeof(b), "Nothing here yet: copy some into SD:/%s", folder.c_str());
    else snprintf(b, sizeof(b), "%d in SD:/%s. Pick one and press Play.", (int)found.size(), folder.c_str());
    say(b);
  }
  void play() {
    int i = list->firstSelectedItem();
    if (i < 0 || i >= (int)paths.size()) { say("Pick one in the list first."); return; }
#ifdef TIDYOS_HTML5
    if (WiFi.status() != WL_CONNECTED) { say("This needs your home Wi-Fi."); return; }
    if (!screenViewer()) { say("Open the TidyOS page in a web browser first."); return; }
    fileServerMedia(true);
    screenMedia("o " + fileServerRunPath(paths[i]) + "\t" + list->items().get(i));
    say(std::string("Opening ") + list->items().get(i) + " in a new browser tab...");
#else
    say("Open " + fileServerUrl() + fileServerRunPath(paths[i]) + " in a web browser.");
#endif
  }
};

static AppFrame * s_htmlGames = nullptr, * s_htmlProgs = nullptr;
void openHtmlGames()    { if (!s_htmlGames) new WebShelfApp("HTML Games", "games", &s_htmlGames); s_htmlGames->open(); }
void openHtmlPrograms() { if (!s_htmlProgs) new WebShelfApp("HTML Programs", "programs", &s_htmlProgs); s_htmlProgs->open(); }
