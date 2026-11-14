// TidyOS rich text document (see richdoc.h)
#include "richdoc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

void RichDoc::clear(const RStyle & st)
{
  paras.clear();
  RPara p;
  p.runs.push_back(RRun{ "", st });
  paras.push_back(p);
  totalHeight = 0;
}

std::string RichDoc::text(int p) const
{
  std::string s;
  for (auto & r : paras[p].runs) s += r.t;
  return s;
}

int RichDoc::len(int p) const
{
  int n = 0;
  for (auto & r : paras[p].runs) n += r.t.size();
  return n;
}

RStyle RichDoc::styleAt(int p, int o) const
{
  const auto & runs = paras[p].runs;
  int pos = 0;
  for (size_t i = 0; i < runs.size(); ++i) {
    int e = pos + runs[i].t.size();
    if (o <= e && (o > pos || i == 0 || runs[i].t.empty())) {
      if (o == pos && i > 0) return runs[i - 1].s;
      return runs[i].s;
    }
    pos = e;
  }
  return runs.empty() ? RStyle() : runs.back().s;
}

RPos RichDoc::clamp(RPos x) const
{
  if (x.p < 0) return RPos{ 0, 0 };
  if (x.p >= (int)paras.size()) return endPos();
  x.o = std::max(0, std::min(x.o, len(x.p)));
  return x;
}

int RichDoc::splitRun(int p, int o)
{
  auto & runs = paras[p].runs;
  int pos = 0;
  for (size_t i = 0; i < runs.size(); ++i) {
    int n = runs[i].t.size();
    if (o == pos) return i;
    if (o < pos + n) {
      RRun tail{ runs[i].t.substr(o - pos), runs[i].s };
      runs[i].t.erase(o - pos);
      runs.insert(runs.begin() + i + 1, tail);
      return i + 1;
    }
    pos += n;
  }
  return runs.size();
}

void RichDoc::tidy(int p)
{
  auto & runs = paras[p].runs;
  for (size_t i = 0; i < runs.size();) {
    if (runs[i].t.empty() && runs.size() > 1) { runs.erase(runs.begin() + i); continue; }
    if (i > 0 && runs[i - 1].s == runs[i].s) { runs[i - 1].t += runs[i].t; runs.erase(runs.begin() + i); continue; }
    ++i;
  }
  if (runs.empty()) runs.push_back(RRun{ "", RStyle() });
  paras[p].dirty = true;
}

RPos RichDoc::insert(RPos at, const std::string & s, const RStyle & st)
{
  at = clamp(at);
  size_t i = 0;
  while (i <= s.size()) {
    size_t nl = s.find('\n', i);
    std::string piece = s.substr(i, nl == std::string::npos ? std::string::npos : nl - i);
    // drop CRs and other control characters (tabs become spaces)
    std::string clean;
    for (char c : piece) { if (c == '\t') clean += "    "; else if ((uint8_t)c >= 32) clean += c; }
    if (!clean.empty()) {
      int ri = splitRun(at.p, at.o);
      auto & runs = paras[at.p].runs;
      if (len(at.p) == 0) { runs.clear(); runs.push_back(RRun{ clean, st }); }
      else runs.insert(runs.begin() + ri, RRun{ clean, st });
      tidy(at.p);
      at.o += clean.size();
    }
    if (nl == std::string::npos) break;
    // new paragraph: the rest of this one moves down
    int ri = splitRun(at.p, at.o);
    RPara np;
    np.after = paras[at.p].after; np.spacing = paras[at.p].spacing; np.align = paras[at.p].align;
    auto & runs = paras[at.p].runs;
    np.runs.assign(runs.begin() + ri, runs.end());
    runs.erase(runs.begin() + ri, runs.end());
    if (runs.empty()) runs.push_back(RRun{ "", st });
    if (np.runs.empty()) np.runs.push_back(RRun{ "", st });
    paras.insert(paras.begin() + at.p + 1, np);
    tidy(at.p);
    tidy(at.p + 1);
    at.p += 1; at.o = 0;
    i = nl + 1;
  }
  return at;
}

