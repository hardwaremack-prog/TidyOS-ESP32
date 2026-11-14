// TidyOS apps: Calculator, Neat Lite, Tidy Lite, Ready Lite, Steady Lite,
//              Browser, Files, Settings, System
#include "screen.h"
#include "crashlog.h"
#include "update.h"
#include <sys/stat.h>
#include <algorithm>
#include <WiFi.h>
#include <math.h>
#include "core.h"
#include "style.h"
#include "widgets.h"
#include "input.h"
#include "net.h"
#include "print.h"
#include "spell.h"
#include "shelfmsg.h"
#include "fileserver.h"

using namespace fabgl;

// ====================================================================== base window

class AppFrame : public uiFrame {
public:
  AppFrame(const char * title, Point pos, Size size, AppFrame ** slot)
    : uiFrame(gApp->rootWindow(), title, pos, size, false), m_slot(slot)
  {
    applyStyle(this);
    frameProps().hasMaximizeButton = false;
    frameProps().hasMinimizeButton = false;
    frameProps().resizeable        = false;
    *slot = this;
    Point o = clientOrigin(this);
    cx = o.X + 6;
    cy = o.Y + 6;
  }

  void processEvent(uiEvent * e) override {
    if (e->id == UIEVT_CLOSE) {
      if (!canClose()) return;
      if (m_timer) { gApp->killTimer(m_timer); m_timer = nullptr; }
      onClosing();
      *m_slot = nullptr;
      gApp->destroyWindow(this);
      return;
    }
    uiFrame::processEvent(e);
  }

  void open() { bringToFront(this); }
  void startTimer(int ms) { m_timer = gApp->setTimer(this, ms); }

  virtual bool canClose() { return true; }
  virtual void onClosing() {}

  uiButton * button(const char * t, int x, int y, int w, int h = 22) { return S(new uiButton(this, t, Point(x, y), Size(w, h))); }
  uiLabel  * label(const char * t, int x, int y, int w = 0, int h = 0) { return S(new uiLabel(this, t, Point(x, y), Size(w, h))); }
  uiTextEdit * edit(const char * t, int x, int y, int w, int h = 22) { return S(new uiTextEdit(this, t, Point(x, y), Size(w, h))); }

  int cx, cy;     // top-left of usable area

protected:
  AppFrame ** m_slot;
  uiTimerHandle m_timer = nullptr;
};

static int s_cascade = 0;
static Point nextPos(int w, int h)
{
  int x = 180 + (s_cascade % 6) * 18, y = 12 + (s_cascade % 6) * 14;
  ++s_cascade;
  if (x + w > SCREEN_W) x = SCREEN_W - w;
  if (y + h > SCREEN_H - 22) y = imax(0, SCREEN_H - 22 - h);
  return Point(x, y);
}

static std::string joinPath(const char * dir, const char * name)
{
  std::string p = dir;
  if (p.empty() || p.back() != '/') p += '/';
  return p + name;
}

// a file dialog wrapper. returns full path or "" when cancelled
static std::string askFile(const char * title, const char * okText, const char * defName, const std::string & startDir)
{
  char dir[128], name[64];
  strlcpy(dir, startDir.c_str(), sizeof(dir));
  strlcpy(name, defName ? defName : "", sizeof(name));
  if (gApp->fileDialog(title, dir, sizeof(dir), name, sizeof(name), okText, "Cancel", 300, 300) != uiMessageBoxResult::ButtonOK)
    return "";
  if (!name[0]) return "";
  return joinPath(dir, name);
}

static std::string dirOf(const std::string & path)
{
  size_t p = path.rfind('/');
  return p == std::string::npos ? documentsDir() : path.substr(0, p);
}

static std::string baseName(const std::string & path)
{
  size_t p = path.rfind('/');
  return p == std::string::npos ? path : path.substr(p + 1);
}

// ====================================================================== Calculator

class CalcApp : public AppFrame {
public:
  static AppFrame * inst;
  uiLabel * disp;
  double acc = 0;
  char   op = 0;
  std::string cur = "0";
  bool   fresh = true;

  CalcApp() : AppFrame("Calculator", Point(200, 60), Size(236, 318), &inst) {
    disp = label("0", cx, cy, 216, 40);
    disp->labelStyle().textFont = &FONT_std_24;
    disp->labelStyle().textAlign = uiHAlign::Right;
    disp->labelStyle().backgroundColor = T.face;
    static const char * keys[20] = { "C", "<-", "%", "/", "7", "8", "9", "*", "4", "5", "6", "-",
                                     "1", "2", "3", "+", "+/-", "0", ".", "=" };
    for (int i = 0; i < 20; ++i) {
      auto b = button(keys[i], cx + (i % 4) * 55, cy + 48 + (i / 4) * 46, 51, 42);
      b->buttonStyle().textFont = &FONT_std_18;
      b->setFocusIndex(-1);
      if (i == 19 || (i % 4 == 3 && i > 0)) { b->buttonStyle().backgroundColor = T.accent; b->buttonStyle().textColor = T.accentText; }
      b->onClick = [this, i]() { press(keys[i]); gApp->setFocusedWindow(nullptr); };
    }
    onKeyDown = [this](uiKeyEventInfo const & k) {
      char c = k.ASCII;
      if (c >= '0' && c <= '9') { char s[2] = { c, 0 }; press(s); }
      else if (c == '.' || c == ',') press(".");
      else if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%') { char s[2] = { c, 0 }; press(s); }
      else if (c == '=' || k.VK == VK_RETURN || k.VK == VK_KP_ENTER) press("=");
      else if (k.VK == VK_BACKSPACE) press("<-");
      else if (k.VK == VK_ESCAPE || k.VK == VK_DELETE || c == 'c' || c == 'C') press("C");
    };
  }

  double val() { return atof(cur.c_str()); }

  void show(double v) {
    if (isnan(v) || isinf(v)) { cur = "Error"; }
    else {
      char b[32];
      snprintf(b, sizeof(b), "%.10g", v);
      cur = b;
    }
    disp->setText(cur.c_str());
    disp->repaint();
  }

  double apply(double a, char o, double b) {
    switch (o) {
      case '+': return a + b;
      case '-': return a - b;
      case '*': return a * b;
      case '/': return b == 0 ? NAN : a / b;
    }
    return b;
  }

  void press(const char * k) {
    if (cur == "Error" && k[0] != 'C') { cur = "0"; fresh = true; op = 0; }
    if (isdigit((unsigned char)k[0]) || k[0] == '.') {
      if (fresh) { cur = (k[0] == '.') ? "0." : k; fresh = false; }
      else {
        if (k[0] == '.' && cur.find('.') != std::string::npos) return;
        if (cur.size() >= 14) return;
        if (cur == "0" && k[0] != '.') cur = k; else cur += k;
      }
    } else if (!strcmp(k, "C")) { cur = "0"; acc = 0; op = 0; fresh = true; }
    else if (!strcmp(k, "<-")) { if (!fresh && cur.size() > 1) cur.pop_back(); else { cur = "0"; fresh = true; } }
    else if (!strcmp(k, "+/-")) { if (cur[0] == '-') cur.erase(0, 1); else if (cur != "0") cur = "-" + cur; }
    else if (!strcmp(k, "%")) { show(val() / 100); fresh = true; return; }
    else if (!strcmp(k, "=")) { if (op) { acc = apply(acc, op, val()); op = 0; show(acc); } fresh = true; return; }
    else {   // operator
      if (op && !fresh) { acc = apply(acc, op, val()); show(acc); }
      else if (!op) acc = val();
      op = k[0];
      fresh = true;
      return;
    }
    disp->setText(cur.c_str());
    disp->repaint();
  }
};
AppFrame * CalcApp::inst = nullptr;

void openCalculator()
{
  if (!CalcApp::inst) new CalcApp();
  CalcApp::inst->open();
}

// ====================================================================== Neat Lite (text editor)

// Neat Lite: a small word processor. Fonts, sizes (slider), bold, underline, alignment,
// paragraph spacing; saves .rtf (opens in Word / LibreOffice) or plain .txt.
#include "richtext.h"

class NeatApp : public AppFrame {
public:
  static AppFrame * inst;
  RichText * rt;
  uiLabel *  saved;
  uiButton * fontBtn, * boldBtn, * underBtn, * alignBtn[3], * linesBtn;
  uiSlider * sizeSl;
  uiLabel  * sizeLbl, * afterLbl;
  std::string path;
  bool dirty = false;
  bool updating = false;

  NeatApp() : AppFrame("Neat Lite - Untitled", Point(4, 4), Size(632, 452), &inst) {
    // row 1: file
    button("New",      cx,       cy, 44)->onClick = [this]() { newDoc(); };
    button("Open",     cx + 48,  cy, 46)->onClick = [this]() { openDoc(); };
    button("Save",     cx + 98,  cy, 44)->onClick = [this]() { save(); };
    button("Save as",  cx + 146, cy, 60)->onClick = [this]() { saveAs(); };
    button("Print",    cx + 210, cy, 46)->onClick = [this]() { print(); };
    button("Spelling", cx + 260, cy, 70)->onClick = [this]() { spelling(); };
    auto hint = label("Ctrl+B bold   Ctrl+U underline", cx + 338, cy + 4, 200, 16);
    hint->labelStyle().textFont = &FONT_std_12;
    saved = makeSavedLight(this, cx + 546, cy + 2);

    // row 2: text
    int y = cy + 26;
    fontBtn = button("", cx, y, 120);
    fontBtn->onClick = [this]() { rt->setFont((rt->styleHere().font + 1) % richFontCount()); refreshBar(); focusText(); };
    boldBtn = button("B", cx + 124, y, 26);
    boldBtn->buttonStyle().textFont = &FONT_std_16;
    boldBtn->onClick = [this]() { rt->toggleFlag(RS_BOLD); refreshBar(); focusText(); };
    underBtn = button("U", cx + 154, y, 26);
    underBtn->onClick = [this]() { rt->toggleFlag(RS_UNDER); refreshBar(); focusText(); };
    label("Size", cx + 188, y + 4, 30, 16);
    sizeSl = S(new uiSlider(this, Point(cx + 220, y + 1), Size(190, 20), uiOrientation::Horizontal));
    sizeSl->setup(8, 48, 8);
    sizeSl->sliderStyle().backgroundColor    = T.win;
    sizeSl->sliderStyle().slideColor         = T.face;
    sizeSl->sliderStyle().rangeColor         = T.accent;
    sizeSl->sliderStyle().gripColor          = T.text;
    sizeSl->sliderStyle().mouseOverGripColor = T.accent;
    sizeSl->sliderStyle().ticksColor         = T.shadow;
    sizeSl->onChange = [this]() { if (!updating) { rt->setSize(sizeSl->position()); refreshBar(); } };
    sizeLbl = label("", cx + 414, y + 4, 42, 16);
    static const char * al[3] = { "Left", "Centre", "Right" };
    for (int i = 0; i < 3; ++i) {
      alignBtn[i] = button(al[i], cx + 462 + i * 54, y, 50);
      alignBtn[i]->onClick = [this, i]() { rt->paraAlign(i); refreshBar(); focusText(); };
    }

    // row 3: paragraph
    y = cy + 52;
    label("Paragraph:", cx, y + 4, 74, 16);
    label("space after", cx + 76, y + 4, 70, 16)->labelStyle().textFont = &FONT_std_12;
    button("-", cx + 148, y, 24)->onClick = [this]() { rt->paraAfter(-4); refreshBar(); focusText(); };
    afterLbl = label("", cx + 176, y + 4, 44, 16);
    button("+", cx + 222, y, 24)->onClick = [this]() { rt->paraAfter(4); refreshBar(); focusText(); };
    linesBtn = button("", cx + 256, y, 110);
    linesBtn->onClick = [this]() {
      int sp = rt->doc.paras[rt->cur.p].spacing;
      rt->paraSpacing(sp == 100 ? 150 : sp == 150 ? 200 : 100);
      refreshBar(); focusText();
    };
    auto tip = label("Select text first to change just that part", cx + 374, y + 4, 240, 16);
    tip->labelStyle().textFont = &FONT_std_12;

    rt = new RichText(this, Point(cx, cy + 78), Size(618, 344));
    rt->isMisspelled = [](const std::string & w) { return !spellIsWord(w); };
    rt->onModified = [this]() { if (!dirty) { dirty = true; setSavedLight(saved, false); } };
    rt->onSelChange = [this]() { refreshBar(); };
    rt->onKey = [this](uiKeyEventInfo const & k) {
      if (!k.CTRL) return false;
      if (k.VK == VK_s || k.VK == VK_S) { save(); return true; }
      if (k.VK == VK_o || k.VK == VK_O) { openDoc(); return true; }
      if (k.VK == VK_n || k.VK == VK_N) { newDoc(); return true; }
      if (k.VK == VK_p || k.VK == VK_P) { print(); return true; }
      if (k.VK == VK_PLUS || k.VK == VK_EQUALS || k.VK == VK_KP_PLUS) { rt->setSize(rt->styleHere().size + 2); refreshBar(); return true; }
      if (k.VK == VK_MINUS || k.VK == VK_KP_MINUS) { rt->setSize(rt->styleHere().size - 2); refreshBar(); return true; }
      return false;
    };
    refreshBar();
  }

  void focusText() { gApp->setFocusedWindow(rt); }

  static void mark(uiButton * b, bool on) {
    b->buttonStyle().backgroundColor = on ? T.accent : T.face;
    b->buttonStyle().textColor       = on ? T.accentText : T.text;
    b->repaint();
  }

  void refreshBar() {
    updating = true;
    RStyle st = rt->styleHere();
    char b[40];
    snprintf(b, sizeof(b), "Font: %s", richFontName(st.font));
    fontBtn->setText(b); fontBtn->repaint();
    mark(boldBtn, st.flags & RS_BOLD);
    mark(underBtn, st.flags & RS_UNDER);
    if (sizeSl->position() != st.size) sizeSl->setPosition(st.size);
    snprintf(b, sizeof(b), "%d px", st.size);
    sizeLbl->setText(b); sizeLbl->repaint();
    const RPara & P = rt->doc.paras[rt->doc.clamp(rt->cur).p];
    for (int i = 0; i < 3; ++i) mark(alignBtn[i], P.align == i);
    snprintf(b, sizeof(b), "%d px", P.after);
    afterLbl->setText(b); afterLbl->repaint();
    snprintf(b, sizeof(b), "Lines: %s", P.spacing == 200 ? "double" : P.spacing == 150 ? "1.5" : "single");
    linesBtn->setText(b); linesBtn->repaint();
    updating = false;
  }

  std::string docTitle() { return path.empty() ? std::string("Untitled") : baseName(path); }

  void print() {
    PrintDoc d;
    d.title = docTitle();
    for (size_t p = 0; p < rt->doc.paras.size(); ++p) d.lines.push_back(rt->doc.text(p));
    printDialog(d);
  }

  // next misspelled word at or after `from` (stops at `stop` if given)
  bool findNext(RPos from, RPos stop, bool useStop, RPos & a, RPos & b) {
    for (int p = from.p; p < (int)rt->doc.paras.size(); ++p) {
      std::string l = rt->doc.text(p);
      int i = p == from.p ? from.o : 0, n = l.size();
      while (i < n) {
        while (i < n && !isalpha((unsigned char)l[i])) ++i;
        int st = i;
        while (i < n && (isalpha((unsigned char)l[i]) || (l[i] == '\'' && i + 1 < n && isalpha((unsigned char)l[i + 1])))) ++i;
        if (useStop && !(RPos(p, st) < stop)) return false;
        if (i > st && !spellIsWord(l.substr(st, i - st))) { a = RPos(p, st); b = RPos(p, i); return true; }
      }
    }
    return false;
  }

