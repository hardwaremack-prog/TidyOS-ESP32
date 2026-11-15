// TidyOS custom widgets
#include <math.h>
#include "widgets.h"

using namespace fabgl;

// ====================================================================== TextArea

TextArea::TextArea(uiWindow * parent, const Point & pos, const Size & size, bool editable_)
  : uiControl(parent, pos, size, true, 0), editable(editable_), font(&FONT_8x14)
{
  windowStyle().borderSize         = 1;
  windowStyle().focusedBorderSize  = 1;
  windowStyle().borderColor        = T.shadow;
  windowStyle().activeBorderColor  = T.shadow;
  windowStyle().focusedBorderColor = T.accent;
  windowStyle().defaultCursor      = editable ? CursorName::CursorTextInput : CursorName::CursorPointerSimpleReduced;
  setFocusIndex(1);
  windowProps().focusable = true;     // FabGL only gives keyboard focus to controls marked focusable
  lines.push_back("");
}

int TextArea::visRows()
{
  Rect r = clientRect(uiOrigin::Window);
  return imax(1, (r.height() - textY0() * 2) / font->height);
}

int TextArea::visCols()
{
  Rect r = clientRect(uiOrigin::Window);
  return imax(1, (r.width() - textX0() - 8) / font->width);   // 8 px for the scroll indicator
}

void TextArea::setText(const std::string & s)
{
  lines.clear();
  size_t st = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '\n') {
      std::string l = s.substr(st, i - st);
      if (!l.empty() && l.back() == '\r') l.pop_back();
      for (auto & ch : l) if (ch == '\t') ch = ' ';
      lines.push_back(l);
      st = i + 1;
    }
  }
  if (lines.empty()) lines.push_back("");
  crow = ccol = top = left = 0;
  modified = false;
  repaint();
}

void TextArea::setLines(std::vector<std::string> && l)
{
  lines = std::move(l);
  if (lines.empty()) lines.push_back("");
  crow = ccol = top = left = 0;
  repaint();
}

std::string TextArea::getText()
{
  std::string s;
  size_t n = 0;
  for (auto & l : lines) n += l.size() + 1;
  s.reserve(n);
  for (size_t i = 0; i < lines.size(); ++i) {
    s += lines[i];
    if (i + 1 < lines.size()) s += '\n';
  }
  return s;
}

void TextArea::scrollTo(int line)
{
  top = imax(0, imin(line, (int)lines.size() - 1));
  repaint();
}

void TextArea::changed()
{
  hlRow = -1;
  if (!modified) {
    modified = true;
  }
  if (onModified) onModified();
}

void TextArea::ensureVisible()
{
  int vr = visRows(), vc = visCols();
  if (crow < top) top = crow;
  if (crow >= top + vr) top = crow - vr + 1;
  if (ccol < left) left = imax(0, ccol - 8);
  if (ccol >= left + vc) left = ccol - vc + 8;
}

void TextArea::paint(Rect const & clip)
{
  Rect cr = clientRect(uiOrigin::Window);
  auto cv = canvas();
  cv->setBrushColor(T.win);
  cv->fillRectangle(cr.X1, cr.Y1, cr.X2, cr.Y2);
  int vr = visRows(), vc = visCols();
  int fw = font->width, fh = font->height;
  char buf[160];
  bool showCursor = editable && hasFocus();
  for (int i = 0; i < vr; ++i) {
    int row = top + i;
    if (row >= (int)lines.size()) break;
    int y = cr.Y1 + textY0() + i * fh;
    if (y + fh < clip.Y1 || y > clip.Y2) continue;
    const std::string & l = lines[row];
    int n = 0;
    for (int c = left; c < (int)l.size() && n < vc && n < (int)sizeof(buf) - 1; ++c)
      buf[n++] = (uint8_t)l[c] < 32 ? ' ' : l[c];
    buf[n] = 0;
    cv->setPenColor(T.text);
    cv->setBrushColor(T.win);
    if (n) cv->drawText(font, cr.X1 + textX0(), y, buf);
    // highlighted word (spell check)
    if (row == hlRow && hlLen > 0) {
      int a = imax(hlCol, left), b = imin(hlCol + hlLen, left + n);
      if (b > a) {
        int hx = cr.X1 + textX0() + (a - left) * fw;
        cv->setBrushColor(T.accent);
        cv->fillRectangle(hx, y, hx + (b - a) * fw - 1, y + fh - 1);
        std::string w = l.substr(a, b - a);
        cv->setPenColor(T.accentText);
        cv->drawText(font, hx, y, w.c_str());
      }
    }
    // red underline under misspelled words
    if (isMisspelled && n) {
      int c = 0, len = l.size();
      while (c < len) {
        while (c < len && !(isalpha((unsigned char)l[c]))) ++c;
        int st = c;
        while (c < len && (isalpha((unsigned char)l[c]) || (l[c] == '\'' && c + 1 < len && isalpha((unsigned char)l[c + 1])))) ++c;
        if (c > st && c > left && st < left + n && isMisspelled(l.substr(st, c - st))) {
          int a = imax(st, left), b = imin(c, left + n);
          int x1 = cr.X1 + textX0() + (a - left) * fw, x2 = cr.X1 + textX0() + (b - left) * fw - 1;
          cv->setPenColor(T.warn);
          for (int x = x1; x <= x2; x += 2) { cv->setPixel(x, y + fh - 1); cv->setPixel(x + 1, y + fh - 2); }
        }
      }
    }
    if (showCursor && row == crow && ccol >= left && ccol < left + vc) {
      int x = cr.X1 + textX0() + (ccol - left) * fw;
      cv->setBrushColor(T.text);
      cv->fillRectangle(x, y, x + fw - 1, y + fh - 1);
      char ch[2] = { ccol < (int)l.size() ? l[ccol] : ' ', 0 };
      cv->setPenColor(T.win);
      cv->drawText(font, x, y, ch);
    }
  }
  // scroll indicator
  int total = lines.size();
  if (total > vr) {
    int x1 = cr.X2 - 5, x2 = cr.X2 - 1;
    int h  = cr.height() - 4;
    cv->setBrushColor(T.face);
    cv->fillRectangle(x1, cr.Y1 + 2, x2, cr.Y2 - 2);
    int th = imax(8, h * vr / total);
    int ty = cr.Y1 + 2 + (h - th) * top / imax(1, total - vr);
    cv->setBrushColor(T.accent);
    cv->fillRectangle(x1, ty, x2, imin(cr.Y2 - 2, ty + th));
  }
}

