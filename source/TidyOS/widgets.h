// TidyOS custom widgets: multi-line TextArea and spreadsheet grid
#pragma once
#include <map>
#include "core.h"

using fabgl::uiControl;
using fabgl::uiWindow;
using fabgl::uiEvent;
using fabgl::uiKeyEventInfo;

// ---------------------------------------------------------------- TextArea
// Editable (Neat) or read-only (Browser) monospaced text view.
class TextArea : public uiControl {
public:
  TextArea(uiWindow * parent, const Point & pos, const Size & size, bool editable);

  void processEvent(uiEvent * event) override;

  void        setText(const std::string & s);
  std::string getText();
  void        setLines(std::vector<std::string> && l);
  int         visRows();
  int         visCols();
  void        scrollTo(int line);

  std::vector<std::string> lines;
  bool editable;
  bool modified = false;
  int  crow = 0, ccol = 0, top = 0, left = 0;
  fabgl::FontInfo const * font;

  std::function<void()>              onModified;
  std::function<void(int, int)>      onClickChar;     // row, col (read-only mode)
  std::function<bool(uiKeyEventInfo const &)> onKey;  // return true when handled

private:
  void paint(Rect const & r);
  void ensureVisible();
  void changed();
  void keyDown(uiKeyEventInfo const & k);
  void insertChar(char c);
  int  textX0() { return 3; }
  int  textY0() { return 2; }
};

// ---------------------------------------------------------------- Spreadsheet model
class Sheet {
public:
  static const int COLS = 26;
  static const int ROWS = 200;
  std::map<int, std::string> cells;

  static int key(int r, int c) { return r * COLS + c; }
  const std::string & raw(int r, int c);
  void set(int r, int c, const std::string & v);
  double value(int r, int c, bool & err, int depth = 0);
  std::string display(int r, int c, bool & isNum);
  std::string toCSV();
  void fromCSV(const std::string & s);
  static std::string cellName(int r, int c);

private:
  struct P { const char * s; int depth; bool err; };
  double expr(P & p);
  double term(P & p);
  double power(P & p);
  double unary(P & p);
  double primary(P & p);
  bool   cellRef(P & p, int & r, int & c);
  void   ws(P & p) { while (*p.s == ' ') ++p.s; }
};

class SheetGrid : public uiControl {
public:
  SheetGrid(uiWindow * parent, const Point & pos, const Size & size, Sheet * sheet);
  void processEvent(uiEvent * event) override;

  Sheet * sheet;
  int  selR = 0, selC = 0, topR = 0, leftC = 0;
  bool editing = false;
  std::string edit;
  std::function<void()> onSelect;      // selection / edit buffer changed
  std::function<void()> onModified;
  std::function<bool(uiKeyEventInfo const &)> onKey;

  int visRows();
  int visCols();
  void commit();

private:
  void paint(Rect const & r);
  void ensureVisible();
  void keyDown(uiKeyEventInfo const & k);
  static const int HDRW = 34, COLW = 74, ROWH = 16;
};
