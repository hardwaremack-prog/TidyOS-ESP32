// TidyOS games: Sammy the Snake, Minesweeper, Blocks, 2048, Bricks, Paradrop
// Small and light: each board only repaints the squares that change.
#include <Preferences.h>
#include <deque>
#include <math.h>
#include <vector>
#include "core.h"
#include "style.h"

using namespace fabgl;

static Preferences s_gp;
static int bestScore(const char * key) { s_gp.begin("tidygames", true); int v = s_gp.getInt(key, 0); s_gp.end(); return v; }
static void saveBest(const char * key, int v) { s_gp.begin("tidygames", false); s_gp.putInt(key, v); s_gp.end(); }

// ====================================================================== base board

class Board : public uiControl {
public:
  Board(uiWindow * parent, Point pos, Size size) : uiControl(parent, pos, size, true, 0) {
    windowStyle().borderSize = 1;
    windowStyle().focusedBorderSize = 1;
    windowStyle().borderColor = T.shadow;
    windowStyle().activeBorderColor = T.shadow;
    windowStyle().focusedBorderColor = T.accent;
    setFocusIndex(1);
    windowProps().focusable = true;   // needed to get keyboard focus
  }
  ~Board() { stopTimer(); }

  void processEvent(uiEvent * e) override {
    uiControl::processEvent(e);
    switch (e->id) {
      case UIEVT_PAINT:
        beginPaint(e, clientRect(uiOrigin::Window));
        paint(Rect(e->params.rect.X1, e->params.rect.Y1, e->params.rect.X2, e->params.rect.Y2));
        break;
      case UIEVT_KEYDOWN:
        keyIn(e->params.key, true);
        break;
      case UIEVT_KEYUP:
        keyIn(e->params.key, false);
        break;
      case UIEVT_MOUSEMOVE: {
        Rect cr = clientRect(uiOrigin::Window);
        mouseMove(e->params.mouse.status.X - cr.X1, e->params.mouse.status.Y - cr.Y1);
        break;
      }
      case UIEVT_MOUSEBUTTONDOWN: {
        app()->setFocusedWindow(this);
        Rect cr = clientRect(uiOrigin::Window);
        click(e->params.mouse.status.X - cr.X1, e->params.mouse.status.Y - cr.Y1, e->params.mouse.changedButton);
        break;
      }
      case UIEVT_TIMER:
        tick();
        break;
      default:
        break;
    }
  }

  virtual void paint(Rect const & clip) = 0;
  virtual void key(uiKeyEventInfo const &) {}
  virtual void click(int, int, int) {}
  virtual void keyUp(uiKeyEventInfo const &) {}

  // every game: W A S D work like the arrow keys
  void keyIn(uiKeyEventInfo const & k0, bool down) {
    uiKeyEventInfo k = k0;
    switch (k.VK) {
      case VK_w: case VK_W: k.VK = VK_UP; break;
      case VK_a: case VK_A: k.VK = VK_LEFT; break;
      case VK_s: case VK_S: k.VK = VK_DOWN; break;
      case VK_d: case VK_D: k.VK = VK_RIGHT; break;
      default: break;
    }
    if (down) key(k); else keyUp(k);
  }
  virtual void mouseMove(int, int) {}
  virtual void tick() {}
  virtual void newGame() = 0;

  std::function<void(int, const char *)> onScore;     // score, message ("" = still playing)

  void startTimer(int ms) { stopTimer(); m_timer = app()->setTimer(this, ms); }
  void stopTimer() { if (m_timer) { app()->killTimer(m_timer); m_timer = nullptr; } }

protected:
  uiTimerHandle m_timer = nullptr;
  Rect cr() { return clientRect(uiOrigin::Window); }
  // action games: collect the areas that changed this tick (board coords), then repaint them
  Rect m_dirty[40];
  int  m_nd = 0;
  bool m_all = false;
  void dirty(Rect const & d) {
    if (m_all) return;
    for (int i = 0; i < m_nd; ++i) if (m_dirty[i].intersects(d)) { m_dirty[i] = m_dirty[i].merge(d); return; }
    if (m_nd >= 40) { m_all = true; return; }
    m_dirty[m_nd++] = d;
  }
  void flushDirty() {
    Rect r = cr();
    if (m_all) repaint();
    else for (int i = 0; i < m_nd; ++i) {
      Rect d(r.X1 + m_dirty[i].X1, r.Y1 + m_dirty[i].Y1, r.X1 + m_dirty[i].X2, r.Y1 + m_dirty[i].Y2);
      d = d.intersection(r);
      if (d.X1 <= d.X2 && d.Y1 <= d.Y2) repaint(d);
    }
    m_nd = 0; m_all = false;
  }
  // does board-coords rect b touch the paint clip (window coords)?
  bool hits(Rect const & clip, int x1, int y1, int x2, int y2) {
    Rect r = cr();
    return clip.intersects(Rect(r.X1 + x1, r.Y1 + y1, r.X1 + x2, r.Y1 + y2));
  }
  void repaintCell(int x, int y, int cs, int ox = 0, int oy = 0) {
    Rect r = cr();
    repaint(Rect(r.X1 + ox + x * cs, r.Y1 + oy + y * cs, r.X1 + ox + x * cs + cs - 1, r.Y1 + oy + y * cs + cs - 1));
  }
  void cell3D(Canvas * cv, int x, int y, int s, bool raised) {
    cv->setBrushColor(T.face);
    cv->fillRectangle(x, y, x + s - 1, y + s - 1);
    cv->setPenColor(raised ? T.win : T.shadow);
    cv->drawLine(x, y, x + s - 1, y); cv->drawLine(x, y, x, y + s - 1);
    cv->setPenColor(raised ? T.shadow : T.win);
    cv->drawLine(x + s - 1, y, x + s - 1, y + s - 1); cv->drawLine(x, y + s - 1, x + s - 1, y + s - 1);
  }
};

// ====================================================================== Sammy the Snake

class SnakeBoard : public Board {
public:
  static const int W = 30, H = 21, CS = 16;
  SnakeBoard(uiWindow * p, Point pos) : Board(p, pos, Size(W * CS + 2, H * CS + 2)) { newGame(); }

  std::deque<Point> body;          // front = head
  int dx = 1, dy = 0, ndx = 1, ndy = 0;
  Point food;
  bool alive = false, started = false;
  int score = 0;

  bool onSnake(int x, int y) { for (auto & p : body) if (p.X == x && p.Y == y) return true; return false; }

  void placeFood() {
    do { food = Point(esp_random() % W, esp_random() % H); } while (onSnake(food.X, food.Y));
    repaintCell(food.X, food.Y, CS);
  }

  void newGame() override {
    body.clear();
    for (int i = 0; i < 4; ++i) body.push_back(Point(8 - i, H / 2));
    dx = ndx = 1; dy = ndy = 0;
    score = 0; alive = true; started = false;
    placeFood();
    stopTimer();
    repaint();
    if (onScore) onScore(0, "Press an arrow key to start.");
  }

  void key(uiKeyEventInfo const & k) override {
    int x = 0, y = 0;
    if (k.VK == VK_UP || k.VK == VK_KP_UP || k.VK == VK_w || k.VK == VK_W) y = -1;
    else if (k.VK == VK_DOWN || k.VK == VK_KP_DOWN || k.VK == VK_s || k.VK == VK_S) y = 1;
    else if (k.VK == VK_LEFT || k.VK == VK_KP_LEFT || k.VK == VK_a || k.VK == VK_A) x = -1;
    else if (k.VK == VK_RIGHT || k.VK == VK_KP_RIGHT || k.VK == VK_d || k.VK == VK_D) x = 1;
    else if (k.VK == VK_SPACE && !alive) { newGame(); return; }
    else return;
    if (!alive) return;
    if (x == -dx && y == -dy) return;            // can't turn back on yourself
    ndx = x; ndy = y;
    if (!started) { started = true; startTimer(130); if (onScore) onScore(score, ""); }
  }

