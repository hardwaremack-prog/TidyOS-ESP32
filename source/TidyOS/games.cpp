// TidyOS games: Sammy the Snake, Minesweeper, Blocks, 2048
// Small and light: each board only repaints the squares that change.
#include <Preferences.h>
#include <deque>
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
        key(e->params.key);
        break;
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
  virtual void tick() {}
  virtual void newGame() = 0;

  std::function<void(int, const char *)> onScore;     // score, message ("" = still playing)

  void startTimer(int ms) { stopTimer(); m_timer = app()->setTimer(this, ms); }
  void stopTimer() { if (m_timer) { app()->killTimer(m_timer); m_timer = nullptr; } }

protected:
  uiTimerHandle m_timer = nullptr;
  Rect cr() { return clientRect(uiOrigin::Window); }
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

// ====================================================================== game windows

class GameFrame : public uiFrame {
public:
  static GameFrame * open[4];
  int idx;
  Board * board;
  uiLabel * info;

  GameFrame(int i) : uiFrame(gApp->rootWindow(), "", Point(0, 0), Size(100, 100), false), idx(i) {
    applyStyle(this);
    frameProps().hasMaximizeButton = false;
    frameProps().hasMinimizeButton = false;
    frameProps().resizeable = false;
    static const char * titles[4] = { "Sammy the Snake", "Minesweeper", "Blocks", "2048" };
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
      default: board = new Board2048(this, Point(x, y)); break;
    }
    Size bs = board->size();
    int w = imax(bs.width + 12 + o.X * 2, 300), h = bs.height + y + 8;
    if (i == 0 || i == 1) w = imax(w, 520);
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
    uiFrame::processEvent(e);
    if (e->id == UIEVT_ACTIVATE) gApp->setFocusedWindow(board);
  }
};
GameFrame * GameFrame::open[4] = { nullptr, nullptr, nullptr, nullptr };

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
  GamesFrame() : uiFrame(gApp->rootWindow(), "Games", Point(200, 90), Size(300, 270), false) {
    applyStyle(this);
    frameProps().hasMaximizeButton = false;
    frameProps().hasMinimizeButton = false;
    frameProps().resizeable = false;
    Point o = clientOrigin(this);
    static const char * names[4] = { "Sammy the Snake", "Minesweeper", "Blocks", "2048" };
    static const char * about[4] = { "Eat apples, don't bite your tail", "Find the 40 mines", "Stack and clear lines", "Slide tiles to make 2048" };
    for (int i = 0; i < 4; ++i) {
      auto b = S(new uiButton(this, names[i], Point(o.X + 10, o.Y + 10 + i * 56), Size(270, 30)));
      b->buttonStyle().textFont = &FONT_std_16;
      b->onClick = [i]() { openGame(i); };
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