  // 0 stop, 1 change, 2 ignore, 3 add
  int spellAsk(const std::string & word, std::string & replacement) {
    std::vector<std::string> sug;
    spellSuggest(word, sug, 6);
    auto f = new uiFrame(gApp->rootWindow(), "Spelling", Point(276, 226), Size(356, 226), false);
    applyStyle(f);
    f->frameProps().resizeable = false;
    f->frameProps().hasMaximizeButton = false;
    f->frameProps().hasMinimizeButton = false;
    Point o = clientOrigin(f);
    int x = o.X + 8, y = o.Y + 6;
    S(new uiLabel(f, "Not in the dictionary:", Point(x, y), Size(150, 18)));
    auto wl = S(new uiLabel(f, word.c_str(), Point(x + 150, y - 2), Size(186, 22)));
    wl->labelStyle().textFont = &FONT_std_18;
    wl->labelStyle().textColor = T.warn;
    y += 26;
    auto list = S(new uiListBox(f, Point(x, y), Size(200, 132)));
    list->items().takeStrings();
    for (auto & w : sug) list->items().append(w.c_str());
    if (sug.empty()) list->items().append("(no suggestions)");
    else list->selectItem(0);
    auto change = S(new uiButton(f, "Change",            Point(x + 210, y),       Size(126, 26)));
    auto ignore = S(new uiButton(f, "Ignore",            Point(x + 210, y + 34),  Size(126, 26)));
    auto add    = S(new uiButton(f, "Add to dictionary", Point(x + 210, y + 68),  Size(126, 26)));
    auto stop   = S(new uiButton(f, "Stop",              Point(x + 210, y + 106), Size(126, 26)));
    change->onClick = [f]() { f->exitModal(1); };
    ignore->onClick = [f]() { f->exitModal(2); };
    add->onClick    = [f]() { f->exitModal(3); };
    stop->onClick   = [f]() { f->exitModal(0); };
    list->onDblClick = [f]() { f->exitModal(1); };
    int r = gApp->showModalWindow(f);
    int i = list->firstSelectedItem();
    replacement = (i >= 0 && i < (int)sug.size()) ? sug[i] : "";
    gApp->destroyWindow(f);
    if (r == 1 && replacement.empty()) r = 2;
    return r;
  }

  void spelling() {
    RPos start = rt->cur, at = start;
    bool wrapped = false;
    int fixed = 0;
    for (;;) {
      RPos a, b;
      bool found = findNext(at, start, wrapped, a, b);
      if (!found) {
        if (!wrapped && start != RPos(0, 0)) { wrapped = true; at = RPos(0, 0); continue; }
        break;
      }
      rt->hlA = a; rt->hlB = b;
      rt->selectRange(a, a);
      std::string word = rt->doc.plainRange(a, b), rep;
      int r = spellAsk(word, rep);
      if (r == 0) break;
      if (r == 1) {
        rt->selectRange(a, b);
        rt->replaceSel(rep);
        at = RPos(a.p, a.o + rep.size());
        if (wrapped) start = rt->doc.clamp(start);
        ++fixed;
      } else {
        if (r == 2) spellIgnore(word);
        if (r == 3) spellAddWord(word);
        at = b;
      }
    }
    rt->hlA = rt->hlB = RPos();
    rt->repaint();
    char b[80];
    snprintf(b, sizeof(b), fixed ? "Spell check done. %d word%s fixed." : "Spell check done.", fixed, fixed == 1 ? "" : "s");
    gApp->messageBox("Spelling", b, "OK", nullptr, nullptr, uiMessageBoxIcon::Info);
    focusText();
  }

  void setPath(const std::string & p) {
    path = p;
    setTitle(("Neat Lite - " + docTitle()).c_str());
    repaint();
  }

  static bool isRtf(const std::string & p) { return strcasecmp(fileExt(p.c_str()), "rtf") == 0; }

  void loadFile(const char * p) {
    std::string s;
    if (!readFileStr(p, s, 96 * 1024)) { gApp->messageBox("Neat", "Could not open that file.", "OK", nullptr, nullptr, uiMessageBoxIcon::Error); return; }
    if (!(s.compare(0, 5, "{\\rtf") == 0 && rt->setRtf(s))) rt->setPlain(s);
    setPath(p);
    dirty = false;
    setSavedLight(saved, true);
    focusText();
  }

  bool askSave() {    // true = go on
    if (!dirty) return true;
    auto r = gApp->messageBox("Neat", "Save your changes first?", "Save", "Don't save", "Cancel", uiMessageBoxIcon::Question);
    if (r == uiMessageBoxResult::Button1) return save();
    return r == uiMessageBoxResult::Button2;
  }

  void newDoc() {
    if (!askSave()) return;
    rt->setPlain("");
    setPath("");
    dirty = false;
    setSavedLight(saved, true);
    focusText();
  }

  void openDoc() {
    if (!askSave()) return;
    std::string p = askFile("Open document", "Open", "", path.empty() ? documentsDir() : dirOf(path));
    if (!p.empty()) loadFile(p.c_str());
  }

  bool save() {
    if (path.empty()) return saveAs();
    if (!isRtf(path) && rt->doc.hasFormatting(rt->normal)) {
      auto r = gApp->messageBox("Neat", "A .txt file can't keep fonts and sizes.", "Save as .rtf", "Plain text", "Cancel", uiMessageBoxIcon::Question);
      if (r == uiMessageBoxResult::Button3 || r == uiMessageBoxResult::Cancel) return false;
      if (r == uiMessageBoxResult::Button1) {
        std::string p = path;
        size_t dot = p.rfind('.');
        if (dot != std::string::npos && dot > p.rfind('/')) p.erase(dot);
        setPath(p + ".rtf");
      }
    }
    std::string out = isRtf(path) ? rt->doc.toRtf() : rt->doc.plain();
    if (!writeFileStr(path.c_str(), out)) {
      gApp->messageBox("Neat", "Could not save the document.", "OK", nullptr, nullptr, uiMessageBoxIcon::Error);
      return false;
    }
    dirty = false;
    setSavedLight(saved, true);
    return true;
  }

  bool saveAs() {
    std::string p = askFile("Save document", "Save", path.empty() ? "note.rtf" : baseName(path).c_str(), path.empty() ? documentsDir() : dirOf(path));
    if (p.empty()) return false;
    if (!strlen(fileExt(p.c_str()))) p += ".rtf";
    setPath(p);
    return save();
  }

  bool canClose() override { return askSave(); }
};
AppFrame * NeatApp::inst = nullptr;

void openNeat(const char * path)
{
  if (!NeatApp::inst) new NeatApp();
  auto n = (NeatApp*)NeatApp::inst;
  n->open();
  if (path) { if (n->askSave()) n->loadFile(path); }
  else gApp->setFocusedWindow(n->rt);
}

// ====================================================================== Tidy Lite (spreadsheet)

class SheetApp : public AppFrame {
public:
  static AppFrame * inst;
  Sheet       sheet;
  SheetGrid * grid;
  uiLabel *   cellLbl;
  uiLabel *   saved;
  uiLabel *   nameLbl;
  std::string path;
  bool        dirty = false;

  SheetApp() : AppFrame("Tidy Lite", Point(4, 6), Size(632, 450), &inst) {
    button("New",     cx,       cy, 50)->onClick = [this]() { newDoc(); };
    button("Open",    cx + 54,  cy, 50)->onClick = [this]() { openDoc(); };
    button("Save",    cx + 108, cy, 50)->onClick = [this]() { save(); };
    button("Save as", cx + 162, cy, 64)->onClick = [this]() { saveAs(); };
    button("Print",   cx + 230, cy, 50)->onClick = [this]() { print(); };
    nameLbl = label("Untitled", cx + 288, cy + 3, 240, 18);
    saved = makeSavedLight(this, cx + 542, cy + 2);
    cellLbl = label("A1:", cx, cy + 28, 612, 18);
    cellLbl->labelStyle().backgroundColor = T.face;
    grid = new SheetGrid(this, Point(cx, cy + 50), Size(614, 340), &sheet);
    label("Type to fill a cell, Enter to confirm, F2 or double-click to edit.  =A1+B2  =SUM(A1:A9)  AVG MIN MAX COUNT ROUND IF",
          cx, cy + 394, 612, 16)->labelStyle().textFont = &FONT_std_12;
    grid->onSelect = [this]() { updateCell(); };
    grid->onModified = [this]() { if (!dirty) { dirty = true; setSavedLight(saved, false); } };
    grid->onKey = [this](uiKeyEventInfo const & k) {
      if (!k.CTRL) return false;
      if (k.VK == VK_s || k.VK == VK_S) { grid->commit(); save(); return true; }
      if (k.VK == VK_o || k.VK == VK_O) { openDoc(); return true; }
      if (k.VK == VK_p || k.VK == VK_P) { grid->commit(); print(); return true; }
      return false;
    };
    updateCell();
  }

  void print() {
    grid->commit();
    int maxR = -1, maxC = -1;
    for (auto & kv : sheet.cells) { maxR = imax(maxR, kv.first / Sheet::COLS); maxC = imax(maxC, kv.first % Sheet::COLS); }
    PrintDoc d;
    d.title = path.empty() ? std::string("Untitled sheet") : baseName(path);
    if (maxR < 0) { d.lines.push_back("(empty sheet)"); printDialog(d); return; }
    // column widths from the shown values
    std::vector<int> w(maxC + 1, 3);
    for (int r = 0; r <= maxR; ++r)
      for (int c = 0; c <= maxC; ++c) { bool n; w[c] = imax(w[c], imin(16, (int)sheet.display(r, c, n).size())); }
    std::string head = "    ";
    for (int c = 0; c <= maxC; ++c) { std::string h(1, 'A' + c); head += " " + h + std::string(w[c] - 1, ' '); }
    d.lines.push_back(head.substr(0, 80));
    d.lines.push_back(std::string(imin(80, (int)head.size()), '-'));
    for (int r = 0; r <= maxR; ++r) {
      char rn[8];
      snprintf(rn, sizeof(rn), "%3d ", r + 1);
      std::string line = rn;
      for (int c = 0; c <= maxC; ++c) {
        bool isNum;
        std::string v = sheet.display(r, c, isNum).substr(0, w[c]);
        std::string pad(w[c] - v.size(), ' ');
        line += " " + (isNum ? pad + v : v + pad);
      }
      while (!line.empty() && line.back() == ' ') line.pop_back();
      d.lines.push_back(line.substr(0, 80));
    }
    if ((int)head.size() > 80) d.lines.push_back("(columns past the page edge were left off)");
    printDialog(d);
  }

  void updateCell() {
    std::string s = Sheet::cellName(grid->selR, grid->selC) + ":  " + (grid->editing ? grid->edit + "_" : sheet.raw(grid->selR, grid->selC));
    cellLbl->setText(s.c_str());
    cellLbl->repaint();
  }

  void setPath(const std::string & p) {
    path = p;
    nameLbl->setText(p.empty() ? "Untitled" : baseName(p).c_str());
    nameLbl->repaint();
  }

  bool askSave() {
    grid->commit();
    if (!dirty) return true;
    auto r = gApp->messageBox("Tidy", "Save your changes first?", "Save", "Don't save", "Cancel", uiMessageBoxIcon::Question);
    if (r == uiMessageBoxResult::Button1) return save();
    return r == uiMessageBoxResult::Button2;
  }

  void loadFile(const char * p) {
    std::string s;
    if (!readFileStr(p, s, 64 * 1024)) { gApp->messageBox("Tidy", "Could not open that file.", "OK", nullptr, nullptr, uiMessageBoxIcon::Error); return; }
    sheet.fromCSV(s);
    grid->selR = grid->selC = grid->topR = grid->leftC = 0;
    grid->repaint();
    setPath(p);
    dirty = false;
    setSavedLight(saved, true);
    updateCell();
    gApp->setFocusedWindow(grid);
  }

  void newDoc() {
    if (!askSave()) return;
    sheet.cells.clear();
    grid->selR = grid->selC = grid->topR = grid->leftC = 0;
    grid->repaint();
    setPath("");
    dirty = false;
    setSavedLight(saved, true);
    updateCell();
  }

  void openDoc() {
    if (!askSave()) return;
    std::string p = askFile("Open sheet (.csv)", "Open", "", path.empty() ? documentsDir() : dirOf(path));
    if (!p.empty()) loadFile(p.c_str());
  }

  bool save() {
    grid->commit();
    if (path.empty()) return saveAs();
    if (!writeFileStr(path.c_str(), sheet.toCSV())) {
      gApp->messageBox("Tidy", "Could not save. Is the SD card in?", "OK", nullptr, nullptr, uiMessageBoxIcon::Error);
      return false;
    }
    dirty = false;
    setSavedLight(saved, true);
    return true;
  }

  bool saveAs() {
    std::string p = askFile("Save sheet", "Save", path.empty() ? "sheet.csv" : baseName(path).c_str(), path.empty() ? documentsDir() : dirOf(path));
    if (p.empty()) return false;
    if (strcmp(fileExt(p.c_str()), "csv")) p += ".csv";
    setPath(p);
    return save();
  }

  bool canClose() override { return askSave(); }
};
AppFrame * SheetApp::inst = nullptr;

void openTidy(const char * path)
{
  if (!SheetApp::inst) new SheetApp();
  auto t = (SheetApp*)SheetApp::inst;
  t->open();
  if (path) { if (t->askSave()) t->loadFile(path); }
  else gApp->setFocusedWindow(t->grid);
}

// ====================================================================== Ready Lite (to-do list)

class ReadyApp : public AppFrame {
public:
  static AppFrame * inst;
  uiTextEdit * newTask;
  uiListBox *  list;
  uiLabel *    progress;
  uiLabel *    saved;
  std::vector<std::pair<bool, std::string>> items;

  ReadyApp() : AppFrame("Ready Lite - To do", nextPos(420, 380), Size(420, 380), &inst) {
    newTask = edit("", cx, cy, 300);
    button("Add", cx + 306, cy, 96)->onClick = [this]() { add(); };
    newTask->onKeyType = [this](uiKeyEventInfo const & k) { if (k.VK == VK_RETURN || k.VK == VK_KP_ENTER) add(); };
    list = S(new uiListBox(this, Point(cx, cy + 30), Size(402, 240)));
    list->items().takeStrings();
    list->listBoxStyle().itemHeight = 18;
    list->onDblClick = [this]() { toggle(); };
    button("Done / undo", cx,       cy + 278, 110)->onClick = [this]() { toggle(); };
    button("Delete",      cx + 116, cy + 278, 80)->onClick  = [this]() { del(); };
    button("Clear done",  cx + 202, cy + 278, 100)->onClick = [this]() { clearDone(); };
    button("Print",       cx + 308, cy + 278, 94)->onClick  = [this]() {
      PrintDoc d;
      d.title = "To do";
      for (auto & it : items) d.lines.push_back(std::string(it.first ? "[x] " : "[ ] ") + it.second);
      if (items.empty()) d.lines.push_back("(nothing on the list)");
      printDialog(d);
    };
    progress = label("", cx, cy + 310, 320, 18);
    saved = makeSavedLight(this, cx + 332, cy + 310);
    load();
    refresh();
    gApp->setFocusedWindow(newTask);
  }

  void load() {
    std::string s;
    items.clear();
    if (!readFileStr(dataPath("ready.txt").c_str(), s)) return;
    size_t st = 0;
    for (size_t i = 0; i <= s.size(); ++i)
      if (i == s.size() || s[i] == '\n') {
        std::string l = s.substr(st, i - st);
        st = i + 1;
        if (l.size() >= 2 && l[1] == '|') items.push_back({ l[0] == '1', l.substr(2) });
      }
  }

  void save() {
    std::string s;
    for (auto & it : items) s += std::string(it.first ? "1|" : "0|") + it.second + "\n";
    setSavedLight(saved, writeFileStr(dataPath("ready.txt").c_str(), s));
  }

  void refresh(int sel = -1) {
    list->items().clear();
    int done = 0;
    for (auto & it : items) {
      std::string l = std::string(it.first ? "[x] " : "[ ] ") + it.second;
      list->items().append(l.c_str());
      if (it.first) ++done;
    }
    if (sel >= 0 && sel < (int)items.size()) list->selectItem(sel);
    list->repaint();
    char b[80];
    if (items.empty()) snprintf(b, sizeof(b), "Nothing to do yet - add your first task!");
    else if (done == (int)items.size()) snprintf(b, sizeof(b), "All %d done. Nice work!", done);
    else snprintf(b, sizeof(b), "%d of %d done", done, (int)items.size());
    progress->setText(b);
    progress->repaint();
  }

  void add() {
    std::string t = newTask->text();
    if (t.empty()) return;
    items.push_back({ false, t });
    newTask->setText("");
    newTask->repaint();
    save();
    refresh(items.size() - 1);
  }