  void tick() override {
    if (!alive) return;
    dx = ndx; dy = ndy;
    Point h(body.front().X + dx, body.front().Y + dy);
    bool eat = h.X == food.X && h.Y == food.Y;
    Point tail = body.back();
    if (h.X < 0 || h.Y < 0 || h.X >= W || h.Y >= H || (onSnake(h.X, h.Y) && !(h.X == tail.X && h.Y == tail.Y && !eat))) {
      alive = false;
      stopTimer();
      repaintCell(body.front().X, body.front().Y, CS);
      if (onScore) onScore(score, "Bonk! Game over - press Space or New game.");
      return;
    }
    Point oldHead = body.front();
    body.push_front(h);
    if (eat) {
      score += 10;
      placeFood();
      if (onScore) onScore(score, "");
      if (body.size() % 5 == 0) startTimer(imax(60, 130 - (int)body.size()));   // a bit faster as Sammy grows
    } else {
      body.pop_back();
      repaintCell(tail.X, tail.Y, CS);
    }
    repaintCell(oldHead.X, oldHead.Y, CS);
    repaintCell(h.X, h.Y, CS);
  }

  void paint(Rect const & clip) override {
    auto cv = canvas();
    Rect r = cr();
    cv->setBrushColor(T.win);
    cv->fillRectangle(clip.X1, clip.Y1, clip.X2, clip.Y2);
    auto inClip = [&](int x, int y) {
      int px = r.X1 + x * CS, py = r.Y1 + y * CS;
      return px + CS > clip.X1 && px <= clip.X2 && py + CS > clip.Y1 && py <= clip.Y2;
    };
    // food: a little apple
    if (inClip(food.X, food.Y)) {
      int px = r.X1 + food.X * CS, py = r.Y1 + food.Y * CS;
      cv->setBrushColor(T.warn);
      cv->fillEllipse(px + CS / 2, py + CS / 2 + 1, CS - 4, CS - 4);
      cv->setPenColor(T.good);
      cv->drawLine(px + CS / 2, py + 1, px + CS / 2 + 2, py + 3);
    }
    bool head = true;
    for (auto & p : body) {
      if (inClip(p.X, p.Y)) {
        int px = r.X1 + p.X * CS, py = r.Y1 + p.Y * CS;
        cv->setBrushColor(head ? T.text : T.accent);
        cv->fillRectangle(px + 1, py + 1, px + CS - 2, py + CS - 2);
        if (head) {                                   // eyes
          cv->setBrushColor(alive ? T.win : T.warn);
          cv->fillRectangle(px + 4, py + 4, px + 6, py + 6);
          cv->fillRectangle(px + 9, py + 4, px + 11, py + 6);
        }
      }
      head = false;
    }
  }
};

// ====================================================================== Minesweeper

class MinesBoard : public Board {
public:
  static const int W = 16, H = 16, CS = 22, MINES = 40;
  MinesBoard(uiWindow * p, Point pos) : Board(p, pos, Size(W * CS + 2, H * CS + 2)) { newGame(); }

  uint8_t mine[H][W], open[H][W], flag[H][W], count[H][W];
  bool first = true, over = false;
  int opened = 0, flags = 0, seconds = 0, cx = 0, cy = 0;

  void newGame() override {
    memset(mine, 0, sizeof(mine)); memset(open, 0, sizeof(open)); memset(flag, 0, sizeof(flag)); memset(count, 0, sizeof(count));
    first = true; over = false; opened = flags = seconds = 0;
    stopTimer();
    repaint();
    status("Click a square to start. Right-click (or F) to flag.");
  }

  void status(const char * m) {
    if (!onScore) return;
    char b[96];
    snprintf(b, sizeof(b), "Mines left: %d   Time: %ds   %s", MINES - flags, seconds, m);
    onScore(seconds, b);
  }

  void layMines(int sx, int sy) {
    int n = 0;
    while (n < MINES) {
      int x = esp_random() % W, y = esp_random() % H;
      if (mine[y][x] || (abs(x - sx) <= 1 && abs(y - sy) <= 1)) continue;   // first click is always safe
      mine[y][x] = 1; ++n;
    }
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        for (int j = -1; j <= 1; ++j)
          for (int i = -1; i <= 1; ++i) {
            int xx = x + i, yy = y + j;
            if (xx >= 0 && yy >= 0 && xx < W && yy < H && mine[yy][xx]) count[y][x]++;
          }
  }

  void reveal(int x, int y) {
    // flood fill without recursion
    std::vector<Point> st;
    st.push_back(Point(x, y));
    while (!st.empty()) {
      Point p = st.back(); st.pop_back();
      if (p.X < 0 || p.Y < 0 || p.X >= W || p.Y >= H || open[p.Y][p.X] || flag[p.Y][p.X]) continue;
      open[p.Y][p.X] = 1; ++opened;
      repaintCell(p.X, p.Y, CS);
      if (!mine[p.Y][p.X] && count[p.Y][p.X] == 0)
        for (int j = -1; j <= 1; ++j) for (int i = -1; i <= 1; ++i) if (i || j) st.push_back(Point(p.X + i, p.Y + j));
    }
  }

  void dig(int x, int y) {
    if (over || flag[y][x] || open[y][x]) return;
    if (first) { first = false; layMines(x, y); startTimer(1000); }
    if (mine[y][x]) {
      over = true; stopTimer();
      for (int j = 0; j < H; ++j) for (int i = 0; i < W; ++i) if (mine[j][i]) open[j][i] = 1;
      repaint();
      status("Boom! Click New game to try again.");
      return;
    }
    reveal(x, y);
    if (opened == W * H - MINES) {
      over = true; stopTimer();
      int best = bestScore("mines");
      if (!best || seconds < best) { saveBest("mines", seconds); status("You cleared it! New best time!"); }
      else status("You cleared the field!");
    } else status("");
  }

  void toggleFlag(int x, int y) {
    if (over || open[y][x]) return;
    flag[y][x] ^= 1;
    flags += flag[y][x] ? 1 : -1;
    repaintCell(x, y, CS);
    status("");
  }

  void click(int x, int y, int btn) override {
    int gx = x / CS, gy = y / CS;
    if (gx < 0 || gy < 0 || gx >= W || gy >= H) return;
    int ox = cx, oy = cy;
    cx = gx; cy = gy;
    repaintCell(ox, oy, CS);
    if (btn == 3) toggleFlag(gx, gy); else if (btn == 1) dig(gx, gy);
  }

  void key(uiKeyEventInfo const & k) override {
    int ox = cx, oy = cy;
    if (k.VK == VK_LEFT) cx = imax(0, cx - 1);
    else if (k.VK == VK_RIGHT) cx = imin(W - 1, cx + 1);
    else if (k.VK == VK_UP) cy = imax(0, cy - 1);
    else if (k.VK == VK_DOWN) cy = imin(H - 1, cy + 1);
    else if (k.VK == VK_SPACE || k.VK == VK_RETURN) { dig(cx, cy); return; }
    else if (k.VK == VK_f || k.VK == VK_F) { toggleFlag(cx, cy); return; }
    else return;
    repaintCell(ox, oy, CS);
    repaintCell(cx, cy, CS);
  }

  void tick() override { if (!over && !first) { ++seconds; status(""); } }