void TextArea::insertChar(char c)
{
  std::string & l = lines[crow];
  if (ccol > (int)l.size()) ccol = l.size();
  if (l.size() >= 1000) return;
  l.insert(l.begin() + ccol, c);
  ++ccol;
}

void TextArea::keyDown(uiKeyEventInfo const & k)
{
  if (onKey && onKey(k)) return;
  int vr = visRows();
  int oldTop = top, oldLeft = left, oldRow = crow;
  bool full = false;

  if (!editable) {
    switch (k.VK) {
      case VK_UP: case VK_KP_UP:         top = imax(0, top - 1); break;
      case VK_DOWN: case VK_KP_DOWN:     top = imin(imax(0, (int)lines.size() - vr), top + 1); break;
      case VK_PAGEUP: case VK_KP_PAGEUP: top = imax(0, top - vr + 1); break;
      case VK_PAGEDOWN: case VK_KP_PAGEDOWN: case VK_SPACE: top = imin(imax(0, (int)lines.size() - vr), top + vr - 1); break;
      case VK_HOME: case VK_KP_HOME:     top = 0; break;
      case VK_END: case VK_KP_END:       top = imax(0, (int)lines.size() - vr); break;
      default: return;
    }
    if (top != oldTop) repaint();
    return;
  }

  std::string & l = lines[crow];
  if (ccol > (int)l.size()) ccol = l.size();
  switch (k.VK) {
    case VK_LEFT: case VK_KP_LEFT:
      if (ccol > 0) --ccol; else if (crow > 0) { --crow; ccol = lines[crow].size(); }
      break;
    case VK_RIGHT: case VK_KP_RIGHT:
      if (ccol < (int)l.size()) ++ccol; else if (crow + 1 < (int)lines.size()) { ++crow; ccol = 0; }
      break;
    case VK_UP: case VK_KP_UP:
      if (crow > 0) { --crow; ccol = imin(ccol, (int)lines[crow].size()); }
      break;
    case VK_DOWN: case VK_KP_DOWN:
      if (crow + 1 < (int)lines.size()) { ++crow; ccol = imin(ccol, (int)lines[crow].size()); }
      break;
    case VK_HOME: case VK_KP_HOME:
      if (k.CTRL) crow = 0;
      ccol = 0;
      break;
    case VK_END: case VK_KP_END:
      if (k.CTRL) crow = lines.size() - 1;
      ccol = lines[crow].size();
      break;
    case VK_PAGEUP: case VK_KP_PAGEUP:
      crow = imax(0, crow - vr + 1); top = imax(0, top - vr + 1); ccol = imin(ccol, (int)lines[crow].size());
      break;
    case VK_PAGEDOWN: case VK_KP_PAGEDOWN:
      crow = imin((int)lines.size() - 1, crow + vr - 1); top = imin(imax(0, (int)lines.size() - vr), top + vr - 1);
      ccol = imin(ccol, (int)lines[crow].size());
      break;
    case VK_RETURN: case VK_KP_ENTER:
      if (lines.size() < 3000) {
        std::string rest = l.substr(ccol);
        l.erase(ccol);
        // keep indentation
        int ind = 0;
        while (ind < (int)l.size() && l[ind] == ' ') ++ind;
        lines.insert(lines.begin() + crow + 1, std::string(ind, ' ') + rest);
        ++crow; ccol = ind;
        full = true;
        changed();
      }
      break;
    case VK_BACKSPACE:
      if (ccol > 0) { l.erase(ccol - 1, 1); --ccol; changed(); }
      else if (crow > 0) {
        ccol = lines[crow - 1].size();
        lines[crow - 1] += l;
        lines.erase(lines.begin() + crow);
        --crow; full = true; changed();
      }
      break;
    case VK_DELETE: case VK_KP_DELETE:
      if (ccol < (int)l.size()) { l.erase(ccol, 1); changed(); }
      else if (crow + 1 < (int)lines.size()) {
        l += lines[crow + 1];
        lines.erase(lines.begin() + crow + 1);
        full = true; changed();
      }
      break;
    case VK_TAB:
      insertChar(' '); insertChar(' ');
      changed();
      break;
    default:
      if (!k.CTRL && !k.LALT && k.ASCII >= 32 && k.ASCII < 127) {
        insertChar(k.ASCII);
        changed();
      } else {
        return;
      }
  }
  ensureVisible();
  if (full || top != oldTop || left != oldLeft) {
    repaint();
  } else {
    // repaint only the touched lines
    Rect cr = clientRect(uiOrigin::Window);
    int fh = font->height;
    int a = imin(oldRow, crow) - top, b = imax(oldRow, crow) - top;
    repaint(Rect(cr.X1, cr.Y1 + textY0() + a * fh, cr.X2, cr.Y1 + textY0() + (b + 1) * fh));
  }
}