  void toggle() {
    int i = list->firstSelectedItem();
    if (i < 0 || i >= (int)items.size()) return;
    items[i].first = !items[i].first;
    save();
    refresh(i);
  }

  void del() {
    int i = list->firstSelectedItem();
    if (i < 0 || i >= (int)items.size()) return;
    items.erase(items.begin() + i);
    save();
    refresh(imin(i, (int)items.size() - 1));
  }

  void clearDone() {
    items.erase(std::remove_if(items.begin(), items.end(), [](const std::pair<bool, std::string> & p) { return p.first; }), items.end());
    save();
    refresh();
  }
};
AppFrame * ReadyApp::inst = nullptr;

void openReady()
{
  if (!ReadyApp::inst) new ReadyApp();
  ReadyApp::inst->open();
}

// ====================================================================== Steady Lite (bills)

struct Bill { std::string name; double amount; int day; bool paid; };

class SteadyApp : public AppFrame {
public:
  static AppFrame * inst;
  uiTextEdit * nameEd, * amtEd, * dayEd;
  uiListBox *  list;
  uiLabel *    summary, * cheer, * saved;
  std::vector<Bill> bills;

  SteadyApp() : AppFrame("Steady Lite - Bills", nextPos(540, 410), Size(540, 410), &inst) {
    label("Bill", cx, cy + 3, 30, 18);
    nameEd = edit("", cx + 30, cy, 190);
    label("$", cx + 228, cy + 3, 12, 18);
    amtEd = edit("", cx + 240, cy, 80);
    label("Due day", cx + 328, cy + 3, 56, 18);
    dayEd = edit("", cx + 386, cy, 36);
    button("Add", cx + 428, cy, 92)->onClick = [this]() { add(); };
    dayEd->onKeyType = [this](uiKeyEventInfo const & k) { if (k.VK == VK_RETURN || k.VK == VK_KP_ENTER) add(); };
    list = S(new uiListBox(this, Point(cx, cy + 30), Size(520, 230)));
    list->items().takeStrings();
    list->listBoxStyle().itemHeight = 18;
    list->listBoxStyle().textFont = &FONT_8x14;
    list->onDblClick = [this]() { togglePaid(); };
    button("Paid / not paid", cx,       cy + 268, 130)->onClick = [this]() { togglePaid(); };
    button("Delete",          cx + 136, cy + 268, 80)->onClick  = [this]() { del(); };
    button("New month",       cx + 222, cy + 268, 100)->onClick = [this]() { newMonth(); };
    button("Print",           cx + 328, cy + 268, 70)->onClick  = [this]() {
      PrintDoc d;
      d.title = "Bills";
      for (int i = 0; i < list->items().count(); ++i) d.lines.push_back(list->items().get(i));
      d.lines.push_back("");
      d.lines.push_back(summary->text());
      d.lines.push_back(cheer->text());
      printDialog(d);
    };
    saved = makeSavedLight(this, cx + 450, cy + 270);
    summary = label("", cx, cy + 300, 520, 18);
    cheer   = label("", cx, cy + 322, 520, 18);
    cheer->labelStyle().textColor = T.accent;
    load();
    refresh();
    gApp->setFocusedWindow(nameEd);
  }

  void load() {
    std::string s;
    bills.clear();
    if (!readFileStr(dataPath("steady.csv").c_str(), s)) return;
    Sheet tmp;
    tmp.fromCSV(s);
    for (int r = 0; r < Sheet::ROWS; ++r) {
      const std::string & n = tmp.raw(r, 0);
      if (n.empty()) continue;
      bills.push_back({ n, atof(tmp.raw(r, 1).c_str()), atoi(tmp.raw(r, 2).c_str()), tmp.raw(r, 3) == "1" });
    }
  }

  void save() {
    Sheet tmp;
    for (size_t i = 0; i < bills.size() && i < (size_t)Sheet::ROWS; ++i) {
      char b[32];
      tmp.set(i, 0, bills[i].name);
      snprintf(b, sizeof(b), "%.2f", bills[i].amount); tmp.set(i, 1, b);
      tmp.set(i, 2, std::to_string(bills[i].day));
      tmp.set(i, 3, bills[i].paid ? "1" : "0");
    }
    setSavedLight(saved, writeFileStr(dataPath("steady.csv").c_str(), tmp.toCSV()));
  }

  static const char * suffix(int d) {
    if (d >= 11 && d <= 13) return "th";
    switch (d % 10) { case 1: return "st"; case 2: return "nd"; case 3: return "rd"; default: return "th"; }
  }

  void refresh(int sel = -1) {
    std::stable_sort(bills.begin(), bills.end(), [](const Bill & a, const Bill & b) { return a.day < b.day; });
    list->items().clear();
    double total = 0, paid = 0;
    int npaid = 0;
    for (auto & b : bills) {
      char line[96];
      char nm[25];
      strlcpy(nm, b.name.c_str(), sizeof(nm));
      snprintf(line, sizeof(line), "%-5s %-24s %10s   due %2d%s", b.paid ? "PAID" : "", nm, fmtMoney(b.amount).c_str(), b.day, suffix(b.day));
      list->items().append(line);
      total += b.amount;
      if (b.paid) { paid += b.amount; ++npaid; }
    }
    if (sel >= 0 && sel < (int)bills.size()) list->selectItem(sel);
    list->repaint();
    std::string s = "This month: " + fmtMoney(total) + " total   " + fmtMoney(paid) + " paid   " + fmtMoney(total - paid) + " left";
    summary->setText(s.c_str());
    summary->repaint();
    char c[100];
    if (bills.empty()) snprintf(c, sizeof(c), "Add your bills above. A plan is the first step - you've got this!");
    else if (npaid == 0) snprintf(c, sizeof(c), "%d bills on the plan. Pay the first one and watch this fill up.", (int)bills.size());
    else if (npaid < (int)bills.size()) snprintf(c, sizeof(c), "%d of %d paid - keep going, you're making real progress!", npaid, (int)bills.size());
    else snprintf(c, sizeof(c), "Every bill is paid this month. Great job!");
    cheer->setText(c);
    cheer->repaint();
  }

  void add() {
    std::string n = nameEd->text();
    if (n.empty()) { gApp->setFocusedWindow(nameEd); return; }
    double a = atof(amtEd->text());
    int d = atoi(dayEd->text());
    if (d < 1 || d > 31) d = 1;
    bills.push_back({ n, a, d, false });
    nameEd->setText(""); amtEd->setText(""); dayEd->setText("");
    nameEd->repaint(); amtEd->repaint(); dayEd->repaint();
    save();
    refresh();
    gApp->setFocusedWindow(nameEd);
  }

  void togglePaid() {
    int i = list->firstSelectedItem();
    if (i < 0 || i >= (int)bills.size()) return;
    bills[i].paid = !bills[i].paid;
    save();
    refresh(i);
  }

  void del() {
    int i = list->firstSelectedItem();
    if (i < 0 || i >= (int)bills.size()) return;
    if (gApp->messageBox("Steady", "Delete this bill?", "Delete", "Cancel", nullptr, uiMessageBoxIcon::Question) != uiMessageBoxResult::Button1) return;
    bills.erase(bills.begin() + i);
    save();
    refresh();
  }

  void newMonth() {
    if (gApp->messageBox("Steady", "Start a new month? All bills go back to not paid.", "Yes", "Cancel", nullptr, uiMessageBoxIcon::Question) != uiMessageBoxResult::Button1) return;
    for (auto & b : bills) b.paid = false;
    save();
    refresh();
  }
};
AppFrame * SteadyApp::inst = nullptr;

void openSteady()
{
  if (!SteadyApp::inst) new SteadyApp();
  SteadyApp::inst->open();
}

// ====================================================================== Browser

class BrowserApp : public AppFrame {
public:
  static AppFrame * inst;
  uiTextEdit * urlEd;
  uiLabel *    status;
  TextArea *   view;
  std::vector<std::string> links;
  std::vector<std::string> history;
  std::string  current;

  BrowserApp() : AppFrame("Browser", Point(4, 4), Size(632, 452), &inst) {
    button("<",    cx,      cy, 28)->onClick = [this]() { back(); };
    button("Home", cx + 32, cy, 48)->onClick = [this]() { go(gSet.home.c_str(), true); };
    urlEd = edit("", cx + 84, cy, 470);
    button("Go",   cx + 558, cy, 54)->onClick = [this]() { goTyped(); };
    urlEd->onKeyType = [this](uiKeyEventInfo const & k) { if (k.VK == VK_RETURN || k.VK == VK_KP_ENTER) goTyped(); };
    status = label("Type an address or some words to search, then press Enter.", cx, cy + 27, 612, 16);
    status->labelStyle().textFont = &FONT_std_12;
    view = new TextArea(this, Point(cx, cy + 46), Size(614, 374), false);
    view->onClickChar = [this](int r, int c) { clickAt(r, c); };
    view->onKey = [this](uiKeyEventInfo const & k) {
      if (k.VK == VK_BACKSPACE || (k.LALT && k.VK == VK_LEFT)) { back(); return true; }
      if (k.CTRL && (k.VK == VK_l || k.VK == VK_L)) { gApp->setFocusedWindow(urlEd); return true; }
      return false;
    };
    std::vector<std::string> intro = {
      "TidyOS Browser",
      "",
      "* Type a web address (example: frogfind.com) or a few words to search.",
      "* Click a [number] after a link to follow it, or type the number in the",
      "  address box and press Enter.",
      "* Backspace goes back.  Ctrl+L jumps to the address box.",
      "",
      "Pages are shown as plain text. For the best results, run the TidyOS",
      "browser helper (tidyos_proxy.py) on your Pi or The Shelf and enter its",
      "address in Settings.  Without it, secure sites open through FrogFind.",
    };
    view->setLines(std::move(intro));
    startTimer(250);
    onTimer = [this](uiTimerHandle) { poll(); };
    gApp->setFocusedWindow(urlEd);
  }

  void goTyped() {
    std::string t = urlEd->text();
    bool num = !t.empty() && t.size() < 4;
    for (char c : t) if (!isdigit((unsigned char)c)) num = false;
    if (num) { follow(atoi(t.c_str())); return; }
    go(urlFromInput(t).c_str(), true);
  }

  void go(const char * url, bool push) {
    if (!url || !*url) return;
    if (fetchState() == FETCH_BUSY) return;
    if (push && !current.empty()) {
      history.push_back(current);
      if (history.size() > 25) history.erase(history.begin());
    }
    current = url;
    urlEd->setText(url);
    urlEd->repaint();
    status->setText("Loading...");
    status->repaint();
    fetchStart(url);
  }

  void back() {
    if (history.empty()) return;
    std::string u = history.back();
    history.pop_back();
    go(u.c_str(), false);
  }

  void follow(int n) {
    if (n < 1 || n > (int)links.size()) { status->setText("No link with that number."); status->repaint(); return; }
    go(links[n - 1].c_str(), true);
  }

  void clickAt(int r, int c) {
    if (r < 0 || r >= (int)view->lines.size()) return;
    const std::string & l = view->lines[r];
    // marker under the click?
    int s = imin(c, (int)l.size() - 1);
    for (int i = s; i >= 0 && i > s - 6; --i) {
      if (l[i] == '[') {
        int e = l.find(']', i);
        if (e != (int)std::string::npos && e >= c) {
          int n = atoi(l.c_str() + i + 1);
          if (n > 0) { follow(n); return; }
        }
        break;
      }
    }
    // otherwise first marker to the right on this line
    size_t p = l.find('[', imax(0, c));
    if (p != std::string::npos && (int)p - c < 40 && isdigit((unsigned char)l[p + 1])) {
      int n = atoi(l.c_str() + p + 1);
      if (n > 0) follow(n);
    }
  }

  void poll() {
    FetchState st = fetchState();
    if (st != FETCH_DONE && st != FETCH_ERROR) return;
    std::string r = fetchTake();
    std::vector<std::string> lines;
    links.clear();
    if (st == FETCH_ERROR) {
      status->setText("Could not load the page.");
      status->repaint();
      wrapText(r, view->visCols(), lines);
      view->setLines(std::move(lines));
      return;
    }
    std::string title, finalUrl, body;
    size_t sep = r.find("\n---\n");
    std::string head = sep == std::string::npos ? "" : r.substr(0, sep);
    size_t bodyStart = sep == std::string::npos ? 0 : sep + 5;
    size_t lk = r.find("\n#LINKS#\n", bodyStart);
    body = r.substr(bodyStart, lk == std::string::npos ? std::string::npos : lk - bodyStart);
    if (lk != std::string::npos) {
      size_t p = lk + 9;
      while (p < r.size()) {
        size_t e = r.find('\n', p);
        if (e == std::string::npos) e = r.size();
        std::string l = r.substr(p, e - p);
        p = e + 1;
        size_t sp = l.find(' ');
        if (sp != std::string::npos) links.push_back(l.substr(sp + 1));
      }
    }
    size_t t = head.find("TITLE: ");
    if (t != std::string::npos) title = head.substr(t + 7, head.find('\n', t) - t - 7);
    r.clear();
    r.shrink_to_fit();
    wrapText(body, view->visCols(), lines);
    body.clear();
    view->setLines(std::move(lines));
    setTitle(title.empty() ? "Browser" : (std::string("Browser - ") + title.substr(0, 60)).c_str());
    repaint();
    char b[64];
    snprintf(b, sizeof(b), "%d links.  Click a [number] to follow it.", (int)links.size());
    status->setText(b);
    status->repaint();
    gApp->setFocusedWindow(view);
  }
};
AppFrame * BrowserApp::inst = nullptr;

void openBrowser(const char * url)
{
  if (!BrowserApp::inst) new BrowserApp();
  auto b = (BrowserApp*)BrowserApp::inst;
  b->open();
  if (url) b->go(url, true);
}

// ====================================================================== Files

class FilesApp : public AppFrame {
public:
  static AppFrame * inst;
  uiFileBrowser * fb;
  uiLabel * info;
  uiButton * shareBtn;

  void updateShare() {
    shareBtn->setText(fileServerOn() ? "Wi-Fi sharing: ON" : "Wi-Fi sharing: off");
    shareBtn->buttonStyle().backgroundColor = fileServerOn() ? T.accent : T.face;
    shareBtn->buttonStyle().textColor = fileServerOn() ? T.accentText : T.text;
    shareBtn->repaint();
    updateInfo();
  }

  FilesApp() : AppFrame("Files", nextPos(470, 360), Size(470, 360), &inst) {
    fb = S(new uiFileBrowser(this, Point(cx, cy), Size(320, 300)));
    fb->setDirectory(gSDMounted ? SD_MOUNT : FLASH_MOUNT);
    fb->onDblClick = [this]() { if (!fb->isDirectory()) openSel(); };
    fb->onChange = [this]() { updateInfo(); };
    int bx = cx + 328;
    button("Open",       bx, cy,       122)->onClick = [this]() { if (fb->isDirectory()) { fb->changeDirectory(fb->filename()); updateInfo(); } else openSel(); };
    button("New folder", bx, cy + 28,  122)->onClick = [this]() { newFolder(); };
    button("Rename",     bx, cy + 56,  122)->onClick = [this]() { rename(); };
    button("Delete",     bx, cy + 84,  122)->onClick = [this]() { del(); };
    button("New note",   bx, cy + 124, 122)->onClick = []() { openNeat(nullptr); };
    button("New sheet",  bx, cy + 152, 122)->onClick = []() { openTidy(nullptr); };
    shareBtn = button("", bx, cy + 192, 122, 40);
    shareBtn->onClick = [this]() { fileServerSet(!fileServerOn()); updateShare(); };
    updateShare();
    info = label("", cx, cy + 306, 440, 18);
    info->labelStyle().textFont = &FONT_std_12;
    updateInfo();
  }

  std::string selPath() { return joinPath(fb->directory(), fb->filename()); }

  void openSel() {
    if (!fb->filename() || !*fb->filename()) return;
    std::string p = selPath();
    const char * e = fileExt(p.c_str());
    if (!strcmp(e, "csv") || !strcmp(e, "tsv")) openTidy(p.c_str());
    else openNeat(p.c_str());
  }

  void newFolder() {
    char n[32] = "";
    if (gApp->inputBox("New folder", "Name", n, sizeof(n) - 1, "Create", "Cancel") == uiMessageBoxResult::Button1 && n[0]) {
      fb->content().makeDirectory(n);
      fb->update();
    }
  }