  void paint(Rect const & clip) override {
    auto cv = canvas();
    Rect r = cr();
    static const char * digits = "012345678";
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        int px = r.X1 + x * CS, py = r.Y1 + y * CS;
        if (px + CS <= clip.X1 || px > clip.X2 || py + CS <= clip.Y1 || py > clip.Y2) continue;
        if (!open[y][x]) {
          cell3D(cv, px, py, CS, true);
          if (flag[y][x]) {
            cv->setPenColor(T.text);
            cv->drawLine(px + 8, py + 4, px + 8, py + 17);
            cv->setBrushColor(T.warn);
            cv->fillRectangle(px + 9, py + 4, px + 15, py + 9);
          }
        } else {
          cv->setBrushColor(mine[y][x] ? (over && !flag[y][x] ? T.warn : T.face) : T.win);
          cv->fillRectangle(px, py, px + CS - 1, py + CS - 1);
          cv->setPenColor(T.face);
          cv->drawRectangle(px, py, px + CS - 1, py + CS - 1);
          if (mine[y][x]) {
            cv->setBrushColor(T.text);
            cv->fillEllipse(px + CS / 2, py + CS / 2, 11, 11);
          } else if (count[y][x]) {
            char s[2] = { digits[count[y][x]], 0 };
            cv->setPenColor(count[y][x] == 1 ? T.accent : count[y][x] == 2 ? T.good : count[y][x] == 3 ? T.warn : T.text);
            cv->setBrushColor(T.win);
            cv->drawText(&FONT_std_16, px + 7, py + 3, s);
          }
        }
        if (x == cx && y == cy && hasFocus()) {
          cv->setPenColor(T.accent);
          cv->drawRectangle(px + 1, py + 1, px + CS - 2, py + CS - 2);
        }
      }
  }
};

// ====================================================================== Blocks (falling blocks)

class BlocksBoard : public Board {
public:
  static const int W = 10, H = 20, CS = 18, SIDE = 110;
  BlocksBoard(uiWindow * p, Point pos) : Board(p, pos, Size(W * CS + SIDE + 2, H * CS + 2)) { newGame(); }

  uint8_t well[H][W];
  int piece, nextPiece, rot, px, py, score, lines, level;
  bool over = false, paused = false;

  // 7 pieces, each 4 cells in a 4x4 box (x,y pairs) for rotation 0
  static const int8_t * shape(int p) {
    static const int8_t S[7][8] = {
      { 0,1, 1,1, 2,1, 3,1 },   // I
      { 1,0, 2,0, 1,1, 2,1 },   // O
      { 0,1, 1,1, 2,1, 1,0 },   // T
      { 0,1, 1,1, 1,0, 2,0 },   // S
      { 0,0, 1,0, 1,1, 2,1 },   // Z
      { 0,0, 0,1, 1,1, 2,1 },   // J
      { 2,0, 0,1, 1,1, 2,1 },   // L
    };
    return S[p];
  }
  void cells(int p, int r, int out[4][2]) {
    const int8_t * s = shape(p);
    int n = (p == 0) ? 4 : 3;          // rotate inside a 4x4 (I) or 3x3 box
    for (int i = 0; i < 4; ++i) {
      int x = s[i * 2], y = s[i * 2 + 1];
      for (int k = 0; k < r % 4 && p != 1; ++k) { int t = x; x = n - 1 - y; y = t; }
      out[i][0] = x; out[i][1] = y;
    }
  }
  bool fits(int p, int r, int x, int y) {
    int c[4][2];
    cells(p, r, c);
    for (auto & q : c) {
      int xx = x + q[0], yy = y + q[1];
      if (xx < 0 || xx >= W || yy >= H) return false;
      if (yy >= 0 && well[yy][xx]) return false;
    }
    return true;
  }
  void repaintPiece() {
    int c[4][2];
    cells(piece, rot, c);
    for (auto & q : c) if (py + q[1] >= 0) repaintCell(px + q[0], py + q[1], CS);
  }
  void spawn() {
    piece = nextPiece;
    nextPiece = esp_random() % 7;
    rot = 0; px = 3; py = -1;
    Rect r = cr();
    repaint(Rect(r.X1 + W * CS, r.Y1, r.X2, r.Y2));
    if (!fits(piece, rot, px, py)) {
      over = true; stopTimer();
      int best = bestScore("blocks");
      if (score > best) { saveBest("blocks", score); report("Game over - new high score! Space for a new game."); }
      else report("Game over. Press Space for a new game.");
    }
  }
  void report(const char * m) {
    if (!onScore) return;
    char b[96];
    snprintf(b, sizeof(b), "Score %d   Lines %d   Level %d   %s", score, lines, level, m);
    onScore(score, b);
  }
  void newGame() override {
    memset(well, 0, sizeof(well));
    score = lines = 0; level = 1; over = paused = false;
    nextPiece = esp_random() % 7;
    spawn();
    startTimer(600);
    repaint();
    report("Arrows move, Up turns, Space drops, P pauses.");
  }
  void lockPiece() {
    int c[4][2];
    cells(piece, rot, c);
    for (auto & q : c) if (py + q[1] >= 0) well[py + q[1]][px + q[0]] = piece + 1;
    int cleared = 0;
    for (int y = H - 1; y >= 0; --y) {
      bool full = true;
      for (int x = 0; x < W; ++x) if (!well[y][x]) full = false;
      if (full) {
        for (int yy = y; yy > 0; --yy) memcpy(well[yy], well[yy - 1], W);
        memset(well[0], 0, W);
        ++cleared; ++y;
      }
    }
    if (cleared) {
      static const int pts[5] = { 0, 100, 300, 500, 800 };
      score += pts[cleared] * level;
      lines += cleared;
      int nl = 1 + lines / 10;
      if (nl != level) { level = nl; startTimer(imax(90, 600 - (level - 1) * 55)); }
      Rect r = cr();
      repaint(Rect(r.X1, r.Y1, r.X1 + W * CS - 1, r.Y2));
    }
    report("");
    spawn();
  }
  bool move(int dx, int dy, int dr) {
    if (!fits(piece, rot + dr, px + dx, py + dy)) return false;
    repaintPiece();
    px += dx; py += dy; rot += dr;
    repaintPiece();
    return true;
  }
  void tick() override { if (!over && !paused && !move(0, 1, 0)) lockPiece(); }
  void key(uiKeyEventInfo const & k) override {
    if (over) { if (k.VK == VK_SPACE || k.VK == VK_RETURN) newGame(); return; }
    if (k.VK == VK_p || k.VK == VK_P) { paused = !paused; report(paused ? "Paused." : ""); return; }
    if (paused) return;
    switch (k.VK) {
      case VK_LEFT:  move(-1, 0, 0); break;
      case VK_RIGHT: move(1, 0, 0); break;
      case VK_DOWN:  if (move(0, 1, 0)) score += 1; break;
      case VK_UP:    if (!move(0, 0, 1) && !move(-1, 0, 1)) move(1, 0, 1); break;   // wall kick
      case VK_SPACE: { int d = 0; while (move(0, 1, 0)) ++d; score += d * 2; lockPiece(); break; }
      default: break;
    }
  }
  void block(Canvas * cv, int x, int y, int color) {
    static const RGB888 * cols[7] = { &T.accent, &T.good, &T.warn, &T.accent, &T.good, &T.shadow, &T.warn };
    cv->setBrushColor(*cols[color % 7]);
    cv->fillRectangle(x + 1, y + 1, x + CS - 2, y + CS - 2);
    cv->setPenColor(T.win);
    cv->drawLine(x + 1, y + 1, x + CS - 3, y + 1);
    cv->drawLine(x + 1, y + 1, x + 1, y + CS - 3);
  }
  void paint(Rect const & clip) override {
    auto cv = canvas();
    Rect r = cr();
    cv->setBrushColor(T.win);
    cv->fillRectangle(clip.X1, clip.Y1, clip.X2, clip.Y2);
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        if (well[y][x]) block(cv, r.X1 + x * CS, r.Y1 + y * CS, well[y][x] - 1);
    if (!over) {
      int c[4][2];
      cells(piece, rot, c);
      for (auto & q : c) if (py + q[1] >= 0) block(cv, r.X1 + (px + q[0]) * CS, r.Y1 + (py + q[1]) * CS, piece);
    }
    // side panel
    int sx = r.X1 + W * CS;
    cv->setBrushColor(T.face);
    cv->fillRectangle(sx, r.Y1, r.X2, r.Y2);
    cv->setPenColor(T.shadow);
    cv->drawLine(sx, r.Y1, sx, r.Y2);
    cv->setPenColor(T.text);
    cv->setBrushColor(T.face);
    cv->drawText(&FONT_std_14, sx + 12, r.Y1 + 10, "Next");
    int c[4][2];
    cells(nextPiece, 0, c);
    for (auto & q : c) block(cv, sx + 18 + q[0] * CS, r.Y1 + 34 + q[1] * CS, nextPiece);
    char b[32];
    cv->setPenColor(T.text);
    cv->setBrushColor(T.face);
    snprintf(b, sizeof(b), "Best"); cv->drawText(&FONT_std_14, sx + 12, r.Y1 + 120, b);
    snprintf(b, sizeof(b), "%d", imax(score, bestScore("blocks"))); cv->drawText(&FONT_std_14, sx + 12, r.Y1 + 138, b);
  }
};

