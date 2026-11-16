#pragma once
// ====================================================================== Pictures + Music
// (included from apps.cpp)
//
// Pictures: shows JPEG and BMP files dithered to the screen's colours, like a classic Mac.
// The picture is decoded once into a cache file (one byte per pixel, on the SD card when
// it's on), and the window draws from that file - so a picture takes no RAM after loading.
// In the HTML5 edition "Full colour" opens the original in the browser.
//
// Music: the ESP32 has no speaker, so in the HTML5 edition the browser plays the music:
// TidyOS streams the file from the card and tells the page what to play.

#include "esp32/rom/tjpgd.h"
#include "screen.h"
#include "fileserver.h"

static const int PV_W = 616, PV_H = 368;            // picture area

static std::string pvCache() { return dataPath("view.raw"); }

static bool isPictureFile(const char * n)
{
  const char * e = fileExt(n);
  return !strcmp(e, "jpg") || !strcmp(e, "jpeg") || !strcmp(e, "bmp");
}
static bool isMusicFile(const char * n)
{
  const char * e = fileExt(n);
  return !strcmp(e, "mp3") || !strcmp(e, "m4a") || !strcmp(e, "wav") || !strcmp(e, "ogg") || !strcmp(e, "mid") || !strcmp(e, "midi");
}
static bool isMidiFile(const char * n) { const char * e = fileExt(n); return !strcmp(e, "mid") || !strcmp(e, "midi"); }

// a random song in dir (not cur); midiOnly: just .mid files. "" if there are none
static std::string randomSong(const std::string & dir, const std::string & cur, bool midiOnly)
{
  // one pass, keeping just one name (a big MIDI folder can hold thousands: listing them all ran out of memory)
  DIR * dd = opendir(dir.c_str());
  if (!dd) return "";
  struct dirent * e;
  char pick[128] = "", any[128] = "";
  uint32_t n = 0, scanned = 0, t0 = millis();
  while ((e = readdir(dd)) != nullptr && scanned < 4000 && millis() - t0 < 1500) {
    ++scanned;
    if (e->d_name[0] == '.' || !isMusicFile(e->d_name) || (midiOnly && !isMidiFile(e->d_name))) continue;
    if (!any[0]) strlcpy(any, e->d_name, sizeof(any));
    if (cur == e->d_name) continue;
    if (esp_random() % ++n == 0) strlcpy(pick, e->d_name, sizeof(pick));
    if ((scanned & 63) == 0) vTaskDelay(1);
  }
  closedir(dd);
  if (!pick[0]) strlcpy(pick, any, sizeof(pick));
  if (!pick[0]) return "";
  return joinPath(dir.c_str(), pick);
}

// the card's Music or MIDI folder (any capitalisation), or Documents
static std::string mediaFolder(bool midi)
{
  if (gSDMounted) {
    static const char * M[3] = { SD_MOUNT "/Music", SD_MOUNT "/music", SD_MOUNT "/MUSIC" };
    static const char * D[3] = { SD_MOUNT "/MIDI", SD_MOUNT "/Midi", SD_MOUNT "/midi" };
    for (int i = 0; i < 3; ++i) {
      DIR * t = opendir(midi ? D[i] : M[i]);
      if (t) { closedir(t); return midi ? D[i] : M[i]; }
    }
  }
  return documentsDir();
}

