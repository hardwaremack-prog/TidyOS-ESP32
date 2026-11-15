// TidyOS custom widgets: multi-line TextArea and spreadsheet grid
#pragma once
#include "stream.h"
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
  std::function<bool(const std::string &)> isMisspelled;   // set to underline spelling mistakes
  int hlRow = -1, hlCol = 0, hlLen = 0;                    // highlighted word (spell check)

private:
  void paint(Rect const & r);
  void ensureVisible();
  void changed();
  void keyDown(uiKeyEventInfo const & k);
  void insertChar(char c);
  int  textX0() { return 3; }
  int  textY0() { return 2; }
};

// word-wrap text into lines of at most `width` characters
void wrapText(const std::string & text, int width, std::vector<std::string> & out);

// ---------------------------------------------------------------- Spreadsheet model
std::string fmtNum(double v);   // how Tidy shows a number

class Sheet {
public:
  static const int COLS = 26;
  static const int ROWS = 200;
  std::map<int, std::string> cells;
  uint8_t colW[COLS];                  // column widths, pixels
  uint8_t rowH[ROWS];                  // row heights, pixels
  bool    sizesChanged = false;
  std::map<int, uint8_t> align;        // per cell: 1 left, 2 centre, 3 right (missing = numbers right, text left)
  uint8_t colAlign[COLS];              // whole-column alignment (0 = none)
  Sheet() { resetSizes(); }
  void resetSizes() { memset(colW, 74, sizeof(colW)); memset(rowH, 16, sizeof(rowH)); memset(colAlign, 0, sizeof(colAlign)); align.clear(); sizesChanged = false; }
  bool sizesDefault() const { if (!align.empty()) return false; for (auto a : colAlign) if (a) return false; for (auto w : colW) if (w != 74) return false; for (auto x : rowH) if (x != 16) return false; return true; }
  int  alignOf(int r, int c) const { auto it = align.find(key(r, c)); return it == align.end() ? colAlign[c] : it->second; }

  static int key(int r, int c) { return r * COLS + c; }
  const std::string & raw(int r, int c);
  void set(int r, int c, const std::string & v);
  double value(int r, int c, bool & err, int depth = 0);
  std::string display(int r, int c, bool & isNum);
  std::string toCSV();
  void fromCSV(const std::string & s);
  void writeCSV(RSink & o);
  void readCSV(RSource & in);
  static std::string cellName(int r, int c);
  void shift(bool rows, int at, int d);          // insert (d > 0) or delete (d < 0) rows / columns at `at`
  bool wouldDrop(bool rows, int d);               // inserting d would push filled cells off the end
  static int  checkState(const std::string & s) { return s == "[x]" || s == "[X]" ? 1 : s == "[ ]" ? 0 : -1; }   // -1 = not a check box

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
  int  ancR = 0, ancC = 0;             // other corner of a dragged / shift-selected range
  bool bands = false;                  // shade every other row
  bool freezeTop = false;              // row 1 stays put while scrolling
  bool editing = false;
  std::string edit;
  std::function<void()> onSelect;      // selection / edit buffer changed
  std::function<void()> onModified;
  std::function<bool(uiKeyEventInfo const &)> onKey;
  std::function<void(int sx, int sy)> onRightClick;   // screen position
  bool allRows() const { return r1() == 0 && r2() == Sheet::ROWS - 1; }   // whole columns picked
  bool allCols() const { return c1() == 0 && c2() == Sheet::COLS - 1; }   // whole rows picked

  int visRows();
  int visCols();
  void commit();
  int  r1() const { return selR < ancR ? selR : ancR; }
  int  r2() const { return selR > ancR ? selR : ancR; }
  int  c1() const { return selC < ancC ? selC : ancC; }
  int  c2() const { return selC > ancC ? selC : ancC; }
  bool isRange() const { return selR != ancR || selC != ancC; }
  void collapse() { ancR = selR; ancC = selC; }
  void toggleChecks();                 // selected cells: add check boxes, or tick / untick them
  void selectRows(int a, int b);       // whole rows a..b
  void selectCols(int a, int b);       // whole columns a..b

private:
  void paint(Rect const & r);
  void ensureVisible();
  void keyDown(uiKeyEventInfo const & k);
  int  rowAt(int i) const { return freezeTop ? (i == 0 ? 0 : topR + i) : topR + i; }   // screen row -> sheet row
  int  hitTest(int px, int py, int & r, int & c);
  int  colX(int c);
  int  rowY(int r);
  int  drag = 0, dragIdx = 0, dragFrom = 0;   // 1 cells, 2 columns, 3 rows, 4 column edge, 5 row edge
  static const int HDRW = 34, HDRH = 16;
};