// ====================================================================== 2048

class Board2048 : public Board {
public:
  static const int N = 4, CS = 84;
  Board2048(uiWindow * p, Point pos) : Board(p, pos, Size(N * CS + 2, N * CS + 2)) { newGame(); }

  int g[N][N];
  int score = 0;
  bool over = false, won = false;

  void add() {
    int empty[16][2], n = 0;
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) if (!g[y][x]) { empty[n][0] = x; empty[n][1] = y; ++n; }
    if (!n) return;
    int k = esp_random() % n;
    g[empty[k][1]][empty[k][0]] = (esp_random() % 10) ? 2 : 4;
  }
  void report(const char * m) {
    if (!onScore) return;
    char b[96];
    snprintf(b, sizeof(b), "Score %d   Best %d   %s", score, imax(score, bestScore("2048")), m);
    onScore(score, b);
  }
  void newGame() override {
    memset(g, 0, sizeof(g));
    score = 0; over = won = false;
    add(); add();
    repaint();
    report("Use the arrow keys to slide the tiles.");
  }
  // slide one line toward index 0. returns true when something moved
  bool slide(int * v[N]) {
    int out[N] = { 0 }, n = 0, last = 0;
    bool moved = false;
    for (int i = 0; i < N; ++i) {
      if (!*v[i]) continue;
      if (last && last == *v[i]) { out[n - 1] *= 2; score += out[n - 1]; if (out[n - 1] == 2048 && !won) won = true; last = 0; }
      else { out[n++] = *v[i]; last = *v[i]; }
    }
    for (int i = 0; i < N; ++i) { if (*v[i] != out[i]) moved = true; *v[i] = out[i]; }
    return moved;
  }
  bool canMove() {
    for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
      if (!g[y][x]) return true;
      if (x + 1 < N && g[y][x] == g[y][x + 1]) return true;
      if (y + 1 < N && g[y][x] == g[y + 1][x]) return true;
    }
    return false;
  }
  void key(uiKeyEventInfo const & k) override {
    if (over) { if (k.VK == VK_SPACE || k.VK == VK_RETURN) newGame(); return; }
    int dir = -1;
    if (k.VK == VK_LEFT || k.VK == VK_a || k.VK == VK_A) dir = 0;
    else if (k.VK == VK_RIGHT || k.VK == VK_d || k.VK == VK_D) dir = 1;
    else if (k.VK == VK_UP || k.VK == VK_w || k.VK == VK_W) dir = 2;
    else if (k.VK == VK_DOWN || k.VK == VK_s || k.VK == VK_S) dir = 3;
    if (dir < 0) return;
    bool moved = false, wasWon = won;
    for (int i = 0; i < N; ++i) {
      int * line[N];
      for (int j = 0; j < N; ++j) {
        switch (dir) {
          case 0: line[j] = &g[i][j]; break;
          case 1: line[j] = &g[i][N - 1 - j]; break;
          case 2: line[j] = &g[j][i]; break;
          default: line[j] = &g[N - 1 - j][i]; break;
        }
      }
      if (slide(line)) moved = true;
    }
    if (!moved) return;
    add();
    repaint();
    if (score > bestScore("2048")) saveBest("2048", score);
    if (!canMove()) { over = true; report("No more moves. Space for a new game."); }
    else report(won && !wasWon ? "2048! You did it - keep going!" : "");
  }
  void paint(Rect const & clip) override {
    auto cv = canvas();
    Rect r = cr();
    cv->setBrushColor(T.shadow);
    cv->fillRectangle(clip.X1, clip.Y1, clip.X2, clip.Y2);
    for (int y = 0; y < N; ++y)
      for (int x = 0; x < N; ++x) {
        int px = r.X1 + x * CS + 4, py = r.Y1 + y * CS + 4, s = CS - 8;
        int v = g[y][x];
        RGB888 bg = !v ? T.face : v < 8 ? T.win : v < 64 ? T.good : v < 512 ? T.accent : T.warn;
        cv->setBrushColor(bg);
        cv->fillRectangle(px, py, px + s - 1, py + s - 1);
        if (!v) continue;
        char b[8];
        snprintf(b, sizeof(b), "%d", v);
        auto f = v < 1000 ? &FONT_std_24 : &FONT_std_18;
        int w = cv->textExtent(f, b);
        cv->setPenColor(v < 8 ? T.text : T.accentText);
        cv->setBrushColor(bg);
        cv->drawText(f, px + s / 2 - w / 2, py + s / 2 - f->height / 2, b);
      }
  }
};

// ====================================================================== Bricks (Arkanoid-style)

class BricksBoard : public Board {
public:
  static const int W = 448, H = 360, COLS = 14, ROWS = 8, BW = 32, BH = 14, TOP = 34, PY = H - 22, PH = 8;
  BricksBoard(uiWindow * p, Point pos) : Board(p, pos, Size(W + 2, H + 2)) { newGame(); }

  uint8_t br[ROWS][COLS];                 // hits left (0 = gone)
  float bx, by, vx, vy, speed, px, drawnPx = W / 2;   // drawnPx: where the paddle is on screen now
  int   drawnPw = 64;
  int   pw = 64, score = 0, lives = 3, level = 1, left = 0, held = 0;
  bool  stuck = true, over = false, paused = false;
  int   capX = -1, capY = 0, capT = 0;    // falling capsule (capX < 0 = none)