// ---- dithering to the running palette (ordered 4x4 Bayer)
struct Ditherer {
  int n;
  uint8_t pr[16], pg[16], pb[16], out[16];      // palette colours and their RGBA2222 bytes
  Ditherer() {
    n = gColors;
    for (int i = 0; i < n; ++i) {
      RGB888 c = themePal(i);
      pr[i] = c.R; pg[i] = c.G; pb[i] = c.B;
      out[i] = 0xC0 | ((c.B >> 6) << 4) | ((c.G >> 6) << 2) | (c.R >> 6);
    }
  }
  uint8_t px(int x, int y, int r, int g, int b) const {
    static const int8_t B4[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    int d = (B4[y & 3][x & 3] - 8) * (n <= 2 ? 16 : 6);
    r += d; g += d; b += d;
    int best = 0; long bd = 1L << 30;
    for (int i = 0; i < n; ++i) {
      long dr = r - pr[i], dg = g - pg[i], db = b - pb[i];
      long dd = dr * dr * 3 + dg * dg * 4 + db * db * 2;
      if (dd < bd) { bd = dd; best = i; }
    }
    return out[best];
  }
};

// ---- JPEG (tjpgd in the ESP32's ROM)
struct JpgJob {
  FILE * f;
  FILE * out;
  int w, h;
  uint8_t * stripe;          // w x 16 dithered pixels: one row of 8x8/16x16 blocks
  int stripeTop = -1, stripeRows = 0;
  Ditherer dz;
};
static UINT jpgIn(JDEC * jd, BYTE * buf, UINT n)
{
  JpgJob * j = (JpgJob *)jd->device;
  if (!buf) return fseek(j->f, n, SEEK_CUR) == 0 ? n : 0;
  return fread(buf, 1, n, j->f);
}
static UINT jpgOut(JDEC * jd, void * bitmap, JRECT * r)
{
  JpgJob * j = (JpgJob *)jd->device;
  if (j->stripeTop != r->top) {                      // a new row of blocks: write out the last one
    if (j->stripeTop >= 0) fwrite(j->stripe, 1, (size_t)j->w * j->stripeRows, j->out);
    j->stripeTop = r->top;
    j->stripeRows = 0;
    memset(j->stripe, 0xFF, (size_t)j->w * 16);
  }
  j->stripeRows = imax(j->stripeRows, imin(16, (int)r->bottom - j->stripeTop + 1));
  const uint8_t * p = (const uint8_t *)bitmap;
  for (int y = r->top; y <= r->bottom; ++y)
    for (int x = r->left; x <= r->right; ++x, p += 3)
      if (x < j->w && y - j->stripeTop < 16)
        j->stripe[(y - j->stripeTop) * j->w + x] = j->dz.px(x, y, p[0], p[1], p[2]);
  return 1;
}

// decodes a picture into the cache file; returns "" or an error message
static std::string pvDecode(const std::string & path, int & outW, int & outH, int & srcW, int & srcH)
{
  FILE * f = fopen(path.c_str(), "rb");
  if (!f) return "Can't open that file.";
  std::string err;
  const char * e = fileExt(path.c_str());
  if (!strcmp(e, "jpg") || !strcmp(e, "jpeg")) {
    void * work = malloc(3500);
    if (!work) { fclose(f); return "Not enough memory to open pictures."; }
    JpgJob j; j.f = f; j.out = nullptr; j.stripe = nullptr;
    JDEC jd;
    JRESULT res = jd_prepare(&jd, jpgIn, work, 3500, &j);
    if (res != JDR_OK) err = res == JDR_FMT3 ? "This JPEG type isn't supported (save it as a normal, not progressive, JPEG)." : "That JPEG couldn't be read.";
    else {
      srcW = jd.width; srcH = jd.height;
      int k = 0;                                   // decode at 1/1, 1/2, 1/4 or 1/8 size to fit
      while (k < 3 && ((int)(jd.width >> k) > PV_W || (int)(jd.height >> k) > PV_H)) ++k;
      j.w = jd.width >> k; j.h = jd.height >> k;  // (still too big at 1/8: the view shows the top-left)
      j.stripe = (uint8_t *)malloc((size_t)j.w * 16 + 16);
      j.out = fopen(pvCache().c_str(), "wb");
      if (!j.stripe || !j.out) err = !j.stripe ? "Not enough memory to open this picture." : "Can't write the picture cache.";
      else {
        uint16_t hd[2] = { (uint16_t)j.w, (uint16_t)j.h };
        fwrite(hd, 2, 2, j.out);
        res = jd_decomp(&jd, jpgOut, k);
        if (j.stripeTop >= 0) fwrite(j.stripe, 1, (size_t)j.w * j.stripeRows, j.out);
        if (res != JDR_OK) err = "That JPEG is damaged.";
        outW = j.w; outH = j.h;
      }
      if (j.out) fclose(j.out);
      free(j.stripe);
    }
    free(work);
  } else {                                           // BMP
    uint8_t h[54];
    if (fread(h, 1, 54, f) != 54 || h[0] != 'B' || h[1] != 'M') err = "That isn't a BMP picture.";
    else {
      auto g32 = [&](int o) { return (uint32_t)(h[o] | h[o + 1] << 8 | h[o + 2] << 16 | (uint32_t)h[o + 3] << 24); };
      uint32_t data = g32(10), hsz = g32(14), comp = g32(30);
      int w = (int)g32(18), hh = (int)g32(22), bpp = h[28] | h[29] << 8;
      bool topDown = hh < 0; if (topDown) hh = -hh;
      int rowBytes = ((w * bpp + 31) / 32) * 4;
      if ((bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24 && bpp != 32) || (comp != 0 && comp != 3) || w <= 0 || hh <= 0)
        err = "This kind of BMP isn't supported.";
      else if (rowBytes > 24000) err = "That picture is too wide.";
      else {
        uint8_t pal[256][4];
        int ncol = bpp <= 8 ? (g32(46) ? (int)g32(46) : 1 << bpp) : 0;
        if (ncol) { fseek(f, 14 + hsz, SEEK_SET); fread(pal, 4, imin(ncol, 256), f); }
        srcW = w; srcH = hh;
        float sc = fmaxf(1.0f, fmaxf((float)w / PV_W, (float)hh / PV_H));
        int ow = imax(1, (int)(w / sc)), oh = imax(1, (int)(hh / sc));
        uint8_t * row = (uint8_t *)malloc(rowBytes);
        uint8_t * line = (uint8_t *)malloc(ow);
        FILE * out = fopen(pvCache().c_str(), "wb");
        if (!row || !line || !out) err = "Not enough memory to open this picture.";
        else {
          uint16_t hd[2] = { (uint16_t)ow, (uint16_t)oh };
          fwrite(hd, 2, 2, out);
          Ditherer dz;
          for (int oy = 0; oy < oh; ++oy) {
            int sy = imin(hh - 1, (int)(oy * sc));
            int fy = topDown ? sy : hh - 1 - sy;
            fseek(f, data + (long)fy * rowBytes, SEEK_SET);
            fread(row, 1, rowBytes, f);
            for (int ox = 0; ox < ow; ++ox) {
              int sx = imin(w - 1, (int)(ox * sc));
              int r, g, b;
              if (bpp >= 24) { const uint8_t * q = row + sx * (bpp / 8); b = q[0]; g = q[1]; r = q[2]; }
              else {
                int idx = bpp == 8 ? row[sx] : bpp == 4 ? (row[sx >> 1] >> ((sx & 1) ? 0 : 4)) & 15 : (row[sx >> 3] >> (7 - (sx & 7))) & 1;
                b = pal[idx][0]; g = pal[idx][1]; r = pal[idx][2];
              }
              line[ox] = dz.px(ox, oy, r, g, b);
            }
            fwrite(line, 1, ow, out);
          }
          outW = ow; outH = oh;
        }
        if (out) fclose(out);
        free(row); free(line);
      }
    }
  }
  fclose(f);
  return err;
}

class PictureView : public uiControl {
public:
  int w = 0, h = 0;          // picture in the cache (0 = none)
  PictureView(uiWindow * p, Point pos) : uiControl(p, pos, Size(PV_W + 2, PV_H + 2), true, 0) {
    windowStyle().borderSize = 1;
    windowStyle().borderColor = T.shadow;
    windowStyle().activeBorderColor = T.shadow;
    windowStyle().focusedBorderColor = T.shadow;
  }
  void processEvent(uiEvent * e) override {
    uiControl::processEvent(e);
    if (e->id != UIEVT_PAINT) return;
    Rect cr = clientRect(uiOrigin::Window);
    beginPaint(e, cr);
    Rect clip = Rect(e->params.rect.X1, e->params.rect.Y1, e->params.rect.X2, e->params.rect.Y2).intersection(cr);
    auto cv = canvas();
    cv->setBrushColor(T.win);
    cv->fillRectangle(clip);
    if (w <= 0 || h <= 0) {
      cv->setPenColor(T.shadow);
      cv->drawText(&FONT_std_14, cr.X1 + 20, cr.Y1 + 20, "Open a .jpg or .bmp picture.");
      return;
    }
    int dw = imin(w, PV_W), dh = imin(h, PV_H);
    int x0 = cr.X1 + (PV_W - dw) / 2, y0 = cr.Y1 + (PV_H - dh) / 2;
    FILE * f = fopen(pvCache().c_str(), "rb");
    if (!f) return;
    const int RS = 4;                                // rows per strip (w <= 616: ~2.5 KB)
    uint8_t * strip = (uint8_t *)malloc((size_t)dw * RS);
    if (!strip) { fclose(f); return; }
    int ya = imax(0, clip.Y1 - y0), yb = imin(dh - 1, clip.Y2 - y0);
    for (int y = ya; y <= yb; y += RS) {
      int n = imin(RS, yb - y + 1);
      for (int i = 0; i < n; ++i) {
        fseek(f, 4 + (long)(y + i) * w, SEEK_SET);
        if (fread(strip + i * dw, 1, dw, f) != (size_t)dw) memset(strip + i * dw, 0xFF, dw);
      }
      Bitmap bm(dw, n, strip, PixelFormat::RGBA2222);
      cv->drawBitmap(x0, y0 + y, &bm);
      cv->waitCompletion();
    }
    free(strip);
    fclose(f);
  }
};

class PicturesApp : public AppFrame {
public:
  static AppFrame * inst;
  PictureView * view;
  uiLabel * info;
  std::string path;

