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

static std::string fmtNum(double v)
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

int SheetGrid::visRows() { return imax(1, (clientRect(uiOrigin::Window).height() - ROWH) / ROWH); }
int SheetGrid::visCols() { return imax(1, (clientRect(uiOrigin::Window).width() - HDRW) / COLW); }

void SheetGrid::ensureVisible()
{
  int vr = visRows(), vc = visCols();
  if (selR < topR) topR = selR;
  if (selR >= topR + vr) topR = selR - vr + 1;
  if (selC < leftC) leftC = selC;
  if (selC >= leftC + vc) leftC = selC - vc + 1;
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
  int vr = visRows(), vc = visCols();
  // headers
  cv->setBrushColor(T.face);
  cv->fillRectangle(cr.X1, cr.Y1, cr.X2, cr.Y1 + ROWH - 1);
  cv->fillRectangle(cr.X1, cr.Y1, cr.X1 + HDRW - 1, cr.Y2);
  cv->setPenColor(T.shadow);
  char b[64];
  for (int i = 0; i <= vc; ++i) {
    int x = cr.X1 + HDRW + i * COLW;
    cv->drawLine(x - 1, cr.Y1, x - 1, cr.Y2);
    if (i < vc && leftC + i < Sheet::COLS) {
      b[0] = 'A' + leftC + i; b[1] = 0;
      cv->setPenColor(T.text);
      cv->setBrushColor(T.face);
      cv->drawText(font, x + COLW / 2 - 3, cr.Y1 + 3, b);
      cv->setPenColor(T.shadow);
    }
  }
  for (int i = 0; i <= vr; ++i) {
    int y = cr.Y1 + ROWH + i * ROWH;
    cv->drawLine(cr.X1, y - 1, cr.X2, y - 1);
    if (i < vr && topR + i < Sheet::ROWS) {
      snprintf(b, sizeof(b), "%3d", topR + i + 1);
      cv->setPenColor(T.text);
      cv->setBrushColor(T.face);
      cv->drawText(font, cr.X1 + 6, y + 3, b);
      cv->setPenColor(T.shadow);
    }
  }
  // cells
  int maxCh = (COLW - 6) / font->width;
  for (int i = 0; i < vr; ++i) {
    int r = topR + i;
    if (r >= Sheet::ROWS) break;
    int y = cr.Y1 + ROWH + i * ROWH;
    if (y + ROWH < clip.Y1 || y > clip.Y2) continue;
    for (int j = 0; j < vc; ++j) {
      int c = leftC + j;
      if (c >= Sheet::COLS) break;
      int x = cr.X1 + HDRW + j * COLW;
      bool sel = (r == selR && c == selC);
      bool isNum = false;
      std::string s = (sel && editing) ? edit : sheet->display(r, c, isNum);
      if (sel) {
        cv->setBrushColor(editing ? T.win : T.accent);
        cv->fillRectangle(x, y, x + COLW - 2, y + ROWH - 2);
        if (editing) {
          cv->setPenColor(T.accent);
          cv->drawRectangle(x, y, x + COLW - 2, y + ROWH - 2);
        }
      }
      if (s.empty() && !(sel && editing)) continue;
      if ((int)s.size() > maxCh) s = (sel && editing) ? s.substr(s.size() - maxCh) : s.substr(0, maxCh);
      int tx = x + 3;
      if (isNum && !editing) tx = x + COLW - 4 - s.size() * font->width;
      cv->setPenColor(sel && !editing ? T.accentText : (s == "#ERR" ? T.warn : T.text));
      cv->setBrushColor(sel && !editing ? T.accent : T.win);
      cv->drawText(font, tx, y + 3, s.c_str());
      if (sel && editing) {
        int cx = x + 3 + s.size() * font->width;
        cv->setPenColor(T.text);
        cv->drawLine(cx, y + 2, cx, y + ROWH - 4);
      }
    }
  }
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
    switch (k.VK) {
      case VK_UP: case VK_KP_UP:       selR = imax(0, selR - 1); break;
      case VK_DOWN: case VK_KP_DOWN: case VK_RETURN: case VK_KP_ENTER: selR = imin(Sheet::ROWS - 1, selR + 1); break;
      case VK_LEFT: case VK_KP_LEFT:   selC = imax(0, selC - 1); break;
      case VK_RIGHT: case VK_KP_RIGHT: case VK_TAB: selC = imin(Sheet::COLS - 1, selC + 1); break;
      case VK_PAGEUP:   selR = imax(0, selR - visRows()); break;
      case VK_PAGEDOWN: selR = imin(Sheet::ROWS - 1, selR + visRows()); break;
      case VK_HOME:     selC = 0; if (k.CTRL) selR = 0; break;
      case VK_F2:       editing = true; edit = sheet->raw(selR, selC); break;
      case VK_DELETE: case VK_BACKSPACE:
        if (!sheet->raw(selR, selC).empty()) { sheet->set(selR, selC, ""); if (onModified) onModified(); }
        break;
      default:
        if (k.ASCII >= 32 && k.ASCII < 127 && !k.CTRL) { editing = true; edit = std::string(1, (char)k.ASCII); }
        else return;
    }
  }
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
      if (event->params.mouse.changedButton == 1) {
        app()->setFocusedWindow(this);
        Rect cr = clientRect(uiOrigin::Window);
        int mx = event->params.mouse.status.X - cr.X1 - HDRW, my = event->params.mouse.status.Y - cr.Y1 - ROWH;
        if (mx >= 0 && my >= 0) {
          commit();
          selC = imin(Sheet::COLS - 1, leftC + mx / COLW);
          selR = imin(Sheet::ROWS - 1, topR + my / ROWH);
          repaint();
          if (onSelect) onSelect();
        }
      }
      break;
    case UIEVT_DBLCLICK:
      if (!editing) { editing = true; edit = sheet->raw(selR, selC); repaint(); if (onSelect) onSelect(); }
      break;
    case UIEVT_MOUSEWHEEL:
      topR = imax(0, imin(Sheet::ROWS - visRows(), topR + event->params.mouse.status.wheelDelta * 2));
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
