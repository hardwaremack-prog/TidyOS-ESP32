// TidyOS apps: Calculator, Neat Lite, Tidy Lite, Ready Lite, Steady Lite,
//              Browser, Files, Settings, System
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

class NeatApp : public AppFrame {
public:
  static AppFrame * inst;
  TextArea * ta;
  uiLabel *  saved;
  std::string path;
  bool dirty = false;

  NeatApp() : AppFrame("Neat Lite - Untitled", Point(150, 20), Size(484, 432), &inst) {
    button("New",      cx,       cy, 44)->onClick = [this]() { newDoc(); };
    button("Open",     cx + 48,  cy, 46)->onClick = [this]() { openDoc(); };
    button("Save",     cx + 98,  cy, 44)->onClick = [this]() { save(); };
    button("Save as",  cx + 146, cy, 60)->onClick = [this]() { saveAs(); };
    button("Print",    cx + 210, cy, 46)->onClick = [this]() { print(); };
    button("Spelling", cx + 260, cy, 70)->onClick = [this]() { spelling(); };
    saved = makeSavedLight(this, cx + 396, cy + 2);
    ta = new TextArea(this, Point(cx, cy + 28), Size(466, 374), true);
    ta->isMisspelled = [](const std::string & w) { return !spellIsWord(w); };
    ta->onModified = [this]() { if (!dirty) { dirty = true; setSavedLight(saved, false); } };
    ta->onKey = [this](uiKeyEventInfo const & k) {
      if (!k.CTRL) return false;
      if (k.VK == VK_s || k.VK == VK_S) { save(); return true; }
      if (k.VK == VK_o || k.VK == VK_O) { openDoc(); return true; }
      if (k.VK == VK_n || k.VK == VK_N) { newDoc(); return true; }
      if (k.VK == VK_p || k.VK == VK_P) { print(); return true; }
      return false;
    };
  }

  std::string docTitle() { return path.empty() ? std::string("Untitled") : baseName(path); }

  void print() {
    PrintDoc d;
    d.title = docTitle();
    d.lines = ta->lines;
    printDialog(d);
  }