void TextArea::processEvent(uiEvent * event)
{
  uiControl::processEvent(event);
  switch (event->id) {
    case UIEVT_PAINT:
      beginPaint(event, clientRect(uiOrigin::Window));
      paint(Rect(event->params.rect.X1, event->params.rect.Y1, event->params.rect.X2, event->params.rect.Y2));
      break;
    case UIEVT_KEYDOWN:
      keyDown(event->params.key);
      break;
    case UIEVT_MOUSEBUTTONDOWN:
      if (event->params.mouse.changedButton == 1) {
        app()->setFocusedWindow(this);
        Rect cr = clientRect(uiOrigin::Window);
        int mx = event->params.mouse.status.X, my = event->params.mouse.status.Y;
        if (mx >= cr.X2 - 7) {                 // click on scroll indicator: jump
          int vr = visRows();
          top = imax(0, imin((int)lines.size() - vr, (int)((my - cr.Y1) * (int)lines.size() / imax(1, cr.height())) - vr / 2));
          repaint();
          break;
        }
        int row = top + (my - cr.Y1 - textY0()) / font->height;
        int col = left + (mx - cr.X1 - textX0()) / font->width;
        row = imax(0, imin(row, (int)lines.size() - 1));
        col = imax(0, col);
        if (editable) {
          crow = row;
          ccol = imin(col, (int)lines[row].size());
          repaint();
        } else if (onClickChar) {
          onClickChar(row, col);
        }
      }
      break;
    case UIEVT_MOUSEWHEEL:
      {
        int vr = visRows();
        top = imax(0, imin(imax(0, (int)lines.size() - vr), top + event->params.mouse.status.wheelDelta * 3));
        repaint();
      }
      break;
    case UIEVT_SETFOCUS:
    case UIEVT_KILLFOCUS:
      repaint();
      break;
    default:
      break;
  }
}

void wrapText(const std::string & text, int width, std::vector<std::string> & out)
{
  size_t st = 0;
  for (size_t i = 0; i <= text.size(); ++i) {
    if (i < text.size() && text[i] != '\n') continue;
    std::string para = text.substr(st, i - st);
    st = i + 1;
    if (para.empty()) { out.push_back(""); continue; }
    while ((int)para.size() > width) {
      int cut = para.rfind(' ', width);
      if (cut <= 0) cut = width;
      out.push_back(para.substr(0, cut));
      para.erase(0, cut);
      while (!para.empty() && para[0] == ' ') para.erase(0, 1);
    }
    out.push_back(para);
    if (out.size() > 4000) return;
  }
}

// ====================================================================== Sheet model

static const std::string s_empty;

const std::string & Sheet::raw(int r, int c)
{
  auto it = cells.find(key(r, c));
  return it == cells.end() ? s_empty : it->second;
}

void Sheet::set(int r, int c, const std::string & v)
{
  if (v.empty()) cells.erase(key(r, c));
  else cells[key(r, c)] = v;
}

// formulas: move cell references after an insert / delete. A reference inside deleted rows points at the first row after them.
static std::string shiftRefs(const std::string & f, bool rows, int at, int d)
{
  std::string o;
  size_t i = 0, n = f.size();
  while (i < n) {
    char up = toupper((unsigned char)f[i]);
    bool prevAlpha = i > 0 && isalpha((unsigned char)f[i - 1]);
    if (!prevAlpha && up >= 'A' && up <= 'Z' && i + 1 < n && isdigit((unsigned char)f[i + 1])) {
      size_t j = i + 1; int r = 0;
      while (j < n && isdigit((unsigned char)f[j])) r = r * 10 + (f[j++] - '0');
      if (j < n && isalpha((unsigned char)f[j])) { o.append(f, i, j - i); i = j; continue; }
      int c = up - 'A'; r -= 1;
      int & v = rows ? r : c;
      int lim = rows ? Sheet::ROWS : Sheet::COLS;
      if (v >= at) { if (d < 0 && v < at - d) v = at; else v += d; }
      if (v < 0) v = 0;
      if (v >= lim) v = lim - 1;
      o += Sheet::cellName(r, c);
      i = j;
      continue;
    }
    o += f[i++];
  }
  return o;
}

void Sheet::shift(bool rows, int at, int d)
{
  std::map<int, std::string> out;
  int lim = rows ? ROWS : COLS;
  for (auto & kv : cells) {
    int r = kv.first / COLS, c = kv.first % COLS;
    int & v = rows ? r : c;
    if (v >= at) {
      if (d < 0 && v < at - d) continue;     // deleted
      v += d;
      if (v >= lim) continue;                // pushed off the end
    }
    std::string s = kv.second;
    if (!s.empty() && s[0] == '=') s = shiftRefs(s, rows, at, d);
    out[key(r, c)] = s;
  }
  cells.swap(out);
  std::map<int, uint8_t> al;                 // alignment moves with its cell
  for (auto & kv : align) {
    int r = kv.first / COLS, c = kv.first % COLS;
    int & v = rows ? r : c;
    if (v >= at) { if (d < 0 && v < at - d) continue; v += d; if (v >= lim) continue; }
    al[key(r, c)] = kv.second;
  }
  align.swap(al);
  if (!rows) {                               // whole-column alignment moves with its column
    if (d > 0) { for (int i = COLS - 1; i >= at + d; --i) colAlign[i] = colAlign[i - d]; for (int i = at; i < at + d && i < COLS; ++i) colAlign[i] = 0; }
    else { int k = -d; for (int i = at; i + k < COLS; ++i) colAlign[i] = colAlign[i + k]; for (int i = COLS - k; i < COLS; ++i) if (i >= 0) colAlign[i] = 0; }
  }
  uint8_t * a = rows ? rowH : colW;
  uint8_t def = rows ? 16 : 74;
  if (d > 0) {
    for (int i = lim - 1; i >= at + d; --i) a[i] = a[i - d];
    for (int i = at; i < at + d && i < lim; ++i) a[i] = def;
  } else {
    int k = -d;
    for (int i = at; i + k < lim; ++i) a[i] = a[i + k];
    for (int i = lim - k; i < lim; ++i) if (i >= 0) a[i] = def;
  }
}

bool Sheet::wouldDrop(bool rows, int d)
{
  int lim = rows ? ROWS : COLS;
  for (auto & kv : cells) { int v = rows ? kv.first / COLS : kv.first % COLS; if (v >= lim - d) return true; }
  return false;
}

std::string Sheet::cellName(int r, int c)
{
  char b[16];
  snprintf(b, sizeof(b), "%c%d", 'A' + c, r + 1);
  return b;
}