  PicturesApp() : AppFrame("Pictures", Point(4, 4), Size(632, 452), &inst) {
    int x = cx;
    auto b = [&](const char * t, int w, std::function<void()> f) { auto bt = button(t, x, cy, w, 24); bt->onClick = f; x += w + 4; return bt; };
    b("Open", 54, [this]() { std::string p = askFile("Open picture", "Open", "", path.empty() ? documentsDir() : dirOf(path)); if (!p.empty()) load(p); });
    b("< Prev", 60, [this]() { step(-1); });
    b("Next >", 60, [this]() { step(1); });
#ifdef TIDYOS_HTML5
    b("Full colour", 90, [this]() {
      if (path.empty()) return;
      if (WiFi.status() != WL_CONNECTED) { gApp->messageBox("Pictures", "Full colour needs your home Wi-Fi.", "OK", nullptr, nullptr, uiMessageBoxIcon::Info); return; }
      fileServerMedia(true);
      screenMedia("i " + fileServerMediaPath(path));
    });
#endif
    info = label("", x + 6, cy + 4, cx + 610 - x - 6, 16);
    info->labelStyle().textFont = &FONT_std_12;
    view = new PictureView(this, Point(cx, cy + 30));
  }

  void load(const std::string & p) {
    info->setText("Opening..."); info->repaint();
    int w = 0, h = 0, sw = 0, sh = 0;
    std::string err = pvDecode(p, w, h, sw, sh);
    if (!err.empty()) { view->w = view->h = 0; view->repaint(); info->setText(err.c_str()); info->repaint(); return; }
    path = p;
    view->w = w; view->h = h;
    view->repaint();
    char b[120];
    snprintf(b, sizeof(b), "%.50s   %d x %d%s", baseName(p).c_str(), sw, sh, (w < sw) ? "  (shrunk to fit)" : "");
    info->setText(b); info->repaint();
    setTitle(("Pictures - " + baseName(p)).c_str());
  }