  void rename() {
    if (!fb->filename()) return;
    char n[48];
    strlcpy(n, fb->filename(), sizeof(n));
    if (gApp->inputBox("Rename", "New name", n, sizeof(n) - 1, "Rename", "Cancel") == uiMessageBoxResult::Button1 && n[0]) {
      fb->content().rename(fb->filename(), n);
      fb->update();
    }
  }

  void del() {
    if (!fb->filename() || !strcmp(fb->filename(), "..")) return;
    std::string q = std::string("Delete \"") + fb->filename() + "\"?";
    if (gApp->messageBox("Delete", q.c_str(), "Delete", "Cancel", nullptr, uiMessageBoxIcon::Warning) == uiMessageBoxResult::Button1) {
      fb->content().remove(fb->filename());
      fb->update();
      updateInfo();
    }
  }

  void updateInfo() {
    int64_t total = 0, used = 0;
    bool sd = strncmp(fb->directory(), SD_MOUNT, strlen(SD_MOUNT)) == 0;
    char b[96];
    std::string url = fileServerUrl();
    if (!url.empty())
      snprintf(b, sizeof(b), "Sharing on Wi-Fi: open %s in a browser", url.c_str());
    else if (FileBrowser::getFSInfo(sd ? DriveType::SDCard : DriveType::SPIFFS, 0, &total, &used))
      snprintf(b, sizeof(b), "%s   %lld MB free of %lld MB", fb->directory(), (total - used) >> 20, total >> 20);
    else
      snprintf(b, sizeof(b), "%s", fb->directory());
    info->setText(b);
    info->repaint();
  }
};
AppFrame * FilesApp::inst = nullptr;

void openFiles()
{
  if (!FilesApp::inst) new FilesApp();
  FilesApp::inst->open();
}

// ====================================================================== Settings

class SettingsApp : public AppFrame {
public:
  static AppFrame * inst;
  uiTextEdit * ssidEd, * passEd, * proxyEd, * homeEd, * tzEd;
  uiListBox  * nets, * devs;
  uiLabel    * wifiLbl, * btLbl;
  uiCheckBox * dstCb, * shareCb;
  uiLabel    * shareLbl;
  uiButton   * colorsBtn;
  std::vector<std::string> scan;
  bool scanning = false;
  int  devCount = -1;
  int  colors;

  SettingsApp() : AppFrame("Settings", Point(20, 2), Size(600, 454), &inst) {
    colors = gSet.colors;
    int y = cy;
    head("Wi-Fi", y);
    label("Network",  cx, y + 22, 70, 18);
    ssidEd = edit(gSet.ssid.c_str(), cx + 72, y + 19, 200);
    label("Password", cx, y + 48, 70, 18);
    passEd = edit(gSet.pass.c_str(), cx + 72, y + 45, 200);
    passEd->textEditProps().passwordMode = true;
    button("Connect",       cx + 72,  y + 71, 96)->onClick = [this]() {
      if (screenApOn() && !netConnected()) {
        std::string m = std::string("Next: put this device on ") + ssidEd->text() + ", open tidyos.local";
        if (gApp->messageBox("Joining your Wi-Fi", m.c_str(), "Join", "Cancel", nullptr, uiMessageBoxIcon::Info) != uiMessageBoxResult::Button1) return;
      }
      netConnect(ssidEd->text(), passEd->text());
      settingsSave();
    };
    button("Find networks", cx + 290, y + 19, 120)->onClick = [this]() { netScanStart(); scanning = true; setWifiText("Looking for networks..."); };
    nets = S(new uiListBox(this, Point(cx + 290, y + 45), Size(296, 64)));
    nets->items().takeStrings();
    nets->onChange = [this]() {
      int i = nets->firstSelectedItem();
      if (i >= 0 && i < (int)scan.size()) { ssidEd->setText(scan[i].c_str()); ssidEd->repaint(); gApp->setFocusedWindow(passEd); }
    };
    wifiLbl = label("", cx + 176, y + 74, 110, 18);
    wifiLbl->labelStyle().textFont = &FONT_std_12;

    y += 116;
    head("Bluetooth keyboard and mouse", y);
    button("Find devices", cx,       y + 19, 110)->onClick = [this]() { inputStartPairScan(12); devCount = -1; };
    button("Pair",         cx,       y + 45, 110)->onClick = [this]() { int i = devs->firstSelectedItem(); if (i >= 0) inputPair(i); };
    button("Forget all",   cx,       y + 71, 110)->onClick = [this]() {
      if (gApp->messageBox("Bluetooth", "Forget every paired keyboard and mouse?", "Forget", "Cancel", nullptr, uiMessageBoxIcon::Warning) == uiMessageBoxResult::Button1)
        inputForgetAll();
    };
    devs = S(new uiListBox(this, Point(cx + 118, y + 19), Size(468, 54)));
    devs->items().takeStrings();
    devs->onDblClick = [this]() { int i = devs->firstSelectedItem(); if (i >= 0) inputPair(i); };
    btLbl = label("", cx + 118, y + 76, 468, 18);
    btLbl->labelStyle().textFont = &FONT_std_12;

    y += 100;
    head("Browser", y);
    label("Helper address", cx, y + 22, 100, 18);
    proxyEd = edit(gSet.proxy.c_str(), cx + 104, y + 19, 180);
    label("(Pi / The Shelf, e.g. 192.168.1.50:8088 - leave empty to skip)", cx + 290, y + 22, 300, 18)->labelStyle().textFont = &FONT_std_12;
    label("Home page", cx, y + 48, 100, 18);
    homeEd = edit(gSet.home.c_str(), cx + 104, y + 45, 300);

    y += 74;
    head("Clock and screen", y);
    label("UTC offset (hours)", cx, y + 22, 120, 18);
    char tz[12];
    snprintf(tz, sizeof(tz), "%g", gSet.tzHours10 / 10.0);
    tzEd = edit(tz, cx + 124, y + 19, 50);
    dstCb = S(new uiCheckBox(this, Point(cx + 186, y + 22), Size(16, 16)));
    dstCb->setChecked(gSet.dst);
    label("Daylight saving", cx + 208, y + 22, 110, 18);
    colorsBtn = button("", cx + 330, y + 19, 256);
    colorsBtn->onClick = [this]() { colors = colors == 0 ? 8 : colors == 8 ? 4 : 0; showColors(); };
    showColors();

    y += 50;
    head("File sharing", y);
    shareCb = S(new uiCheckBox(this, Point(cx, y + 22), Size(16, 16)));
    shareCb->setChecked(fileServerOn());
    shareCb->onChange = [this]() { fileServerSet(shareCb->checked()); };
    label("Share the SD card on Wi-Fi (open the address in any browser)", cx + 22, y + 22, 380, 18);
    shareLbl = label("", cx + 404, y + 22, 182, 18);
    shareLbl->labelStyle().textColor = T.accent;

    y += 48;
    button("Save",           cx + 300, y, 136, 26)->onClick = [this]() { saveAll(); gApp->messageBox("Settings", "Saved.", "OK", nullptr, nullptr, uiMessageBoxIcon::Info); };
    button("Save + restart", cx + 450, y, 136, 26)->onClick = [this]() { saveAll(); delay(300); ESP.restart(); };
    button("Check for updates", cx, y, 150, 26)->onClick = [this]() {
      if (!updateLoaderPresent()) {
        gApp->messageBox("Updates", "Install once from the web flasher to enable this.", "OK", nullptr, nullptr, uiMessageBoxIcon::Info);
        return;
      }
      if (gApp->messageBox("Updates", "Restart and check GitHub? Needs Wi-Fi and the SD card.", "Check", "Cancel", nullptr, uiMessageBoxIcon::Question) == uiMessageBoxResult::Button1) {
        saveAll();
        updateRequest();
      }
    };
    label("Colours apply after restart.", cx + 156, y + 5, 140, 18)->labelStyle().textFont = &FONT_std_12;

    startTimer(500);
    onTimer = [this](uiTimerHandle) { tick(); };
    tick();
  }

  void head(const char * t, int y) {
    auto l = label(t, cx, y, 586, 16);
    l->labelStyle().backgroundColor = T.accent;
    l->labelStyle().textColor = T.accentText;
    l->labelStyle().textFont = &FONT_std_12;
  }

  void showColors() {
    char b[64];
    if (colors == 0) snprintf(b, sizeof(b), "Colours: Auto (now %d)", gColors);
    else snprintf(b, sizeof(b), "Colours: %d (now %d)", colors, gColors);
    colorsBtn->setText(b);
    colorsBtn->repaint();
  }

  void setWifiText(const char * t) { wifiLbl->setText(t); wifiLbl->repaint(); }

  void saveAll() {
    gSet.ssid  = ssidEd->text();
    gSet.pass  = passEd->text();
    gSet.proxy = proxyEd->text();
    gSet.proxy.trim();
    if (gSet.proxy.startsWith("http://")) gSet.proxy = gSet.proxy.substring(7);
    while (gSet.proxy.endsWith("/")) gSet.proxy.remove(gSet.proxy.length() - 1);
    gSet.home  = homeEd->text();
    gSet.tzHours10 = (int)lround(atof(tzEd->text()) * 10);
    gSet.dst   = dstCb->checked();
    gSet.colors = colors;
    settingsSave();
    netApplyClock();
  }

  void tick() {
    if (scanning) {
      int n = netScanResults(scan);
      if (n >= 0) {
        scanning = false;
        nets->items().clear();
        for (auto & s : scan) nets->items().append(s.c_str());
        nets->repaint();
        setWifiText(n ? "Pick your network" : "No networks found");
      }
    } else {
      setWifiText(netConnected() ? "Connected!" : (gSet.ssid.length() ? "Not connected" : ""));
    }
    int n = inputFoundCount();
    if (n != devCount) {
      devCount = n;
      int sel = devs->firstSelectedItem();
      devs->items().clear();
      for (int i = 0; i < n; ++i) {
        FoundDev d;
        if (!inputGetFound(i, d)) continue;
        std::string l = d.name + (d.isKbd ? "  (keyboard)" : d.isMouse ? "  (mouse)" : "") + (d.bonded ? "  - paired before" : "");
        devs->items().append(l.c_str());
      }
      if (sel >= 0 && sel < n) devs->selectItem(sel);
      devs->repaint();
    }
    std::string u = fileServerUrl();
    std::string sh = fileServerOn() ? (u.empty() ? std::string("waiting for Wi-Fi") : u) : std::string("off");
    if (sh != shareLbl->text()) { shareLbl->setText(sh.c_str()); shareLbl->repaint(); }
    std::string s = inputStatus();
    if (inputScanning()) s += "   (searching - put the device in pairing mode)";
    btLbl->setText(s.c_str());
    btLbl->repaint();
  }
};
AppFrame * SettingsApp::inst = nullptr;

void openSettings()
{
  if (!SettingsApp::inst) new SettingsApp();
  SettingsApp::inst->open();
}

// ====================================================================== Task Manager -> see taskmgr section at the end

// ====================================================================== Messages (The Shelf + Coms mesh)

static std::string clockText(uint64_t t)
{
  if (!t) return "";
  time_t s = t / 1000;
  struct tm tmv;
  localtime_r(&s, &tmv);
  char b[24];
  int h = tmv.tm_hour % 12; if (!h) h = 12;
  time_t now = time(nullptr);
  struct tm tn;
  localtime_r(&now, &tn);
  if (tn.tm_yday == tmv.tm_yday && tn.tm_year == tmv.tm_year) snprintf(b, sizeof(b), "%d:%02d %s", h, tmv.tm_min, tmv.tm_hour < 12 ? "AM" : "PM");
  else snprintf(b, sizeof(b), "%d/%d %d:%02d%s", tmv.tm_mon + 1, tmv.tm_mday, h, tmv.tm_min, tmv.tm_hour < 12 ? "a" : "p");
  return b;
}

class MessagesApp : public AppFrame {
public:
  static AppFrame * inst;
  uiButton *   shelfBtn, * comsBtn, * addBtn;
  uiTextEdit * nameEd, * msgEd;
  uiListBox *  list;
  uiLabel *    listHead, * status, * convHead;
  TextArea *   view;
  bool         coms = false;
  uint32_t     seen = 0;
  std::vector<MsgPeer> peers;
  std::string  current;      // selected peer id

  MessagesApp() : AppFrame("Messages", Point(10, 10), Size(620, 442), &inst) {
    shelfBtn = button("The Shelf (Wi-Fi)",     cx,       cy, 150);
    comsBtn  = button("Coms mesh (2.4 GHz)",   cx + 154, cy, 160);
    shelfBtn->onClick = [this]() { setMode(false); };
    comsBtn->onClick  = [this]() { setMode(true); };
    label("My name", cx + 326, cy + 3, 56, 18);
    nameEd = edit(msgMyName().c_str(), cx + 384, cy, 150);
    button("Save", cx + 538, cy, 62)->onClick = [this]() { msgSetName(nameEd->text()); status->setText("Name saved."); status->repaint(); };

    listHead = label("", cx, cy + 30, 186, 18);
    list = S(new uiListBox(this, Point(cx, cy + 50), Size(186, 286)));
    list->items().takeStrings();
    list->listBoxStyle().itemHeight = 18;
    list->onChange = [this]() { pick(); };
    addBtn = button("Add by address", cx, cy + 342, 186);
    addBtn->onClick = [this]() {
      char a[48] = "";
      if (gApp->inputBox("Add a Shelf", "Address (like 192.168.1.20 or pi.local:8080)", a, sizeof(a) - 1, "Add", "Cancel") == uiMessageBoxResult::Button1 && a[0]) {
        msgAddAddress(a);
        status->setText("Checking that address...");
        status->repaint();
      }
    };

    convHead = label("", cx + 194, cy + 30, 406, 18);
    view = new TextArea(this, Point(cx + 194, cy + 50), Size(406, 286), false);
    view->setFocusIndex(-1);
    msgEd = edit("", cx + 194, cy + 342, 330);
    button("Send", cx + 528, cy + 342, 72)->onClick = [this]() { send(); };
    msgEd->onKeyType = [this](uiKeyEventInfo const & k) { if (k.VK == VK_RETURN || k.VK == VK_KP_ENTER) send(); };
    status = label("", cx, cy + 372, 600, 32);
    status->labelStyle().textFont = &FONT_std_12;

    startTimer(500);
    onTimer = [this](uiTimerHandle) { tick(); };
    setMode(false);
  }

  void setMode(bool c) {
    coms = c;
    shelfBtn->buttonStyle().backgroundColor = c ? T.face : T.accent;
    shelfBtn->buttonStyle().textColor       = c ? T.text : T.accentText;
    comsBtn->buttonStyle().backgroundColor  = c ? T.accent : T.face;
    comsBtn->buttonStyle().textColor        = c ? T.accentText : T.text;
    shelfBtn->repaint(); comsBtn->repaint();
    addBtn->setText(c ? "(boards found by radio)" : "Add by address");
    addBtn->repaint();
    listHead->setText(c ? "Boards on the mesh" : "Shelves");
    listHead->repaint();
    seen = 0;
    tick();
    gApp->setFocusedWindow(msgEd);
  }

  void pick() {
    if (coms) return;
    int i = list->firstSelectedItem();
    if (i >= 0 && i < (int)peers.size()) { current = peers[i].id; showConversation(); }
  }

  void showLines(const std::vector<ChatLine> & chat, bool showHops) {
    std::vector<std::string> lines;
    int w = view->visCols();
    for (auto & c : chat) {
      std::string head = (c.in ? c.who : std::string("Me")) + "  " + clockText(c.t);
      if (showHops && c.in && c.hops) head += "  (" + std::to_string(c.hops) + " hop" + (c.hops == 1 ? ")" : "s)");
      if (showHops && !c.in && c.receipts) head += "  - received by " + std::to_string(c.receipts);
      if (c.failed) head += "  - NOT DELIVERED";
      lines.push_back(c.in ? head : std::string(">> ") + head);
      wrapText(c.text, w - 2, lines);
      lines.push_back("");
    }
    if (chat.empty()) lines.push_back(coms ? "No mesh messages yet." : "No messages yet. Say hello!");
    int vr = view->visRows();
    view->setLines(std::move(lines));
    view->top = imax(0, (int)view->lines.size() - vr);
    view->repaint();
  }