RPos RichDoc::erase(RPos a, RPos b)
{
  a = clamp(a); b = clamp(b);
  if (b < a) std::swap(a, b);
  if (a == b) return a;
  RStyle keep = styleAt(a.p, a.o + (a.o < len(a.p) ? 1 : 0));
  if (a.p == b.p) {
    int r0 = splitRun(a.p, a.o), r1 = splitRun(a.p, b.o);
    auto & runs = paras[a.p].runs;
    runs.erase(runs.begin() + r0, runs.begin() + r1);
    if (runs.empty()) runs.push_back(RRun{ "", keep });
    tidy(a.p);
    return a;
  }
  // cut the end of a.p, the start of b.p, the paragraphs between, then join
  int r0 = splitRun(a.p, a.o);
  paras[a.p].runs.erase(paras[a.p].runs.begin() + r0, paras[a.p].runs.end());
  int r1 = splitRun(b.p, b.o);
  paras[b.p].runs.erase(paras[b.p].runs.begin(), paras[b.p].runs.begin() + r1);
  for (auto & r : paras[b.p].runs) paras[a.p].runs.push_back(r);
  paras.erase(paras.begin() + a.p + 1, paras.begin() + b.p + 1);
  if (paras[a.p].runs.empty()) paras[a.p].runs.push_back(RRun{ "", keep });
  tidy(a.p);
  return a;
}

void RichDoc::applyStyle(RPos a, RPos b, const std::function<void(RStyle &)> & f)
{
  a = clamp(a); b = clamp(b);
  if (b < a) std::swap(a, b);
  for (int p = a.p; p <= b.p; ++p) {
    int o0 = p == a.p ? a.o : 0, o1 = p == b.p ? b.o : len(p);
    if (o0 >= o1) {
      if (len(p) == 0) { f(paras[p].runs[0].s); paras[p].dirty = true; }   // empty paragraph: its typing style
      continue;
    }
    int r0 = splitRun(p, o0), r1 = splitRun(p, o1);
    for (int r = r0; r < r1; ++r) f(paras[p].runs[r].s);
    tidy(p);
  }
}

void RichDoc::forParas(int p0, int p1, const std::function<void(RPara &)> & f)
{
  for (int p = std::max(0, p0); p <= p1 && p < (int)paras.size(); ++p) { f(paras[p]); paras[p].dirty = true; }
}

std::string RichDoc::plainRange(RPos a, RPos b) const
{
  if (b < a) std::swap(a, b);
  std::string s;
  for (int p = a.p; p <= b.p; ++p) {
    std::string t = text(p);
    int o0 = p == a.p ? a.o : 0, o1 = p == b.p ? b.o : (int)t.size();
    s += t.substr(o0, o1 - o0);
    if (p != b.p) s += '\n';
  }
  return s;
}

std::vector<RPara> RichDoc::copyRange(RPos a, RPos b) const
{
  if (b < a) std::swap(a, b);
  RichDoc tmp;
  tmp.paras.assign(paras.begin() + a.p, paras.begin() + b.p + 1);
  RPos e{ (int)tmp.paras.size() - 1, b.o };
  tmp.erase(e, tmp.endPos());
  tmp.erase(RPos{ 0, 0 }, RPos{ 0, a.o });
  for (auto & P : tmp.paras) { P.lines.clear(); P.dirty = true; }
  return tmp.paras;
}

RPos RichDoc::pasteParas(RPos at, const std::vector<RPara> & ps)
{
  for (size_t i = 0; i < ps.size(); ++i) {
    if (i > 0) at = insert(at, "\n", styleAt(at.p, at.o));
    for (auto & r : ps[i].runs) if (!r.t.empty()) at = insert(at, r.t, r.s);
    if (i > 0) { paras[at.p].after = ps[i].after; paras[at.p].spacing = ps[i].spacing; paras[at.p].align = ps[i].align; }
  }
  return at;
}

// ---------------------------------------------------------------- layout