  // previous / next picture in the same folder
  void step(int d) {
    if (path.empty()) return;
    std::string dir = dirOf(path), cur = baseName(path), prev, next, first, last;
    bool seen = false;
    DIR * dd = opendir(dir.c_str());
    if (!dd) return;
    struct dirent * e;
    while ((e = readdir(dd)) != nullptr) {
      if (e->d_name[0] == '.' || !isPictureFile(e->d_name)) continue;
      std::string n = e->d_name;
      if (first.empty()) first = n;
      last = n;
      if (n == cur) { seen = true; continue; }
      if (!seen) prev = n; else if (next.empty()) next = n;
    }
    closedir(dd);
    std::string to = d < 0 ? (prev.empty() ? last : prev) : (next.empty() ? first : next);
    if (!to.empty() && to != cur) load(joinPath(dir.c_str(), to.c_str()));
  }

  void onClosing() override { remove(pvCache().c_str()); }
};
AppFrame * PicturesApp::inst = nullptr;

void openPictures(const char * file)
{
  if (!PicturesApp::inst) new PicturesApp();
  PicturesApp::inst->open();
  if (file) ((PicturesApp *)PicturesApp::inst)->load(file);
}

// ---------------------------------------------------------------- Music

static bool s_musicShuffle = false;

class MusicApp : public AppFrame {
public:
  static AppFrame * inst;
  FileList * list;
  uiLabel * now, * timeLbl;
  uiButton * playBtn;
  std::string playing;          // full path ("" = nothing)
  float vol = 0.8f;
  bool paused = false;
  int lastState = -1;

