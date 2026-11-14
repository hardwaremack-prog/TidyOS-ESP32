// TidyOS rich text editor control (Neat Lite): styled text with fonts, sizes, bold, underline,
// paragraph spacing and alignment, on top of RichDoc.
#pragma once
#include <functional>
#include "fabgl.h"
#include "fabui.h"
#include "richdoc.h"

using fabgl::uiControl;
using fabgl::uiWindow;
using fabgl::uiEvent;
using fabgl::uiKeyEventInfo;

int         richFontCount();
const char * richFontName(int i);

class RichText : public uiControl {
public:
  RichText(uiWindow * parent, const fabgl::Point & pos, const fabgl::Size & size);

  void processEvent(uiEvent * event) override;

  RichDoc doc;
  RPos    cur, anchor;           // caret and the other end of the selection
  RStyle  typing;                // style for the next typed text when nothing is selected
  RStyle  normal;                // default style of new documents
  int     top = 0;               // scroll position (pixels)
  RPos    hlA, hlB;              // highlighted range (spell check); hlA == hlB = none

  std::function<void()>                       onModified;
  std::function<void()>                       onSelChange;   // caret/selection moved: update the toolbar
  std::function<bool(uiKeyEventInfo const &)> onKey;         // return true when handled
  std::function<bool(const std::string &)>    isMisspelled;

  bool hasSel() const { return cur != anchor; }
  void selRange(RPos & a, RPos & b) const { a = cur < anchor ? cur : anchor; b = cur < anchor ? anchor : cur; }
  RStyle styleHere() const;                           // style at the caret / start of the selection

  // formatting (selection, or the typing style when nothing is selected)
  void setSize(int px);
  void setFont(int f);
  void toggleFlag(uint8_t flag);
  void paraAfter(int delta);                          // space after paragraph(s), px
  void paraSpacing(int percent);                      // line spacing 100 / 150 / 200
  void paraAlign(int a);                              // 0 left, 1 centre, 2 right

  void setPlain(const std::string & s);
  bool setRtf(const std::string & s);
  void selectRange(RPos a, RPos b);                   // select + scroll into view
  void replaceSel(const std::string & s);             // typed text / spelling fix
  void selectAll();
  void copy(bool cut);
  void paste();
  void relayout();                                    // after loading / big changes

private:
  int  textW();
  void changed(bool layoutAll = false);
  void ensureVisible();
  void moveTo(RPos p, bool extend);
  void keyDown(uiKeyEventInfo const & k);
  void paint(fabgl::Rect const & clip);
  void drawLine(fabgl::Canvas * cv, const RPara & P, const RLine & L, int x0, int yTop, RPos sa, RPos sb, int pIdx);
  int  wantX = -1;                                    // remembered x for up/down
  bool dragging = false;
};