  void showConversation() {
    std::vector<ChatLine> chat;
    std::string name;
    for (auto & p : peers) if (p.id == current) name = p.name + (p.online ? "  (online)" : "  (offline)");
    convHead->setText(name.empty() ? "Pick a Shelf on the left" : name.c_str());
    convHead->repaint();
    if (!current.empty()) msgConversation(current, chat);
    showLines(chat, false);
  }

  void tick() {
    uint32_t v = msgVersion();
    if (coms) {
      char b[160];
      int ch = comsChannel();
      snprintf(b, sizeof(b), comsUp() ? "Coms mesh on Wi-Fi channel %d. Shelf boards must be on the same channel to hear each other\n(boards on their own Wi-Fi use channel 1; boards on your home Wi-Fi use its channel)." : "The 2.4 GHz radio couldn't start the mesh.", ch);
      if (strcmp(status->text(), b)) { status->setText(b); status->repaint(); }
    } else {
      std::string st = msgStatus();
      if (st != status->text()) { status->setText(st.c_str()); status->repaint(); }
    }
    if (v == seen) return;
    seen = v;
    if (coms) {
      std::vector<std::string> members;
      comsMembers(members);
      list->items().clear();
      for (auto & m : members) list->items().append(m.c_str());
      if (members.empty()) list->items().append("(none heard yet)");
      list->repaint();
      convHead->setText("Everyone on the mesh");
      convHead->repaint();
      std::vector<ChatLine> chat;
      comsLog(chat);
      comsMarkRead();
      showLines(chat, true);
    } else {
      msgPeers(peers);
      int sel = -1;
      list->items().clear();
      for (size_t i = 0; i < peers.size(); ++i) {
        std::string l = std::string(peers[i].online ? "* " : "  ") + peers[i].name;
        if (peers[i].unread) l += "  (" + std::to_string(peers[i].unread) + ")";
        list->items().append(l.c_str());
        if (peers[i].id == current) sel = i;
      }
      if (peers.empty()) list->items().append("(looking for Shelves...)");
      if (sel < 0 && !peers.empty() && current.empty()) { sel = 0; current = peers[0].id; }
      if (sel >= 0) list->selectItem(sel);
      list->repaint();
      showConversation();
    }
  }

  void send() {
    std::string t = msgEd->text();
    if (t.empty()) return;
    if (coms) comsSend(t);
    else {
      if (current.empty()) { status->setText("Pick who to send to first."); status->repaint(); return; }
      msgSend(current, t);
      status->setText("Sending...");
      status->repaint();
    }
    msgEd->setText("");
    msgEd->repaint();
    gApp->setFocusedWindow(msgEd);
  }
};
AppFrame * MessagesApp::inst = nullptr;

void openMessages()
{
  if (!msgStarted()) {
    gApp->messageBox("Messages", "Messages start once TidyOS is on your home Wi-Fi (Settings).", "OK", nullptr, nullptr, uiMessageBoxIcon::Info);
    return;
  }
  if (!MessagesApp::inst) new MessagesApp();
  MessagesApp::inst->open();
}

// ====================================================================== Rugged Lite (grid-down guide, from the Tidy Suite's Rugged)

#include "rugged_data.h"

class RuggedApp : public AppFrame {
public:
  static AppFrame * inst;
  uiButton * tabs[4];
  uiPanel  * panes[4] = { nullptr, nullptr, nullptr, nullptr };   // built the first time each tab is opened (saves RAM)
  int        tab = 0;

  // guide + cards
  uiListBox * cardList = nullptr, * guideList = nullptr;
  TextArea  * cardView = nullptr, * guideView = nullptr;
  uiTextEdit * search = nullptr;
  std::vector<int> guideRows;

  // supplies
  std::string  supVal[7];
  uiTextEdit * sup[7] = {};
  uiLabel    * supOut[4] = {}, * nextJob = nullptr;

  // barter
  uiListBox  * haveList = nullptr, * needList = nullptr, * tradeList = nullptr;
  uiTextEdit * haveEd, * needEd, * tWho, * tGave, * tGot;
  std::vector<std::string> have, need, trades;
  uiLabel    * saved;

  uiPanel * pane(int i) {
    auto p = new uiPanel(this, Point(cx, cy + 30), Size(612, 384), false);
    p->panelStyle().backgroundColor = T.win;
    p->windowStyle().borderSize = 0;
    panes[i] = p;
    return p;
  }
  template <class W> W * add(uiPanel * p, W * w) { applyStyle(w); return w; }

  RuggedApp() : AppFrame("Rugged Lite - when the grid is down", Point(4, 4), Size(632, 452), &inst) {
    static const char * names[4] = { "If this happens", "Field guide", "How long can we last?", "Barter board" };
    static const int    widths[4] = { 120, 100, 170, 110 };
    int x = cx;
    for (int i = 0; i < 4; ++i) {
      tabs[i] = button(names[i], x, cy, widths[i]);
      tabs[i]->onClick = [this, i]() { show(i); };
      x += widths[i] + 4;
    }
    saved = makeSavedLight(this, cx + 542, cy + 2);
    load();
    show(0);
  }

  void build(int i) {
    if (panes[i]) return;
    auto p = pane(i);
    if (i == 0) build0(p); else if (i == 1) build1(p); else if (i == 2) build2(p); else build3(p);
  }

  void build0(uiPanel * p) {
    // ---- If this happens
    cardList = add(p, new uiListBox(p, Point(0, 0), Size(200, 384)));
    cardList->items().takeStrings();
    for (int i = 0; i < RUG_QUICK_COUNT; ++i) cardList->items().append(RUG_QUICK[i].title);
    cardView = new TextArea(p, Point(206, 0), Size(406, 384), false);
    cardList->onChange = [this]() { showCard(); };
    cardList->selectItem(0);
    showCard();

  }

  void build1(uiPanel * p) {
    // ---- Field guide
    add(p, new uiLabel(p, "Search", Point(0, 3), Size(46, 18)));
    search = add(p, new uiTextEdit(p, "", Point(48, 0), Size(152, 22)));
    search->onChange = [this]() { filterGuide(); };
    guideList = add(p, new uiListBox(p, Point(0, 28), Size(200, 356)));
    guideList->items().takeStrings();
    guideView = new TextArea(p, Point(206, 0), Size(406, 384), false);
    guideList->onChange = [this]() { showGuide(); };
    filterGuide();

  }

  void build2(uiPanel * p) {
    // ---- How long can we last?
    static const char * supNames[7] = { "People", "Water stored (gallons)", "Food on hand (days of meals)", "Firewood (armloads)",
                                        "Armloads burned per day", "Fuel (gallons gas/propane)", "Fuel used per day (gallons)" };
    for (int i = 0; i < 7; ++i) {
      add(p, new uiLabel(p, supNames[i], Point(0, 6 + i * 32), Size(200, 18)));
      sup[i] = add(p, new uiTextEdit(p, supVal[i].c_str(), Point(204, 3 + i * 32), Size(80, 24)));
      sup[i]->onChange = [this, i]() { supVal[i] = sup[i]->text(); calc(); };
    }
    static const char * outNames[4] = { "Water", "Food", "Firewood", "Fuel" };
    for (int i = 0; i < 4; ++i) {
      supOut[i] = add(p, new uiLabel(p, outNames[i], Point(320, 6 + i * 44), Size(280, 36)));
      supOut[i]->labelStyle().textFont = &FONT_std_22;
    }
    nextJob = add(p, new uiLabel(p, "", Point(320, 190), Size(290, 40)));
    nextJob->labelStyle().textFont = &FONT_std_14;
    add(p, new uiLabel(p, "Water: about 1 gallon per person per day for drinking and cooking.", Point(0, 240), Size(600, 18)))->labelStyle().textFont = &FONT_std_12;
    add(p, new uiLabel(p, "Anything under 7 days is shown as the next job.", Point(0, 258), Size(600, 18)))->labelStyle().textFont = &FONT_std_12;
    add(p, new uiButton(p, "Print", Point(0, 290), Size(100, 24)))->onClick = [this]() {
      PrintDoc d;
      d.title = "How long can we last";
      static const char * nm[7] = { "People", "Water (gal)", "Food (days)", "Firewood (armloads)", "Armloads per day", "Fuel (gal)", "Fuel per day" };
      for (int i = 0; i < 7; ++i) d.lines.push_back(std::string(nm[i]) + ": " + supVal[i]);
      d.lines.push_back("");
      for (int i = 0; i < 4; ++i) d.lines.push_back(supOut[i]->text());
      d.lines.push_back(nextJob->text());
      printDialog(d);
    };

    calc();
  }

  void build3(uiPanel * p) {
    // ---- Barter board
    add(p, new uiLabel(p, "We have / can do", Point(0, 0), Size(196, 18)));
    haveList = add(p, new uiListBox(p, Point(0, 20), Size(196, 150)));
    haveEd = add(p, new uiTextEdit(p, "", Point(0, 176), Size(140, 22)));
    add(p, new uiButton(p, "Add", Point(144, 176), Size(52, 22)))->onClick = [this]() { addTo(have, haveEd, haveList); };
    add(p, new uiButton(p, "Remove", Point(0, 204), Size(196, 22)))->onClick = [this]() { removeFrom(have, haveList); };

    add(p, new uiLabel(p, "We need", Point(206, 0), Size(196, 18)));
    needList = add(p, new uiListBox(p, Point(206, 20), Size(196, 150)));
    needEd = add(p, new uiTextEdit(p, "", Point(206, 176), Size(140, 22)));
    add(p, new uiButton(p, "Add", Point(350, 176), Size(52, 22)))->onClick = [this]() { addTo(need, needEd, needList); };
    add(p, new uiButton(p, "Remove", Point(206, 204), Size(196, 22)))->onClick = [this]() { removeFrom(need, needList); };

    add(p, new uiLabel(p, "Trades made", Point(412, 0), Size(200, 18)));
    tradeList = add(p, new uiListBox(p, Point(412, 20), Size(200, 150)));
    add(p, new uiLabel(p, "Who", Point(412, 179), Size(36, 18)));
    tWho = add(p, new uiTextEdit(p, "", Point(450, 176), Size(162, 22)));
    add(p, new uiLabel(p, "Gave", Point(412, 205), Size(36, 18)));
    tGave = add(p, new uiTextEdit(p, "", Point(450, 202), Size(162, 22)));
    add(p, new uiLabel(p, "Got", Point(412, 231), Size(36, 18)));
    tGot = add(p, new uiTextEdit(p, "", Point(450, 228), Size(162, 22)));
    add(p, new uiButton(p, "Log trade", Point(450, 256), Size(162, 22)))->onClick = [this]() { logTrade(); };
    add(p, new uiButton(p, "Remove", Point(412, 284), Size(200, 22)))->onClick = [this]() { removeFrom(trades, tradeList); };
    add(p, new uiLabel(p, "Fair trades build trust. Keep a record so nobody argues later.", Point(0, 240), Size(400, 18)))->labelStyle().textFont = &FONT_std_12;
    add(p, new uiButton(p, "Print barter board", Point(0, 284), Size(196, 22)))->onClick = [this]() {
      PrintDoc d;
      d.title = "Barter board";
      d.lines.push_back("WE HAVE / CAN DO");
      for (auto & s : have) d.lines.push_back(" * " + s);
      d.lines.push_back(""); d.lines.push_back("WE NEED");
      for (auto & s : need) d.lines.push_back(" * " + s);
      d.lines.push_back(""); d.lines.push_back("TRADES MADE");
      for (auto & s : trades) d.lines.push_back(" * " + s);
      printDialog(d);
    };
    for (auto l : { haveList, needList, tradeList }) l->items().takeStrings();
    refill(haveList, have);
    refill(needList, need);
    refill(tradeList, trades);
  }

  void show(int i) {
    if (!panes[i] && ramFree() < 16000) {
      gApp->messageBox("Low memory", "Not enough memory for this tab. Close an app first.", "OK", nullptr, nullptr, uiMessageBoxIcon::Warning);
      return;
    }
    build(i);
    tab = i;
    for (int k = 0; k < 4; ++k) {
      if (panes[k]) gApp->showWindow(panes[k], k == i);
      tabs[k]->buttonStyle().backgroundColor = k == i ? T.accent : T.face;
      tabs[k]->buttonStyle().textColor       = k == i ? T.accentText : T.text;
      tabs[k]->repaint();
    }
    if (i == 1) gApp->setFocusedWindow(search);
  }

  void showText(TextArea * v, const char * title, const char * body) {
    std::vector<std::string> lines;
    lines.push_back(title);
    lines.push_back(std::string(strlen(title), '='));
    lines.push_back("");
    wrapText(body, v->visCols() - 1, lines);
    v->setLines(std::move(lines));
  }

  void showCard() {
    int i = cardList->firstSelectedItem();
    if (i >= 0 && i < RUG_QUICK_COUNT) showText(cardView, RUG_QUICK[i].title, RUG_QUICK[i].body);
  }

  void filterGuide() {
    std::string q = search->text();
    for (auto & c : q) c = tolower(c);
    guideRows.clear();
    guideList->items().clear();
    const char * lastCat = "";
    for (int i = 0; i < RUG_GUIDE_COUNT; ++i) {
      const RugEntry & g = RUG_GUIDE[i];
      if (!q.empty()) {
        std::string hay = std::string(g.title) + " " + g.keys + " " + g.cat;
        for (auto & c : hay) c = tolower(c);
        if (hay.find(q) == std::string::npos && !strcasestr(g.body, q.c_str())) continue;
      }
      if (strcmp(lastCat, g.cat)) {
        guideList->items().append((std::string("- ") + g.cat + " -").c_str());
        guideRows.push_back(-1);
        lastCat = g.cat;
      }
      guideList->items().append((std::string("  ") + g.title).c_str());
      guideRows.push_back(i);
    }
    guideList->repaint();
  }

  void showGuide() {
    int r = guideList->firstSelectedItem();
    if (r < 0 || r >= (int)guideRows.size() || guideRows[r] < 0) return;
    const RugEntry & g = RUG_GUIDE[guideRows[r]];
    showText(guideView, g.title, g.body);
  }

  void calc() {
    if (!panes[2]) return;
    double v[7];
    for (int i = 0; i < 7; ++i) v[i] = atof(supVal[i].c_str());
    double ppl = v[0] < 1 ? 1 : v[0];
    double days[4] = { v[1] / ppl, v[2], v[4] > 0 ? v[3] / v[4] : -1, v[6] > 0 ? v[5] / v[6] : -1 };
    static const char * nm[4] = { "Water", "Food", "Firewood", "Fuel" };
    int worst = -1;
    for (int i = 0; i < 4; ++i) {
      char b[64];
      if (days[i] < 0) snprintf(b, sizeof(b), "%s:  -", nm[i]);
      else snprintf(b, sizeof(b), days[i] >= 10 ? "%s:  %.0f days" : "%s:  %.1f days", nm[i], days[i]);
      supOut[i]->labelStyle().textColor = (days[i] >= 0 && days[i] < 7) ? T.warn : T.text;
      supOut[i]->setText(b);
      supOut[i]->repaint();
      if (days[i] >= 0 && days[i] < 7 && (worst < 0 || days[i] < days[worst])) worst = i;
    }
    std::string nj = worst < 0 ? std::string("Nothing under a week. Keep building.") : std::string("Next job: ") + nm[worst] + " (under a week).";
    nextJob->setText(nj.c_str());
    nextJob->repaint();
    save();
  }

  // ---- barter helpers
  void refill(uiListBox * l, const std::vector<std::string> & v) {
    if (!l) return;
    l->items().clear();
    for (auto & s : v) l->items().append(s.c_str());
    l->repaint();
  }
  void addTo(std::vector<std::string> & v, uiTextEdit * e, uiListBox * l) {
    std::string t = e->text();
    if (t.empty()) return;
    for (auto & c : t) if (c == '|') c = '/';
    v.push_back(t);
    e->setText(""); e->repaint();
    refill(l, v);
    save();
  }
  void removeFrom(std::vector<std::string> & v, uiListBox * l) {
    int i = l->firstSelectedItem();
    if (i < 0 || i >= (int)v.size()) return;
    v.erase(v.begin() + i);
    refill(l, v);
    save();
  }
  void logTrade() {
    std::string who = tWho->text(), gave = tGave->text(), got = tGot->text();
    if (who.empty() && gave.empty() && got.empty()) return;
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char d[12] = "";
    if (tmv.tm_year > 100) snprintf(d, sizeof(d), "%d/%d ", tmv.tm_mon + 1, tmv.tm_mday);
    std::string t = std::string(d) + who + ": gave " + gave + ", got " + got;
    for (auto & c : t) if (c == '|') c = '/';
    trades.insert(trades.begin(), t);
    for (auto e : { tWho, tGave, tGot }) { e->setText(""); e->repaint(); }
    refill(tradeList, trades);
    save();
  }