void RichDoc::layoutPara(RPara & P)
{
  P.lines.clear();
  // flatten to (char, style) without copying the text twice
  struct Ch { uint8_t c; uint8_t run; };
  std::vector<Ch> cs;
  for (size_t r = 0; r < P.runs.size(); ++r) for (char c : P.runs[r].t) cs.push_back(Ch{ (uint8_t)c, (uint8_t)std::min<size_t>(r, 255) });
  auto styleOf = [&](size_t i) -> const RStyle & { return P.runs[cs[i].run].s; };
  int n = cs.size();
  int y = 0;
  auto finish = [&](int a, int b) {
    RLine L;
    L.a = a; L.b = b;
    int h = 0, asc = 0, w = 0, wVis = 0;
    if (a == b) {
      RStyle s = P.runs.empty() ? RStyle() : (a > 0 ? styleOf(a - 1) : P.runs[0].s);
      h = rFontH(s); asc = rAscent(s);
    }
    for (int i = a; i < b; ++i) {
      const RStyle & s = styleOf(i);
      h = std::max(h, rFontH(s)); asc = std::max(asc, rAscent(s));
      w += rCharW(s, cs[i].c);
      if (cs[i].c != ' ') wVis = w;
    }
    L.asc = asc;
    L.h = h * P.spacing / 100;
    L.w = wVis;
    L.y = y;
    y += L.h;
    P.lines.push_back(L);
  };
  int a = 0, x = 0, lastBreak = -1;
  for (int i = 0; i < n; ++i) {
    int cw = rCharW(styleOf(i), cs[i].c);
    if (x + cw > width && i > a && cs[i].c != ' ') {
      int b = lastBreak > a ? lastBreak : i;      // break after the last space, or mid-word if there is none
      finish(a, b);
      a = b; x = 0; lastBreak = -1;
      for (int j = a; j < i; ++j) x += rCharW(styleOf(j), cs[j].c);
    }
    x += cw;
    if (cs[i].c == ' ') lastBreak = i + 1;
  }
  finish(a, n);
  P.height = y + P.after;
  P.dirty = false;
}

void RichDoc::layout()
{
  int y = 0;
  for (auto & P : paras) {
    if (P.dirty) layoutPara(P);
    P.y = y;
    y += P.height;
  }
  totalHeight = y;
}

int RichDoc::lineX(const RPara & P, const RLine & L) const
{
  if (P.align == 1) return std::max(0, (width - L.w) / 2);
  if (P.align == 2) return std::max(0, width - L.w);
  return 0;
}

int RichDoc::lineOf(RPos pos) const
{
  const RPara & P = paras[pos.p];
  for (size_t i = 0; i < P.lines.size(); ++i) {
    bool last = i + 1 == P.lines.size();
    if (pos.o < P.lines[i].b || (pos.o == P.lines[i].b && last)) return i;
  }
  return P.lines.empty() ? 0 : P.lines.size() - 1;
}

void RichDoc::caret(RPos pos, int & x, int & y, int & h, int & asc) const
{
  pos = clamp(pos);
  const RPara & P = paras[pos.p];
  if (P.lines.empty()) { x = 0; y = P.y; h = 16; asc = 12; return; }
  const RLine & L = P.lines[lineOf(pos)];
  int cx = lineX(P, L);
  int o = 0;
  for (auto & r : P.runs) {
    for (char c : r.t) {
      if (o >= L.a && o < pos.o) cx += rCharW(r.s, (uint8_t)c);
      ++o;
    }
  }
  x = cx; y = P.y + L.y; h = L.h; asc = L.asc;
}

RPos RichDoc::hit(int x, int y) const
{
  if (y < 0) return RPos{ 0, 0 };
  int p = 0;
  while (p + 1 < (int)paras.size() && paras[p + 1].y <= y) ++p;
  const RPara & P = paras[p];
  if (P.lines.empty()) return RPos{ p, 0 };
  int li = 0;
  while (li + 1 < (int)P.lines.size() && P.y + P.lines[li + 1].y <= y) ++li;
  const RLine & L = P.lines[li];
  int cx = lineX(P, L);
  int o = 0;
  for (auto & r : P.runs) {
    for (char c : r.t) {
      if (o >= L.a && o < L.b) {
        int w = rCharW(r.s, (uint8_t)c);
        if (x < cx + w / 2) return RPos{ p, o };
        cx += w;
      }
      ++o;
    }
  }
  bool last = li + 1 == (int)P.lines.size();
  int end = L.b;
  if (!last && end > L.a && text(p)[end - 1] == ' ') --end;     // stay on this line
  return RPos{ p, end };
}

