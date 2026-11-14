// TidyOS rich text editor control (see richtext.h)
#include "richtext.h"
#include "core.h"

using namespace fabgl;

// ---------------------------------------------------------------- fonts
// Each family lists the bitmap sizes FabGL has; other sizes are drawn 2x/3x/4x enlarged.

struct RFam { const char * name; const FontInfo * f[8]; };
static const RFam FAMS[] = {
  { "Sans",        { &FONT_std_12, &FONT_std_14, &FONT_std_15, &FONT_std_16, &FONT_std_17, &FONT_std_18, &FONT_std_22, &FONT_std_24 } },
  { "Serif",       { &FONT_BIGSERIF_8x14, &FONT_BIGSERIF_8x16 } },
  { "Mono",        { &FONT_6x10, &FONT_7x13, &FONT_8x14, &FONT_8x16, &FONT_9x18, &FONT_10x20 } },
  { "Courier",     { &FONT_COURIER_8x14 } },
  { "Slant",       { &FONT_SLANT_8x14 } },
  { "Computer",    { &FONT_COMPUTER_8x14 } },
  { "LCD",         { &FONT_LCD_8x14 } },
  { "Old English", { &FONT_OLDENGL_8x16 } },
  { "Broadway",    { &FONT_BROADWAY_8x14 } },
  { "Block",       { &FONT_BLOCK_8x14 } },
};
static const int NFAMS = sizeof(FAMS) / sizeof(FAMS[0]);

int richFontCount() { return NFAMS; }
const char * richFontName(int i) { return FAMS[i % NFAMS].name; }

struct Pick { const FontInfo * f; int s; };

static Pick pick(const RStyle & st)
{
  static RStyle lastSt; static Pick last = { nullptr, 1 };
  if (last.f && lastSt.font == st.font && lastSt.size == st.size) return last;
  const RFam & fam = FAMS[st.font < NFAMS ? st.font : 0];
  Pick best = { fam.f[0], 1 };
  int bestScore = 1 << 30;
  for (int s = 1; s <= 4; ++s)
    for (int i = 0; i < 8 && fam.f[i]; ++i) {
      int score = abs(fam.f[i]->height * s - st.size) * 4 + (s - 1) * 3;   // prefer real (unenlarged) fonts
      if (score < bestScore) { bestScore = score; best = { fam.f[i], s }; }
    }
  lastSt = st; last = best;
  return best;
}

static inline int glyphW(const FontInfo * f, uint8_t c)
{
  if (f->flags & FONTINFOFLAGS_VARWIDTH) return f->data[f->chptr[c]];
  return f->width;
}

int rCharW(const RStyle & s, uint8_t c)
{
  Pick p = pick(s);
  return glyphW(p.f, c) * p.s + ((s.flags & RS_BOLD) ? p.s : 0);
}
int rFontH(const RStyle & s)  { Pick p = pick(s); return p.f->height * p.s; }
int rAscent(const RStyle & s) { Pick p = pick(s); return p.f->ascent * p.s; }

// enlarged glyph: each font pixel becomes an s x s square (runs merged)
static int drawGlyphScaled(Canvas * cv, const FontInfo * f, int s, int x, int y, uint8_t c)
{
  int gw;
  const uint8_t * g;
  if (f->flags & FONTINFOFLAGS_VARWIDTH) { g = f->data + f->chptr[c]; gw = *g++; }
  else { gw = f->width; g = f->data + c * f->height * ((gw + 7) / 8); }
  int bpr = (gw + 7) / 8;
  for (int r = 0; r < f->height; ++r) {
    const uint8_t * row = g + r * bpr;
    int run = -1;
    for (int i = 0; i <= gw; ++i) {
      bool on = i < gw && (row[i >> 3] & (0x80 >> (i & 7)));
      if (on && run < 0) run = i;
      if (!on && run >= 0) { cv->fillRectangle(x + run * s, y + r * s, x + i * s - 1, y + (r + 1) * s - 1); run = -1; }
    }
  }
  return gw * s;
}

// ---------------------------------------------------------------- control