  bool loading = false;

  void save() {
    if (loading) return;
    std::string s;
    for (int i = 0; i < 7; ++i) s += std::string("S|") + std::to_string(i) + "|" + supVal[i] + "\n";
    for (auto & x : have)   s += "H|" + x + "\n";
    for (auto & x : need)   s += "N|" + x + "\n";
    for (auto & x : trades) s += "T|" + x + "\n";
    setSavedLight(saved, writeFileStr(dataPath("rugged.txt").c_str(), s));
  }

  void load() {
    loading = true;
    std::string s;
    if (readFileStr(dataPath("rugged.txt").c_str(), s)) {
      size_t st = 0;
      for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == '\n') {
          std::string l = s.substr(st, i - st);
          st = i + 1;
          if (l.size() < 2 || l[1] != '|') continue;
          std::string v = l.substr(2);
          if (l[0] == 'S') { int k = atoi(v.c_str()); size_t b = v.find('|'); if (k >= 0 && k < 7 && b != std::string::npos) supVal[k] = v.substr(b + 1); }
          else if (l[0] == 'H') have.push_back(v);
          else if (l[0] == 'N') need.push_back(v);
          else if (l[0] == 'T') trades.push_back(v);
        }
    }
    loading = false;
  }
};
AppFrame * RuggedApp::inst = nullptr;

void openRugged()
{
  if (!RuggedApp::inst) new RuggedApp();
  RuggedApp::inst->open();
}

// ====================================================================== Task Manager

#include <esp_freertos_hooks.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
#include <nvs.h>
#include <dirent.h>
#include "ping/ping_sock.h"
#include "freertos/task_snapshot.h"
#include "lwip/inet.h"

// ---- CPU load from idle time: each core's idle task counts while it has nothing to do
static volatile uint32_t s_idleCount[2] = { 0, 0 };
static bool idleHook0() { s_idleCount[0]++; return false; }
static bool idleHook1() { s_idleCount[1]++; return false; }

// ---- ping to the router
static esp_ping_handle_t s_ping = nullptr;
static volatile int s_pingMs = -1, s_pingSent = 0, s_pingLost = 0;
static void pingOk(esp_ping_handle_t h, void *) { uint32_t t = 0; esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &t, sizeof(t)); s_pingMs = t; s_pingSent++; }
static void pingTimeout(esp_ping_handle_t, void *) { s_pingLost++; s_pingSent++; }

class Graph : public uiControl {
public:
  static const int N = 60;
  float a[N], b[N];       // two series (b < 0 = unused)
  int n = 0;
  bool two;
  RGB888 ca, cb;
  Graph(uiWindow * p, Point pos, Size sz, bool twoLines) : uiControl(p, pos, sz, true, 0), two(twoLines) {
    windowStyle().borderSize = 1; windowStyle().borderColor = T.shadow; windowStyle().activeBorderColor = T.shadow;
    setFocusIndex(-1);
    ca = T.accent; cb = T.text;
    for (int i = 0; i < N; ++i) a[i] = b[i] = -1;
  }
  void push(float va, float vb = -1) {
    memmove(a, a + 1, sizeof(float) * (N - 1)); memmove(b, b + 1, sizeof(float) * (N - 1));
    a[N - 1] = va; b[N - 1] = vb;
    repaint();
  }
  void processEvent(uiEvent * e) override {
    uiControl::processEvent(e);
    if (e->id != UIEVT_PAINT) return;
    Rect r = clientRect(uiOrigin::Window);
    beginPaint(e, r);
    auto cv = canvas();
    cv->setBrushColor(T.win);
    cv->fillRectangle(r.X1, r.Y1, r.X2, r.Y2);
    cv->setPenColor(T.face);
    for (int k = 1; k < 4; ++k) { int y = r.Y1 + r.height() * k / 4; cv->drawLine(r.X1, y, r.X2, y); }
    for (int k = 1; k < 6; ++k) { int x = r.X1 + r.width() * k / 6; cv->drawLine(x, r.Y1, x, r.Y2); }
    auto plot = [&](float * v, RGB888 c) {
      cv->setPenColor(c);
      int px = -1, py = 0;
      for (int i = 0; i < N; ++i) {
        if (v[i] < 0) { px = -1; continue; }
        int x = r.X1 + (r.width() - 1) * i / (N - 1);
        int y = r.Y2 - (int)((r.height() - 1) * fmin(100, v[i]) / 100);
        if (px >= 0) { cv->drawLine(px, py, x, y); cv->drawLine(px, py - 1, x, y - 1); }
        px = x; py = y;
      }
    };
    if (two) plot(b, cb);
    plot(a, ca);
  }
};

static std::string kb(uint64_t v) { char b[24]; if (v >= 10ULL << 20) snprintf(b, sizeof(b), "%.1f MB", v / 1048576.0); else snprintf(b, sizeof(b), "%.1f KB", v / 1024.0); return b; }

static std::string bar(double frac, int width = 24)
{
  int n = (int)(fmax(0, fmin(1, frac)) * width + 0.5);
  return "[" + std::string(n, '\xDB') + std::string(width - n, '\xB0') + "]";
}

static void dirSize(const std::string & path, uint64_t & bytes, int & files, int depth = 0)
{
  DIR * d = opendir(path.c_str());
  if (!d) return;
  struct dirent * e;
  while ((e = readdir(d)) != nullptr) {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    std::string full = path + "/" + n;
    if (e->d_type == DT_DIR) { if (depth < 4) dirSize(full, bytes, files, depth + 1); }
    else { struct stat st; if (stat(full.c_str(), &st) == 0) bytes += st.st_size; ++files; }
  }
  closedir(d);
}

static const char * taskInfo(const char * n)
{
  static const char * map[][2] = {
    { "loopTask", "TidyOS desktop and apps" }, { "IDLE", "Idle (free time)" }, { "IDLE0", "Idle core 0" }, { "IDLE1", "Idle core 1" },
    { "nimble_host", "Bluetooth host" }, { "btController", "Bluetooth radio" }, { "ble", "Keyboard/mouse manager" },
    { "keyrep", "Key repeat" }, { "serkbd", "Serial keyboard" }, { "wifi", "Wi-Fi driver" }, { "tiT", "TCP/IP network" },
    { "sys_evt", "System events" }, { "esp_timer", "System timers" }, { "Tmr Svc", "Timer service" }, { "ipc0", "Core 0 helper" },
    { "ipc1", "Core 1 helper" }, { "shelfmsg", "Shelf messaging + mesh" }, { "fileweb", "File sharing server" },
    { "fetch", "Browser download" }, { "led", "Blue status light" }, { "print", "Printing" }, { "findprn", "Printer search" }, { "mdns", "tidyos.local name" },
    { "ping", "Router ping" }, { "msgadd", "Shelf lookup" }, { "FabGL", "Screen drawing" },
  };
  for (auto & m : map) if (!strcmp(m[0], n)) return m[1];
  return "";
}

class TaskManagerApp : public AppFrame {
public:
  static AppFrame * inst;
  uiButton * tabs[6];
  uiPanel  * panes[6];
  int        tab = 0;
  Graph    * cpuGraph, * ramGraph;
  uiLabel  * cpuLbl[2], * ramLbl, * summary, * summary2;
  uiListBox * appList;
  std::vector<uiFrame *> appWins;
  TextArea * tasksView, * netView, * storeView, * powerView;
  uint32_t lastIdle[2] = { 0, 0 };
  float maxIdle[2] = { 1, 1 };
  int ticks = 0;
  bool hooks = false;

  uiPanel * pane(int i) {
    auto p = new uiPanel(this, Point(cx, cy + 30), Size(612, 386), false);
    p->panelStyle().backgroundColor = T.win;
    p->windowStyle().borderSize = 0;
    return panes[i] = p;
  }
  template <class W> W * add(W * w) { applyStyle(w); return w; }

  TaskManagerApp() : AppFrame("Task Manager", Point(4, 4), Size(632, 452), &inst) {
    static const char * names[6] = { "Performance", "Apps", "Tasks", "Network", "Storage", "Power & info" };
    int x = cx;
    for (int i = 0; i < 6; ++i) {
      int w = (i == 5) ? 100 : 98;
      tabs[i] = button(names[i], x, cy, w);
      tabs[i]->onClick = [this, i]() { show(i); };
      x += w + 4;
    }
    // Performance
    auto p = pane(0);
    add(new uiLabel(p, "CPU  (blue = core 0: radios,  black = core 1: screen + apps)", Point(0, 0), Size(612, 18)));
    cpuGraph = new Graph(p, Point(0, 20), Size(430, 130), true);
    cpuLbl[0] = add(new uiLabel(p, "", Point(440, 30), Size(170, 26))); cpuLbl[0]->labelStyle().textFont = &FONT_std_18;
    cpuLbl[1] = add(new uiLabel(p, "", Point(440, 64), Size(170, 26))); cpuLbl[1]->labelStyle().textFont = &FONT_std_18;
    add(new uiLabel(p, "Memory in use (RAM)", Point(0, 160), Size(300, 18)));
    ramGraph = new Graph(p, Point(0, 180), Size(430, 110), false);
    ramLbl = add(new uiLabel(p, "", Point(440, 190), Size(170, 26))); ramLbl->labelStyle().textFont = &FONT_std_18;
    summary = add(new uiLabel(p, "", Point(0, 300), Size(612, 16)));
    summary->labelStyle().textFont = &FONT_std_12;
    summary2 = add(new uiLabel(p, "", Point(0, 316), Size(612, 16)));
    summary2->labelStyle().textFont = &FONT_std_12;
    // Apps
    p = pane(1);
    add(new uiLabel(p, "Open apps", Point(0, 0), Size(300, 18)));
    appList = add(new uiListBox(p, Point(0, 20), Size(440, 300)));
    appList->items().takeStrings();
    add(new uiButton(p, "Switch to", Point(450, 20), Size(150, 26)))->onClick = [this]() {
      int i = appList->firstSelectedItem();
      if (i >= 0 && i < (int)appWins.size()) bringToFront(appWins[i]);
    };
    add(new uiButton(p, "End task", Point(450, 54), Size(150, 26)))->onClick = [this]() {
      int i = appList->firstSelectedItem();
      if (i >= 0 && i < (int)appWins.size()) { uiEvent evt(appWins[i], UIEVT_CLOSE); gApp->postEvent(&evt); }
    };
    add(new uiLabel(p, "End task closes the app. Apps with unsaved work ask first.", Point(0, 330), Size(600, 18)))->labelStyle().textFont = &FONT_std_12;
    add(new uiLabel(p, "Tip: Ctrl+Shift+Esc opens the Task Manager from anywhere.", Point(0, 350), Size(600, 18)))->labelStyle().textFont = &FONT_std_12;
    // Tasks, Network, Storage, Power: text views
    p = pane(2); tasksView = new TextArea(p, Point(0, 0), Size(612, 386), false);
    p = pane(3); netView   = new TextArea(p, Point(0, 0), Size(612, 386), false);
    p = pane(4); storeView = new TextArea(p, Point(0, 0), Size(612, 386), false);
    p = pane(5); powerView = new TextArea(p, Point(0, 0), Size(612, 386), false);

    // CPU meters
    esp_register_freertos_idle_hook_for_cpu(idleHook0, 0);
    esp_register_freertos_idle_hook_for_cpu(idleHook1, 1);
    hooks = true;
    lastIdle[0] = s_idleCount[0]; lastIdle[1] = s_idleCount[1];
    startPing();
    startTimer(1000);
    onTimer = [this](uiTimerHandle) { tick(); };
    show(0);
  }

  void onClosing() override {
    if (hooks) { esp_deregister_freertos_idle_hook_for_cpu(idleHook0, 0); esp_deregister_freertos_idle_hook_for_cpu(idleHook1, 1); hooks = false; }
    stopPing();
  }

  void startPing() {
    if (s_ping || !netConnected()) return;
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    ip_addr_t gw;
    if (!ipaddr_aton(WiFi.gatewayIP().toString().c_str(), &gw)) return;
    cfg.target_addr = gw;
    cfg.count = ESP_PING_COUNT_INFINITE;
    cfg.interval_ms = 2000;
    cfg.timeout_ms = 1500;
    cfg.task_stack_size = 2560;
    esp_ping_callbacks_t cbs = {};
    cbs.on_ping_success = pingOk;
    cbs.on_ping_timeout = pingTimeout;
    s_pingMs = -1; s_pingSent = s_pingLost = 0;
    if (esp_ping_new_session(&cfg, &cbs, &s_ping) == ESP_OK) esp_ping_start(s_ping);
    else s_ping = nullptr;
  }
  void stopPing() { if (s_ping) { esp_ping_stop(s_ping); esp_ping_delete_session(s_ping); s_ping = nullptr; } }

  void show(int i) {
    tab = i;
    for (int k = 0; k < 6; ++k) {
      gApp->showWindow(panes[k], k == i);
      tabs[k]->buttonStyle().backgroundColor = k == i ? T.accent : T.face;
      tabs[k]->buttonStyle().textColor       = k == i ? T.accentText : T.text;
      tabs[k]->repaint();
    }
    refresh(true);
  }

  void tick() {
    // CPU: compare this second's idle count with the most we've seen in one second
    float load[2];
    for (int c = 0; c < 2; ++c) {
      uint32_t now = s_idleCount[c], d = now - lastIdle[c];
      lastIdle[c] = now;
      if (d > maxIdle[c]) maxIdle[c] = d;
      load[c] = maxIdle[c] > 1 ? 100.0f * (1.0f - d / maxIdle[c]) : 0;
      maxIdle[c] *= 0.9995f;                 // slowly forget, in case the quiet moment was unusual
    }
    if (++ticks < 2) { load[0] = load[1] = -1; }
    cpuGraph->push(load[0], load[1]);
    char b[64];
    for (int c = 0; c < 2; ++c) {
      if (load[c] < 0) snprintf(b, sizeof(b), "Core %d: ...", c);
      else snprintf(b, sizeof(b), "Core %d: %d%%", c, (int)(load[c] + 0.5f));
      cpuLbl[c]->setText(b); cpuLbl[c]->repaint();
    }
    size_t total = heap_caps_get_total_size(MALLOC_CAP_8BIT), freeB = ramFree();
    float ramPct = total ? 100.0f * (total - freeB) / total : 0;
    ramGraph->push(ramPct);
    snprintf(b, sizeof(b), "%d%% used", (int)(ramPct + 0.5f));
    ramLbl->setText(b); ramLbl->repaint();
    if (!s_ping && netConnected()) startPing();
    refresh(false);
  }

  void refresh(bool force) {
    if (tab == 0) perfSummary();
    else if (tab == 1) listApps();
    else if (tab == 2 && (force || ticks % 2 == 0)) listTasks();
    else if (tab == 3) network();
    else if (tab == 4 && (force || ticks % 5 == 0)) storage();
    else if (tab == 5 && (force || ticks % 2 == 0)) power();
  }

  static void setLinesKeep(TextArea * v, std::vector<std::string> && l) { int t = v->top; v->setLines(std::move(l)); v->top = imin(t, imax(0, (int)v->lines.size() - 1)); v->repaint(); }

  void perfSummary() {
    char b[300];
    uint32_t up = millis() / 1000;
    snprintf(b, sizeof(b), "Up %uh %02um %02us    CPU %u MHz x 2 cores    Tasks running: %u",
             up / 3600, (up / 60) % 60, up % 60, (unsigned)getCpuFrequencyMhz(), (unsigned)uxTaskGetNumberOfTasks());
    summary->setText(b); summary->repaint();
    snprintf(b, sizeof(b), "Free RAM %s   (biggest piece %s, lowest ever %s)",
             kb(ramFree()).c_str(), kb(ramBiggest()).c_str(), kb(ramLowest()).c_str());
    summary2->setText(b); summary2->repaint();
  }