  uiButton * tabMp3, * tabMidi;
  uiProgressBar * volBar;
  uiButton * shuffleBtn;
  void showTab(bool midi) {
    list->setDir(mediaFolder(midi));
    tabMp3->setText(midi ? "MP3" : "[ MP3 ]"); tabMidi->setText(midi ? "[ MIDI ]" : "MIDI");
    tabMp3->repaint(); tabMidi->repaint();
  }

  MusicApp() : AppFrame("Music", nextPos(470, 410), Size(470, 410), &inst) {
    tabMp3 = button("[ MP3 ]", cx, cy, 80, 24);
    tabMidi = button("MIDI", cx + 84, cy, 80, 24);
    tabMp3->onClick = [this]() { showTab(false); };
    tabMidi->onClick = [this]() { showTab(true); };
    cy += 30;
    list = new FileList(this, Point(cx, cy), Size(330, FileList::RH * 15 + 2));
    list->onPick = [this](const std::string & nm, bool isDir, bool) {
      if (isDir) {
        std::string d = list->dir;
        if (nm == "..") { size_t p = d.rfind('/'); if (p > 0) d = d.substr(0, p); }
        else d = joinPath(d.c_str(), nm.c_str());
        list->setDir(d);
        return;
      }
      if (isMusicFile(nm.c_str())) play(joinPath(list->dir.c_str(), nm.c_str()));
    };
    list->setDir(mediaFolder(false));
    int bx = cx + 338;
    playBtn = button("Play", bx, cy, 110, 26);
    playBtn->onClick = [this]() { playPause(); };
    button("Stop", bx, cy + 30, 110, 26)->onClick = [this]() { stop(); };
    button("< Prev", bx, cy + 64, 53, 26)->onClick = [this]() { skip(-1); };
    button("Next >", bx + 57, cy + 64, 53, 26)->onClick = [this]() { skip(1); };
    button("Vol -", bx, cy + 98, 53, 26)->onClick = [this]() { setVol(vol - 0.1f); };
    button("Vol +", bx + 57, cy + 98, 53, 26)->onClick = [this]() { setVol(vol + 0.1f); };
    volBar = S(new uiProgressBar(this, Point(bx, cy + 127), Size(110, 9)));
    volBar->progressBarProps().showPercentage = false;
    volBar->progressBarStyle().backgroundColor = T.win;
    volBar->progressBarStyle().foregroundColor = T.accent;
    volBar->windowStyle().borderColor = T.text;
    volBar->setPercentage((int)(vol * 100 + 0.5f));
    button("Scroll up", bx, cy + 140, 110, 22)->onClick = [this]() { list->scroll(-10); };
    button("Scroll down", bx, cy + 166, 110, 22)->onClick = [this]() { list->scroll(10); };
    shuffleBtn = button(s_musicShuffle ? "Shuffle: on" : "Shuffle: off", bx, cy + 196, 110, 26);
    shuffleBtn->onClick = [this]() { s_musicShuffle = !s_musicShuffle; shuffleBtn->setText(s_musicShuffle ? "Shuffle: on" : "Shuffle: off"); shuffleBtn->repaint(); };
    now = label("Click a song to play it.", cx, cy + FileList::RH * 15 + 10, 440, 18);
    timeLbl = label("", cx, cy + FileList::RH * 15 + 30, 440, 18);
    timeLbl->labelStyle().textFont = &FONT_std_12;
    startTimer(1000);
    onTimer = [this](uiTimerHandle) { tick(); };
    if (WiFi.status() == WL_CONNECTED) fileServerMedia(true);   // listening before the first song is asked for
  }