// ---------------------------------------------------------------- plain text

void RichDoc::setPlain(const std::string & s, const RStyle & st)
{
  clear(st);
  insert(RPos{ 0, 0 }, s, st);
}

std::string RichDoc::plain() const
{
  std::string s;
  for (size_t p = 0; p < paras.size(); ++p) { if (p) s += '\n'; s += text(p); }
  return s;
}

bool RichDoc::hasFormatting(const RStyle & normal) const
{
  for (auto & P : paras) {
    if (P.after != 6 || P.spacing != 100 || P.align != 0) return true;
    for (auto & r : P.runs) if (!r.t.empty() && r.s != normal) return true;
  }
  return false;
}

size_t RichDoc::bytes() const
{
  size_t n = 0;
  for (size_t p = 0; p < paras.size(); ++p) n += len(p) + 1;
  return n;
}

// ---------------------------------------------------------------- RTF
// Writes a small, standard subset that Word, LibreOffice and WordPad read. Sizes are pixels at
// 96 dpi (1 px = 0.75 pt, \fs counts half-points); spacing is in twips (1 px = 15 twips).

static const char * RTF_FONTS[] = { "Arial", "Times New Roman", "Courier New", "Courier New", "Arial",
                                    "Lucida Console", "Consolas", "Old English Text MT", "Broadway", "Arial Black" };
static const int RTF_FONT_COUNT = sizeof(RTF_FONTS) / sizeof(RTF_FONTS[0]);

std::string RichDoc::toRtf() const
{
  std::string o = "{\\rtf1\\ansi\\ansicpg1252\\deff0{\\fonttbl";
  char b[96];
  for (int i = 0; i < RTF_FONT_COUNT; ++i) { snprintf(b, sizeof(b), "{\\f%d %s;}", i, RTF_FONTS[i]); o += b; }
  o += "}\n";
  for (auto & P : paras) {
    snprintf(b, sizeof(b), "\\pard%s\\sa%d\\sl%d\\slmult1 ", P.align == 1 ? "\\qc" : P.align == 2 ? "\\qr" : "\\ql", P.after * 15, P.spacing * 240 / 100);
    o += b;
    for (auto & r : P.runs) {
      if (r.t.empty()) continue;
      snprintf(b, sizeof(b), "{\\f%d\\fs%d%s%s ", r.s.font, (r.s.size * 3 + 1) / 2, r.s.flags & RS_BOLD ? "\\b" : "", r.s.flags & RS_UNDER ? "\\ul" : "");
      o += b;
      for (unsigned char c : r.t) {
        if (c == '\\' || c == '{' || c == '}') { o += '\\'; o += c; }
        else if (c >= 128) { snprintf(b, sizeof(b), "\\'%02x", c); o += b; }
        else o += c;
      }
      o += "}";
    }
    o += "\\par\n";
  }
  o += "}\n";
  return o;
}