  void layout() {
    memset(br, 0, sizeof(br));
    left = 0;
    for (int r = 0; r < ROWS; ++r)
      for (int c = 0; c < COLS; ++c) {
        int v = 0;
        switch ((level - 1) % 4) {
          case 0: v = r == 0 ? 2 : (r < 6 ? 1 : 0); break;                                         // wall
          case 1: v = (abs(c * 2 - (COLS - 1)) <= r * 2 + 1) ? (r == 7 ? 2 : 1) : 0; break;        // pyramid
          case 2: v = ((r + c) % 2 == 0) ? ((r % 3 == 0) ? 2 : 1) : 0; break;                      // checks
          default: v = (esp_random() % 10 < 7) ? ((esp_random() % 4 == 0) ? 2 : 1) : 0; break;    // random
        }
        br[r][c] = v;
        if (v) ++left;
      }
  }
  void report(const char * m = "") {
    if (!onScore) return;
    char b[110];
    snprintf(b, sizeof(b), "Score %d   Lives %d   Level %d   Best %d   %s", score, lives, level, imax(score, bestScore("bricks")), m);
    onScore(score, b);
  }
  void serve() {
    stuck = true; pw = 64;
    speed = 3.6f + level * 0.35f;
    bx = px; by = PY - 4;
  }
  void newGame() override {
    score = 0; lives = 3; level = 1; over = paused = false; capX = -1; held = 0;
    px = W / 2;
    layout(); serve();
    startTimer(30);
    repaint();
    report("Mouse or arrows move, Space or click launches, P pauses.");
  }
  Rect ballR()   { return Rect((int)bx - 4, (int)by - 4, (int)bx + 4, (int)by + 4); }
  Rect padR()    { return Rect((int)px - pw / 2 - 1, PY, (int)px + pw / 2 + 1, PY + PH); }
  Rect brickR(int r, int c) { return Rect(c * BW, TOP + r * BH, c * BW + BW - 1, TOP + r * BH + BH - 1); }

  void hitBrick(int r, int c) {
    if (--br[r][c] == 0) {
      --left; score += 10 + level * 2;
      if (capX < 0 && esp_random() % 7 == 0) { capX = c * BW + BW / 2; capY = TOP + r * BH; capT = esp_random() % 3; }
    } else score += 2;
    dirty(brickR(r, c));
  }
  // does the ball (as a box) touch a brick? hit the first one found
  bool brickAt(float x, float y) {
    int x0 = ((int)x - 3) / BW, x1 = ((int)x + 3) / BW, y0 = ((int)y - 3 - TOP) / BH, y1 = ((int)y + 3 - TOP) / BH;
    if ((int)y + 3 < TOP) return false;
    for (int r = imax(0, y0); r <= imin(ROWS - 1, y1); ++r)
      for (int c = imax(0, x0); c <= imin(COLS - 1, x1); ++c)
        if (br[r][c]) { hitBrick(r, c); return true; }
    return false;
  }
  void lose() {
    if (--lives <= 0) {
      over = true; stopTimer();
      if (score > bestScore("bricks")) { saveBest("bricks", score); report("Game over - new high score! Space plays again."); }
      else report("Game over. Space plays again.");
      return;
    }
    capX = -1;
    serve();
    report("Ball lost - Space or click to launch.");
  }
  void tick() override {
    if (over || paused) return;
    Rect oldPad((int)drawnPx - drawnPw / 2 - 1, PY, (int)drawnPx + drawnPw / 2 + 1, PY + PH), oldBall = ballR();
    if (held) px += held * 9;
    px = fmaxf(pw / 2, fminf(W - pw / 2, px));
    if (stuck) { bx = px; by = PY - 4; }
    else {
      int steps = (int)(fmaxf(fabsf(vx), fabsf(vy)) / 3) + 1;
      for (int s = 0; s < steps; ++s) {
        float ox = bx, oy = by;
        bx += vx / steps;
        if (bx < 3) { bx = 3; vx = fabsf(vx); }
        if (bx > W - 4) { bx = W - 4; vx = -fabsf(vx); }
        if (brickAt(bx, by)) { bx = ox; vx = -vx; }
        by += vy / steps;
        if (by < 3) { by = 3; vy = fabsf(vy); }
        if (brickAt(bx, by)) { by = oy; vy = -vy; }
        // paddle
        if (vy > 0 && oy + 3 < PY && by + 3 >= PY && bx >= px - pw / 2 - 3 && bx <= px + pw / 2 + 3) {
          float rel = fmaxf(-1, fminf(1, (bx - px) / (pw / 2)));
          speed = fminf(7.5f, speed + 0.06f);
          float a = rel * 1.05f;                       // up to ~60 degrees
          vx = speed * sinf(a); vy = -speed * cosf(a);
          by = PY - 4;
        }
        if (by > H + 4) { dirty(oldBall); lose(); flushDirty(); return; }
      }
      if (left == 0) {
        ++level; score += 100;
        layout(); serve(); capX = -1;
        repaint();
        report("Level cleared! Space or click to launch.");
        return;
      }
    }
    // capsule
    if (capX >= 0) {
      Rect old(capX - 12, capY - 1, capX + 12, capY + 9);
      capY += 2;
      Rect now(capX - 12, capY - 1, capX + 12, capY + 9);
      dirty(old.merge(now));
      if (now.intersects(padR())) {
        if (capT == 0) pw = 96;
        else if (capT == 1) { speed = fmaxf(3.2f, speed - 1.6f); float l = sqrtf(vx * vx + vy * vy); if (l > 0) { vx = vx / l * speed; vy = vy / l * speed; } }
        else ++lives;
        score += 20;
        capX = -1;
        report(capT == 0 ? "Wide paddle!" : capT == 1 ? "Slower ball!" : "Extra life!");
      } else if (capY > H) capX = -1;
    }
    dirty(oldPad.merge(padR()));
    drawnPx = px; drawnPw = pw;
    dirty(oldBall.merge(ballR()));
    flushDirty();
    static int lastScore = -1;
    if (score != lastScore) { lastScore = score; report(); }
  }
  void launch() {
    if (over) { newGame(); return; }
    if (!stuck) return;
    stuck = false;
    float a = ((int)(esp_random() % 61) - 30) * 0.0174f;
    vx = speed * sinf(a); vy = -speed * cosf(a);
    report();
  }
  void key(uiKeyEventInfo const & k) override {
    if (k.VK == VK_SPACE || k.VK == VK_RETURN || k.VK == VK_UP) { launch(); return; }
    if (k.VK == VK_p || k.VK == VK_P) { if (!over) { paused = !paused; report(paused ? "Paused." : ""); } return; }
    if (k.VK == VK_LEFT) held = -1;
    if (k.VK == VK_RIGHT) held = 1;
  }
  void keyUp(uiKeyEventInfo const & k) override {
    if ((k.VK == VK_LEFT && held < 0) || (k.VK == VK_RIGHT && held > 0)) held = 0;
  }
  void mouseMove(int x, int) override { if (!paused && !over) px = x; }
  void click(int, int, int) override { launch(); }

  void paint(Rect const & clip) override {
    auto cv = canvas();
    Rect r = cr();
    cv->setBrushColor(T.win);
    cv->fillRectangle(clip.X1, clip.Y1, clip.X2, clip.Y2);
    for (int rr = 0; rr < ROWS; ++rr)
      for (int c = 0; c < COLS; ++c) {
        if (!br[rr][c]) continue;
        Rect b = brickR(rr, c);
        if (!hits(clip, b.X1, b.Y1, b.X2, b.Y2)) continue;
        int x = r.X1 + b.X1, y = r.Y1 + b.Y1;
        cv->setBrushColor(br[rr][c] == 2 ? T.shadow : (rr % 2 ? T.accent : T.face));
        cv->fillRectangle(x + 1, y + 1, x + BW - 2, y + BH - 2);
        cv->setPenColor(T.win);
        cv->drawLine(x + 1, y + 1, x + BW - 3, y + 1);
      }
    Rect p = padR();
    if (hits(clip, p.X1, p.Y1, p.X2, p.Y2)) {
      int x1 = r.X1 + (int)px - pw / 2, x2 = r.X1 + (int)px + pw / 2, y = r.Y1 + PY;
      cv->setBrushColor(T.accent);
      cv->fillRectangle(x1 + 6, y, x2 - 6, y + PH);
      cv->setBrushColor(T.shadow);
      cv->fillRectangle(x1, y, x1 + 7, y + PH);
      cv->fillRectangle(x2 - 7, y, x2, y + PH);
      cv->setPenColor(T.win);
      cv->drawLine(x1 + 8, y + 1, x2 - 8, y + 1);
    }
    Rect b = ballR();
    if (!over && hits(clip, b.X1, b.Y1, b.X2, b.Y2)) {
      cv->setBrushColor(T.text);
      cv->fillEllipse(r.X1 + (int)bx, r.Y1 + (int)by, 7, 7);
    }
    if (capX >= 0 && hits(clip, capX - 12, capY - 1, capX + 12, capY + 9)) {
      int x = r.X1 + capX, y = r.Y1 + capY;
      cv->setBrushColor(T.face);
      cv->fillRectangle(x - 11, y, x + 11, y + 8);
      cv->setPenColor(T.text);
      cv->drawRectangle(x - 11, y, x + 11, y + 8);
      cv->setBrushColor(T.face);
      cv->drawText(&FONT_std_12, x - 3, y - 2, capT == 0 ? "E" : capT == 1 ? "S" : "+");
    }
  }
};