RichText::RichText(uiWindow * parent, const Point & pos, const Size & size)
  : uiControl(parent, pos, size, true, 0)
{
  windowStyle().borderSize         = 1;
  windowStyle().focusedBorderSize  = 1;
  windowStyle().borderColor        = T.shadow;
  windowStyle().activeBorderColor  = T.shadow;
  windowStyle().focusedBorderColor = T.accent;
  windowStyle().defaultCursor      = CursorName::CursorTextInput;
  setFocusIndex(1);
  windowProps().focusable = true;
  doc.clear(normal);
  typing = normal;
  doc.width = textW();
  doc.layout();
}

int RichText::textW()
{
  Rect r = clientRect(uiOrigin::Window);
  return imax(40, r.width() - 4 - 4 - 8);   // margins + scroll indicator
}

RStyle RichText::styleHere() const
{
  if (!hasSel()) return typing;
  RPos a, b; selRange(a, b);
  return doc.styleAt(a.p, a.o + (a.o < doc.len(a.p) ? 1 : 0));
}

void RichText::changed(bool layoutAll)
{
  if (layoutAll) doc.dirtyAll();
  doc.width = textW();
  doc.layout();
  ensureVisible();
  repaint();
  if (onModified) onModified();
  if (onSelChange) onSelChange();
}

void RichText::relayout()
{
  doc.width = textW();
  doc.dirtyAll();
  doc.layout();
  ensureVisible();
  repaint();
}

void RichText::ensureVisible()
{
  Rect cr = clientRect(uiOrigin::Window);
  int vh = cr.height() - 4;
  int x, y, h, asc;
  doc.caret(cur, x, y, h, asc);
  if (y < top) top = y;
  else if (y + h > top + vh) top = y + h - vh;
  top = imax(0, imin(top, imax(0, doc.totalHeight - vh)));
}

void RichText::moveTo(RPos p, bool extend)
{
  cur = doc.clamp(p);
  if (!extend) anchor = cur;
  typing = doc.styleAt(cur.p, cur.o);
  ensureVisible();
  repaint();
  if (onSelChange) onSelChange();
}

void RichText::selectRange(RPos a, RPos b)
{
  anchor = doc.clamp(a);
  cur = doc.clamp(b);
  ensureVisible();
  repaint();
  if (onSelChange) onSelChange();
}

void RichText::selectAll() { anchor = RPos(0, 0); cur = doc.endPos(); repaint(); if (onSelChange) onSelChange(); }

void RichText::setPlain(const std::string & s)
{
  doc.setPlain(s, normal);
  cur = anchor = RPos(0, 0);
  typing = normal;
  top = 0;
  hlA = hlB = RPos();
  relayout();
  if (onSelChange) onSelChange();
}

bool RichText::setRtf(const std::string & s)
{
  if (!doc.fromRtf(s)) return false;
  cur = anchor = RPos(0, 0);
  typing = doc.styleAt(0, 0);
  top = 0;
  hlA = hlB = RPos();
  relayout();
  if (onSelChange) onSelChange();
  return true;
}

void RichText::replaceSel(const std::string & s)
{
  RPos a, b; selRange(a, b);
  RStyle st = hasSel() ? doc.styleAt(a.p, a.o + (a.o < doc.len(a.p) ? 1 : 0)) : typing;
  if (hasSel()) a = doc.erase(a, b);
  if (doc.bytes() + s.size() > 60000) return;       // keep documents to a size the ESP32 can hold
  cur = anchor = doc.insert(a, s, st);
  typing = st;
  changed();
}

// ---- formatting
void RichText::setSize(int px)
{
  px = imax(8, imin(48, px));
  if (!hasSel()) { typing.size = px; if (doc.len(cur.p) == 0) { doc.applyStyle(cur, cur, [&](RStyle & s) { s.size = px; }); changed(); } return; }
  RPos a, b; selRange(a, b);
  doc.applyStyle(a, b, [&](RStyle & s) { s.size = px; });
  changed();
}

void RichText::setFont(int f)
{
  if (!hasSel()) { typing.font = f; if (doc.len(cur.p) == 0) { doc.applyStyle(cur, cur, [&](RStyle & s) { s.font = f; }); changed(); } return; }
  RPos a, b; selRange(a, b);
  doc.applyStyle(a, b, [&](RStyle & s) { s.font = f; });
  changed();
}