  void listApps() {
    std::vector<uiFrame *> wins;
    for (uiWindow * w = gApp->rootWindow()->firstChild(); w; w = w->next())
      if (w->objectType().uiFrame && w->state().visible && w != this) wins.push_back((uiFrame *)w);
    bool same = wins == appWins;
    appWins = wins;
    if (same && appList->items().count() == (int)wins.size()) return;
    int sel = appList->firstSelectedItem();
    appList->items().clear();
    for (auto w : appWins) appList->items().append(w->title() ? w->title() : "(window)");
    if (sel >= 0 && sel < (int)appWins.size()) appList->selectItem(sel);
    appList->repaint();
  }

  void listTasks() {
    struct TI { TaskHandle_t h; const char * name; UBaseType_t prio; uint32_t stack; eTaskState state; };
    UBaseType_t n = uxTaskGetNumberOfTasks();
    std::vector<TaskSnapshot_t> snap(n + 4);
    UBaseType_t tcb = 0;
    n = uxTaskGetSnapshotAll(snap.data(), snap.size(), &tcb);
    std::vector<TI> st;
    for (UBaseType_t i = 0; i < n; ++i) {
      TaskHandle_t h = (TaskHandle_t)snap[i].pxTCB;
      st.push_back({ h, pcTaskGetName(h), uxTaskPriorityGet(h), (uint32_t)uxTaskGetStackHighWaterMark(h), eTaskGetState(h) });
    }
    std::sort(st.begin(), st.end(), [](const TI & a, const TI & b) { return a.prio > b.prio; });
    std::vector<std::string> l;
    char b[120];
    snprintf(b, sizeof(b), "%u tasks running on 2 cores (FreeRTOS)", (unsigned)n); l.push_back(b);
    l.push_back("");
    l.push_back("Task          Core Prio  Stack left  State     What it does");
    l.push_back("------------- ---- ----  ----------  --------  ---------------------------");
    static const char * states[] = { "Running", "Ready", "Waiting", "Paused", "Ending", "?" };
    for (size_t i = 0; i < st.size(); ++i) {
      BaseType_t core = xTaskGetAffinity(st[i].h);
      char cs[6];
      if (core == tskNO_AFFINITY) strcpy(cs, "any"); else snprintf(cs, sizeof(cs), "%d", (int)core);
      int sidx = st[i].state <= eDeleted ? st[i].state : 5;
      snprintf(b, sizeof(b), "%-13.13s %4s %4u  %7u B  %-8s  %s", st[i].name, cs, (unsigned)st[i].prio,
               (unsigned)st[i].stack, states[sidx], taskInfo(st[i].name));
      l.push_back(b);
    }
    l.push_back("");
    l.push_back("Stack left = the least free stack space each task has had (low = close to running out).");
    setLinesKeep(tasksView, std::move(l));
  }

  void network() {
    std::vector<std::string> l;
    char b[140];
    bool on = netConnected();
    l.push_back("WI-FI");
    if (on) {
      int rssi = WiFi.RSSI();
      int bars = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : rssi >= -85 ? 1 : 0;
      snprintf(b, sizeof(b), "  Network     %s", WiFi.SSID().c_str()); l.push_back(b);
      snprintf(b, sizeof(b), "  Signal      %d dBm  %s  %s", rssi, bar(bars / 4.0, 8).c_str(), bars >= 3 ? "good" : bars == 2 ? "fair" : "weak"); l.push_back(b);
      snprintf(b, sizeof(b), "  Channel     %d   (2.4 GHz)    Router %s", (int)WiFi.channel(), WiFi.BSSIDstr().c_str()); l.push_back(b);
      snprintf(b, sizeof(b), "  Address     %s  (tidyos.local)", WiFi.localIP().toString().c_str()); l.push_back(b);
      snprintf(b, sizeof(b), "  Subnet      %s    Gateway %s", WiFi.subnetMask().toString().c_str(), WiFi.gatewayIP().toString().c_str()); l.push_back(b);
      snprintf(b, sizeof(b), "  DNS         %s", WiFi.dnsIP().toString().c_str()); l.push_back(b);
      if (s_pingMs >= 0) snprintf(b, sizeof(b), "  Router ping %d ms    lost %d of %d", (int)s_pingMs, (int)s_pingLost, (int)s_pingSent);
      else snprintf(b, sizeof(b), "  Router ping waiting...");
      l.push_back(b);
    } else {
      l.push_back(gSet.ssid.length() ? "  Not connected - trying to join " + std::string(gSet.ssid.c_str()) : "  Not set up - open Settings to join your Wi-Fi");
    }
    snprintf(b, sizeof(b), "  This board  MAC %s", WiFi.macAddress().c_str()); l.push_back(b);
    int8_t pw = 0; esp_wifi_get_max_tx_power(&pw);
    snprintf(b, sizeof(b), "  Radio power %.1f dBm", pw / 4.0); l.push_back(b);
    l.push_back("");
    l.push_back("TIDYOS TRAFFIC SINCE START");
    snprintf(b, sizeof(b), "  Received    %s", kb(gNetRx).c_str()); l.push_back(b);
    snprintf(b, sizeof(b), "  Sent        %s", kb(gNetTx).c_str()); l.push_back(b);
    l.push_back("");
    l.push_back("SHELVES AND MESH");
    std::vector<MsgPeer> peers;
    msgPeers(peers);
    int onl = 0; for (auto & p : peers) if (p.online) ++onl;
    snprintf(b, sizeof(b), "  Shelves     %d online, %d known", onl, (int)peers.size()); l.push_back(b);
    for (auto & p : peers) if (p.online) { snprintf(b, sizeof(b), "    * %-24.24s %s:%d", p.name.c_str(), p.ip.c_str(), p.port); l.push_back(b); }
    int sent = 0, heard = 0;
    comsCounts(sent, heard);
    std::vector<std::string> mem;
    comsMembers(mem);
    snprintf(b, sizeof(b), "  Coms mesh   %s, channel %d, %d boards, %d sent, %d heard", comsUp() ? "on" : "off", comsChannel(), (int)mem.size(), sent, heard); l.push_back(b);
    std::string u = fileServerUrl();
    l.push_back("  File share  " + (u.empty() ? std::string(fileServerOn() ? "on (waiting for Wi-Fi)" : "off") : "on at " + u));
    l.push_back("  Helper      " + (gSet.proxy.length() ? std::string(gSet.proxy.c_str()) : std::string("not set (browser uses FrogFind)")));
    l.push_back("");
    l.push_back("BLUETOOTH");
    snprintf(b, sizeof(b), "  %d keyboard(s), %d mouse/mice connected", inputKeyboards(), inputMice()); l.push_back(b);
    l.push_back("  " + inputStatus());
    setLinesKeep(netView, std::move(l));
  }

  void storage() {
    std::vector<std::string> l;
    char b[140];
    int64_t total = 0, used = 0;
    l.push_back("SD CARD");
    if (gSDMounted && FileBrowser::getFSInfo(DriveType::SDCard, 0, &total, &used)) {
      snprintf(b, sizeof(b), "  %s  %s used of %s  (%s free)", bar(total ? (double)used / total : 0).c_str(), kb(used).c_str(), kb(total).c_str(), kb(total - used).c_str()); l.push_back(b);
      l.push_back(std::string("  Wiring      ") + gSDWiring);
      uint64_t bytes = 0; int files = 0;
      dirSize(SD_MOUNT "/Documents", bytes, files);
      snprintf(b, sizeof(b), "  Documents   %d files, %s", files, kb(bytes).c_str()); l.push_back(b);
      bytes = 0; files = 0;
      dirSize(SD_MOUNT "/tidyos", bytes, files);
      snprintf(b, sizeof(b), "  TidyOS data %d files, %s (lists, bills, messages, settings)", files, kb(bytes).c_str()); l.push_back(b);
    } else l.push_back("  No SD card");
    if (gFlashMounted && FileBrowser::getFSInfo(DriveType::SPIFFS, 0, &total, &used)) {
      l.push_back(""); l.push_back("INTERNAL STORAGE (used because there's no SD card)");
      snprintf(b, sizeof(b), "  %s  %s used of %s", bar(total ? (double)used / total : 0).c_str(), kb(used).c_str(), kb(total).c_str()); l.push_back(b);
    }
    l.push_back("");
    l.push_back("FLASH CHIP");
    uint32_t flash = ESP.getFlashChipSize();
    const esp_partition_t * app = esp_ota_get_running_partition();
    uint32_t sketch = ESP.getSketchSize();
    snprintf(b, sizeof(b), "  Size        %s at %u MHz", kb(flash).c_str(), (unsigned)(ESP.getFlashChipSpeed() / 1000000)); l.push_back(b);
    if (app) { snprintf(b, sizeof(b), "  TidyOS      %s  %s of %s", bar((double)sketch / app->size).c_str(), kb(sketch).c_str(), kb(app->size).c_str()); l.push_back(b); }
    l.push_back("              (includes the 78,000-word dictionary and the Rugged guide)");
    nvs_stats_t ns;
    if (nvs_get_stats(nullptr, &ns) == ESP_OK) {
      snprintf(b, sizeof(b), "  Settings    %s  %u of %u slots", bar(ns.total_entries ? (double)ns.used_entries / ns.total_entries : 0).c_str(), (unsigned)ns.used_entries, (unsigned)ns.total_entries);
      l.push_back(b);
    }
    l.push_back("");
    l.push_back("MEMORY (RAM)");
    size_t tot = heap_caps_get_total_size(MALLOC_CAP_8BIT), fr = ramFree();
    snprintf(b, sizeof(b), "  %s  %s used of %s", bar(tot ? (double)(tot - fr) / tot : 0).c_str(), kb(tot - fr).c_str(), kb(tot).c_str()); l.push_back(b);
    int fb = gColors == 8 ? 640 * 480 * 3 / 8 : 640 * 480 / 4;
    snprintf(b, sizeof(b), "  Screen      %s  (640x480, %d colours)", kb(fb).c_str(), gColors); l.push_back(b);
    snprintf(b, sizeof(b), "  Free        %s   biggest piece %s   lowest ever %s", kb(fr).c_str(), kb(ramBiggest()).c_str(), kb(ramLowest()).c_str()); l.push_back(b);
    snprintf(b, sizeof(b), "  DMA-able    %s free", kb(heap_caps_get_free_size(MALLOC_CAP_DMA)).c_str()); l.push_back(b);
    l.push_back("  Extra RAM   none (this board has no PSRAM)");
    setLinesKeep(storeView, std::move(l));
  }

  void power() {
    std::vector<std::string> l;
    char b[140];
    // rough current estimate from what is switched on (this board has no power sensor)
    int ma = 45 + (getCpuFrequencyMhz() >= 240 ? 20 : 0);          // two busy cores (the screen keeps one busy)
    bool wifiOn = netConnected();
    if (wifiOn) ma += 45; else if (gSet.ssid.length()) ma += 70;    // searching costs more than staying connected
#ifdef TIDYOS_HTML5
    int bt = 0;
#else
    int bt = inputKeyboards() + inputMice();
#endif
    ma += bt * 8 + (inputScanning() ? 25 : 0);
    if (comsUp()) ma += 5;
    if (gSDMounted) ma += 3;
    l.push_back("POWER (estimate - the NodeMCU has no power meter)");
    snprintf(b, sizeof(b), "  About %d mA at 3.3 V = about %.2f W  (%.2f W from USB at 5 V)", ma, ma * 3.3 / 1000, ma * 5.0 / 1000 * 1.1); l.push_back(b);
    snprintf(b, sizeof(b), "  A 10,000 mAh power bank would last roughly %.0f hours", 10000.0 * 3.7 * 0.85 / (ma * 5.0 * 1.1)); l.push_back(b);
#ifdef TIDYOS_HTML5
    l.push_back("  The phone or PC showing TidyOS uses its own power.");
#else
    l.push_back("  The VGA monitor uses its own power (often 10-25 W) - by far the biggest draw.");
#endif
    l.push_back("");
    l.push_back("  What's using power:");
    snprintf(b, sizeof(b), "    CPU         %u MHz, both cores", (unsigned)getCpuFrequencyMhz()); l.push_back(b);
    l.push_back(std::string("    Wi-Fi       ") + (wifiOn ? "connected" : gSet.ssid.length() ? "searching" : "idle"));
#ifdef TIDYOS_HTML5
    l.push_back(std::string("    Browser     ") + (screenViewer() ? "connected" : "none"));
#else
    snprintf(b, sizeof(b), "    Bluetooth   %d device(s)%s", bt, inputScanning() ? ", scanning" : ""); l.push_back(b);
#endif
    l.push_back(std::string("    Mesh        ") + (comsUp() ? "listening" : "off"));
    l.push_back("");
    float t = temperatureRead();
    if (t > 0 && fabs(t - 53.33f) > 0.05f) { snprintf(b, sizeof(b), "  Chip temperature  about %.0f C  (rough)", t); l.push_back(b); }
    else l.push_back("  Chip temperature  not available on this chip");
    l.push_back("");
    l.push_back("THIS BOARD");
    snprintf(b, sizeof(b), "  Chip        %s rev %d, %d cores", ESP.getChipModel(), (int)ESP.getChipRevision(), (int)ESP.getChipCores()); l.push_back(b);
    snprintf(b, sizeof(b), "  TidyOS      %s    ESP-IDF %s", TIDYOS_VERSION, ESP.getSdkVersion()); l.push_back(b);
    uint32_t up = millis() / 1000;
    snprintf(b, sizeof(b), "  Up time     %ud %uh %02um", up / 86400, (up / 3600) % 24, (up / 60) % 60); l.push_back(b);
    static const char * rr[] = { "unknown", "power on", "external pin", "restart", "crash", "crash (interrupt)", "crash (task)", "watchdog", "deep sleep", "low voltage (brownout)", "SDIO" };
    int r = esp_reset_reason();
    l.push_back(std::string("  Last start  ") + (r >= 0 && r <= 10 ? rr[r] : "other"));
    if (r == ESP_RST_BROWNOUT) l.push_back("  -> The power dipped. Try a better USB cable or power supply.");
    std::string ct = crashText();
    if (!ct.empty()) {
      l.push_back("");
      l.push_back("LAST CRASH (also saved in tidyos/crash.txt)");
      std::vector<std::string> w;
      wrapText(ct, powerView->visCols() - 3, w);
      for (auto & x : w) l.push_back("  " + x);
      l.push_back("  A photo of this helps find the bug.");
    }
    setLinesKeep(powerView, std::move(l));
  }
};
AppFrame * TaskManagerApp::inst = nullptr;

void openSystem()
{
  if (!TaskManagerApp::inst) new TaskManagerApp();
  TaskManagerApp::inst->open();
}

// ====================================================================== Ping
// Saved list of addresses (IPs or names). Start pings every one of them about once a second,
// round and round, until Stop. Each line shows up/down, last time, average, min/max and loss;
// the log below shows every reply like "ping -t".

#include "lwip/sockets.h"
#include "lwip/inet_chksum.h"
#include "lwip/prot/icmp.h"
#include "lwip/prot/ip4.h"
#include "lwip/netdb.h"
#include <mutex>

struct PingTarget {
  std::string addr;
  uint32_t ip = 0;          // network order, 0 = not resolved
  int sent = 0, got = 0, last = -1, minMs = 0, maxMs = 0, streakLost = 0;
  uint64_t sumMs = 0;
  bool up = false, seen = false;
};

static std::vector<PingTarget> s_pt;
// log: a fixed ring of lines (no memory is asked for while pinging)
static char     s_plog[48][60];
static uint32_t s_plogN = 0;              // lines written so far
static std::mutex s_pmx;
static volatile bool s_pRun = false, s_pTaskAlive = false;
static volatile int  s_pChanged = 0;