  void play(const std::string & p) {
    if (WiFi.status() != WL_CONNECTED) { gApp->messageBox("Music", "Music plays over your home Wi-Fi - join it in Settings.", "OK", nullptr, nullptr, uiMessageBoxIcon::Info); return; }
    fileServerMedia(true);
    playing = p; paused = false;
    lastState = 2;           // ignore "ended" until the browser has started this song (its last report may be the old song's)
    char v[24]; snprintf(v, sizeof(v), "v %.2f", vol);
    screenMedia(v);
    screenMedia((isMidiFile(p.c_str()) ? "m " : "a ") + fileServerMediaPath(p));
    now->setText(("Playing: " + baseName(p)).c_str()); now->repaint();
    playBtn->setText("Pause"); playBtn->repaint();
  }
  void playPause() {
    if (playing.empty()) {          // nothing yet: play the first song in the folder
      skip(1);
      return;
    }
    paused = !paused;
    screenMedia(paused ? "p" : "r");
    playBtn->setText(paused ? "Play" : "Pause"); playBtn->repaint();
  }
  void stop() {
    if (playing.empty()) return;
    screenMedia("s");
    playing.clear(); paused = false;
    now->setText("Stopped."); now->repaint();
    timeLbl->setText(""); timeLbl->repaint();
    playBtn->setText("Play"); playBtn->repaint();
  }
  void setVol(float v) {
    vol = fmaxf(0, fminf(1, v));
    volBar->setPercentage((int)(vol * 100 + 0.5f));
    char b[24]; snprintf(b, sizeof(b), "v %.2f", vol);
    screenMedia(b);
  }
  void skip(int d) {
    std::string dir = playing.empty() ? list->dir : dirOf(playing), cur = playing.empty() ? "" : baseName(playing);
    if (s_musicShuffle && d > 0) {                       // shuffle: any other song in the folder
      std::string p = randomSong(dir, cur, false);
      if (p.empty()) { now->setText("No music files in this folder."); now->repaint(); return; }
      play(p);
      return;
    }
    std::string prev, next, first, last;
    bool seen = false;
    DIR * dd = opendir(dir.c_str());
    if (!dd) return;
    struct dirent * e;
    while ((e = readdir(dd)) != nullptr) {
      if (e->d_name[0] == '.' || !isMusicFile(e->d_name)) continue;
      std::string n = e->d_name;
      if (first.empty()) first = n;
      last = n;
      if (n == cur) { seen = true; continue; }
      if (!seen) prev = n; else if (next.empty()) next = n;
    }
    closedir(dd);
    std::string to = cur.empty() ? first : d < 0 ? (prev.empty() ? last : prev) : (next.empty() ? first : next);
    if (to.empty()) { now->setText("No music files in this folder."); now->repaint(); return; }
    play(joinPath(dir.c_str(), to.c_str()));
  }
  void tick() {
    if (playing.empty()) return;
    int st, pos, dur;
    screenMediaState(st, pos, dur);
    char b[64];
    if (!screenViewer()) snprintf(b, sizeof(b), "The browser isn't connected - music stops with it.");
    else snprintf(b, sizeof(b), "%d:%02d / %d:%02d   volume %d%%", pos / 60, pos % 60, dur / 60, dur % 60, (int)(vol * 100 + 0.5f));
    timeLbl->setText(b); timeLbl->repaint();
    if (st == 2 && lastState != 2) skip(1);       // song ended: next one
    lastState = st;
  }
  void onClosing() override {
    if (!playing.empty()) screenMedia("s");
    fileServerMedia(false);
  }
};
AppFrame * MusicApp::inst = nullptr;

void openMusic()
{
#ifndef TIDYOS_HTML5
  gApp->messageBox("Music", "Music plays through the web browser in the HTML5 edition.", "OK", nullptr, nullptr, uiMessageBoxIcon::Info);
  return;
#endif
  if (!MusicApp::inst) new MusicApp();
  MusicApp::inst->open();
}

// ---- music while you play a game (Sammy's Play music / Pause buttons). Uses the Music window when it's open.
static std::string s_qPlaying;
static bool s_qPaused = false;
static int  s_qLast = 2;

static float s_qVol = 0.8f;
static void quickPlayFile(const std::string & p)
{
  fileServerMedia(true);
  s_qPlaying = p; s_qPaused = false; s_qLast = 2;
  char v[24]; snprintf(v, sizeof(v), "v %.2f", s_qVol);
  screenMedia(v);
  screenMedia((isMidiFile(p.c_str()) ? "m " : "a ") + fileServerMediaPath(p));
}

// returns a line for the game's info bar
std::string musicQuickPlay()
{
  if (MusicApp::inst) {
    auto m = (MusicApp *)MusicApp::inst;
    if (m->playing.empty() || m->paused) m->playPause();
    return m->playing.empty() ? "No music files found." : "Music: " + baseName(m->playing);
  }
  if (WiFi.status() != WL_CONNECTED) return "Music needs your home Wi-Fi (see Settings).";
  if (!screenViewer()) return "Music plays in the web browser.";
  if (!s_qPlaying.empty() && s_qPaused) { screenMedia("r"); s_qPaused = false; return "Music: " + baseName(s_qPlaying); }
  if (!s_qPlaying.empty()) return "Music: " + baseName(s_qPlaying);
  std::string p = randomSong(mediaFolder(true), "", true);          // Sammy plays the MIDI folder, shuffled
  if (p.empty()) return "No .mid files in the MIDI folder on the card.";
  quickPlayFile(p);
  return "Music: " + baseName(p);
}

void musicQuickPause()
{
  if (MusicApp::inst) { auto m = (MusicApp *)MusicApp::inst; if (!m->playing.empty() && !m->paused) m->playPause(); return; }
  if (!s_qPlaying.empty() && !s_qPaused) { screenMedia("p"); s_qPaused = true; }
}

// the games' music bar: MP3 / MIDI (play or pause), Next, volume
std::string musicQuickToggle(bool midi)
{
  if (MusicApp::inst) {
    auto m = (MusicApp *)MusicApp::inst;
    m->playPause();
    return m->playing.empty() ? "No music files found." : (m->paused ? "Music paused." : "Music: " + baseName(m->playing));
  }
  if (WiFi.status() != WL_CONNECTED && !screenViewer()) return "Music plays in the web browser.";
  if (!screenViewer()) return "Music plays in the web browser.";
  bool same = !s_qPlaying.empty() && isMidiFile(s_qPlaying.c_str()) == midi;
  if (same && !s_qPaused) { screenMedia("p"); s_qPaused = true; return "Music paused."; }
  if (same && s_qPaused)  { screenMedia("r"); s_qPaused = false; return "Music: " + baseName(s_qPlaying); }
  std::string p = randomSong(mediaFolder(midi), "", midi);
  if (p.empty()) return midi ? "No .mid files in the MIDI folder on the card." : "No music in the Music folder on the card.";
  quickPlayFile(p);
  return "Music: " + baseName(p);
}

std::string musicQuickNext()
{
  if (MusicApp::inst) { auto m = (MusicApp *)MusicApp::inst; m->skip(1); return m->playing.empty() ? "" : "Music: " + baseName(m->playing); }
  if (s_qPlaying.empty()) return "Press MP3 or MIDI first.";
  std::string n = randomSong(dirOf(s_qPlaying), baseName(s_qPlaying), isMidiFile(s_qPlaying.c_str()));
  if (n.empty()) return "No other songs there.";
  quickPlayFile(n);
  return "Music: " + baseName(n);
}

std::string musicQuickVol(int d)
{
  if (MusicApp::inst) { auto m = (MusicApp *)MusicApp::inst; m->setVol(m->vol + d * 0.1f); s_qVol = m->vol; }
  else {
    s_qVol = fmaxf(0, fminf(1, s_qVol + d * 0.1f));
    char v[24]; snprintf(v, sizeof(v), "v %.2f", s_qVol);
    screenMedia(v);
  }
  char b[32]; snprintf(b, sizeof(b), "Volume %d%%", (int)(s_qVol * 100 + 0.5f));
  return b;
}

bool musicQuickPlaying()          // for Sammy's one music button
{
  if (MusicApp::inst) { auto m = (MusicApp *)MusicApp::inst; return !m->playing.empty() && !m->paused; }
  return !s_qPlaying.empty() && !s_qPaused;
}

void musicQuickTick()             // once a second: on to the next song when one ends
{
  if (MusicApp::inst || s_qPlaying.empty() || s_qPaused) return;
  int st, pos, dur;
  screenMediaState(st, pos, dur);
  if (st == 2 && s_qLast != 2) { std::string n = randomSong(dirOf(s_qPlaying), baseName(s_qPlaying), true); if (!n.empty()) quickPlayFile(n); return; }
  s_qLast = st;
}

void musicQuickStop()
{
  if (MusicApp::inst || s_qPlaying.empty()) return;
  screenMedia("s");
  s_qPlaying.clear(); s_qPaused = false;
  fileServerMedia(false);
}

void openMusicFile(const char * file)
{
  openMusic();
  if (MusicApp::inst) ((MusicApp *)MusicApp::inst)->play(file);
}
