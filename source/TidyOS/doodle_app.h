#pragma once
// ====================================================================== Doodle
// A little MacPaint-style drawing app. The picture lives in a 1-bit .bmp file (on the SD card
// when it's on, otherwise internal storage); the screen is the working copy. Strokes are drawn
// straight onto the screen and written to the file when the mouse button comes up, so the
// picture never needs a copy in RAM. Undo keeps the changed rows in a file too.
// (included from apps.cpp)

static const int DW = 480, DH = 320;            // picture size in pixels
static const int DROW = DW / 8;                 // 60 bytes per row (already a multiple of 4, as BMP needs)
static const int BMP_HDR = 62;

static std::string doodleWork() { return dataPath("doodle.bmp"); }
static std::string doodleUndo() { return dataPath("doodle-undo.bin"); }
static long dRowOff(int y) { return BMP_HDR + (long)(DH - 1 - y) * DROW; }

static void put16(uint8_t * p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t * p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get32(const uint8_t * p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void bmpHeader(uint8_t * h)
{
  memset(h, 0, BMP_HDR);
  h[0] = 'B'; h[1] = 'M';
  put32(h + 2, BMP_HDR + DROW * DH); put32(h + 10, BMP_HDR); put32(h + 14, 40);
  put32(h + 18, DW); put32(h + 22, DH); put16(h + 26, 1); put16(h + 28, 1);
  put32(h + 34, DROW * DH); put32(h + 38, 2835); put32(h + 42, 2835); put32(h + 46, 2);
  h[54] = h[55] = h[56] = 255;                  // colour 0 = white, colour 1 = black (a set bit is ink)
}

static bool doodleBlank(const std::string & path)
{
  return writeFileWith(path.c_str(), [](RSink & o) {
    uint8_t h[BMP_HDR]; bmpHeader(h);
    o.write((const char *)h, BMP_HDR);
    char z[DROW]; memset(z, 0, sizeof(z));
    for (int y = 0; y < DH; ++y) o.write(z, DROW);
  });
}

// framebuffer access (palette index 0 = ink, 1 = paper)
static inline int fbGet(int x, int y)
{
  const uint8_t * r = (const uint8_t *)fabgl::VGAPalettedController::sgetScanline(y);
  if (gColors == 8) { const uint8_t * q = r + (x >> 3) * 3; uint32_t v = q[0] | q[1] << 8 | q[2] << 16; return v >> (21 - 3 * (x & 7)) & 7; }
  return r[x >> 2] >> (6 - 2 * (x & 3)) & 3;
}
static inline void fbSet(int x, int y, int idx)
{
  uint8_t * r = (uint8_t *)fabgl::VGAPalettedController::sgetScanline(y);
  if (gColors == 8) {
    uint8_t * q = r + (x >> 3) * 3; uint32_t v = q[0] | q[1] << 8 | q[2] << 16;
    int sh = 21 - 3 * (x & 7);
    v = (v & ~(7u << sh)) | ((uint32_t)idx << sh);
    q[0] = v; q[1] = v >> 8; q[2] = v >> 16;
  } else {
    int sh = 6 - 2 * (x & 3);
    r[x >> 2] = (r[x >> 2] & ~(3 << sh)) | (idx << sh);
  }
}

class DoodleCanvas : public uiControl {
public:
  enum Tool { PENCIL, BRUSH, ERASER, SPRAY, LINE, BOX, FILLBOX, OVAL, FILLOVAL, FILL, TEXT, NTOOLS };
  int  tool = PENCIL, size = 4;
  bool ink = true;                      // true = black, false = white
  std::function<void()> onChanged;

  DoodleCanvas(uiWindow * parent, const Point & pos)
    : uiControl(parent, pos, Size(DW + 2, DH + 2), true, 0)
  {
    windowStyle().borderSize = 1;
    windowStyle().borderColor = T.shadow;
    windowStyle().activeBorderColor = T.shadow;
    windowStyle().focusedBorderColor = T.shadow;
  }

  void reload() { repaint(); }

  // undo: swaps the rows kept in the undo file with the picture (so undo again = redo)
  bool undo() {
    FILE * u = fopen(doodleUndo().c_str(), "rb");
    if (!u) return false;
    int16_t b[4];
    if (fread(b, 2, 4, u) != 4) { fclose(u); return false; }
    std::string tmp = doodleUndo() + ".t";
    FILE * t = fopen(tmp.c_str(), "wb");
    FILE * p = fopen(doodleWork().c_str(), "r+b");
    if (!t || !p) { fclose(u); if (t) fclose(t); if (p) fclose(p); return false; }
    fwrite(b, 2, 4, t);
    int bx0 = b[0] >> 3, bn = (b[2] >> 3) - bx0 + 1;
    uint8_t oldr[DROW], cur[DROW];
    for (int y = b[1]; y <= b[3]; ++y) {
      if (fread(oldr, 1, bn, u) != (size_t)bn) break;
      fseek(p, dRowOff(y) + bx0, SEEK_SET); fread(cur, 1, bn, p);
      fwrite(cur, 1, bn, t);
      fseek(p, dRowOff(y) + bx0, SEEK_SET); fwrite(oldr, 1, bn, p);
    }
    fclose(u); fclose(t); fclose(p);
    remove(doodleUndo().c_str());
    rename(tmp.c_str(), doodleUndo().c_str());
    Rect cr = clientRect(uiOrigin::Window);
    repaint(Rect(cr.X1 + b[0], cr.Y1 + b[1], cr.X1 + b[2], cr.Y1 + b[3]));
    return true;
  }

  // keep the whole picture for undo, then run f (which changes the file); used by Clear, Open
  void wholeChange(const std::function<void()> & f) {
    saveUndo(0, 0, DW - 1, DH - 1);
    f();
    repaint();
    if (onChanged) onChanged();
  }

  void processEvent(uiEvent * e) override {
    uiControl::processEvent(e);
    switch (e->id) {
      case UIEVT_PAINT: {
        Rect cr = clientRect(uiOrigin::Window);
        beginPaint(e, cr);
        paint(cr, Rect(e->params.rect.X1, e->params.rect.Y1, e->params.rect.X2, e->params.rect.Y2).intersection(cr));
        break;
      }
      case UIEVT_MOUSEBUTTONDOWN:
        if (e->params.mouse.changedButton == 1) down(e->params.mouse.status.X, e->params.mouse.status.Y);
        break;
      case UIEVT_MOUSEMOVE:
        if (m_drag) move(e->params.mouse.status.X, e->params.mouse.status.Y);
        break;
      case UIEVT_MOUSEBUTTONUP:
        if (m_drag && e->params.mouse.changedButton == 1) up(e->params.mouse.status.X, e->params.mouse.status.Y);
        break;
      case UIEVT_TIMER:
        if (m_textTimer) { app()->killTimer(m_textTimer); m_textTimer = nullptr; placeText(); }
        break;
      default:
        break;
    }
  }

private:
  bool m_drag = false, m_preview = false;
  Point m_start, m_last, m_prevEnd;
  int  m_bx0, m_by0, m_bx1, m_by1;          // area touched by this stroke (picture coords)
  uiTimerHandle m_textTimer = nullptr;
  Point m_textAt;
  std::string m_text;

  void paint(Rect const & cr, Rect const & clip) {
    if (clip.X1 > clip.X2 || clip.Y1 > clip.Y2) return;
    auto cv = canvas();
    cv->setBrushColor(T.win);
    cv->fillRectangle(clip);
    FILE * f = fopen(doodleWork().c_str(), "rb");
    if (!f) return;
    static uint8_t strip[16 * DROW];           // 960 bytes, reused for every strip
    int y0 = clip.Y1 - cr.Y1, y1 = clip.Y2 - cr.Y1;
    for (int y = y0; y <= y1; y += 16) {
      int n = imin(16, y1 - y + 1);
      for (int i = 0; i < n; ++i) {
        fseek(f, dRowOff(y + i), SEEK_SET);
        if (fread(strip + i * DROW, 1, DROW, f) != DROW) memset(strip + i * DROW, 0, DROW);
      }
      Bitmap bm(DW, n, strip, PixelFormat::Mask, T.text);
      cv->drawBitmap(cr.X1, cr.Y1 + y, &bm);
      cv->waitCompletion();                     // the strip buffer is reused next time round
    }
    fclose(f);
  }

  // drawing straight onto the screen, clipped to the picture and the screen
  Rect visible() {                              // picture coords that are on screen
    Rect s = clientRect(uiOrigin::Screen);
    return Rect(imax(0, -s.X1), imax(0, -s.Y1), imin(DW - 1, SCREEN_W - 1 - s.X1), imin(DH - 1, SCREEN_H - 1 - s.Y1));
  }
  Canvas * dcBegin() {
    auto cv = canvas();
    Rect s = clientRect(uiOrigin::Screen);
    cv->setOrigin(s.X1, s.Y1);                  // picture coords from here on
    cv->setClippingRect(visible());
    cv->resetPaintOptions();
    cv->resetGlyphOptions();
    cv->setPenWidth(1);
    cv->setPenColor(ink ? T.text : T.win);
    cv->setBrushColor(ink ? T.text : T.win);
    return cv;
  }
  bool covered() {                              // is any other window over the picture?
    uiWindow * fr = parentFrame();
    Rect me = clientRect(uiOrigin::Screen);
    for (uiWindow * w = fr ? fr->next() : nullptr; w; w = w->next())
      if (w->state().visible && w->rect(uiOrigin::Screen).intersects(me)) return true;
    return false;
  }
  void grow(int x, int y, int r) {
    m_bx0 = imin(m_bx0, x - r); m_by0 = imin(m_by0, y - r);
    m_bx1 = imax(m_bx1, x + r); m_by1 = imax(m_by1, y + r);
  }
  Point pic(int wx, int wy) { Rect cr = clientRect(uiOrigin::Window); return Point(wx - cr.X1, wy - cr.Y1); }
  int width() { return tool == PENCIL ? 1 : tool == ERASER ? size * 3 + 4 : size; }

  void stroke(Canvas * cv, Point a, Point b) {
    int w = width();
    if (tool == SPRAY) {
      int r = size * 3 + 4;
      for (int k = 0; k < size * 3 + 4; ++k) {
        int dx = (int)(esp_random() % (2 * r + 1)) - r, dy = (int)(esp_random() % (2 * r + 1)) - r;
        if (dx * dx + dy * dy <= r * r) cv->setPixel(b.X + dx, b.Y + dy);
      }
      grow(b.X, b.Y, r + 1);
      return;
    }
    if (tool == ERASER) { cv->setPenColor(T.win); cv->setBrushColor(T.win); }
    if (w <= 1) cv->drawLine(a.X, a.Y, b.X, b.Y);
    else {
      cv->setPenWidth(w);
      cv->setLineEnds(LineEnds::Circle);
      cv->drawLine(a.X, a.Y, b.X, b.Y);
      cv->setPenWidth(1);
    }
    grow(a.X, a.Y, w / 2 + 2); grow(b.X, b.Y, w / 2 + 2);
  }

  void drawPreview(Canvas * cv, Point a, Point b) {      // inverted outline: drawn twice = gone
    PaintOptions po; po.NOT = 1;
    cv->setPaintOptions(po);
    if (tool == LINE) cv->drawLine(a.X, a.Y, b.X, b.Y);
    else cv->drawRectangle(imin(a.X, b.X), imin(a.Y, b.Y), imax(a.X, b.X), imax(a.Y, b.Y));
    cv->resetPaintOptions();
  }

  void finalShape(Canvas * cv, Point a, Point b) {
    int x0 = imin(a.X, b.X), y0 = imin(a.Y, b.Y), x1 = imax(a.X, b.X), y1 = imax(a.Y, b.Y);
    int w = imax(1, size);
    switch (tool) {
      case LINE:
        cv->setPenWidth(w); cv->setLineEnds(LineEnds::Circle);
        cv->drawLine(a.X, a.Y, b.X, b.Y);
        break;
      case BOX:
        for (int i = 0; i < w && x0 + i <= x1 - i && y0 + i <= y1 - i; ++i) cv->drawRectangle(x0 + i, y0 + i, x1 - i, y1 - i);
        break;
      case FILLBOX:
        cv->fillRectangle(x0, y0, x1, y1);
        break;
      case OVAL:
        for (int i = 0; i < w && x1 - x0 - 2 * i > 0 && y1 - y0 - 2 * i > 0; ++i)
          cv->drawEllipse((x0 + x1) / 2, (y0 + y1) / 2, x1 - x0 - 2 * i, y1 - y0 - 2 * i);
        break;
      case FILLOVAL:
        cv->fillEllipse((x0 + x1) / 2, (y0 + y1) / 2, x1 - x0 + 1, y1 - y0 + 1);
        break;
    }
    cv->setPenWidth(1);
    grow(x0, y0, w + 2); grow(x1, y1, w + 2);
  }

  void down(int wx, int wy) {
    app()->setFocusedWindow(this);
    if (covered()) { bringToFront((uiFrame *)parentFrame()); return; }   // first click just brings Doodle up
    Point p = pic(wx, wy);
    m_bx0 = m_bx1 = p.X; m_by0 = m_by1 = p.Y;
    if (tool == FILL) { floodFill(p); return; }
    if (tool == TEXT) { askText(p); return; }
    m_drag = true;
    m_start = m_last = m_prevEnd = p;
    auto cv = dcBegin();
    if (tool <= SPRAY) stroke(cv, p, p);
    else { drawPreview(cv, p, p); m_preview = true; }
    cv->waitCompletion();
  }

  void move(int wx, int wy) {
    Point p = pic(wx, wy);
    auto cv = dcBegin();
    if (tool <= SPRAY) stroke(cv, m_last, p);
    else {
      if (m_preview) drawPreview(cv, m_start, m_prevEnd);
      drawPreview(cv, m_start, p);
      m_preview = true;
      m_prevEnd = p;
    }
    m_last = p;
  }

  void up(int wx, int wy) {
    m_drag = false;
    Point p = pic(wx, wy);
    auto cv = dcBegin();
    if (tool <= SPRAY) stroke(cv, m_last, p);
    else {
      if (m_preview) drawPreview(cv, m_start, m_prevEnd);
      m_preview = false;
      finalShape(cv, m_start, p);
    }
    cv->waitCompletion();
    commit();
  }

  // ---- writing the screen back into the picture file

  void saveUndo(int x0, int y0, int x1, int y1) {
    FILE * p = fopen(doodleWork().c_str(), "rb");
    if (!p) return;
    writeFileWith(doodleUndo().c_str(), [&](RSink & o) {
      int16_t b[4] = { (int16_t)x0, (int16_t)y0, (int16_t)x1, (int16_t)y1 };
      o.write((const char *)b, sizeof(b));
      int bx0 = x0 >> 3, bn = (x1 >> 3) - bx0 + 1;
      char row[DROW];
      for (int y = y0; y <= y1; ++y) {
        fseek(p, dRowOff(y) + bx0, SEEK_SET);
        if (fread(row, 1, bn, p) != (size_t)bn) memset(row, 0, bn);
        o.write(row, bn);
      }
    });
    fclose(p);
  }

  void commit() {
    Rect v = visible();
    int x0 = imax(m_bx0, v.X1), y0 = imax(m_by0, v.Y1), x1 = imin(m_bx1, v.X2), y1 = imin(m_by1, v.Y2);
    if (x0 > x1 || y0 > y1) return;
    saveUndo(x0, y0, x1, y1);
    FILE * p = fopen(doodleWork().c_str(), "r+b");
    if (!p) return;
#ifndef TIDYOS_HTML5
    auto dc = app()->displayController();
    dc->setMouseCursorPos(-100, -100);          // keep the pointer out of the picture
    canvas()->waitCompletion();
#endif
    Rect s = clientRect(uiOrigin::Screen);
    int bx0 = x0 >> 3, bn = (x1 >> 3) - bx0 + 1;
    uint8_t row[DROW];
    for (int y = y0; y <= y1; ++y) {
      long off = dRowOff(y) + bx0;
      fseek(p, off, SEEK_SET);
      if (fread(row, 1, bn, p) != (size_t)bn) memset(row, 0, bn);
      for (int x = x0; x <= x1; ++x) {
        uint8_t & b = row[(x >> 3) - bx0];
        uint8_t bit = 0x80 >> (x & 7);
        if (fbGet(s.X1 + x, s.Y1 + y) == 0) b |= bit; else b &= ~bit;
      }
      fseek(p, off, SEEK_SET);
      fwrite(row, 1, bn, p);
    }
    fclose(p);
#ifndef TIDYOS_HTML5
    dc->setMouseCursorPos(s.X1 + m_last.X, s.Y1 + m_last.Y);
#endif
    if (onChanged) onChanged();
  }

  // paint bucket: scanline fill straight in the screen memory, then commit what's visible
  void floodFill(Point p) {
    if (gColors != 4 && gColors != 8) return;
    Rect v = visible();
    if (!v.contains(p)) return;
    Rect s = clientRect(uiOrigin::Screen);
    canvas()->waitCompletion();
    auto isInk = [&](int x, int y) { return fbGet(s.X1 + x, s.Y1 + y) == 0; };
    bool from = isInk(p.X, p.Y);
    if (from == ink) return;
    int to = ink ? 0 : 1;
    std::vector<Point> st;
    st.push_back(p);
    while (!st.empty()) {
      Point q = st.back(); st.pop_back();
      if (isInk(q.X, q.Y) != from) continue;
      int l = q.X, r = q.X;
      while (l > v.X1 && isInk(l - 1, q.Y) == from) --l;
      while (r < v.X2 && isInk(r + 1, q.Y) == from) ++r;
      for (int x = l; x <= r; ++x) fbSet(s.X1 + x, s.Y1 + q.Y, to);
      for (int dy = -1; dy <= 1; dy += 2) {
        int ny = q.Y + dy;
        if (ny < v.Y1 || ny > v.Y2) continue;
        bool in = false;
        for (int x = l; x <= r; ++x) {
          bool m = isInk(x, ny) == from;
          if (m && !in) { if (st.size() < 2000) st.push_back(Point(x, ny)); in = true; }
          else if (!m) in = false;
        }
      }
    }
    m_bx0 = v.X1; m_by0 = v.Y1; m_bx1 = v.X2; m_by1 = v.Y2;
    commit();
  }

  void askText(Point p) {
    char t[64] = "";
    if (app()->inputBox("Text", "Type the words to place:", t, sizeof(t), "Place", "Cancel") != uiMessageBoxResult::Button1 || !t[0]) return;
    m_text = t;
    m_textAt = p;
    // wait a moment so the screen under the box is repainted before the text is drawn and kept
    m_textTimer = app()->setTimer(this, 150);
  }

  void placeText() {
    auto cv = dcBegin();
    cv->setGlyphOptions(GlyphOptions().FillBackground(false));
    cv->drawText(&FONT_std_16, m_textAt.X, m_textAt.Y, m_text.c_str());
    cv->waitCompletion();
    int w = cv->textExtent(&FONT_std_16, m_text.c_str());
    m_bx0 = m_textAt.X; m_by0 = m_textAt.Y; m_bx1 = m_textAt.X + w; m_by1 = m_textAt.Y + 20;
    commit();
  }
};

class DoodleApp : public AppFrame {
public:
  static AppFrame * inst;
  DoodleCanvas * cvs;
  uiButton * tools[DoodleCanvas::NTOOLS];
  uiButton * inkBtn, * sizeBtn;
  uiLabel  * saved;
  std::string path;
  bool dirty = false;

  DoodleApp() : AppFrame("Doodle", Point(24, 14), Size(592, 418), &inst) {
    if (fileSize(doodleWork().c_str()) != BMP_HDR + DROW * DH) doodleBlank(doodleWork());
    remove(doodleUndo().c_str());
    int x = cx;
    auto b = [&](const char * t, int w, std::function<void()> f) { auto bt = button(t, x, cy, w, 24); bt->onClick = f; x += w + 4; return bt; };
    b("New", 48, [this]() { if (askSave()) { cvs->wholeChange([]() { doodleBlank(doodleWork()); }); path.clear(); setTitle("Doodle - Untitled"); markSaved(true); } });
    b("Open", 52, [this]() { openPic(); });
    b("Save", 50, [this]() { save(); });
    b("Save as", 66, [this]() { saveAs(); });
    b("Undo", 50, [this]() { if (cvs->undo()) markSaved(false); });
    b("Clear", 52, [this]() { cvs->wholeChange([]() { doodleBlank(doodleWork()); }); });
    inkBtn  = b("Ink: Black", 88, [this]() { cvs->ink = !cvs->ink; refresh(); });
    sizeBtn = b("Size: 4", 64, [this]() { cvs->size = cvs->size >= 8 ? 1 : cvs->size * 2; refresh(); });
    saved = makeSavedLight(this, cx + 508, cy + 3);
    static const char * names[] = { "Pencil", "Brush", "Eraser", "Spray", "Line", "Box", "Filled box", "Oval", "Filled oval", "Paint fill", "Text" };
    for (int i = 0; i < DoodleCanvas::NTOOLS; ++i) {
      tools[i] = button(names[i], cx, cy + 32 + i * 29, 82, 26);
      tools[i]->onClick = [this, i]() { cvs->tool = i; refresh(); };
    }
    cvs = new DoodleCanvas(this, Point(cx + 88, cy + 32));
    cvs->onChanged = [this]() { markSaved(false); };
    setTitle("Doodle - Untitled");
    refresh();
  }

  void refresh() {
    for (int i = 0; i < DoodleCanvas::NTOOLS; ++i) {
      bool on = cvs->tool == i;
      tools[i]->buttonStyle().backgroundColor = on ? T.accent : T.face;
      tools[i]->buttonStyle().textColor       = on ? T.accentText : T.text;
      tools[i]->repaint();
    }
    inkBtn->setText(cvs->ink ? "Ink: Black" : "Ink: White"); inkBtn->repaint();
    char s[16]; snprintf(s, sizeof(s), "Size: %d", cvs->size);
    sizeBtn->setText(s); sizeBtn->repaint();
  }

  void markSaved(bool s) { if (dirty == !s) return; dirty = !s; setSavedLight(saved, s); }

  bool askSave() {
    if (!dirty) return true;
    auto r = gApp->messageBox("Doodle", "Save your picture first?", "Save", "Don't save", "Cancel", uiMessageBoxIcon::Question);
    if (r == uiMessageBoxResult::Button1) return save();
    return r == uiMessageBoxResult::Button2;
  }

  bool copyTo(const std::string & dst) {
    FILE * f = fopen(doodleWork().c_str(), "rb");
    if (!f) return false;
    bool ok = writeFileWith(dst.c_str(), [&](RSink & o) { char b[256]; size_t n; while ((n = fread(b, 1, sizeof(b), f)) > 0) o.write(b, n); });
    fclose(f);
    return ok;
  }

  bool save() {
    if (path.empty()) return saveAs();
    if (!copyTo(path)) { gApp->messageBox("Doodle", "Could not save the picture.", "OK", nullptr, nullptr, uiMessageBoxIcon::Error); return false; }
    markSaved(true);
    return true;
  }

  bool saveAs() {
    std::string p = askFile("Save picture (.bmp)", "Save", path.empty() ? "doodle.bmp" : baseName(path).c_str(), path.empty() ? documentsDir() : dirOf(path));
    if (p.empty()) return false;
    if (strcasecmp(fileExt(p.c_str()), "bmp") != 0) p += ".bmp";
    path = p;
    setTitle(("Doodle - " + baseName(p)).c_str());
    return save();
  }

  void openPic() {
    if (!askSave()) return;
    std::string p = askFile("Open picture (.bmp)", "Open", "", path.empty() ? documentsDir() : dirOf(path));
    if (!p.empty()) loadPic(p);
  }
  // opens 1-bit .bmp pictures (any size: cropped or padded to 480 x 320)
  void loadPic(const std::string & p) {
    FILE * f = fopen(p.c_str(), "rb");
    uint8_t h[BMP_HDR];
    if (!f || fread(h, 1, BMP_HDR, f) != BMP_HDR || h[0] != 'B' || h[1] != 'M' || h[28] != 1) {
      if (f) fclose(f);
      gApp->messageBox("Doodle", "Doodle opens black and white .bmp files.", "OK", nullptr, nullptr, uiMessageBoxIcon::Warning);
      return;
    }
    uint32_t data = get32(h + 10), hsz = get32(h + 14);
    int w = (int)get32(h + 18), hh = (int)get32(h + 22);
    bool topDown = hh < 0; if (topDown) hh = -hh;
    uint8_t pal[8] = { 0, 0, 0, 0, 255, 255, 255, 0 };
    fseek(f, 14 + hsz, SEEK_SET); fread(pal, 1, 8, f);
    bool zeroDark = pal[0] + pal[1] + pal[2] < pal[4] + pal[5] + pal[6];     // which colour is the ink?
    int srcRow = ((w + 31) / 32) * 4;
    cvs->wholeChange([&]() {
      writeFileWith(doodleWork().c_str(), [&](RSink & o) {
        uint8_t hd[BMP_HDR]; bmpHeader(hd);
        o.write((const char *)hd, BMP_HDR);
        uint8_t row[DROW];
        for (int fy = 0; fy < DH; ++fy) {            // file order: bottom row first
          int y = DH - 1 - fy;                     // picture row
          memset(row, 0, DROW);
          if (y < hh) {
            int sy = topDown ? y : hh - 1 - y;
            fseek(f, data + (long)sy * srcRow, SEEK_SET);
            int n = imin(DROW, srcRow);
            fread(row, 1, n, f);
            if (zeroDark) for (int i = 0; i < n; ++i) row[i] = ~row[i];
            if (w < DW) {                          // blank anything past the source width
              for (int x = w; x < DW; ++x) row[x >> 3] &= ~(0x80 >> (x & 7));
            }
          }
          o.write((const char *)row, DROW);
        }
      });
    });
    fclose(f);
    path = p;
    setTitle(("Doodle - " + baseName(p)).c_str());
    markSaved(true);
  }

  bool canClose() override { return askSave(); }
};
AppFrame * DoodleApp::inst = nullptr;

void openDoodle()
{
  if (!DoodleApp::inst) {
    if (ramFree() < 20000) {
      gApp->messageBox("Doodle", "Not enough memory. Close a window first.", "OK", nullptr, nullptr, uiMessageBoxIcon::Warning);
      return;
    }
    new DoodleApp();
  }
  DoodleApp::inst->open();
}

void openDoodleFile(const char * path)
{
  openDoodle();
  auto d = (DoodleApp *)DoodleApp::inst;
  if (d && d->askSave()) d->loadPic(path);
}