  // next misspelled word at or after (r, c)
  bool findNext(int r, int c, int stopRow, int & fr, int & fc, int & flen) {
    for (int row = r; row < (int)ta->lines.size() && row <= stopRow; ++row) {
      const std::string & l = ta->lines[row];
      int i = row == r ? c : 0, n = l.size();
      while (i < n) {
        while (i < n && !isalpha((unsigned char)l[i])) ++i;
        int st = i;
        while (i < n && (isalpha((unsigned char)l[i]) || (l[i] == '\'' && i + 1 < n && isalpha((unsigned char)l[i + 1])))) ++i;
        if (i > st && !spellIsWord(l.substr(st, i - st))) { fr = row; fc = st; flen = i - st; return true; }
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
    int startR = ta->crow, startC = ta->ccol;
    int r = startR, c = startC, stopRow = ta->lines.size() - 1;
    bool wrapped = false;
    int fixed = 0;
    for (;;) {
      int fr, fc, flen;
      bool found = findNext(r, c, stopRow, fr, fc, flen);
      if (found && wrapped && (fr > startR || (fr == startR && fc >= startC))) found = false;
      if (!found) {
        if (!wrapped && (startR > 0 || startC > 0)) { wrapped = true; r = 0; c = 0; stopRow = startR; continue; }
        break;
      }
      // show the word
      ta->crow = fr; ta->ccol = fc;
      ta->hlRow = fr; ta->hlCol = fc; ta->hlLen = flen;
      if (fr < ta->top || fr >= ta->top + ta->visRows() - 1) ta->top = imax(0, fr - 2);
      ta->left = 0;
      ta->repaint();
      std::string word = ta->lines[fr].substr(fc, flen), rep;
      int a = spellAsk(word, rep);
      if (a == 0) break;
      if (a == 1) {
        ta->lines[fr].replace(fc, flen, rep);
        c = fc + rep.size();
        ++fixed;
        if (!dirty) { dirty = true; setSavedLight(saved, false); }
      } else {
        if (a == 2) spellIgnore(word);
        if (a == 3) spellAddWord(word);
        c = fc + flen;
      }
      r = fr;
    }
    ta->hlRow = -1;
    ta->repaint();
    char b[80];
    snprintf(b, sizeof(b), fixed ? "Spell check done. %d word%s fixed." : "Spell check done.", fixed, fixed == 1 ? "" : "s");
    gApp->messageBox("Spelling", b, "OK", nullptr, nullptr, uiMessageBoxIcon::Info);
    gApp->setFocusedWindow(ta);
  }

  void setPath(const std::string & p) {
    path = p;
    setTitle(("Neat Lite - " + docTitle()).c_str());
    repaint();
  }

  void loadFile(const char * p) {
    std::string s;
    if (!readFileStr(p, s)) { gApp->messageBox("Neat", "Could not open that file.", "OK", nullptr, nullptr, uiMessageBoxIcon::Error); return; }
    ta->setText(s);
    setPath(p);
    dirty = false;
    setSavedLight(saved, true);
    gApp->setFocusedWindow(ta);
  }

  bool askSave() {    // true = go on
    if (!dirty) return true;
    auto r = gApp->messageBox("Neat", "Save your changes first?", "Save", "Don't save", "Cancel", uiMessageBoxIcon::Question);
    if (r == uiMessageBoxResult::Button1) return save();
    return r == uiMessageBoxResult::Button2;
  }

  void newDoc() {
    if (!askSave()) return;
    ta->setText("");
    setPath("");
    dirty = false;
    setSavedLight(saved, true);
  }

  void openDoc() {
    if (!askSave()) return;
    std::string p = askFile("Open document", "Open", "", path.empty() ? documentsDir() : dirOf(path));
    if (!p.empty()) loadFile(p.c_str());
  }

  bool save() {
    if (path.empty()) return saveAs();
    if (!writeFileStr(path.c_str(), ta->getText())) {
      gApp->messageBox("Neat", "Could not save. Is the SD card in?", "OK", nullptr, nullptr, uiMessageBoxIcon::Error);
      return false;
    }
    dirty = false;
    setSavedLight(saved, true);
    return true;
  }

  bool saveAs() {
    std::string p = askFile("Save document", "Save", path.empty() ? "note.txt" : baseName(path).c_str(), path.empty() ? documentsDir() : dirOf(path));
    if (p.empty()) return false;
    if (!strlen(fileExt(p.c_str()))) p += ".txt";
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
  else gApp->setFocusedWindow(n->ta);
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
    if (FileBrowser::getFSInfo(sd ? DriveType::SDCard : DriveType::SPIFFS, 0, &total, &used))
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
  uiCheckBox * dstCb;
  uiButton   * colorsBtn;
  std::vector<std::string> scan;
  bool scanning = false;
  int  devCount = -1;
  int  colors;

  SettingsApp() : AppFrame("Settings", Point(20, 8), Size(600, 446), &inst) {
    colors = gSet.colors;
    int y = cy;
    head("Wi-Fi", y);
    label("Network",  cx, y + 22, 70, 18);
    ssidEd = edit(gSet.ssid.c_str(), cx + 72, y + 19, 200);
    label("Password", cx, y + 48, 70, 18);
    passEd = edit(gSet.pass.c_str(), cx + 72, y + 45, 200);
    passEd->textEditProps().passwordMode = true;
    button("Connect",       cx + 72,  y + 71, 96)->onClick = [this]() { netConnect(ssidEd->text(), passEd->text()); };
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

    y += 102;
    head("Browser", y);
    label("Helper address", cx, y + 22, 100, 18);
    proxyEd = edit(gSet.proxy.c_str(), cx + 104, y + 19, 180);
    label("(Pi / The Shelf, e.g. 192.168.1.50:8088 - leave empty to skip)", cx + 290, y + 22, 300, 18)->labelStyle().textFont = &FONT_std_12;
    label("Home page", cx, y + 48, 100, 18);
    homeEd = edit(gSet.home.c_str(), cx + 104, y + 45, 300);

    y += 76;
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

    y += 52;
    button("Save",           cx + 300, y, 136, 26)->onClick = [this]() { saveAll(); gApp->messageBox("Settings", "Saved.", "OK", nullptr, nullptr, uiMessageBoxIcon::Info); };
    button("Save + restart", cx + 450, y, 136, 26)->onClick = [this]() { saveAll(); delay(300); ESP.restart(); };
    label("Screen colours apply after restart.", cx, y + 5, 280, 18)->labelStyle().textFont = &FONT_std_12;

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

// ====================================================================== System

class SystemApp : public AppFrame {
public:
  static AppFrame * inst;
  TextArea * view;

  SystemApp() : AppFrame("System", nextPos(400, 330), Size(400, 330), &inst) {
    view = new TextArea(this, Point(cx, cy), Size(380, 254), false);
    view->setFocusIndex(-1);
    button("Restart", cx + 280, cy + 262, 100, 24)->onClick = []() {
      if (gApp->messageBox("Restart", "Restart TidyOS now? Unsaved work will be lost.", "Restart", "Cancel", nullptr, uiMessageBoxIcon::Warning) == uiMessageBoxResult::Button1)
        ESP.restart();
    };
    label("TidyOS " TIDYOS_VERSION "  -  FabGL + NimBLE", cx, cy + 266, 260, 18)->labelStyle().textFont = &FONT_std_12;
    startTimer(1000);
    onTimer = [this](uiTimerHandle) { update(); };
    update();
  }

  void update() {
    std::vector<std::string> l;
    char b[96];
    uint32_t up = millis() / 1000;
    snprintf(b, sizeof(b), "Up time        %uh %02um %02us", up / 3600, (up / 60) % 60, up % 60); l.push_back(b);
    snprintf(b, sizeof(b), "Screen         640 x 480, %d colours", gColors); l.push_back(b);
    snprintf(b, sizeof(b), "CPU            ESP32 %u MHz, 2 cores", (unsigned)getCpuFrequencyMhz()); l.push_back(b);
    snprintf(b, sizeof(b), "Free RAM       %u KB", (unsigned)(ESP.getFreeHeap() / 1024)); l.push_back(b);
    snprintf(b, sizeof(b), "Biggest block  %u KB", (unsigned)(ESP.getMaxAllocHeap() / 1024)); l.push_back(b);
    snprintf(b, sizeof(b), "Lowest free    %u KB", (unsigned)(ESP.getMinFreeHeap() / 1024)); l.push_back(b);
    snprintf(b, sizeof(b), "Flash          %u MB", (unsigned)(ESP.getFlashChipSize() >> 20)); l.push_back(b);
    l.push_back("");
    l.push_back("Wi-Fi          " + netStatus());
    if (netConnected()) { snprintf(b, sizeof(b), "Signal         %d dBm", (int)WiFi.RSSI()); l.push_back(b); }
    snprintf(b, sizeof(b), "Bluetooth      %d keyboard(s), %d mouse/mice", inputKeyboards(), inputMice()); l.push_back(b);
    l.push_back("");
    int64_t total = 0, used = 0;
    if (gSDMounted && FileBrowser::getFSInfo(DriveType::SDCard, 0, &total, &used)) {
      snprintf(b, sizeof(b), "SD card        %lld MB free of %lld MB", (total - used) >> 20, total >> 20); l.push_back(b);
    } else if (gFlashMounted && FileBrowser::getFSInfo(DriveType::SPIFFS, 0, &total, &used)) {
      snprintf(b, sizeof(b), "Storage        internal flash, %lld KB free", (total - used) >> 10); l.push_back(b);
    } else {
      l.push_back("Storage        none");
    }
    int top = view->top;
    view->setLines(std::move(l));
    view->top = top;
  }
};
AppFrame * SystemApp::inst = nullptr;

void openSystem()
{
  if (!SystemApp::inst) new SystemApp();
  SystemApp::inst->open();
}

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
  if (!MessagesApp::inst) new MessagesApp();
  MessagesApp::inst->open();
}