static bool parseNumber(const std::string & s, double & v)
{
  if (s.empty()) return false;
  const char * p = s.c_str();
  while (*p == ' ' || *p == '$') ++p;
  char * end;
  std::string clean;
  for (const char * q = p; *q; ++q) if (*q != ',') clean += *q;   // allow 1,234.50
  v = strtod(clean.c_str(), &end);
  if (end == clean.c_str()) return false;
  while (*end == ' ') ++end;
  if (*end == '%') { v /= 100; ++end; }
  return *end == 0;
}

double Sheet::value(int r, int c, bool & err, int depth)
{
  const std::string & s = raw(r, c);
  if (s.empty()) return 0;
  if (s[0] == '=') {
    if (depth > 24) { err = true; return 0; }
    P p = { s.c_str() + 1, depth + 1, false };
    double v = expr(p);
    ws(p);
    if (p.err || *p.s) { err = true; return 0; }
    return v;
  }
  double v;
  if (parseNumber(s, v)) return v;
  if (checkState(s) == 1) return 1;   // ticked box counts as 1, so =SUM() counts ticks
  return 0;   // text counts as 0
}

bool Sheet::cellRef(P & p, int & r, int & c)
{
  const char * s = p.s;
  char ch = toupper(*s);
  if (ch < 'A' || ch > 'Z' || !isdigit((unsigned char)s[1])) return false;
  c = ch - 'A';
  ++s;
  int n = 0;
  while (isdigit((unsigned char)*s)) n = n * 10 + (*s++ - '0');
  if (n < 1 || n > ROWS) return false;
  r = n - 1;
  p.s = s;
  return true;
}

double Sheet::primary(P & p)
{
  ws(p);
  if (*p.s == '(') {
    ++p.s;
    double v = expr(p);
    ws(p);
    if (*p.s == ')') ++p.s; else p.err = true;
    return v;
  }
  if (isdigit((unsigned char)*p.s) || *p.s == '.') {
    char * e;
    double v = strtod(p.s, &e);
    p.s = e;
    if (*p.s == '%') { v /= 100; ++p.s; }
    return v;
  }
  if (isalpha((unsigned char)*p.s)) {
    // function?
    const char * s = p.s;
    std::string name;
    while (isalpha((unsigned char)*s)) name += toupper(*s++);
    if (*s == '(' && name.size() > 1) {
      p.s = s + 1;
      double sum = 0, mn = INFINITY, mx = -INFINITY;
      int count = 0;
      std::vector<double> args;
      for (;;) {
        ws(p);
        if (*p.s == ')') { ++p.s; break; }
        int r1, c1, r2, c2;
        const char * save = p.s;
        if (cellRef(p, r1, c1) && *p.s == ':') {
          ++p.s;
          if (!cellRef(p, r2, c2)) { p.err = true; return 0; }
          for (int r = imin(r1, r2); r <= imax(r1, r2); ++r)
            for (int c = imin(c1, c2); c <= imax(c1, c2); ++c) {
              const std::string & cs = raw(r, c);
              double dv;
              bool isNum = !cs.empty() && (cs[0] == '=' || parseNumber(cs, dv));
              if (!isNum) continue;
              bool e = false;
              double v = value(r, c, e, p.depth);
              if (e) { p.err = true; return 0; }
              sum += v; mn = fmin(mn, v); mx = fmax(mx, v); ++count;
            }
        } else {
          p.s = save;
          double v = expr(p);
          args.push_back(v);
          sum += v; mn = fmin(mn, v); mx = fmax(mx, v); ++count;
        }
        ws(p);
        if (*p.s == ',' || *p.s == ';') { ++p.s; continue; }
        if (*p.s == ')') { ++p.s; break; }
        p.err = true; return 0;
      }
      if (name == "SUM") return sum;
      if (name == "AVG" || name == "AVERAGE") return count ? sum / count : 0;
      if (name == "MIN") return count ? mn : 0;
      if (name == "MAX") return count ? mx : 0;
      if (name == "COUNT") return count;
      if (name == "ABS" && args.size() == 1) return fabs(args[0]);
      if (name == "SQRT" && args.size() == 1) return sqrt(args[0]);
      if (name == "INT" && args.size() == 1) return floor(args[0]);
      if (name == "ROUND" && args.size() >= 1) {
        double f = pow(10, args.size() > 1 ? args[1] : 0);
        return round(args[0] * f) / f;
      }
      if (name == "IF" && args.size() == 3) return args[0] != 0 ? args[1] : args[2];
      p.err = true;
      return 0;
    }
    int r, c;
    if (cellRef(p, r, c)) {
      bool e = false;
      double v = value(r, c, e, p.depth);
      if (e) p.err = true;
      return v;
    }
  }
  p.err = true;
  return 0;
}

double Sheet::unary(P & p)
{
  ws(p);
  if (*p.s == '-') { ++p.s; return -unary(p); }
  if (*p.s == '+') { ++p.s; return unary(p); }
  return primary(p);
}

double Sheet::power(P & p)
{
  double v = unary(p);
  ws(p);
  if (*p.s == '^') { ++p.s; v = pow(v, unary(p)); }
  return v;
}

double Sheet::term(P & p)
{
  double v = power(p);
  for (;;) {
    ws(p);
    if (*p.s == '*') { ++p.s; v *= power(p); }
    else if (*p.s == '/') { ++p.s; double d = power(p); if (d == 0) { p.err = true; return 0; } v /= d; }
    else return v;
  }
}

double Sheet::expr(P & p)
{
  double v = term(p);
  for (;;) {
    ws(p);
    if (*p.s == '+') { ++p.s; v += term(p); }
    else if (*p.s == '-') { ++p.s; v -= term(p); }
    else if (*p.s == '>' ) { ++p.s; bool eq = *p.s == '='; if (eq) ++p.s; double w = term(p); v = eq ? v >= w : v > w; }
    else if (*p.s == '<' ) { ++p.s; bool eq = *p.s == '='; if (eq) ++p.s; double w = term(p); v = eq ? v <= w : v < w; }
    else return v;
  }
}

std::string fmtNum(double v)
{
  char b[32];
  if (fabs(v) < 1e15 && fabs(v - round(v)) < 1e-9) snprintf(b, sizeof(b), "%.0f", v);
  else snprintf(b, sizeof(b), "%.2f", v);
  return b;
}