bool RichDoc::fromRtf(const std::string & s)
{
  if (s.compare(0, 5, "{\\rtf") != 0) return false;
  paras.clear();
  RPara cur;
  struct St { RStyle s; bool skip; };
  std::vector<St> stack;
  St st{ RStyle(), false };
  std::string pending;
  RStyle pendStyle;
  auto flush = [&]() {
    if (pending.empty()) return;
    if (!cur.runs.empty() && cur.runs.back().s == pendStyle) cur.runs.back().t += pending;
    else cur.runs.push_back(RRun{ pending, pendStyle });
    pending.clear();
  };
  auto addChar = [&](char c) {
    if (st.skip) return;
    if (!pending.empty() && pendStyle != st.s) flush();
    pendStyle = st.s;
    pending += c;
  };
  auto endPara = [&]() {
    flush();
    if (cur.runs.empty()) cur.runs.push_back(RRun{ "", st.s });
    paras.push_back(cur);
    RPara n;
    n.after = cur.after; n.spacing = cur.spacing; n.align = cur.align;
    cur = n;
  };
  size_t i = 0, n = s.size();
  bool firstInGroup = false;
  while (i < n) {
    char c = s[i];
    if (c == '{') { stack.push_back(st); firstInGroup = true; ++i; continue; }
    if (c == '}') { flush(); if (!stack.empty()) { st = stack.back(); stack.pop_back(); } firstInGroup = false; ++i; continue; }
    if (c == '\r' || c == '\n') { ++i; continue; }
    if (c != '\\') { addChar(c); firstInGroup = false; ++i; continue; }
    // control word / symbol
    ++i;
    if (i >= n) break;
    char d = s[i];
    if (d == '\\' || d == '{' || d == '}') { addChar(d); ++i; firstInGroup = false; continue; }
    if (d == '\'') {
      if (i + 2 < n) { char h[3] = { s[i + 1], s[i + 2], 0 }; addChar((char)strtol(h, nullptr, 16)); }
      i += 3; firstInGroup = false; continue;
    }
    if (d == '*') { st.skip = true; ++i; continue; }
    if (!isalpha((unsigned char)d)) { ++i; continue; }   // other symbols (\~, \-, ...)
    size_t w0 = i;
    while (i < n && isalpha((unsigned char)s[i])) ++i;
    std::string word = s.substr(w0, i - w0);
    bool hasNum = false; long num = 0; bool neg = false;
    if (i < n && s[i] == '-') { neg = true; ++i; }
    while (i < n && isdigit((unsigned char)s[i])) { hasNum = true; num = num * 10 + (s[i] - '0'); ++i; }
    if (neg) num = -num;
    if (i < n && s[i] == ' ') ++i;
    // destinations we don't show
    if (firstInGroup && (word == "fonttbl" || word == "colortbl" || word == "stylesheet" || word == "info" || word == "pict" ||
                         word == "header" || word == "footer" || word == "listtable" || word == "listoverridetable" || word == "generator")) st.skip = true;
    firstInGroup = false;
    if (word == "par" || word == "line") { if (!st.skip) endPara(); }
    else if (word == "tab") addChar(' ');
    else if (word == "pard") { flush(); cur.after = 0; cur.spacing = 100; cur.align = 0; }
    else if (word == "plain") { flush(); st.s.flags = 0; st.s.font = 0; st.s.size = 16; }
    else if (word == "f" && hasNum) { if (!st.skip) { flush(); st.s.font = (num >= 0 && num < RTF_FONT_COUNT) ? num : 0; } }
    else if (word == "fs" && hasNum) { flush(); st.s.size = std::max(8L, std::min(48L, (num * 2 + 1) / 3)); }
    else if (word == "b") { flush(); if (hasNum && num == 0) st.s.flags &= ~RS_BOLD; else st.s.flags |= RS_BOLD; }
    else if (word == "ul") { flush(); if (hasNum && num == 0) st.s.flags &= ~RS_UNDER; else st.s.flags |= RS_UNDER; }
    else if (word == "ulnone") { flush(); st.s.flags &= ~RS_UNDER; }
    else if (word == "qc") cur.align = 1;
    else if (word == "qr") cur.align = 2;
    else if (word == "ql" || word == "qj") cur.align = 0;
    else if (word == "sa" && hasNum) cur.after = std::max(0L, std::min(60L, num / 15));
    else if (word == "sl" && hasNum) { long sp = labs(num) * 100 / 240; cur.spacing = sp >= 175 ? 200 : sp >= 125 ? 150 : 100; }
    else if (word == "u" && hasNum) { addChar(num > 0 && num < 256 ? (char)num : '?'); if (i < n && s[i] == '?') ++i; }
  }
  flush();
  bool anyText = false;
  for (auto & r : cur.runs) if (!r.t.empty()) anyText = true;
  if (anyText || paras.empty()) { if (cur.runs.empty()) cur.runs.push_back(RRun{ "", st.s }); paras.push_back(cur); }
  for (size_t p = 0; p < paras.size(); ++p) tidy(p);
  return true;
}