void RichText::toggleFlag(uint8_t flag)
{
  bool on = !(styleHere().flags & flag);
  if (!hasSel()) { if (on) typing.flags |= flag; else typing.flags &= ~flag; if (onSelChange) onSelChange(); return; }
  RPos a, b; selRange(a, b);
  doc.applyStyle(a, b, [&](RStyle & s) { if (on) s.flags |= flag; else s.flags &= ~flag; });
  changed();
}

void RichText::paraAfter(int delta)
{
  RPos a, b; selRange(a, b);
  doc.forParas(a.p, b.p, [&](RPara & P) { P.after = imax(0, imin(60, P.after + delta)); });
  changed();
}

void RichText::paraSpacing(int percent)
{
  RPos a, b; selRange(a, b);
  doc.forParas(a.p, b.p, [&](RPara & P) { P.spacing = percent; });
  changed();
}

void RichText::paraAlign(int al)
{
  RPos a, b; selRange(a, b);
  doc.forParas(a.p, b.p, [&](RPara & P) { P.align = al; });
  changed();
}

// ---- clipboard (inside TidyOS)
static std::vector<RPara> s_clip;

void RichText::copy(bool cut)
{
  if (!hasSel()) return;
  RPos a, b; selRange(a, b);
  s_clip = doc.copyRange(a, b);
  if (cut) { cur = anchor = doc.erase(a, b); changed(); }
}

void RichText::paste()
{
  if (s_clip.empty()) return;
  RPos a, b; selRange(a, b);
  if (hasSel()) a = doc.erase(a, b);
  size_t add = 0;
  for (auto & P : s_clip) for (auto & r : P.runs) add += r.t.size() + 1;
  if (doc.bytes() + add > 60000) return;
  cur = anchor = doc.pasteParas(a, s_clip);
  typing = doc.styleAt(cur.p, cur.o);
  changed();
}

// ---- keyboard
static bool isWordCh(char c) { return isalnum((unsigned char)c) || c == '\''; }