std::string Sheet::display(int r, int c, bool & isNum)
{
  const std::string & s = raw(r, c);
  isNum = false;
  if (s.empty()) return s;
  if (s[0] == '=') {
    bool err = false;
    double v = value(r, c, err);
    if (err) return "#ERR";
    isNum = true;
    return fmtNum(v);
  }
  double v;
  if (parseNumber(s, v)) { isNum = true; return s; }
  return s;
}

std::string Sheet::toCSV()
{
  std::string out;
  StrSink o(out);
  writeCSV(o);
  return out;
}

void Sheet::writeCSV(RSink & o)
{
  int maxR = -1, maxC = -1;
  for (auto & kv : cells) { maxR = imax(maxR, kv.first / COLS); maxC = imax(maxC, kv.first % COLS); }
  for (int r = 0; r <= maxR; ++r) {
    for (int c = 0; c <= maxC; ++c) {
      const std::string & s = raw(r, c);
      if (s.find_first_of(",\"\n") != std::string::npos) {
        o.put('"');
        for (char ch : s) { if (ch == '"') o.put('"'); o.put(ch); }
        o.put('"');
      } else o.put(s);
      if (c < maxC) o.put(',');
    }
    o.put('\n');
  }
}

void Sheet::fromCSV(const std::string & s)
{
  StrSource in(s);
  readCSV(in);
}

void Sheet::readCSV(RSource & in)
{
  cells.clear();
  int r = 0, c = 0;
  std::string cur;
  bool q = false;
  for (bool end = false; !end; ) {
    int ch = in.peek();
    if (ch < 0) { ch = '\n'; end = true; } else in.skip();
    if (q) {
      if (ch == '"') { if (in.peek() == '"') { cur += '"'; in.skip(); } else q = false; }
      else cur += (char)ch;
      continue;
    }
    if (ch == '"') { q = true; continue; }
    if (ch == ',' || ch == ';' || ch == '\t' || ch == '\n') {
      if (r < ROWS && c < COLS && !cur.empty()) set(r, c, cur);
      cur.clear();
      if (ch == '\n') { ++r; c = 0; } else ++c;
      continue;
    }
    if (ch != '\r') cur += (char)ch;
  }
}

// ====================================================================== SheetGrid


SheetGrid::SheetGrid(uiWindow * parent, const Point & pos, const Size & size, Sheet * s)
  : uiControl(parent, pos, size, true, 0), sheet(s)
{
  windowStyle().borderSize         = 1;
  windowStyle().focusedBorderSize  = 1;
  windowStyle().borderColor        = T.shadow;
  windowStyle().activeBorderColor  = T.shadow;
  windowStyle().focusedBorderColor = T.accent;
  setFocusIndex(1);
  windowProps().focusable = true;     // FabGL only gives keyboard focus to controls marked focusable
}

// how many whole rows / columns fit from the current scroll position
int SheetGrid::visRows()
{
  int h = clientRect(uiOrigin::Window).height() - HDRH, n = 0;
  for (int i = 0; ; ++i) {
    int r = rowAt(i);
    if (r >= Sheet::ROWS) break;
    h -= sheet->rowH[r];
    if (h < 0) break;
    ++n;
  }
  return imax(1, n);
}

int SheetGrid::visCols()
{
  int w = clientRect(uiOrigin::Window).width() - HDRW, n = 0;
  for (int c = leftC; c < Sheet::COLS; ++c) { w -= sheet->colW[c]; if (w < 0) break; ++n; }
  return imax(1, n);
}

void SheetGrid::ensureVisible()
{
  if (freezeTop) {
    if (selR > 0 && selR < topR + 1) topR = selR - 1;
  } else if (selR < topR) topR = selR;
  while (topR < Sheet::ROWS - 1) {                 // scroll down until the row is wholly on screen
    int vr = visRows();
    bool shown = (freezeTop && selR == 0);
    for (int i = 0; i < vr && !shown; ++i) if (rowAt(i) == selR) shown = true;
    if (shown) break;
    ++topR;
  }
  if (selC < leftC) leftC = selC;
  while (leftC < selC && selC >= leftC + visCols()) ++leftC;
  topR = imax(0, topR);
}

// what's under a point: 0 nothing, 1 cell, 2 column letter, 3 row number, 4 column edge, 5 row edge, 6 corner
int SheetGrid::hitTest(int px, int py, int & r, int & c)
{
  Rect cr = clientRect(uiOrigin::Window);
  int mx = px - cr.X1, my = py - cr.Y1;
  r = c = -1;
  if (mx < 0 || my < 0) return 0;
  // column
  int x = HDRW;
  if (mx >= HDRW) {
    for (int cc = leftC; cc < Sheet::COLS; ++cc) {
      int w = sheet->colW[cc];
      if (mx < x + w) { c = cc; if (my < HDRH && mx >= x + w - 4) return 4; break; }
      x += w;
      if (x > cr.width()) break;
    }
    if (c < 0) c = imin(Sheet::COLS - 1, leftC + visCols());
    if (my < HDRH) return 2;
  }
  // row
  int y = HDRH;
  if (my >= HDRH) {
    for (int i = 0; ; ++i) {
      int rr = rowAt(i);
      if (rr >= Sheet::ROWS) { r = Sheet::ROWS - 1; break; }
      int h = sheet->rowH[rr];
      if (my < y + h) { r = rr; if (mx < HDRW && my >= y + h - 3) return 5; break; }
      y += h;
      if (y > cr.height()) { r = rr; break; }
    }
    if (mx < HDRW) return 3;
    return 1;
  }
  return 6;
}

// screen x of a column's left edge (or -1 when it's scrolled off), y of a row's top
int SheetGrid::colX(int c)
{
  if (c < leftC) return -1;
  int x = clientRect(uiOrigin::Window).X1 + HDRW;
  for (int cc = leftC; cc < c; ++cc) x += sheet->colW[cc];
  return x;
}