// ====================================================================== Paradrop (Airborne!-style)
// Guard the gun: shoot the helicopters before their paratroopers land. Four troopers on one
// side of the gun and they storm it. From level 2, jets come over and drop bombs on the gun.

class ParadropBoard : public Board {
public:
  static const int W = 480, H = 340, GY = H - 12, GX = W / 2, PIVY = H - 36;
  ParadropBoard(uiWindow * p, Point pos) : Board(p, pos, Size(W + 2, H + 2)) { newGame(); }

  struct Heli  { float x; int y, dir; float sp; bool drop; };
  struct Para  { float x, y; int st; };      // st: 0 free fall, 1 chute, 2 landed, 3 shot-down chute
  struct Shell { float x, y, vx, vy; };
  struct Jet   { float x; int y, dir; bool bombed; };
  struct Bomb  { float x, y, vx, vy; };
  std::vector<Heli> helis; std::vector<Para> paras; std::vector<Shell> shells; std::vector<Jet> jets; std::vector<Bomb> bombs;
  float ang = 0;             // barrel angle, radians, 0 = straight up
  int   turn = 0, score = 0, level = 1, kills = 0, frame = 0, cool = 0, nextHeli = 0, nextJet = 0;
  bool  over = false, paused = false, firing = false;

  void report(const char * m = "") {
    if (!onScore) return;
    int l = 0, rr = 0;
    for (auto & p : paras) if (p.st == 2) (p.x < GX ? l : rr)++;
    char b[110];
    snprintf(b, sizeof(b), "Score %d   Level %d   Landed %d|%d   Best %d   %s", score, level, l, rr, imax(score, bestScore("paradrop")), m);
    onScore(score, b);
  }
  void newGame() override {
    helis.clear(); paras.clear(); shells.clear(); jets.clear(); bombs.clear();
    ang = 0; turn = 0; score = 0; level = 1; kills = 0; frame = 0; cool = 0; nextHeli = 20; nextJet = 400;
    over = paused = firing = false;
    startTimer(40);
    repaint();
    report("Left/Right aim, Space fires (or use the mouse). P pauses.");
  }
  void gameOver(const char * why) {
    over = true; stopTimer();
    char b[80];
    if (score > bestScore("paradrop")) { saveBest("paradrop", score); snprintf(b, sizeof(b), "%s New high score! Space plays again.", why); }
    else snprintf(b, sizeof(b), "%s Space plays again.", why);
    report(b);
    repaint();
  }
  // bounding boxes (board coords)
  static Rect heliR(Heli const & h)  { return Rect((int)h.x - 20, h.y - 9, (int)h.x + 20, h.y + 8); }
  static Rect paraR(Para const & p)  { return Rect((int)p.x - 9, (int)p.y - 25, (int)p.x + 9, (int)p.y + 1); }
  static Rect shellR(Shell const & s){ return Rect((int)s.x - 2, (int)s.y - 2, (int)s.x + 2, (int)s.y + 2); }
  static Rect jetR(Jet const & j)    { return Rect((int)j.x - 18, j.y - 6, (int)j.x + 18, j.y + 6); }
  static Rect bombR(Bomb const & b)  { return Rect((int)b.x - 3, (int)b.y - 3, (int)b.x + 3, (int)b.y + 4); }
  static Rect gunR()                 { return Rect(GX - 30, PIVY - 30, GX + 30, GY); }