void RichText::keyDown(uiKeyEventInfo const & k)
{
  if (onKey && onKey(k)) return;
  bool sh = k.SHIFT;
  int x, y, h, asc;
  if (k.CTRL) {
    switch (k.VK) {
      case VK_a: case VK_A: selectAll(); return;
      case VK_c: case VK_C: copy(false); return;
      case VK_x: case VK_X: copy(true); return;
      case VK_v: case VK_V: paste(); return;
      case VK_b: case VK_B: toggleFlag(RS_BOLD); return;
      case VK_u: case VK_U: toggleFlag(RS_UNDER); return;
      case VK_HOME: case VK_KP_HOME: wantX = -1; moveTo(RPos(0, 0), sh); return;
      case VK_END: case VK_KP_END: wantX = -1; moveTo(doc.endPos(), sh); return;
      case VK_LEFT: case VK_KP_LEFT: {     // word left
        RPos p = cur; std::string t = doc.text(p.p);
        if (p.o == 0 && p.p > 0) { p.p--; p.o = doc.len(p.p); }
        else { while (p.o > 0 && !isWordCh(t[p.o - 1])) --p.o; while (p.o > 0 && isWordCh(t[p.o - 1])) --p.o; }
        wantX = -1; moveTo(p, sh); return;
      }
      case VK_RIGHT: case VK_KP_RIGHT: {   // word right
        RPos p = cur; std::string t = doc.text(p.p); int n = t.size();
        if (p.o == n && p.p + 1 < (int)doc.paras.size()) { p.p++; p.o = 0; }
        else { while (p.o < n && isWordCh(t[p.o])) ++p.o; while (p.o < n && !isWordCh(t[p.o])) ++p.o; }
        wantX = -1; moveTo(p, sh); return;
      }
      default: return;
    }
  }
  switch (k.VK) {
    case VK_LEFT: case VK_KP_LEFT:
      wantX = -1;
      if (hasSel() && !sh) { RPos a, b; selRange(a, b); moveTo(a, false); return; }
      if (cur.o > 0) moveTo(RPos(cur.p, cur.o - 1), sh); else if (cur.p > 0) moveTo(RPos(cur.p - 1, doc.len(cur.p - 1)), sh);
      return;
    case VK_RIGHT: case VK_KP_RIGHT:
      wantX = -1;
      if (hasSel() && !sh) { RPos a, b; selRange(a, b); moveTo(b, false); return; }
      if (cur.o < doc.len(cur.p)) moveTo(RPos(cur.p, cur.o + 1), sh); else if (cur.p + 1 < (int)doc.paras.size()) moveTo(RPos(cur.p + 1, 0), sh);
      return;
    case VK_UP: case VK_KP_UP:
    case VK_DOWN: case VK_KP_DOWN: {
      doc.caret(cur, x, y, h, asc);
      if (wantX < 0) wantX = x;
      bool up = k.VK == VK_UP || k.VK == VK_KP_UP;
      int ny = up ? y - 2 : y + h + 1;
      int keep = wantX;
      if (ny < 0) moveTo(RPos(0, 0), sh);
      else if (ny >= doc.totalHeight) moveTo(doc.endPos(), sh);
      else moveTo(doc.hit(wantX, ny), sh);
      wantX = keep;
      return;
    }
    case VK_PAGEUP: case VK_KP_PAGEUP:
    case VK_PAGEDOWN: case VK_KP_PAGEDOWN: {
      Rect cr = clientRect(uiOrigin::Window);
      int page = cr.height() - 24;
      doc.caret(cur, x, y, h, asc);
      bool up = k.VK == VK_PAGEUP || k.VK == VK_KP_PAGEUP;
      top = imax(0, top + (up ? -page : page));
      moveTo(doc.hit(x, imax(0, y + (up ? -page : page))), sh);
      return;
    }
    case VK_HOME: case VK_KP_HOME: {
      const RPara & P = doc.paras[cur.p];
      int li = doc.lineOf(cur);
      wantX = -1; moveTo(RPos(cur.p, P.lines.empty() ? 0 : P.lines[li].a), sh); return;
    }
    case VK_END: case VK_KP_END: {
      const RPara & P = doc.paras[cur.p];
      int li = doc.lineOf(cur);
      int e = P.lines.empty() ? 0 : P.lines[li].b;
      if (li + 1 < (int)P.lines.size() && e > P.lines[li].a && doc.text(cur.p)[e - 1] == ' ') --e;
      wantX = -1; moveTo(RPos(cur.p, e), sh); return;
    }
    case VK_RETURN: case VK_KP_ENTER:
      replaceSel("\n");
      return;
    case VK_BACKSPACE:
      if (hasSel()) { RPos a, b; selRange(a, b); cur = anchor = doc.erase(a, b); changed(); return; }
      if (cur.o > 0) { cur = anchor = doc.erase(RPos(cur.p, cur.o - 1), cur); changed(); }
      else if (cur.p > 0) { RPos a(cur.p - 1, doc.len(cur.p - 1)); cur = anchor = doc.erase(a, cur); changed(); }
      return;
    case VK_DELETE: case VK_KP_DELETE:
      if (hasSel()) { RPos a, b; selRange(a, b); cur = anchor = doc.erase(a, b); changed(); return; }
      if (cur.o < doc.len(cur.p)) { doc.erase(cur, RPos(cur.p, cur.o + 1)); changed(); }
      else if (cur.p + 1 < (int)doc.paras.size()) { doc.erase(cur, RPos(cur.p + 1, 0)); changed(); }
      return;
    case VK_TAB:
      replaceSel("    ");
      return;
    default:
      if (!k.LALT && k.ASCII >= 32 && k.ASCII != 127) { char c[2] = { (char)k.ASCII, 0 }; replaceSel(c); }
      return;
  }
}

// ---------------------------------------------------------------- painting