int SheetGrid::rowY(int r)
{
  int y = clientRect(uiOrigin::Window).Y1 + HDRH;
  int vr = visRows() + 1;
  for (int i = 0; i < vr; ++i) { int rr = rowAt(i); if (rr == r) return y; if (rr >= Sheet::ROWS) break; y += sheet->rowH[rr]; }
  return -1;
}

void SheetGrid::selectRows(int a, int b)
{
  commit();
  ancR = a; selR = b; ancC = Sheet::COLS - 1; selC = 0;
}

void SheetGrid::selectCols(int a, int b)
{
  commit();
  ancC = a; selC = b; ancR = Sheet::ROWS - 1; selR = 0;
}

void SheetGrid::toggleChecks()
{
  commit();
  // a whole row / column picked: only go as far as the filled part of the sheet
  int lastR = 0, lastC = 0;
  for (auto & kv : sheet->cells) { lastR = imax(lastR, kv.first / Sheet::COLS); lastC = imax(lastC, kv.first % Sheet::COLS); }
  int R1 = r1(), R2 = r2(), C1 = c1(), C2 = c2();
  if (R1 == 0 && R2 == Sheet::ROWS - 1) R2 = imax(lastR, selR);
  if (C1 == 0 && C2 == Sheet::COLS - 1) C2 = imax(lastC, selC);
  bool anyBox = false, allTicked = true;
  for (int r = R1; r <= R2; ++r)
    for (int c = C1; c <= C2; ++c) {
      int st = Sheet::checkState(sheet->raw(r, c));
      if (st >= 0) anyBox = true;
      if (st != 1) allTicked = false;
    }
  // no boxes yet: put empty boxes in; otherwise tick them all, or untick if all are ticked
  const char * v = !anyBox ? "[ ]" : allTicked ? "[ ]" : "[x]";
  for (int r = R1; r <= R2; ++r)
    for (int c = C1; c <= C2; ++c) {
      const std::string & cur = sheet->raw(r, c);
      if (anyBox && Sheet::checkState(cur) < 0) continue;     // leave other cells alone
      if (!anyBox && !cur.empty()) continue;
      sheet->set(r, c, v);
    }
  if (onModified) onModified();
  repaint();
  if (onSelect) onSelect();
}

void SheetGrid::commit()
{
  if (!editing) return;
  editing = false;
  if (edit != sheet->raw(selR, selC)) {
    sheet->set(selR, selC, edit);
    if (onModified) onModified();
  }
  repaint();
}

void SheetGrid::paint(Rect const & clip)
{
  Rect cr = clientRect(uiOrigin::Window);
  auto cv = canvas();
  auto font = &FONT_6x10;
  cv->setBrushColor(T.win);
  cv->fillRectangle(cr.X1, cr.Y1, cr.X2, cr.Y2);
  bool wholeCols = r1() == 0 && r2() == Sheet::ROWS - 1, wholeRows = c1() == 0 && c2() == Sheet::COLS - 1;
  char b[16];
  // column letters
  cv->setBrushColor(T.face);
  cv->fillRectangle(cr.X1, cr.Y1, cr.X2, cr.Y1 + HDRH - 1);
  cv->fillRectangle(cr.X1, cr.Y1, cr.X1 + HDRW - 1, cr.Y2);
  int x = cr.X1 + HDRW;
  for (int c = leftC; c < Sheet::COLS && x <= cr.X2; ++c) {
    int w = sheet->colW[c];
    bool hi = wholeCols && c >= c1() && c <= c2();
    if (hi) { cv->setBrushColor(T.accent); cv->fillRectangle(x, cr.Y1, x + w - 2, cr.Y1 + HDRH - 2); }
    b[0] = 'A' + c; b[1] = 0;
    cv->setPenColor(hi ? T.accentText : T.text);
    cv->setBrushColor(hi ? T.accent : T.face);
    cv->drawText(font, x + w / 2 - 3, cr.Y1 + 3, b);
    x += w;
    cv->setPenColor(T.shadow);
    cv->drawLine(x - 1, cr.Y1, x - 1, cr.Y2);
  }
  cv->setPenColor(T.shadow);
  cv->drawLine(cr.X1 + HDRW - 1, cr.Y1, cr.X1 + HDRW - 1, cr.Y2);
  cv->drawLine(cr.X1, cr.Y1 + HDRH - 1, cr.X2, cr.Y1 + HDRH - 1);
  // rows
  int maxRight = x;
  int y = cr.Y1 + HDRH;
  for (int i = 0; y <= cr.Y2; ++i) {
    int r = rowAt(i);
    if (r >= Sheet::ROWS) break;
    int h = sheet->rowH[r];
    int ty = y + (h - 10) / 2;
    bool rowHi = wholeRows && r >= r1() && r <= r2();
    if (y + h >= clip.Y1 && y <= clip.Y2) {
      // row number
      if (rowHi) { cv->setBrushColor(T.accent); cv->fillRectangle(cr.X1, y, cr.X1 + HDRW - 2, y + h - 2); }
      snprintf(b, sizeof(b), "%3d", r + 1);
      cv->setPenColor(rowHi ? T.accentText : T.text);
      cv->setBrushColor(rowHi ? T.accent : T.face);
      cv->drawText(font, cr.X1 + 6, ty, b);
      bool band = bands && (r % 2 == 1);
      if (band) { cv->setBrushColor(T.face); cv->fillRectangle(cr.X1 + HDRW, y, cr.X2, y + h - 2); }
      int cx0 = cr.X1 + HDRW;
      for (int c = leftC; c < Sheet::COLS && cx0 <= cr.X2; ++c) {
        int w = sheet->colW[c];
        int x0 = cx0;
        cx0 += w;
        bool sel = (r == selR && c == selC);
        bool inRange = !sel && isRange() && r >= r1() && r <= r2() && c >= c1() && c <= c2();
        bool isNum = false;
        std::string s = (sel && editing) ? edit : sheet->display(r, c, isNum);
        RGB888 bg = band ? T.face : T.win;
        if (inRange) {
          bg = T.accent;
          cv->setBrushColor(bg);
          cv->fillRectangle(x0, y, x0 + w - 2, y + h - 2);
        }
        int box = (sel && editing) ? -1 : Sheet::checkState(s);
        if (box >= 0) {                                        // a check box instead of the text
          if (sel) { cv->setBrushColor(T.accent); cv->fillRectangle(x0, y, x0 + w - 2, y + h - 2); }
          RGB888 ink = (sel || inRange) ? T.accentText : T.text;
          int bx = x0 + 4, by = y + (h - 11) / 2;
          cv->setPenColor(ink);
          cv->drawRectangle(bx, by, bx + 10, by + 10);
          if (box) {
            cv->drawLine(bx + 2, by + 5, bx + 4, by + 8); cv->drawLine(bx + 3, by + 5, bx + 5, by + 8);
            cv->drawLine(bx + 4, by + 8, bx + 8, by + 2); cv->drawLine(bx + 5, by + 8, bx + 9, by + 2);
          }
          continue;
        }
        if (sel) {
          cv->setBrushColor(editing ? T.win : T.accent);
          cv->fillRectangle(x0, y, x0 + w - 2, y + h - 2);
          if (editing) { cv->setPenColor(T.accent); cv->drawRectangle(x0, y, x0 + w - 2, y + h - 2); }
        }
        if (s.empty() && !(sel && editing)) continue;
        int maxCh = imax(1, (w - 6) / font->width);
        if ((int)s.size() > maxCh) s = (sel && editing) ? s.substr(s.size() - maxCh) : s.substr(0, maxCh);
        int tx = x0 + 3;
        if (!(sel && editing)) {
          int al = sheet->alignOf(r, c);
          int tw = s.size() * font->width;
          if (al == 3 || (al == 0 && isNum)) tx = x0 + w - 4 - tw;
          else if (al == 2) tx = x0 + (w - 1 - tw) / 2;
        }
        bool hi = (sel && !editing) || inRange;
        cv->setPenColor(hi ? T.accentText : (s == "#ERR" ? T.warn : T.text));
        cv->setBrushColor(hi ? T.accent : (sel ? T.win : bg));
        cv->drawText(font, tx, ty, s.c_str());
        if (sel && editing) {
          int cx = x0 + 3 + s.size() * font->width;
          cv->setPenColor(T.text);
          cv->drawLine(cx, y + 2, cx, y + h - 4);
        }
      }
    }
    y += h;
    cv->setPenColor(T.shadow);
    cv->drawLine(cr.X1, y - 1, cr.X2, y - 1);
    if (freezeTop && i == 0) {                                // heavy line under the frozen row
      cv->setPenColor(T.text);
      cv->drawLine(cr.X1, y - 1, cr.X2, y - 1);
      cv->drawLine(cr.X1, y - 2, cr.X2, y - 2);
    }
  }
  // column lines over the cells
  cv->setPenColor(T.shadow);
  x = cr.X1 + HDRW;
  for (int c = leftC; c < Sheet::COLS && x <= cr.X2; ++c) { x += sheet->colW[c]; cv->drawLine(x - 1, cr.Y1 + HDRH, x - 1, cr.Y2); }
  (void)maxRight;
}