static void plog(const std::string & l)
{
  time_t now = time(nullptr);
  struct tm tmv; localtime_r(&now, &tmv);
  char t[16] = "";
  if (tmv.tm_year > 100) snprintf(t, sizeof(t), "%02d:%02d:%02d ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
  snprintf(s_plog[s_plogN % 48], sizeof(s_plog[0]), "%s%s", t, l.c_str());
  s_plogN++;
}

static uint32_t resolveHost(const std::string & h)
{
  ip4_addr_t a;
  if (ip4addr_aton(h.c_str(), &a)) return a.addr;
  struct addrinfo hints = {}, * res = nullptr;
  hints.ai_family = AF_INET;
  if (getaddrinfo(h.c_str(), nullptr, &hints, &res) != 0 || !res) return 0;
  uint32_t ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
  freeaddrinfo(res);
  return ip;
}

// one echo request; returns round-trip ms or -1
static int pingOnce(int sock, uint32_t ip, uint16_t seq, int timeoutMs)
{
  uint8_t pkt[sizeof(struct icmp_echo_hdr) + 32];
  auto * h = (struct icmp_echo_hdr *)pkt;
  ICMPH_TYPE_SET(h, ICMP_ECHO);
  ICMPH_CODE_SET(h, 0);
  h->id = htons(0x7D1D);
  h->seqno = htons(seq);
  for (int i = 0; i < 32; ++i) pkt[sizeof(*h) + i] = 'a' + (i % 26);
  h->chksum = 0;
  h->chksum = inet_chksum(pkt, sizeof(pkt));
  struct sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = ip;
  int64_t t0 = esp_timer_get_time();
  if (sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&to, sizeof(to)) < 0) return -1;
  gNetTx += sizeof(pkt) + 20;
  uint8_t buf[128];
  for (;;) {
    int left = timeoutMs - (int)((esp_timer_get_time() - t0) / 1000);
    if (left <= 0) return -1;
    struct timeval tv = { left / 1000, (left % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in from; socklen_t fl = sizeof(from);
    int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
    if (n <= 0) return -1;
    gNetRx += n;
    auto * iph = (struct ip_hdr *)buf;
    int hl = IPH_HL_BYTES(iph);
    if (n < hl + (int)sizeof(struct icmp_echo_hdr)) continue;
    auto * r = (struct icmp_echo_hdr *)(buf + hl);
    if (from.sin_addr.s_addr == ip && ICMPH_TYPE(r) == ICMP_ER && r->id == htons(0x7D1D) && r->seqno == htons(seq)) {
      int ms = (int)((esp_timer_get_time() - t0) / 1000);
      return ms < 1 ? 1 : ms;
    }
  }
}

static void pingTask(void *)
{
  int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  uint16_t seq = 1;
  while (s_pRun && sock >= 0) {
    uint32_t cycle = millis();
    size_t n;
    { std::lock_guard<std::mutex> lk(s_pmx); n = s_pt.size(); }
    if (n == 0 || !netConnected()) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    for (size_t i = 0; i < n && s_pRun; ++i) {
      std::string addr; uint32_t ip;
      { std::lock_guard<std::mutex> lk(s_pmx); if (i >= s_pt.size()) break; addr = s_pt[i].addr; ip = s_pt[i].ip; }
      if (!ip) ip = resolveHost(addr);
      int ms = ip ? pingOnce(sock, ip, seq++, 1000) : -1;
      std::lock_guard<std::mutex> lk(s_pmx);
      if (i >= s_pt.size() || s_pt[i].addr != addr) break;     // list changed meanwhile
      PingTarget & t = s_pt[i];
      t.ip = ip;
      t.sent++;
      bool wasUp = t.up, wasSeen = t.seen;
      t.seen = true;
      if (ms >= 0) {
        t.got++; t.last = ms; t.sumMs += ms; t.streakLost = 0;
        if (t.got == 1 || ms < t.minMs) t.minMs = ms;
        if (ms > t.maxMs) t.maxMs = ms;
        t.up = true;
        char b[96]; snprintf(b, sizeof(b), "Reply from %s: time=%d ms", addr.c_str(), ms);
        plog(b);
        if (wasSeen && !wasUp) plog("*** " + addr + " is back UP");
      } else {
        t.last = -1; t.streakLost++;
        if (t.streakLost >= 2) t.up = false;           // two misses in a row = down (one can be a fluke)
        plog(ip ? "Request timed out: " + addr : "Can't find " + addr + " (name lookup failed)");
        if (wasUp && !t.up) plog("*** " + addr + " is DOWN");
      }
      s_pChanged++;
    }
    int rest = 1000 - (int)(millis() - cycle);
    if (rest > 0) vTaskDelay(pdMS_TO_TICKS(rest));
  }
  if (sock >= 0) close(sock);
  else { std::lock_guard<std::mutex> lk(s_pmx); plog("Could not open a ping socket"); s_pChanged++; }
  s_pRun = false;
  s_pTaskAlive = false;
  vTaskDelete(nullptr);
}

class PingApp : public AppFrame {
public:
  static AppFrame * inst;
  uiTextEdit * addrEd;
  uiListBox  * list;
  TextArea   * logView;
  uiButton   * startBtn;
  uiLabel    * summary;
  int lastChanged = -1;
  bool wasRunning = false;

  PingApp() : AppFrame("Ping", Point(20, 10), Size(600, 440), &inst) {
    label("Address (IP or name)", cx, cy + 3, 140, 18);
    addrEd = edit("", cx + 142, cy, 190);
    button("Add", cx + 338, cy, 60)->onClick = [this]() { addTarget(); };
    button("Remove", cx + 404, cy, 70)->onClick = [this]() { removeTarget(); };
    startBtn = button("Start", cx + 480, cy, 106);
    startBtn->onClick = [this]() { if (s_pRun) stop(); else start(); };
    addrEd->onKeyType = [this](uiKeyEventInfo const & k) { if (k.VK == VK_RETURN || k.VK == VK_KP_ENTER) addTarget(); };

    list = S(new uiListBox(this, Point(cx, cy + 30), Size(586, 150)));
    list->items().takeStrings();
    list->listBoxStyle().textFont = &FONT_8x14;      // monospace so the columns line up
    list->listBoxStyle().itemHeight = 18;
    summary = label("", cx, cy + 186, 586, 18);
    summary->labelStyle().textFont = &FONT_std_12;
    logView = new TextArea(this, Point(cx, cy + 206), Size(586, 178), false);

    button("Clear stats", cx, cy + 388, 100)->onClick = [this]() {
      std::lock_guard<std::mutex> lk(s_pmx);
      for (auto & t : s_pt) { auto a = t.addr; t = PingTarget(); t.addr = a; }
      s_plogN = 0; s_pChanged++;
    };
    load();
    refresh(true);
    startTimer(500);
    onTimer = [this](uiTimerHandle) { refresh(false); };
  }

  void load() {
    std::lock_guard<std::mutex> lk(s_pmx);
    if (!s_pt.empty()) return;                      // already loaded (app reopened while pinging)
    std::string s;
    if (readFileStr(dataPath("ping.txt").c_str(), s)) {
      size_t st = 0;
      for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == '\n') {
          std::string l = s.substr(st, i - st); st = i + 1;
          while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
          if (!l.empty()) { PingTarget t; t.addr = l; s_pt.push_back(t); }
        }
    }
    if (s_pt.empty()) {                             // a useful start: the router and a public DNS server
      if (netConnected()) { PingTarget r; r.addr = WiFi.gatewayIP().toString().c_str(); s_pt.push_back(r); }
      PingTarget g; g.addr = "8.8.8.8"; s_pt.push_back(g);
    }
  }

  void save() {
    std::string s;
    { std::lock_guard<std::mutex> lk(s_pmx); for (auto & t : s_pt) s += t.addr + "\n"; }
    writeFileStr(dataPath("ping.txt").c_str(), s);
  }

  void addTarget() {
    std::string a = addrEd->text();
    while (!a.empty() && a.back() == ' ') a.pop_back();
    while (!a.empty() && a[0] == ' ') a.erase(0, 1);
    if (a.empty()) return;
    {
      std::lock_guard<std::mutex> lk(s_pmx);
      for (auto & t : s_pt) if (t.addr == a) return;
      PingTarget t; t.addr = a; s_pt.push_back(t); s_pChanged++;
    }
    addrEd->setText(""); addrEd->repaint();
    save();
    refresh(true);
  }

  void removeTarget() {
    int i = list->firstSelectedItem();
    {
      std::lock_guard<std::mutex> lk(s_pmx);
      if (i < 0 || i >= (int)s_pt.size()) return;
      s_pt.erase(s_pt.begin() + i); s_pChanged++;
    }
    save();
    refresh(true);
  }

  void start() {
    if (s_pTaskAlive) return;
    if (!netConnected()) { gApp->messageBox("Ping", "Join a Wi-Fi network first (Settings).", "OK", nullptr, nullptr, uiMessageBoxIcon::Warning); return; }
    s_pRun = true; s_pTaskAlive = true;
    { std::lock_guard<std::mutex> lk(s_pmx); plog("--- started ---"); s_pChanged++; }
    xTaskCreatePinnedToCore(pingTask, "ping", 4096, nullptr, 1, nullptr, 0);
    refresh(true);
  }

  void stop() {
    s_pRun = false;
    { std::lock_guard<std::mutex> lk(s_pmx); plog("--- stopped ---"); s_pChanged++; }
    refresh(true);
  }

  void refresh(bool force) {
    bool running = s_pRun;
    if (running != wasRunning || force) {
      wasRunning = running;
      startBtn->setText(running ? "Stop" : "Start");
      startBtn->buttonStyle().backgroundColor = running ? T.accent : T.face;
      startBtn->buttonStyle().textColor = running ? T.accentText : T.text;
      startBtn->repaint();
    }
    int ch = s_pChanged;
    if (ch == lastChanged && !force) return;
    lastChanged = ch;
    std::vector<std::string> rows, logl;
    int up = 0, down = 0;
    {
      std::lock_guard<std::mutex> lk(s_pmx);
      for (auto & t : s_pt) {
        char b[160];
        const char * st = !t.seen ? "  --  " : t.up ? "  UP  " : " DOWN ";
        if (t.seen) (t.up ? up : down)++;
        int loss = t.sent ? (t.sent - t.got) * 100 / t.sent : 0;
        if (t.got)
          snprintf(b, sizeof(b), "%s%-17.17s last%4s avg%4d min%4d max%4d ms lost %d%%/%d", st, t.addr.c_str(),
                   t.last >= 0 ? (std::to_string(t.last)).c_str() : "--", (int)(t.sumMs / t.got), t.minMs, t.maxMs, loss, t.sent);
        else
          snprintf(b, sizeof(b), "%s%-17.17s %s", st, t.addr.c_str(), t.sent ? (std::string("no replies, ") + std::to_string(t.sent) + " sent").c_str() : "not pinged yet");
        rows.push_back(b);
      }
      // only the lines that fit in the log box
      uint32_t show = imin(imin((int)s_plogN, 48), logView->visRows());
      for (uint32_t i = s_plogN - show; i < s_plogN; ++i) logl.push_back(s_plog[i % 48]);
    }
    int sel = list->firstSelectedItem();
    list->items().clear();
    for (auto & r : rows) list->items().append(r.c_str());
    if (sel >= 0 && sel < (int)rows.size()) list->selectItem(sel);
    list->repaint();
    char b[120];
    snprintf(b, sizeof(b), "%s   %d up, %d down, %d addresses   (saved in tidyos/ping.txt)", running ? "Pinging every second..." : "Stopped.", up, down, (int)rows.size());
    summary->setText(b); summary->repaint();
    logView->setLines(std::move(logl));
    logView->scrollTo(0);
    logView->repaint();
  }
};
AppFrame * PingApp::inst = nullptr;

void openPing()
{
  if (!PingApp::inst) new PingApp();
  PingApp::inst->open();
}

// ====================================================================== Switches
// Things that cost memory, each with an on/off switch. All off by default, because the ESP32
// is short of RAM. Remembered after a restart.
//   SD card        ~23 KB  (card driver + file system). Off: files go to internal flash.
//   Wireless mesh  a few KB (ESP-NOW for The Shelf's Coms mesh)
//   Blue light     ~2 KB   (the little task that fades it)

#include "led.h"

class SwitchesApp : public AppFrame {
public:
  static AppFrame * inst;
  uiButton * sw[3];
  uiLabel  * note[3];
  uiLabel  * sdInfo, * mem;

  SwitchesApp() : AppFrame("Switches", Point(110, 50), Size(420, 360), &inst) {
    static const char * names[3] = { "SD card", "Wireless mesh", "Blue light" };
    for (int i = 0; i < 3; ++i) {
      int y = cy + 4 + i * 96;
      auto l = label(names[i], cx + 4, y + 8, 150, 24);
      l->labelStyle().textFont = &FONT_std_18;
      sw[i] = button("", cx + 250, y, 150, 38);
      sw[i]->buttonStyle().textFont = &FONT_std_18;
      sw[i]->onClick = [this, i]() { flip(i); };
      note[i] = label("", cx + 4, y + 44, 400, 18);
      note[i]->labelStyle().textFont = &FONT_std_12;
    }
    sdInfo = label("", cx + 4, cy + 4 + 62, 400, 18);
    sdInfo->labelStyle().textFont = &FONT_std_12;
    mem = label("", cx + 4, cy + 300, 400, 18);
    mem->labelStyle().textFont = &FONT_std_12;
    refresh();
    startTimer(1000);
    onTimer = [this](uiTimerHandle) { refresh(); };
  }

  void flip(int i) {
    if (i == 0) {
      if (gSDMounted) {
        if (gApp->messageBox("SD card", "Turn the SD card off? Close documents from it first.",
                             "Turn off", "Cancel", nullptr, uiMessageBoxIcon::Question) != uiMessageBoxResult::Button1) return;
        sdTurnOff();
      } else {
        if (ramFree() < 30000) {
          gApp->messageBox("SD card", "Not enough memory to turn the card on. Close an app.", "OK", nullptr, nullptr, uiMessageBoxIcon::Warning);
          return;
        }
        sw[0]->setText("Looking..."); sw[0]->repaint();
        if (!sdTurnOn())
          gApp->messageBox("SD card", "No card found. Check it's in and formatted FAT32.",
                           "OK", nullptr, nullptr, uiMessageBoxIcon::Warning);
      }
    } else if (i == 1) {
      gSet.meshOn = !gSet.meshOn;
      settingsSave();
      comsSetOn(gSet.meshOn);
    } else {
      gSet.ledOn = !gSet.ledOn;
      settingsSave();
      ledSetOn(gSet.ledOn);
    }
    refresh();
  }

  void show(int i, bool on, const char * onText, const char * offText) {
    sw[i]->setText(on ? onText : offText);
    sw[i]->buttonStyle().backgroundColor = on ? T.accent : T.face;
    sw[i]->buttonStyle().textColor       = on ? T.accentText : T.text;
    sw[i]->repaint();
  }
  void set(uiLabel * l, const std::string & t) { l->setText(t.c_str()); l->repaint(); }

  void refresh() {
    show(0, gSDMounted, "ON", gSet.sdOn ? "ON (no card)" : "OFF");
    if (gSDMounted) {
      int64_t total = 0, used = 0;
      FileBrowser::getFSInfo(DriveType::SDCard, 0, &total, &used);
      char b[120];
      snprintf(b, sizeof(b), "%s free of %s  -  wiring %s", kb(total - used).c_str(), kb(total).c_str(), gSDWiring);
      set(note[0], "Documents and TidyOS data are on the card. Uses ~15 KB of memory.");
      set(sdInfo, b);
    } else {
      set(note[0], "Off: files go to internal storage. Saves ~15 KB of memory.");
      set(sdInfo, "Files already on the card stay there for next time.");
    }
    show(1, gSet.meshOn, "ON", "OFF");
    if (gSet.meshOn && !msgStarted()) set(note[1], "Starts once TidyOS is on your home Wi-Fi. Coms boards nearby.");
    else set(note[1], gSet.meshOn ? "Talking with Coms boards nearby (2.4 GHz). Uses a few KB." : "Off: no Coms mesh. Shelf messages on Wi-Fi still work.");
    show(2, gSet.ledOn, "ON", "OFF");
    set(note[2], gSet.ledOn ? "Breathing = ready, flicker = SD card busy. Uses ~2 KB." : "Off: the blue LED stays dark. Saves ~2 KB.");
    char b[80];
    snprintf(b, sizeof(b), "Free memory now: %u KB   (switches are remembered)", (unsigned)(ramFree() / 1024));
    set(mem, b);
  }
};
AppFrame * SwitchesApp::inst = nullptr;

void openSwitches()
{
  if (!SwitchesApp::inst) new SwitchesApp();
  SwitchesApp::inst->open();
}