  void fire() {
    if (over || paused || cool > 0 || shells.size() >= 6) return;
    float sx = sinf(ang), sy = -cosf(ang);
    shells.push_back({ GX + sx * 24, PIVY + sy * 24, sx * 9, sy * 9 });
    cool = 3;
  }
  void tick() override {
    if (over || paused) return;
    ++frame;
    if (cool) --cool;
    if (firing) fire();
    // gun
    if (turn) {
      float na = fmaxf(-1.4f, fminf(1.4f, ang + turn * 0.07f));
      if (na != ang) { ang = na; dirty(gunR()); }
    }
    // spawn helicopters
    if (--nextHeli <= 0) {
      int dir = esp_random() % 2 ? 1 : -1;
      helis.push_back({ dir > 0 ? -20.f : (float)W + 20, 24 + (int)(esp_random() % 110), dir, 1.6f + level * 0.35f + (esp_random() % 10) * 0.08f, false });
      nextHeli = imax(18, 70 - level * 7) + esp_random() % 30;
    }
    if (level >= 2 && --nextJet <= 0) {
      int dir = esp_random() % 2 ? 1 : -1;
      jets.push_back({ dir > 0 ? -20.f : (float)W + 20, 14, dir, false });
      nextJet = imax(120, 420 - level * 40) + esp_random() % 120;
    }
    // helicopters
    for (size_t i = 0; i < helis.size(); ) {
      Heli & h = helis[i];
      Rect o = heliR(h);
      h.x += h.dir * h.sp;
      dirty(o.merge(heliR(h)));
      if (!h.drop && fabsf(h.x - GX) > 44 && h.x > 20 && h.x < W - 20 && esp_random() % 1000 < (unsigned)(9 + level * 3)) {
        h.drop = esp_random() % 3 != 0;          // most drop one trooper, some keep going
        paras.push_back({ h.x, (float)h.y + 8, 0 });
      }
      if (h.x < -30 || h.x > W + 30) { helis.erase(helis.begin() + i); continue; }
      ++i;
    }
    // jets + bombs
    for (size_t i = 0; i < jets.size(); ) {
      Jet & j = jets[i];
      Rect o = jetR(j);
      j.x += j.dir * 7;
      dirty(o.merge(jetR(j)));
      if (!j.bombed && fabsf(j.x - GX) < 200 && fabsf(j.x - GX) > 120) {
        j.bombed = true;
        float t = (PIVY - j.y) / 3.0f;
        bombs.push_back({ j.x, (float)j.y + 6, (GX - j.x) / t, 3 });
      }
      if (j.x < -30 || j.x > W + 30) { jets.erase(jets.begin() + i); continue; }
      ++i;
    }
    for (size_t i = 0; i < bombs.size(); ) {
      Bomb & b = bombs[i];
      Rect o = bombR(b);
      b.x += b.vx; b.y += b.vy;
      dirty(o.merge(bombR(b)));
      if (bombR(b).intersects(Rect(GX - 16, PIVY - 10, GX + 16, GY))) { gameOver("A bomb hit your gun!"); return; }
      if (b.y > GY) { bombs.erase(bombs.begin() + i); continue; }
      ++i;
    }
    // paratroopers
    int landedL = 0, landedR = 0;
    for (size_t i = 0; i < paras.size(); ) {
      Para & p = paras[i];
      Rect o = paraR(p);
      if (p.st == 0) { p.y += 3.2f; if (p.y > 60 + (int)(((int)p.x * 7) % 50)) p.st = 1; }
      else if (p.st == 1) p.y += 0.9f + level * 0.1f;
      else if (p.st == 3) p.y += 4.5f;
      if (p.st != 2) {
        if (p.y >= GY) {
          p.y = GY;
          if (p.st == 3) {           // falls on anyone below: both gone
            for (size_t k = 0; k < paras.size(); ++k)
              if (k != i && paras[k].st == 2 && fabsf(paras[k].x - p.x) < 8) { dirty(paraR(paras[k])); paras[k].st = 4; score += 15; }
            dirty(o.merge(paraR(p)));
            p.st = 4;
          } else p.st = 2;
        }
        dirty(o.merge(paraR(p)));
      }
      if (p.st == 4) { paras.erase(paras.begin() + i); continue; }
      if (p.st == 2) (p.x < GX ? landedL : landedR)++;
      ++i;
    }
    for (size_t i = 0; i < paras.size(); ) { if (paras[i].st == 4) paras.erase(paras.begin() + i); else ++i; }
    // shells
    for (size_t i = 0; i < shells.size(); ) {
      Shell & s = shells[i];
      Rect o = shellR(s);
      s.x += s.vx; s.y += s.vy;
      dirty(o.merge(shellR(s)));
      Rect sr = shellR(s);
      bool gone = s.x < -4 || s.x > W + 4 || s.y < -4;
      for (size_t k = 0; !gone && k < helis.size(); ++k)
        if (sr.intersects(Rect((int)helis[k].x - 14, helis[k].y - 7, (int)helis[k].x + 14, helis[k].y + 6))) {
          dirty(heliR(helis[k])); helis.erase(helis.begin() + k); score += 10; gone = true;
          if (++kills % 15 == 0) { ++level; report("Next level!"); }
        }
      for (size_t k = 0; !gone && k < jets.size(); ++k)
        if (sr.intersects(jetR(jets[k]))) { dirty(jetR(jets[k])); jets.erase(jets.begin() + k); score += 25; gone = true; }
      for (size_t k = 0; !gone && k < bombs.size(); ++k)
        if (sr.intersects(bombR(bombs[k]))) { dirty(bombR(bombs[k])); bombs.erase(bombs.begin() + k); score += 20; gone = true; }
      for (size_t k = 0; !gone && k < paras.size(); ++k) {
        Para & p = paras[k];
        if (p.st == 2 || p.st == 4) continue;
        Rect body((int)p.x - 3, (int)p.y - 11, (int)p.x + 3, (int)p.y);
        Rect chute((int)p.x - 9, (int)p.y - 25, (int)p.x + 9, (int)p.y - 14);
        if (sr.intersects(body)) { dirty(paraR(p)); p.st = 4; score += 5; gone = true; }
        else if (p.st == 1 && sr.intersects(chute)) { dirty(paraR(p)); p.st = 3; score += 5; gone = true; }
      }
      if (gone) { shells.erase(shells.begin() + i); continue; }
      ++i;
    }
    for (size_t i = 0; i < paras.size(); ) { if (paras[i].st == 4) paras.erase(paras.begin() + i); else ++i; }
    flushDirty();
    static int lastScore = -1, lastLand = -1;
    if (landedL >= 4 || landedR >= 4) { gameOver("The paratroopers stormed your gun!"); return; }
    if (score != lastScore || landedL * 10 + landedR != lastLand) { lastScore = score; lastLand = landedL * 10 + landedR; report(); }
  }
  void aimAt(int x, int y) {
    float a = atan2f((float)(x - GX), (float)(PIVY - y));
    ang = fmaxf(-1.4f, fminf(1.4f, a));
    dirty(gunR());
  }
  void key(uiKeyEventInfo const & k) override {
    if (over) { if (k.VK == VK_SPACE || k.VK == VK_RETURN) newGame(); return; }
    if (k.VK == VK_p || k.VK == VK_P) { paused = !paused; report(paused ? "Paused." : ""); return; }
    if (k.VK == VK_LEFT) turn = -1;
    if (k.VK == VK_RIGHT) turn = 1;
    if (k.VK == VK_SPACE || k.VK == VK_UP || k.VK == VK_RETURN) { firing = true; fire(); }
  }
  void keyUp(uiKeyEventInfo const & k) override {
    if ((k.VK == VK_LEFT && turn < 0) || (k.VK == VK_RIGHT && turn > 0)) turn = 0;
    if (k.VK == VK_SPACE || k.VK == VK_UP || k.VK == VK_RETURN) firing = false;
  }
  void mouseMove(int x, int y) override { if (!over && !paused && y < PIVY) aimAt(x, y); }
  void click(int x, int y, int) override {
    if (over) { newGame(); return; }
    if (y < PIVY) aimAt(x, y);
    cool = 0; fire();
  }

  void paint(Rect const & clip) override {
    auto cv = canvas();
    Rect r = cr();
    int ox = r.X1, oy = r.Y1;
    cv->setBrushColor(T.win);
    cv->fillRectangle(clip.X1, clip.Y1, clip.X2, clip.Y2);
    // ground
    if (hits(clip, 0, GY + 1, W - 1, H - 1)) { cv->setBrushColor(T.face); cv->fillRectangle(ox, oy + GY + 1, ox + W - 1, oy + H - 1); }
    // helicopters
    for (auto & h : helis) {
      Rect b = heliR(h); if (!hits(clip, b.X1, b.Y1, b.X2, b.Y2)) continue;
      int x = ox + (int)h.x, y = oy + h.y, d = h.dir;
      cv->setBrushColor(T.text);
      cv->fillEllipse(x, y, 22, 11);
      cv->fillRectangle(d > 0 ? x - 19 : x + 8, y - 2, d > 0 ? x - 8 : x + 19, y);
      cv->setPenColor(T.text);
      cv->drawLine(x - d * 19, y - 5, x - d * 19, y + 1);           // tail rotor
      int rl = (frame & 1) ? 18 : 9;
      cv->drawLine(x - rl, y - 8, x + rl, y - 8);                    // main rotor
      cv->drawLine(x, y - 8, x, y - 5);
      cv->drawLine(x - 8, y + 7, x + 8, y + 7);                      // skids
      cv->setBrushColor(T.win);
      cv->fillRectangle(x + d * 3, y - 3, x + d * 7, y);             // window
    }
    // jets
    for (auto & j : jets) {
      Rect b = jetR(j); if (!hits(clip, b.X1, b.Y1, b.X2, b.Y2)) continue;
      int x = ox + (int)j.x, y = oy + j.y, d = j.dir;
      cv->setBrushColor(T.accent);
      cv->fillRectangle(x - 16, y - 2, x + 16, y + 2);
      cv->setPenColor(T.accent);
      for (int k = 0; k < 6; ++k) cv->drawLine(x - d * 2, y, x - d * (8 + k), y + 6 - k);   // wing
      cv->drawLine(x - d * 16, y, x - d * 16, y - 6);
    }
    // bombs
    for (auto & b : bombs) {
      Rect q = bombR(b); if (!hits(clip, q.X1, q.Y1, q.X2, q.Y2)) continue;
      cv->setBrushColor(T.text);
      cv->fillEllipse(ox + (int)b.x, oy + (int)b.y, 6, 8);
    }
    // paratroopers
    for (auto & p : paras) {
      Rect q = paraR(p); if (!hits(clip, q.X1, q.Y1, q.X2, q.Y2)) continue;
      int x = ox + (int)p.x, y = oy + (int)p.y;
      cv->setPenColor(T.text);
      cv->setBrushColor(T.text);
      cv->fillRectangle(x - 1, y - 10, x + 1, y - 8);               // head
      cv->drawLine(x, y - 8, x, y - 3);                              // body
      cv->drawLine(x - 3, y - 7, x + 3, y - 7);                      // arms
      cv->drawLine(x, y - 3, x - 2, y);                              // legs
      cv->drawLine(x, y - 3, x + 2, y);
      if (p.st == 1) {
        cv->setBrushColor(T.accent);
        cv->fillEllipse(x, y - 19, 18, 11);
        cv->setBrushColor(T.win);
        cv->fillRectangle(x - 9, y - 18, x + 9, y - 14);
        cv->setPenColor(T.text);
        cv->drawLine(x - 8, y - 18, x - 1, y - 9);
        cv->drawLine(x + 8, y - 18, x + 1, y - 9);
      }
    }
    // shells
    for (auto & s : shells) {
      Rect q = shellR(s); if (!hits(clip, q.X1, q.Y1, q.X2, q.Y2)) continue;
      cv->setBrushColor(T.text);
      cv->fillRectangle(ox + (int)s.x - 1, oy + (int)s.y - 1, ox + (int)s.x + 1, oy + (int)s.y + 1);
    }
    // the gun
    Rect g = gunR();
    if (hits(clip, g.X1, g.Y1, g.X2, g.Y2)) {
      int x = ox + GX, y = oy + PIVY;
      cv->setPenColor(T.text);
      cv->setPenWidth(4);
      cv->drawLine(x, y, x + (int)(sinf(ang) * 24), y - (int)(cosf(ang) * 24));
      cv->setPenWidth(1);
      cv->setBrushColor(over ? T.warn : T.accent);
      cv->fillEllipse(x, y + 2, 22, 18);
      cv->setBrushColor(T.text);
      cv->fillRectangle(x - 18, y + 8, x + 18, oy + GY);
      cv->setBrushColor(T.face);
      cv->fillRectangle(x - 14, y + 12, x + 14, oy + GY - 3);
    }
  }
};