void SheetGrid::keyDown(uiKeyEventInfo const & k)
{
  if (onKey && onKey(k)) return;
  int oldR = selR, oldC = selC;
  if (editing) {
    switch (k.VK) {
      case VK_RETURN: case VK_KP_ENTER: commit(); selR = imin(Sheet::ROWS - 1, selR + 1); break;
      case VK_TAB:    commit(); selC = imin(Sheet::COLS - 1, selC + 1); break;
      case VK_ESCAPE: editing = false; break;
      case VK_BACKSPACE: if (!edit.empty()) edit.pop_back(); break;
      case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
        if (!edit.empty() && edit[0] == '=' && (k.VK == VK_LEFT || k.VK == VK_RIGHT)) return;
        commit();
        if (k.VK == VK_UP) selR = imax(0, selR - 1);
        if (k.VK == VK_DOWN) selR = imin(Sheet::ROWS - 1, selR + 1);
        if (k.VK == VK_LEFT) selC = imax(0, selC - 1);
        if (k.VK == VK_RIGHT) selC = imin(Sheet::COLS - 1, selC + 1);
        break;
      default:
        if (k.ASCII >= 32 && k.ASCII < 127 && !k.CTRL && edit.size() < 120) edit += (char)k.ASCII;
        else return;
    }
  } else {
    if (k.VK == VK_SPACE && Sheet::checkState(sheet->raw(selR, selC)) >= 0) { toggleChecks(); return; }
    switch (k.VK) {
      case VK_UP: case VK_KP_UP:       selR = imax(0, selR - 1); break;
      case VK_DOWN: case VK_KP_DOWN: case VK_RETURN: case VK_KP_ENTER: selR = imin(Sheet::ROWS - 1, selR + 1); break;
      case VK_LEFT: case VK_KP_LEFT:   selC = imax(0, selC - 1); break;
      case VK_RIGHT: case VK_KP_RIGHT: case VK_TAB: selC = imin(Sheet::COLS - 1, selC + 1); break;
      case VK_PAGEUP:   selR = imax(0, selR - visRows()); break;
      case VK_PAGEDOWN: selR = imin(Sheet::ROWS - 1, selR + visRows()); break;
      case VK_HOME:     selC = 0; if (k.CTRL) selR = 0; break;
      case VK_F2:       editing = true; edit = sheet->raw(selR, selC); break;
      case VK_DELETE: case VK_BACKSPACE: {
        bool any = false;
        for (int r = r1(); r <= r2(); ++r)
          for (int c = c1(); c <= c2(); ++c)
            if (!sheet->raw(r, c).empty()) { sheet->set(r, c, ""); any = true; }
        if (any && onModified) onModified();
        break;
      }
      default:
        if (k.ASCII >= 32 && k.ASCII < 127 && !k.CTRL) { editing = true; edit = std::string(1, (char)k.ASCII); }
        else return;
    }
  }
  bool moveKey = k.VK == VK_UP || k.VK == VK_DOWN || k.VK == VK_LEFT || k.VK == VK_RIGHT || k.VK == VK_KP_UP || k.VK == VK_KP_DOWN ||
                 k.VK == VK_KP_LEFT || k.VK == VK_KP_RIGHT || k.VK == VK_PAGEUP || k.VK == VK_PAGEDOWN || k.VK == VK_HOME;
  if (!(moveKey && k.SHIFT && !editing) && k.VK != VK_DELETE && k.VK != VK_BACKSPACE) collapse();
  int oldTop = topR, oldLeft = leftC;
  ensureVisible();
  (void)oldR; (void)oldC; (void)oldTop; (void)oldLeft;
  repaint();
  if (onSelect) onSelect();
}

