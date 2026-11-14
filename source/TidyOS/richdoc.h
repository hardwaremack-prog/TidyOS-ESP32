// TidyOS rich text document: paragraphs of styled runs, word-wrap layout, RTF in/out.
// No FabGL in here (so it can be tested on a PC); character sizes come from the r* functions.
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <stdint.h>
#include "stream.h"

enum { RS_BOLD = 1, RS_UNDER = 2 };

struct RStyle {
  uint8_t font  = 0;     // family index (see richfonts / richtext.cpp)
  uint8_t size  = 16;    // pixel height asked for (8..48)
  uint8_t flags = 0;     // RS_BOLD | RS_UNDER
  bool operator==(const RStyle & o) const { return font == o.font && size == o.size && flags == o.flags; }
  bool operator!=(const RStyle & o) const { return !(*this == o); }
};

struct RRun { std::string t; RStyle s; };

struct RLine {
  int a = 0, b = 0;      // byte range [a, b) of the paragraph text
  int y = 0;             // top, relative to the paragraph
  int h = 0, asc = 0;    // height (with line spacing) and baseline
  int w = 0;             // width of the visible text (trailing spaces left out)
};

struct RPara {
  std::vector<RRun> runs;
  uint8_t after   = 6;     // space after the paragraph (px)
  uint8_t spacing = 100;   // line spacing in percent (100, 150, 200)
  uint8_t align   = 0;     // 0 left, 1 centre, 2 right
  // layout cache
  std::vector<RLine> lines;
  int  y = 0, height = 0;
  bool dirty = true;
};

struct RPos {
  int p, o;                // paragraph, byte offset in its text
  RPos(int p_ = 0, int o_ = 0) : p(p_), o(o_) {}
  bool operator<(const RPos & x) const  { return p < x.p || (p == x.p && o < x.o); }
  bool operator==(const RPos & x) const { return p == x.p && o == x.o; }
  bool operator!=(const RPos & x) const { return !(*this == x); }
};

// character metrics, provided by the display code (or a stub in tests)
int rCharW(const RStyle & s, uint8_t c);
int rFontH(const RStyle & s);
int rAscent(const RStyle & s);

class RichDoc {
public:
  std::vector<RPara> paras;
  int width = 400;           // layout width in pixels
  int totalHeight = 0;

  RichDoc() { clear(); }
  void clear(const RStyle & st = RStyle());

  std::string text(int p) const;
  int  len(int p) const;
  RStyle styleAt(int p, int o) const;              // style of the character before o (or the first one)
  RPos endPos() const { return RPos{ (int)paras.size() - 1, len((int)paras.size() - 1) }; }
  RPos clamp(RPos x) const;

  // editing (positions are kept valid by the caller using the returned position)
  RPos insert(RPos at, const std::string & s, const RStyle & st);   // '\n' starts a new paragraph
  RPos erase(RPos a, RPos b);                                        // returns a
  void applyStyle(RPos a, RPos b, const std::function<void(RStyle &)> & f);
  void forParas(int p0, int p1, const std::function<void(RPara &)> & f);
  std::string plainRange(RPos a, RPos b) const;
  std::vector<RPara> copyRange(RPos a, RPos b) const;                // styled copy (for the clipboard)
  RPos pasteParas(RPos at, const std::vector<RPara> & ps);

  // layout + geometry (document coordinates; x from the left edge of the text area)
  void layout();
  void dirtyAll() { for (auto & p : paras) p.dirty = true; }
  int  lineX(const RPara & P, const RLine & L) const;
  void caret(RPos pos, int & x, int & y, int & h, int & asc) const;
  RPos hit(int x, int y) const;
  int  lineOf(RPos pos) const;                     // index of the line inside its paragraph

  // files
  void setPlain(const std::string & s, const RStyle & st = RStyle());
  std::string plain() const;
  std::string toRtf() const;
  bool fromRtf(const std::string & s);
  void writePlain(RSink & o) const;
  void writeRtf(RSink & o) const;
  bool readRtf(RSource & in);                     // false (nothing read) if it isn't RTF
  void readPlain(RSource & in, const RStyle & st = RStyle());
  void compact();                                  // give spare string capacity back
  bool hasFormatting(const RStyle & normal) const;
  size_t bytes() const;

private:
  int  splitRun(int p, int o);                     // makes a run boundary at o, returns the run index starting there
  void tidy(int p);                                // merge equal neighbours, drop empty runs (keeps one)
  void layoutPara(RPara & P);
};