void RichText::drawLine(Canvas * cv, const RPara & P, const RLine & L, int x0, int yTop, RPos sa, RPos sb, int pIdx)
{
  int x = x0 + doc.lineX(P, L);
  int base = yTop + L.asc;
  int o = 0;
  for (auto & r : P.runs) {
    int n = r.t.size();
    if (o + n <= L.a || o >= L.b) { o += n; continue; }
    Pick pk = pick(r.s);
    int gy = base - pk.f->ascent * pk.s;
    int i0 = imax(L.a, o) - o, i1 = imin(L.b, o + n) - o;
    // pieces with the same selection / highlight state
    int i = i0;
    while (i < i1) {
      auto selAt = [&](int k) { RPos q(pIdx, o + k); return (!(q < sa) && q < sb); };
      auto hlAt  = [&](int k) { RPos q(pIdx, o + k); return hlA != hlB && !(q < hlA) && q < hlB; };
      bool sel = selAt(i) || hlAt(i);
      int j = i + 1;
      while (j < i1 && (selAt(j) || hlAt(j)) == sel) ++j;
      int w = 0;
      for (int k = i; k < j; ++k) w += rCharW(r.s, (uint8_t)r.t[k]);
      if (sel) { cv->setBrushColor(T.accent); cv->fillRectangle(x, yTop, x + w - 1, yTop + L.h - 1); }
      RGB888 ink = sel ? T.accentText : T.text;
      cv->setPenColor(ink);
      cv->setBrushColor(ink);
      int bold = (r.s.flags & RS_BOLD) ? pk.s : 0;
      for (int pass = 0; pass <= (bold ? 1 : 0); ++pass) {
        int xx = x + pass * bold;
        if (pk.s == 1) {
          char buf[96];
          int k = i;
          while (k < j) {
            int m = 0;
            int bx = xx;
            while (k < j && m < (int)sizeof(buf) - 1) { buf[m++] = r.t[k]; xx += glyphW(pk.f, (uint8_t)r.t[k]) + (bold ? 1 : 0); ++k; }
            buf[m] = 0;
            if (!bold) { cv->setBrushColor(sel ? T.accent : T.win); cv->setPenColor(ink); cv->drawText(pk.f, bx, gy, buf); }
            else { // keep the bold spacing: draw glyph by glyph
              int gx = bx;
              for (int q = 0; q < m; ++q) { char one[2] = { buf[q], 0 }; cv->setBrushColor(sel ? T.accent : T.win); cv->setPenColor(ink); cv->drawText(pk.f, gx, gy, one); gx += glyphW(pk.f, (uint8_t)buf[q]) + 1; }
            }
          }
        } else {
          for (int k = i; k < j; ++k) xx += drawGlyphScaled(cv, pk.f, pk.s, xx, gy, (uint8_t)r.t[k]) + bold;
        }
        cv->setBrushColor(ink);
      }
      if (r.s.flags & RS_UNDER) { cv->setBrushColor(ink); cv->fillRectangle(x, base + pk.s, x + w - 1, base + pk.s + pk.s - 1); }
      x += w;
      i = j;
    }
    o += n;
  }
  // spelling: dotted line under words the dictionary doesn't know
  if (isMisspelled) {
    std::string t = doc.text(pIdx);
    int a = L.a;
    while (a < L.b) {
      while (a < L.b && !isalpha((unsigned char)t[a])) ++a;
      int st = a;
      while (a < L.b && (isalpha((unsigned char)t[a]) || (t[a] == '\'' && a + 1 < L.b && isalpha((unsigned char)t[a + 1])))) ++a;
      if (a > st && isMisspelled(t.substr(st, a - st))) {
        int cx1, cx2, yy, hh, as2;
        doc.caret(RPos(pIdx, st), cx1, yy, hh, as2);
        doc.caret(RPos(pIdx, a), cx2, yy, hh, as2);
        if (cx2 <= cx1) cx2 = cx1 + 8;
        cv->setPenColor(T.warn);
        for (int xx = cx1; xx < cx2; xx += 2) { cv->setPixel(x0 + xx, base + 2); cv->setPixel(x0 + xx + 1, base + 3); }
      }
    }
  }
}