void SheetGrid::processEvent(uiEvent * event)
{
  uiControl::processEvent(event);
  switch (event->id) {
    case UIEVT_PAINT:
      beginPaint(event, clientRect(uiOrigin::Window));
      paint(Rect(event->params.rect.X1, event->params.rect.Y1, event->params.rect.X2, event->params.rect.Y2));
      break;
    case UIEVT_KEYDOWN:
      keyDown(event->params.key);
      break;
    case UIEVT_MOUSEBUTTONDOWN:
      if (event->params.mouse.changedButton == 3) {          // right button: pick what's under it, then the menu
        app()->setFocusedWindow(this);
        int px = event->params.mouse.status.X, py = event->params.mouse.status.Y;
        int r, c, what = hitTest(px, py, r, c);
        commit();
        drag = 0;
        if (what == 1 || what == 4 || what == 5) {
          if (r < 0) r = selR;
          if (c < 0) c = selC;
          if (r < r1() || r > r2() || c < c1() || c > c2()) { selR = r; selC = c; collapse(); }
        } else if (what == 2) { if (!allRows() || c < c1() || c > c2()) selectCols(c, c); }
        else if (what == 3) { if (!allCols() || r < r1() || r > r2()) selectRows(r, r); }
        repaint();
        if (onSelect) onSelect();
        if (onRightClick) { Rect sr = rect(uiOrigin::Screen); onRightClick(sr.X1 + px, sr.Y1 + py); }
        break;
      }
      if (event->params.mouse.changedButton == 1) {
        app()->setFocusedWindow(this);
        int px = event->params.mouse.status.X, py = event->params.mouse.status.Y;
        int r, c, what = hitTest(px, py, r, c);
        drag = 0;
        if (what == 0) break;
        commit();
        if (what == 4) { drag = 4; dragIdx = c; dragFrom = colX(c); break; }            // resize a column
        if (what == 5) { drag = 5; dragIdx = r; dragFrom = rowY(r); break; }            // resize a row
        if (what == 6) { ancR = Sheet::ROWS - 1; ancC = Sheet::COLS - 1; selR = selC = 0; }   // corner: everything
        else if (what == 2) { selectCols(c, c); drag = 2; dragIdx = c; }
        else if (what == 3) { selectRows(r, r); drag = 3; dragIdx = r; }
        else {
          // clicking the box of a check-box cell ticks it
          int inX = px - colX(c);
          bool onBox = inX >= 0 && inX < 16 && Sheet::checkState(sheet->raw(r, c)) >= 0;
          selR = r; selC = c;
          collapse();
          if (onBox) { toggleChecks(); break; }
          drag = 1;
        }
        repaint();
        if (onSelect) onSelect();
      }
      break;
    case UIEVT_MOUSEMOVE: {
      int px = event->params.mouse.status.X, py = event->params.mouse.status.Y;
      if (!drag || !event->params.mouse.status.buttons.left) { drag = 0; break; }
      if (drag == 4 && dragFrom >= 0) {
        int w = fabgl::iclamp(px - dragFrom + 2, 16, 250);
        if (w != sheet->colW[dragIdx]) { sheet->colW[dragIdx] = w; sheet->sizesChanged = true; repaint(); if (onModified) onModified(); }
        break;
      }
      if (drag == 5 && dragFrom >= 0) {
        int h = fabgl::iclamp(py - dragFrom + 2, 12, 64);
        if (h != sheet->rowH[dragIdx]) { sheet->rowH[dragIdx] = h; sheet->sizesChanged = true; repaint(); if (onModified) onModified(); }
        break;
      }
      int r, c;
      hitTest(px, py, r, c);
      bool changed = false;
      if (drag == 1 && r >= 0 && c >= 0 && (r != selR || c != selC)) { selR = r; selC = c; changed = true; }    // anchor stays put
      if (drag == 2 && c >= 0 && c != selC) { selectCols(dragIdx, c); changed = true; }
      if (drag == 3 && r >= 0 && r != selR) { selectRows(dragIdx, r); changed = true; }
      if (changed) { repaint(); if (onSelect) onSelect(); }
      break;
    }
    case UIEVT_MOUSEBUTTONUP:
      drag = 0;
      break;
    case UIEVT_DBLCLICK: {
      int r, c, what = hitTest(event->params.mouse.status.X, event->params.mouse.status.Y, r, c);
      if (what == 4) {                       // double-click a column edge: fit the widest text
        int w = 30;
        for (int rr = 0; rr < Sheet::ROWS; ++rr) { bool n; w = imax(w, (int)sheet->display(rr, c, n).size() * 6 + 10); }
        sheet->colW[c] = imin(250, w); sheet->sizesChanged = true;
        repaint(); if (onModified) onModified();
        break;
      }
      if (what == 1 && !editing) { editing = true; edit = sheet->raw(selR, selC); repaint(); if (onSelect) onSelect(); }
      break;
    }
    case UIEVT_MOUSEWHEEL:
      topR = imax(0, imin(Sheet::ROWS - 2, topR + event->params.mouse.status.wheelDelta * 2));
      repaint();
      break;
    case UIEVT_SETFOCUS:
    case UIEVT_KILLFOCUS:
      repaint();
      break;
    default:
      break;
  }
}