// ====================================================================== game windows

class GameFrame : public uiFrame {
public:
  static GameFrame * open[6];
  int idx;
  Board * board;
  uiLabel * info;

  GameFrame(int i) : uiFrame(gApp->rootWindow(), "", Point(0, 0), Size(100, 100), false), idx(i) {
    applyStyle(this);
    frameProps().hasMaximizeButton = false;
    frameProps().hasMinimizeButton = false;
    frameProps().resizeable = false;
    static const char * titles[6] = { "Sammy the Snake", "Minesweeper", "Blocks", "2048", "Bricks", "Paradrop" };
    setTitle(titles[i]);
    Point o = clientOrigin(this);
    int x = o.X + 6, y = o.Y + 6;
    auto nb = S(new uiButton(this, "New game", Point(x, y), Size(90, 22)));
    nb->setFocusIndex(-1);
    info = S(new uiLabel(this, "", Point(x + 98, y + 3), Size(400, 18)));
    info->labelStyle().textFont = &FONT_std_12;
    y += 30;
    switch (i) {
      case 0:  board = new SnakeBoard(this, Point(x, y)); break;
      case 1:  board = new MinesBoard(this, Point(x, y)); break;
      case 2:  board = new BlocksBoard(this, Point(x, y)); break;
      case 4:  board = new BricksBoard(this, Point(x, y)); break;
      case 5:  board = new ParadropBoard(this, Point(x, y)); break;
      default: board = new Board2048(this, Point(x, y)); break;
    }
    Size bs = board->size();
    int w = imax(bs.width + 12 + o.X * 2, 300), h = bs.height + y + 8;
    if (i == 0 || i == 1 || i >= 4) w = imax(w, 520);
    info->labelStyle().textAlign = uiHAlign::Left;
    gApp->resizeWindow(info, Size(w - 120, 18));
    gApp->reshapeWindow(this, Rect((SCREEN_W - w) / 2, imax(0, (SCREEN_H - 22 - h) / 2), (SCREEN_W - w) / 2 + w - 1, imax(0, (SCREEN_H - 22 - h) / 2) + h - 1));
    board->onScore = [this](int, const char * m) {
      if (m && *m) { info->setText(m); info->repaint(); }
      else if (idx == 0) {
        char b[64];
        auto sb = (SnakeBoard*)board;
        int best = bestScore("snake");
        if (sb->score > best) saveBest("snake", sb->score);
        snprintf(b, sizeof(b), "Score %d   Best %d", sb->score, imax(best, sb->score));
        info->setText(b); info->repaint();
      }
    };
    nb->onClick = [this]() { board->newGame(); gApp->setFocusedWindow(board); };
    board->newGame();
  }

  void processEvent(uiEvent * e) override {
    if (e->id == UIEVT_CLOSE) {
      board->stopTimer();
      open[idx] = nullptr;
      gApp->destroyWindow(this);
      return;
    }
    // keys meant for the game: if the board lost focus (e.g. to the launcher's button),
    // the frame still gets a copy as the active window - pass it on and take focus back
    if ((e->id == UIEVT_KEYDOWN || e->id == UIEVT_KEYUP) && gApp->focusedWindow() != board) {
      board->keyIn(e->params.key, e->id == UIEVT_KEYDOWN);
      gApp->setFocusedWindow(board);
      return;
    }
    uiFrame::processEvent(e);
    if (e->id == UIEVT_ACTIVATE) gApp->setFocusedWindow(board);
  }
};
GameFrame * GameFrame::open[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };

static void openGame(int i)
{
  if (!GameFrame::open[i]) GameFrame::open[i] = new GameFrame(i);
  bringToFront(GameFrame::open[i]);
  gApp->setFocusedWindow(GameFrame::open[i]->board);
}

// ====================================================================== Games launcher

class GamesFrame : public uiFrame {
public:
  static GamesFrame * inst;
  GamesFrame() : uiFrame(gApp->rootWindow(), "Games", Point(200, 40), Size(300, 380), false) {
    applyStyle(this);
    frameProps().hasMaximizeButton = false;
    frameProps().hasMinimizeButton = false;
    frameProps().resizeable = false;
    Point o = clientOrigin(this);
    static const char * names[6] = { "Sammy the Snake", "Minesweeper", "Blocks", "2048", "Bricks", "Paradrop" };
    static const char * about[6] = { "Eat apples, don't bite your tail", "Find the 40 mines", "Stack and clear lines", "Slide tiles to make 2048",
                                     "Arkanoid-style: break every brick", "Airborne!-style: guard the gun" };
    for (int i = 0; i < 6; ++i) {
      auto b = S(new uiButton(this, names[i], Point(o.X + 10, o.Y + 10 + i * 56), Size(270, 30)));
      b->buttonStyle().textFont = &FONT_std_16;
      b->onClick = [i]() {
        openGame(i);
        // the click is still finishing on this launcher; focus the game once it has
        uiEvent evt(GameFrame::open[i], UIEVT_ACTIVATE);
        gApp->postEvent(&evt);
      };
      auto l = S(new uiLabel(this, about[i], Point(o.X + 14, o.Y + 42 + i * 56), Size(266, 14)));
      l->labelStyle().textFont = &FONT_std_12;
    }
  }
  void processEvent(uiEvent * e) override {
    if (e->id == UIEVT_CLOSE) { inst = nullptr; gApp->destroyWindow(this); return; }
    uiFrame::processEvent(e);
  }
};
GamesFrame * GamesFrame::inst = nullptr;

void openGames()
{
  if (!GamesFrame::inst) GamesFrame::inst = new GamesFrame();
  bringToFront(GamesFrame::inst);
}