void RichText::paint(Rect const & clip)
{
  Rect cr = clientRect(uiOrigin::Window);
  auto cv = canvas();
  cv->setBrushColor(T.win);
  cv->fillRectangle(cr.X1, cr.Y1, cr.X2, cr.Y2);
  int x0 = cr.X1 + 4, y0 = cr.Y1 + 2 - top;
  int vh = cr.height() - 4;
  RPos sa, sb; selRange(sa, sb);
  for (size_t p = 0; p < doc.paras.size(); ++p) {
    const RPara & P = doc.paras[p];
    if (P.y + P.height < top) continue;
    if (P.y > top + vh) break;
    for (auto & L : P.lines) {
      int yTop = y0 + P.y + L.y;
      if (yTop + L.h < cr.Y1 || yTop > cr.Y2) continue;
      if (yTop + L.h < clip.Y1 || yTop > clip.Y2) continue;
      drawLine(cv, P, L, x0, yTop, sa, sb, p);
    }
  }
  // caret
  if (hasFocus()) {
    int x, y, h, asc;
    doc.caret(cur, x, y, h, asc);
    int cy = y0 + y;
    if (cy + h >= cr.Y1 && cy <= cr.Y2) {
      cv->setBrushColor(T.text);
      cv->fillRectangle(x0 + x - 1, imax(cr.Y1, cy), x0 + x, imin(cr.Y2, cy + h - 1));
    }
  }
  // scroll indicator
  if (doc.totalHeight > vh) {
    int bh = cr.height() - 4;
    int th = imax(10, bh * vh / doc.totalHeight);
    int ty = cr.Y1 + 2 + (bh - th) * top / imax(1, doc.totalHeight - vh);
    cv->setBrushColor(T.face);
    cv->fillRectangle(cr.X2 - 6, cr.Y1 + 2, cr.X2 - 2, cr.Y2 - 2);
    cv->setBrushColor(T.shadow);
    cv->fillRectangle(cr.X2 - 6, ty, cr.X2 - 2, ty + th);
  }
}

void RichText::processEvent(uiEvent * event)
{
  uiControl::processEvent(event);
  Rect cr = clientRect(uiOrigin::Window);
  switch (event->id) {
    case UIEVT_PAINT:
      beginPaint(event, cr);
      paint(Rect(event->params.rect.X1, event->params.rect.Y1, event->params.rect.X2, event->params.rect.Y2));
      break;
    case UIEVT_KEYDOWN:
      keyDown(event->params.key);
      break;
    case UIEVT_MOUSEBUTTONDOWN:
      if (event->params.mouse.changedButton == 1) {
        app()->setFocusedWindow(this);
        int mx = event->params.mouse.status.X - cr.X1 - 4, my = event->params.mouse.status.Y - cr.Y1 - 2 + top;
        wantX = -1;
        bool sh = app()->keyboard() && (app()->keyboard()->isVKDown(VK_LSHIFT) || app()->keyboard()->isVKDown(VK_RSHIFT));
        moveTo(doc.hit(mx, my), sh);
        dragging = true;
        app()->captureMouse(this);
      }
      break;
    case UIEVT_MOUSEMOVE:
      if (dragging && event->params.mouse.status.buttons.left) {
        int mx = event->params.mouse.status.X - cr.X1 - 4, my = event->params.mouse.status.Y - cr.Y1 - 2 + top;
        RPos p = doc.hit(mx, my);
        if (p != cur) { cur = p; ensureVisible(); repaint(); if (onSelChange) onSelChange(); }
      }
      break;
    case UIEVT_MOUSEBUTTONUP:
      if (dragging) { dragging = false; app()->captureMouse(nullptr); }
      break;
    case UIEVT_DBLCLICK: {      // select the word
      std::string t = doc.text(cur.p);
      int a = cur.o, b = cur.o;
      while (a > 0 && isWordCh(t[a - 1])) --a;
      while (b < (int)t.size() && isWordCh(t[b])) ++b;
      selectRange(RPos(cur.p, a), RPos(cur.p, b));
      break;
    }
    case UIEVT_MOUSEWHEEL: {
      int vh = cr.height() - 4;
      top = imax(0, imin(imax(0, doc.totalHeight - vh), top + event->params.mouse.status.wheelDelta * 40));
      repaint();
      break;
    }
    case UIEVT_SETFOCUS:
    case UIEVT_KILLFOCUS:
      repaint();
      break;
    default:
      break;
  }
}
