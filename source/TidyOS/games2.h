// TidyOS games, part 2: Deep Drill, Missile Command, StuntCopter (included in games.cpp)

// ====================================================================== Deep Drill
// Dig down, haul ore back up, sell it at the surface (which also refuels and repairs), buy upgrades.

class DrillBoard : public Board {
public:
  static const int COLS = 13, ROWS = 160, BS = 24, VIS = 15, W = 520, H = VIS * BS, GW = COLS * BS;
  enum { E = 0, DIRT, STONE, ROCK, LAVA, ORE0 };
  struct Ore { const char * name; char ch; int value, hard, minDepth, rarity; };
  struct Up { const char * name; int base; float mult; };

  uint8_t type[ROWS][COLS];
  uint8_t hp[ROWS][COLS];
  int pr = 0, pc = COLS / 2, cash, fuel, maxFuel, hull, maxHull, maxCargo, power, cool, deepest;
  int cargo[9];
  int lv[5];
  int held = 0, heldR = 0, heldC = 0;   // a held arrow key keeps digging
  uint32_t lastMove = 0;
  char logMsg[64] = "";

  static const Ore * ores() {
    static const Ore O[9] = {
      { "copper", 'C', 6, 3, 3, 40 },    { "iron", 'I', 14, 5, 8, 32 },     { "silver", 'S', 28, 8, 16, 22 },
      { "gold", 'G', 55, 12, 26, 16 },   { "ruby", 'R', 95, 16, 38, 11 },   { "emerald", 'E', 150, 20, 52, 8 },
      { "diamond", 'D', 240, 26, 70, 5 },{ "mythril", 'M', 420, 34, 95, 3 }, { "voidstone", 'V', 750, 44, 130, 2 } };
    return O;
  }
  static const Up * ups() {
    static const Up U[5] = { { "Drill Power", 22, 1.55f }, { "Fuel Tank", 16, 1.45f }, { "Hull Plating", 16, 1.45f },
                             { "Cargo Bay", 26, 1.5f }, { "Drill Motor", 30, 1.55f } };
    return U;
  }

  DrillBoard(uiWindow * p, Point pos) : Board(p, pos, Size(W + 2, H + 2)) { newGame(); }

  static int fillerHard(int depth) { return 1 + depth / 14; }
  int maxHp(int r, int c) {
    int t = type[r][c];
    if (t == LAVA) return 6;
    if (t >= ORE0) return ores()[t - ORE0].hard;
    return fillerHard(r);
  }
  void gen() {
    for (int r = 0; r < ROWS; ++r)
      for (int c = 0; c < COLS; ++c) {
        uint8_t t;
        if (c == 0 || c == COLS - 1 || r == ROWS - 1) t = ROCK;
        else if (r == 0) t = E;
        else if (r >= 35 && esp_random() % 1000 < 25) t = LAVA;
        else {
          int total = 110;
          for (int i = 0; i < 9; ++i) if (r >= ores()[i].minDepth) total += ores()[i].rarity;
          int x = esp_random() % total;
          t = r < 7 ? DIRT : STONE;
          if (x >= 110) {
            x -= 110;
            for (int i = 0; i < 9; ++i) if (r >= ores()[i].minDepth) { if (x < ores()[i].rarity) { t = ORE0 + i; break; } x -= ores()[i].rarity; }
          }
        }
        type[r][c] = t;
        hp[r][c] = 0;
        hp[r][c] = imin(255, maxHp(r, c));
      }
  }
  void newGame() override {
    gen();
    pr = 0; pc = COLS / 2;
    cash = 0; fuel = maxFuel = 100; hull = maxHull = 100; maxCargo = 20; power = 3; cool = 180;
    deepest = 0;
    memset(cargo, 0, sizeof(cargo)); memset(lv, 0, sizeof(lv));
    strcpy(logMsg, "Dig down, bring ore up to sell.");
    startTimer(40);
    report(); repaint();
  }
  int cargoUsed() { int n = 0; for (int i = 0; i < 9; ++i) n += cargo[i]; return n; }
  int cost(int i) { return (int)roundf(ups()[i].base * powf(ups()[i].mult, lv[i])); }
  void report() {
    char b[96];
    snprintf(b, sizeof(b), "$%d   Depth %d m   Best %d m   1-5 buy upgrades", cash, pr, imax(deepest, bestScore("drill")));
    if (onScore) onScore(cash, b);
  }
  void say(const char * m) { strlcpy(logMsg, m, sizeof(logMsg)); }
  void useFuel(int n) {
    if (fuel > 0) fuel = imax(0, fuel - n);
    else hull -= n;                                      // out of fuel: the hull pays
    checkOver();
  }
  void checkOver() {
    if (hull > 0) return;
    say("Rig wrecked! Evacuated: lost half your cash and cargo.");
    cash /= 2; memset(cargo, 0, sizeof(cargo));
    pr = 0; pc = COLS / 2; fuel = maxFuel; hull = maxHull;
  }
  void sell() {
    int total = 0;
    for (int i = 0; i < 9; ++i) { total += cargo[i] * ores()[i].value; cargo[i] = 0; }
    fuel = maxFuel; hull = maxHull;
    char b[64];
    if (total) { cash += total; snprintf(b, sizeof(b), "Sold cargo for $%d. Refuelled and repaired.", total); }
    else snprintf(b, sizeof(b), "Refuelled and repaired.");
    say(b);
  }
  void move(int dr, int dc) {
    if (millis() - lastMove < (uint32_t)cool) return;
    int nr = pr + dr, nc = pc + dc;
    if (nr < 0 || nr >= ROWS || nc < 0 || nc >= COLS) return;
    int t = type[nr][nc];
    if (t == ROCK) return;
    lastMove = millis();
    if (t == E) {
      pr = nr; pc = nc;
      useFuel(1);
      if (pr > deepest) { deepest = pr; if (deepest > bestScore("drill")) saveBest("drill", deepest); }
      if (pr == 0 && (cargoUsed() || fuel < maxFuel || hull < maxHull)) sell();
    } else {
      useFuel(2);
      if (hp[nr][nc] > power) hp[nr][nc] -= power;
      else {
        if (t == LAVA) { hull -= 22; say("Drilled into lava - hull damaged!"); checkOver(); }
        else if (t >= ORE0) {
          auto & o = ores()[t - ORE0];
          char b[64];
          if (cargoUsed() < maxCargo) { cargo[t - ORE0]++; snprintf(b, sizeof(b), "Found %s (worth $%d).", o.name, o.value); }
          else snprintf(b, sizeof(b), "Cargo full! Go up and sell.");
          say(b);
        }
        type[nr][nc] = E; hp[nr][nc] = 0;
      }
    }
    report();
    repaint();
  }
  void buy(int i) {
    int c = cost(i);
    char b[64];
    if (cash < c) { snprintf(b, sizeof(b), "%s costs $%d.", ups()[i].name, c); say(b); repaint(); return; }
    cash -= c; lv[i]++;
    switch (i) {
      case 0: power += 1; break;
      case 1: maxFuel += 25; fuel = maxFuel; break;
      case 2: maxHull += 25; hull = maxHull; break;
      case 3: maxCargo += 6; break;
      case 4: cool = imax(60, cool - 15); break;
    }
    snprintf(b, sizeof(b), "Upgraded %s to level %d.", ups()[i].name, lv[i]);
    say(b); report(); repaint();
  }
  void key(uiKeyEventInfo const & k) override {
    int dr = 0, dc = 0;
    if (k.VK == VK_UP) dr = -1; else if (k.VK == VK_DOWN) dr = 1; else if (k.VK == VK_LEFT) dc = -1; else if (k.VK == VK_RIGHT) dc = 1;
    else if (k.ASCII >= '1' && k.ASCII <= '5') { buy(k.ASCII - '1'); return; }
    else if (k.VK == VK_n || k.VK == VK_N) { newGame(); return; }
    if (dr || dc) { held = 1; heldR = dr; heldC = dc; move(dr, dc); }
  }
  void keyUp(uiKeyEventInfo const & k) override {
    if (k.VK == VK_UP || k.VK == VK_DOWN || k.VK == VK_LEFT || k.VK == VK_RIGHT) held = 0;
  }
  void tick() override { if (held) move(heldR, heldC); }
  void click(int x, int y, int) override {
    if (x >= GW + 8 && y >= 176 && y < 176 + 5 * 22) { buy((y - 176) / 22); return; }
    if (x < GW) {                                           // click next to the rig to dig that way
      int top = imax(0, imin(ROWS - VIS, pr - VIS / 2));
      int r = y / BS + top, c = x / BS;
      if (r == pr && c != pc) move(0, c < pc ? -1 : 1);
      else if (c == pc && r != pr) move(r < pr ? -1 : 1, 0);
    }
  }

  void bar(Canvas * cv, int x, int y, int w, int v, int mx) {
    cv->setPenColor(T.text);
    cv->drawRectangle(x, y, x + w, y + 9);
    cv->setBrushColor(T.win);
    cv->fillRectangle(x + 1, y + 1, x + w - 1, y + 8);
    int f = mx > 0 ? (w - 2) * imax(0, v) / mx : 0;
    if (f > 0) { cv->setBrushColor(T.accent); cv->fillRectangle(x + 1, y + 1, x + f, y + 8); }
  }
  void paint(Rect const & clip) override {
    auto cv = canvas();
    Rect r = cr();
    int ox = r.X1, oy = r.Y1;
    int top = imax(0, imin(ROWS - VIS, pr - VIS / 2));
    char b[64];
    for (int i = 0; i < VIS; ++i) {
      int row = top + i, y = oy + i * BS;
      for (int c = 0; c < COLS; ++c) {
        int x = ox + c * BS, t = type[row][c];
        if (row == 0) {                                         // the surface: sky with a ground line
          cv->setBrushColor(T.win); cv->fillRectangle(x, y, x + BS - 1, y + BS - 1);
          cv->setBrushColor(T.face); cv->fillRectangle(x, y + BS - 4, x + BS - 1, y + BS - 1);
          continue;
        }
        switch (t) {
          case E:    cv->setBrushColor(T.text); cv->fillRectangle(x, y, x + BS - 1, y + BS - 1); break;
          case ROCK:
            cv->setBrushColor(T.text); cv->fillRectangle(x, y, x + BS - 1, y + BS - 1);
            cv->setPenColor(T.face); cv->drawLine(x + 4, y + 4, x + BS - 5, y + BS - 5); cv->drawLine(x + BS - 5, y + 4, x + 4, y + BS - 5);
            break;
          case DIRT:
            cv->setBrushColor(T.face); cv->fillRectangle(x, y, x + BS - 1, y + BS - 1);
            cv->setPenColor(T.text);
            for (int d = 0; d < 4; ++d) cv->setPixel(x + 4 + (d * 7 + c * 3) % 16, y + 5 + (d * 5 + row) % 14);
            break;
          case STONE:
            cv->setBrushColor(T.face); cv->fillRectangle(x, y, x + BS - 1, y + BS - 1);
            cv->setPenColor(T.text);
            cv->drawLine(x, y + BS / 2, x + BS - 1, y + BS / 2);
            cv->drawLine(x + BS / 2, y, x + BS / 2, y + BS / 2);
            cv->drawLine(x + 4, y + BS / 2, x + 4, y + BS - 1);
            break;
          case LAVA:
            cv->setBrushColor(T.accent); cv->fillRectangle(x, y, x + BS - 1, y + BS - 1);
            cv->setPenColor(T.win);
            for (int k = 0; k < 3; ++k) cv->drawLine(x + 3 + k * 6, y + 10, x + 6 + k * 6, y + 14);
            break;
          default: {                                            // ore: a white tile with its letter
            cv->setBrushColor(T.win); cv->fillRectangle(x + 1, y + 1, x + BS - 2, y + BS - 2);
            cv->setPenColor(T.text); cv->drawRectangle(x + 1, y + 1, x + BS - 2, y + BS - 2);
            char s[2] = { ores()[t - ORE0].ch, 0 };
            cv->setBrushColor(T.win);
            cv->drawText(&FONT_std_14, x + 8, y + 4, s);
          }
        }
        if (t != E && t != ROCK && hp[row][c] < maxHp(row, c)) {         // cracks
          cv->setPenColor(T.text);
          cv->drawLine(x + 3, y + 3, x + BS / 2, y + BS / 2);
          cv->drawLine(x + BS - 4, y + 5, x + BS / 2, y + BS / 2);
        }
      }
    }
    // the rig
    {
      int x = ox + pc * BS, y = oy + (pr - top) * BS;
      cv->setBrushColor(T.accent); cv->fillRectangle(x + 3, y + 4, x + BS - 4, y + 15);
      cv->setPenColor(T.win); cv->drawRectangle(x + 3, y + 4, x + BS - 4, y + 15);
      cv->setBrushColor(T.win); cv->fillRectangle(x + 7, y + 7, x + 12, y + 10);
      Point tip[3] = { Point(x + 6, y + 16), Point(x + BS - 7, y + 16), Point(x + BS / 2, y + BS - 1) };
      cv->setBrushColor(T.face); cv->fillPath(tip, 3);
    }
    // side panel
    int px = ox + GW + 1;
    cv->setBrushColor(T.win); cv->fillRectangle(px, oy, ox + W - 1, oy + H - 1);
    cv->setPenColor(T.shadow); cv->drawLine(px, oy, px, oy + H - 1);
    int tx = px + 8, y = oy + 6;
    cv->setPenColor(T.text); cv->setBrushColor(T.win);
    snprintf(b, sizeof(b), "$%d", cash); cv->drawText(&FONT_std_18, tx, y, b); y += 26;
    snprintf(b, sizeof(b), "Depth %d m  (best %d)", pr, imax(deepest, bestScore("drill"))); cv->drawText(&FONT_std_12, tx, y, b); y += 18;
    snprintf(b, sizeof(b), "Fuel %d / %d", fuel, maxFuel); cv->drawText(&FONT_std_12, tx, y, b); y += 14; bar(cv, tx, y, 180, fuel, maxFuel); y += 16;
    snprintf(b, sizeof(b), "Hull %d / %d", imax(0, hull), maxHull); cv->drawText(&FONT_std_12, tx, y, b); y += 14; bar(cv, tx, y, 180, hull, maxHull); y += 16;
    snprintf(b, sizeof(b), "Cargo %d / %d", cargoUsed(), maxCargo); cv->drawText(&FONT_std_12, tx, y, b); y += 14; bar(cv, tx, y, 180, cargoUsed(), maxCargo); y += 20;
    cv->drawText(&FONT_std_14, tx, y, "Upgrades (keys 1-5)");
    y = oy + 176;
    for (int i = 0; i < 5; ++i) {
      bool can = cash >= cost(i);
      cv->setBrushColor(can ? T.face : T.win);
      cv->fillRectangle(tx - 2, y, tx + 182, y + 19);
      cv->setPenColor(T.shadow); cv->drawRectangle(tx - 2, y, tx + 182, y + 19);
      cv->setPenColor(T.text);
      snprintf(b, sizeof(b), "%d %s  Lv%d  $%d", i + 1, ups()[i].name, lv[i], cost(i));
      cv->drawText(&FONT_std_12, tx + 2, y + 3, b);
      y += 22;
    }
    y += 6;
    cv->setBrushColor(T.win);
    // the log line, wrapped onto two lines if needed
    std::string m = logMsg;
    size_t cut = m.size() > 30 ? m.rfind(' ', 30) : std::string::npos;
    cv->drawText(&FONT_std_12, tx, y, (cut == std::string::npos ? m : m.substr(0, cut)).c_str());
    if (cut != std::string::npos) cv->drawText(&FONT_std_12, tx, y + 14, m.substr(cut + 1).c_str());
    y += 34;
    static const char * L[5] = { "C copper  I iron  S silver", "G gold  R ruby  E emerald", "D diamond  M mythril", "V voidstone", "Surface = sell, refuel, repair" };
    for (int i = 0; i < 5; ++i) { cv->drawText(&FONT_std_12, tx, y, L[i]); y += 13; }
  }
};

// ====================================================================== Missile Command
// Click the sky to launch an interceptor from the nearest base; its blast destroys anything flying through it.

class MissileBoard : public Board {
public:
  static const int W = 520, H = 360, GY = 330;
  struct Enemy { float x0, y0, x, y, vx, vy, tx, ty, ph; int kind; bool live; };   // kind 0 missile, 1 junk, 2 ICBM
  struct Shot { float x0, y0, x, y, vx, vy, tx, ty; bool live; };
  struct Boom { float x, y, r, maxR; bool grow, enemy, live; };
  std::vector<Enemy> en;
  std::vector<Shot> shots;
  std::vector<Boom> booms;
  bool city[6]; int ammo[3]; bool baseUp[3];
  int score, wave, toSpawn, spawnIn, pause, frame;
  bool over, icbmDue, icbmWarn;
  int aimX = W / 2, aimY = H / 2;
  int heldX = 0, heldY = 0;
  static int cityX(int i) { static const int X[6] = { 100, 150, 200, 320, 370, 420 }; return X[i]; }
  static int baseX(int i) { static const int X[3] = { 40, 260, 480 }; return X[i]; }

  MissileBoard(uiWindow * p, Point pos) : Board(p, pos, Size(W + 2, H + 2)) { newGame(); }

  void newGame() override {
    for (auto & c : city) c = true;
    score = 0; wave = 0; over = false; frame = 0;
    en.clear(); shots.clear(); booms.clear();
    startWave();
    startTimer(40);
    report(""); repaint();
  }
  void startWave() {
    ++wave;
    for (int i = 0; i < 3; ++i) { ammo[i] = 10; baseUp[i] = true; }
    toSpawn = 8 + wave * 2; spawnIn = 30; pause = 0;
    icbmDue = wave >= 2; icbmWarn = false;
  }
  int citiesLeft() { int n = 0; for (auto c : city) n += c; return n; }
  void report(const char * m) {
    char b[96];
    if (m && *m) snprintf(b, sizeof(b), "%s", m);
    else snprintf(b, sizeof(b), "Score %d   Wave %d   Cities %d   Best %d", score, wave, citiesLeft(), imax(score, bestScore("missile")));
    if (onScore) onScore(score, b);
  }
  void spawn(int kind) {
    Enemy e;
    e.kind = kind; e.live = true; e.ph = (esp_random() % 628) / 100.0f;
    e.x0 = e.x = 10 + esp_random() % (W - 20); e.y0 = e.y = 0;
    // aim at a city or base that is still standing (or any city)
    int targets[9], n = 0;
    for (int i = 0; i < 6; ++i) if (city[i]) targets[n++] = cityX(i);
    for (int i = 0; i < 3; ++i) if (baseUp[i]) targets[n++] = baseX(i);
    e.tx = n ? targets[esp_random() % n] : cityX(esp_random() % 6);
    e.ty = GY - 4;
    float sp = (kind == 2 ? 2.2f : 0.55f) + wave * (kind == 2 ? 0.15f : 0.1f);
    float dx = e.tx - e.x, dy = e.ty - e.y, d = sqrtf(dx * dx + dy * dy);
    e.vx = dx / d * sp; e.vy = dy / d * sp;
    en.push_back(e);
  }
  void blast(float x, float y, float maxR, bool enemy) { Boom b = { x, y, 2, maxR, true, enemy, true }; booms.push_back(b); }
  void fire(int tx, int ty) {
    if (over) { newGame(); return; }
    if (ty > GY - 24) ty = GY - 24;
    int best = -1, bd = 9999;
    for (int i = 0; i < 3; ++i) if (baseUp[i] && ammo[i] > 0 && abs(baseX(i) - tx) < bd) { bd = abs(baseX(i) - tx); best = i; }
    if (best < 0) return;
    ammo[best]--;
    Shot s; s.x0 = s.x = baseX(best); s.y0 = s.y = GY - 10; s.tx = tx; s.ty = ty; s.live = true;
    float dx = tx - s.x, dy = ty - s.y, d = fmaxf(1, sqrtf(dx * dx + dy * dy));
    s.vx = dx / d * 9; s.vy = dy / d * 9;
    shots.push_back(s);
  }
  void tick() override {
    if (over) return;
    ++frame;
    if (heldX || heldY) { aimX = imax(4, imin(W - 4, aimX + heldX * 8)); aimY = imax(4, imin(GY - 24, aimY + heldY * 8)); }
    if (pause > 0) {                                    // between waves
      if (--pause == 0) { startWave(); report(""); }
      repaint();
      return;
    }
    if (toSpawn > 0 && --spawnIn <= 0) {
      if (icbmDue && toSpawn <= 4 && !icbmWarn) { icbmWarn = true; spawnIn = 40; report("INCOMING ICBM!"); }
      else if (icbmWarn && icbmDue) { spawn(2); icbmDue = false; toSpawn--; spawnIn = 20; report(""); }
      else { spawn(esp_random() % 4 == 0 ? 1 : 0); toSpawn--; spawnIn = imax(8, 34 - wave * 2) + esp_random() % 20; }
    }
    for (auto & e : en) {
      if (!e.live) continue;
      e.x += e.vx; e.y += e.vy;
      if (e.kind == 1) e.x += sinf(e.y / 12 + e.ph) * 0.8f;        // junk wobbles
      if (e.y >= e.ty) {                                            // hit the ground
        e.live = false;
        float R = e.kind == 2 ? 34 : 20;
        blast(e.x, GY - 6, R, true);
        for (int i = 0; i < 6; ++i) if (city[i] && fabsf(cityX(i) - e.x) < R) city[i] = false;
        for (int i = 0; i < 3; ++i) if (baseUp[i] && fabsf(baseX(i) - e.x) < R) { baseUp[i] = false; ammo[i] = 0; }
      }
    }
    for (auto & s : shots) {
      if (!s.live) continue;
      s.x += s.vx; s.y += s.vy;
      if ((s.vy <= 0 && s.y <= s.ty) || (s.vy > 0 && s.y >= s.ty)) { s.live = false; blast(s.tx, s.ty, 26, false); }
    }
    for (size_t bi = 0; bi < booms.size(); ++bi) {
      auto & b = booms[bi];
      if (!b.live) continue;
      if (b.grow) { b.r += 2; if (b.r >= b.maxR) b.grow = false; }
      else { b.r -= 1.2f; if (b.r <= 0) { b.live = false; continue; } }
      if (b.enemy) continue;
      for (auto & e : en) {
        if (!e.live) continue;
        float dx = e.x - b.x, dy = e.y - b.y;
        if (dx * dx + dy * dy <= b.r * b.r) {
          e.live = false;
          score += e.kind == 2 ? 100 : e.kind == 1 ? 15 : 25;
          float ex = e.x, ey = e.y, er = e.kind == 2 ? 30.0f : 16.0f;
          blast(ex, ey, er, false);                                // chain reaction (may grow the vector)
          report("");
          break;
        }
      }
    }
    en.erase(std::remove_if(en.begin(), en.end(), [](const Enemy & e) { return !e.live; }), en.end());
    shots.erase(std::remove_if(shots.begin(), shots.end(), [](const Shot & s) { return !s.live; }), shots.end());
    booms.erase(std::remove_if(booms.begin(), booms.end(), [](const Boom & b) { return !b.live; }), booms.end());
    if (citiesLeft() == 0 && booms.empty()) {
      over = true;
      if (score > bestScore("missile")) { saveBest("missile", score); report("THE END - new high score! Click to play again."); }
      else report("THE END. Click to play again.");
    } else if (toSpawn == 0 && en.empty() && booms.empty() && shots.empty()) {
      int left = ammo[0] + ammo[1] + ammo[2], bonus = citiesLeft() * 50 + left * 5;
      score += bonus;
      char b[80]; snprintf(b, sizeof(b), "Wave %d survived! Bonus %d (cities %d, interceptors %d)", wave, bonus, citiesLeft(), left);
      report(b);
      pause = 70;
    }
    repaint();
  }
  void mouseMove(int x, int y) override { aimX = x; aimY = imin(y, GY - 24); }
  void click(int x, int y, int) override { aimX = x; aimY = y; fire(x, y); }
  void key(uiKeyEventInfo const & k) override {
    if (k.VK == VK_SPACE || k.VK == VK_RETURN) { fire(aimX, aimY); return; }
    if (k.VK == VK_LEFT) heldX = -1; if (k.VK == VK_RIGHT) heldX = 1;
    if (k.VK == VK_UP) heldY = -1; if (k.VK == VK_DOWN) heldY = 1;
  }
  void keyUp(uiKeyEventInfo const & k) override {
    if (k.VK == VK_LEFT || k.VK == VK_RIGHT) heldX = 0;
    if (k.VK == VK_UP || k.VK == VK_DOWN) heldY = 0;
  }
  void paint(Rect const & clip) override {
    auto cv = canvas();
    Rect r = cr();
    int ox = r.X1, oy = r.Y1;
    cv->setBrushColor(T.text);                          // night sky
    cv->fillRectangle(clip.X1, clip.Y1, clip.X2, clip.Y2);
    // ground, cities, bases
    cv->setBrushColor(T.face);
    cv->fillRectangle(ox, oy + GY, ox + W - 1, oy + H - 1);
    for (int i = 0; i < 6; ++i) {
      int x = ox + cityX(i), y = oy + GY;
      if (city[i]) {
        cv->setBrushColor(T.accent);
        cv->fillRectangle(x - 14, y - 8, x - 6, y - 1); cv->fillRectangle(x - 5, y - 14, x + 3, y - 1); cv->fillRectangle(x + 4, y - 10, x + 13, y - 1);
        cv->setPenColor(T.win);
        cv->setPixel(x - 2, y - 11); cv->setPixel(x + 7, y - 7); cv->setPixel(x - 11, y - 5);
      } else {
        cv->setPenColor(T.text);
        cv->drawLine(x - 12, y - 2, x + 12, y - 2);
      }
    }
    for (int i = 0; i < 3; ++i) {
      int x = ox + baseX(i), y = oy + GY;
      Point m[4] = { Point(x - 22, y), Point(x - 12, y - 14), Point(x + 12, y - 14), Point(x + 22, y) };
      cv->setBrushColor(baseUp[i] ? T.win : T.text);
      cv->fillPath(m, 4);
      cv->setPenColor(T.text);
      for (int a = 0; a < ammo[i]; ++a) cv->drawLine(x - 10 + (a % 5) * 5, y - 11 + (a / 5) * 5, x - 10 + (a % 5) * 5, y - 8 + (a / 5) * 5);
    }
    // enemy trails and heads
    for (auto & e : en) {
      cv->setPenColor(e.kind == 1 ? T.face : T.win);
      cv->setPenWidth(e.kind == 2 ? 2 : 1);
      cv->drawLine(ox + (int)e.x0, oy + (int)e.y0, ox + (int)e.x, oy + (int)e.y);
      cv->setPenWidth(1);
      cv->setBrushColor(T.accent);
      int s = e.kind == 2 ? 3 : 2;
      cv->fillRectangle(ox + (int)e.x - s, oy + (int)e.y - s, ox + (int)e.x + s, oy + (int)e.y + s);
      if (e.kind == 1 && (frame & 2)) { cv->setPenColor(T.win); cv->setPixel(ox + (int)e.x - 3, oy + (int)e.y - 4); }
    }
    for (auto & s : shots) {
      cv->setPenColor(T.accent);
      cv->drawLine(ox + (int)s.x0, oy + (int)s.y0, ox + (int)s.x, oy + (int)s.y);
      cv->setPenColor(T.win);
      int tx = ox + (int)s.tx, ty = oy + (int)s.ty;
      cv->drawLine(tx - 3, ty - 3, tx + 3, ty + 3); cv->drawLine(tx - 3, ty + 3, tx + 3, ty - 3);
    }
    for (auto & b : booms) {
      cv->setBrushColor(((frame >> 1) & 1) ? T.win : (b.enemy ? T.face : T.accent));
      int d = (int)(b.r * 2);
      if (d > 1) cv->fillEllipse(ox + (int)b.x, oy + (int)b.y, d, d);
    }
    // crosshair
    cv->setPenColor(T.win);
    int ax = ox + aimX, ay = oy + aimY;
    cv->drawLine(ax - 7, ay, ax - 2, ay); cv->drawLine(ax + 2, ay, ax + 7, ay);
    cv->drawLine(ax, ay - 7, ax, ay - 2); cv->drawLine(ax, ay + 2, ax, ay + 7);
    // text
    char t[48];
    cv->setPenColor(T.win); cv->setBrushColor(T.text);
    snprintf(t, sizeof(t), "%d", score);
    cv->drawText(&FONT_std_16, ox + 8, oy + 6, t);
    snprintf(t, sizeof(t), "WAVE %d", wave);
    cv->drawText(&FONT_std_14, ox + W - 74, oy + 6, t);
    if (icbmWarn && icbmDue && (frame & 8)) cv->drawText(&FONT_std_18, ox + W / 2 - 80, oy + 40, "INCOMING ICBM");
    if (pause > 0) cv->drawText(&FONT_std_18, ox + W / 2 - 70, oy + 120, "WAVE CLEARED");
    if (over) { cv->drawText(&FONT_std_24, ox + W / 2 - 54, oy + 120, "THE END"); cv->drawText(&FONT_std_14, ox + W / 2 - 80, oy + 156, "Click to play again"); }
  }
};

// ====================================================================== StuntCopter
// Fly the copter, drop the stuntman into the moving hay wagon.

class CopterBoard : public Board {
public:
  static const int W = 520, H = 360, GY = 334;
  float hx, hy, hvx, tx, ty;          // copter
  int dir = 1;
  bool manOut; float mx, my, mvx, mvy, dropY;
  float wx, wspeed; int hayW;         // wagon: x = left of the hay
  struct Bird { float x, y, vx, vy; bool live; };
  struct Bullet { float x, y, vx; bool live; };
  std::vector<Bird> birds;
  std::vector<Bullet> bullets;
  float cloudX[2]; int cloudY[2];
  int score, jumpers, level, streak, copters, frame, crash, splat, held, heldY, birdIn;
  bool over;
  char msg[64];

  CopterBoard(uiWindow * p, Point pos) : Board(p, pos, Size(W + 2, H + 2)) { newGame(); }

  void newGame() override {
    hx = tx = W / 2; hy = ty = 60; hvx = 0; dir = 1;
    manOut = false; wx = -120; level = 1; setLevel();
    cloudX[0] = 60; cloudY[0] = 150; cloudX[1] = 330; cloudY[1] = 210;
    score = 0; jumpers = 10; streak = 0; copters = 3; frame = 0; crash = 0; splat = 0; held = 0; heldY = 0; birdIn = 200;
    birds.clear(); bullets.clear();
    over = false;
    strcpy(msg, "");
    startTimer(40);
    report(""); repaint();
  }
  void setLevel() { wspeed = 1.4f + level * 0.45f; hayW = imax(28, 64 - (level - 1) * 6); }
  void report(const char * m) {
    char b[96];
    if (m && *m) snprintf(b, sizeof(b), "%s", m);
    else snprintf(b, sizeof(b), "Score %d   Jumpers %d   Level %d   Copters %d   Best %d", score, jumpers, level, copters, imax(score, bestScore("copter")));
    if (onScore) onScore(score, b);
  }
  // the wagon, left to right: hay box [wx, wx+hayW], driver, horse
  int driverX() { return (int)wx + hayW + 4; }
  int horseX() { return (int)wx + hayW + 14; }
  void drop() {
    if (over) { newGame(); return; }
    if (manOut || crash) return;
    manOut = true; mx = hx; my = hy + 10; mvx = hvx * 0.8f; mvy = 0; dropY = my;
  }
  void shoot() {
    if (crash || over) return;
    if (bullets.size() < 3) { Bullet b = { hx + dir * 20, hy + 2, dir * 9.0f, true }; bullets.push_back(b); }
  }
  void landed(int kind) {          // 0 hay, 1 horse/driver, 2 missed
    manOut = false;
    if (kind == 0) {
      int pts = (25 + (int)(GY - dropY) / 2) * level;
      score += pts; ++streak;
      snprintf(msg, sizeof(msg), "In the hay! +%d", pts);
      if (streak >= 5) { ++level; streak = 0; setLevel(); snprintf(msg, sizeof(msg), "Level %d! The wagon speeds up.", level); }
    } else {
      jumpers -= kind == 1 ? 2 : 1; streak = 0; splat = 25;
      snprintf(msg, sizeof(msg), kind == 1 ? "Ouch - hit the horse team! -2 jumpers" : "Missed the wagon! -1 jumper");
    }
    report(msg);
    if (jumpers <= 0) endGame();
  }
  void endGame() {
    over = true; jumpers = imax(0, jumpers);
    if (score > bestScore("copter")) { saveBest("copter", score); report("Game over - new high score! Click to play again."); }
    else report("Game over. Click to play again.");
  }
  void tick() override {
    if (over) { repaint(); return; }
    ++frame;
    // copter follows the mouse / keys
    if (crash) {
      hy += 3; hx += dir * 1.5f;
      if (--crash == 0) {
        if (copters <= 0) { endGame(); return; }
        hx = tx = W / 2; hy = ty = 60;
      }
    } else {
      if (held) tx = fmaxf(20, fminf(W - 20, tx + held * 7));
      if (heldY) ty = fmaxf(30, fminf(220, ty + heldY * 5));
      float dx = tx - hx; hvx = fmaxf(-6, fminf(6, dx * 0.2f));
      hx += hvx; hy += fmaxf(-4, fminf(4, (ty - hy) * 0.2f));
      if (hvx > 0.3f) dir = 1; else if (hvx < -0.3f) dir = -1;
    }
    // wagon
    wx += wspeed;
    if (wx > W + 20) wx = -hayW - 40;
    for (int i = 0; i < 2; ++i) { cloudX[i] += 0.3f * (i + 1); if (cloudX[i] > W + 50) cloudX[i] = -60; }
    // stuntman
    if (manOut) {
      bool inCloud = false;
      for (int i = 0; i < 2; ++i) if (fabsf(mx - cloudX[i]) < 40 && fabsf(my - cloudY[i]) < 14) inCloud = true;
      mvy = fminf(inCloud ? 1.5f : 7, mvy + 0.25f);
      mx += mvx; my += mvy;
      mvx *= 0.99f;
      if (my >= GY - 18) {
        if (mx >= wx && mx <= wx + hayW) landed(0);
        else if (mx > wx + hayW && mx <= horseX() + 22) landed(1);
        else if (my >= GY - 6) landed(2);
      }
    }
    if (splat) --splat;
    // birds from level 5
    if (level >= 5 && !crash && --birdIn <= 0 && birds.size() < 3) {
      Bird b; b.live = true; b.x = (esp_random() & 1) ? -10 : W + 10; b.y = 30 + esp_random() % 120; b.vx = b.vy = 0;
      birds.push_back(b); birdIn = imax(60, 220 - level * 15);
    }
    for (auto & b : birds) {
      float dx = hx - b.x, dy = hy - b.y, d = fmaxf(1, sqrtf(dx * dx + dy * dy));
      float sp = 1.6f + level * 0.1f;
      b.vx = b.vx * 0.9f + dx / d * sp * 0.1f; b.vy = b.vy * 0.9f + dy / d * sp * 0.1f;
      b.x += b.vx * 2; b.y += b.vy * 2;
      if (!crash && d < 16) {                                      // got the copter
        b.live = false; crash = 50; --copters;
        report(copters > 0 ? "The copter's going down!" : "Last copter down!");
      }
    }
    for (auto & u : bullets) {
      u.x += u.vx;
      if (u.x < -5 || u.x > W + 5) u.live = false;
      for (auto & b : birds) if (b.live && fabsf(b.x - u.x) < 9 && fabsf(b.y - u.y) < 7) { b.live = false; u.live = false; score += 50; report(""); }
    }
    birds.erase(std::remove_if(birds.begin(), birds.end(), [](const Bird & b) { return !b.live; }), birds.end());
    bullets.erase(std::remove_if(bullets.begin(), bullets.end(), [](const Bullet & b) { return !b.live; }), bullets.end());
    repaint();
  }
  void mouseMove(int x, int y) override { tx = x; ty = fmaxf(30, fminf(220, y)); }
  void click(int x, int y, int button) override { if (button == 2 || button == 3) shoot(); else drop(); }
  void key(uiKeyEventInfo const & k) override {
    if (k.VK == VK_SPACE || k.VK == VK_RETURN) drop();
    else if (k.VK == VK_f || k.VK == VK_F || k.VK == VK_LCTRL || k.VK == VK_RCTRL) shoot();
    else if (k.VK == VK_LEFT) held = -1; else if (k.VK == VK_RIGHT) held = 1;
    else if (k.VK == VK_UP) heldY = -1; else if (k.VK == VK_DOWN) heldY = 1;
  }
  void keyUp(uiKeyEventInfo const & k) override {
    if (k.VK == VK_LEFT || k.VK == VK_RIGHT) held = 0;
    if (k.VK == VK_UP || k.VK == VK_DOWN) heldY = 0;
  }
  void cloud(Canvas * cv, int x, int y) {
    cv->setBrushColor(T.face);
    cv->fillEllipse(x, y, 60, 18); cv->fillEllipse(x - 18, y + 3, 34, 14); cv->fillEllipse(x + 18, y + 2, 38, 16); cv->fillEllipse(x + 4, y - 7, 34, 16);
  }
  void paint(Rect const & clip) override {
    auto cv = canvas();
    Rect r = cr();
    int ox = r.X1, oy = r.Y1;
    cv->setBrushColor(T.win);
    cv->fillRectangle(clip.X1, clip.Y1, clip.X2, clip.Y2);
    for (int i = 0; i < 2; ++i) cloud(cv, ox + (int)cloudX[i], oy + cloudY[i]);
    // ground
    cv->setBrushColor(T.face);
    cv->fillRectangle(ox, oy + GY, ox + W - 1, oy + H - 1);
    cv->setPenColor(T.text);
    cv->drawLine(ox, oy + GY, ox + W - 1, oy + GY);
    // wagon: hay, driver, horse
    int x = ox + (int)wx, y = oy + GY;
    cv->setBrushColor(T.accent);
    cv->fillRectangle(x - 2, y - 14, x + hayW + 6, y - 8);                 // wagon bed
    cv->setBrushColor(T.face);
    cv->fillEllipse(x + hayW / 2, y - 18, hayW, 14);                        // the hay
    cv->setPenColor(T.text);
    for (int k = 4; k < hayW - 2; k += 6) cv->drawLine(x + k, y - 22, x + k + 3, y - 15);
    cv->setBrushColor(T.text);
    cv->fillEllipse(x + 6, y - 5, 10, 10); cv->fillEllipse(x + hayW - 4, y - 5, 10, 10);   // wheels
    int dx = ox + driverX();
    cv->fillRectangle(dx - 1, y - 26, dx + 2, y - 15); cv->fillEllipse(dx, y - 29, 6, 6);   // driver
    int hx0 = ox + horseX();
    cv->fillRectangle(hx0, y - 22, hx0 + 18, y - 14);                      // horse body
    cv->fillRectangle(hx0 + 16, y - 30, hx0 + 20, y - 20);                 // neck
    cv->fillRectangle(hx0 + 18, y - 31, hx0 + 25, y - 27);                 // head
    int leg = (frame / 3) & 1;
    cv->setPenColor(T.text);
    cv->drawLine(hx0 + 2, y - 14, hx0 + 2 - leg * 2, y - 1); cv->drawLine(hx0 + 16, y - 14, hx0 + 16 + leg * 2, y - 1);
    cv->drawLine(hx0 + 5, y - 14, hx0 + 5 + leg * 2, y - 1); cv->drawLine(hx0 + 13, y - 14, hx0 + 13 - leg * 2, y - 1);
    cv->drawLine(dx + 2, y - 22, hx0, y - 20);                             // reins
    // the copter
    {
      int cx = ox + (int)hx, cy = oy + (int)hy, d = dir;
      cv->setBrushColor(crash ? T.accent : T.text);
      cv->fillEllipse(cx, cy, 24, 12);
      cv->fillRectangle(d > 0 ? cx - 22 : cx + 8, cy - 2, d > 0 ? cx - 8 : cx + 22, cy);
      cv->setPenColor(T.text);
      cv->drawLine(cx - d * 22, cy - 6, cx - d * 22, cy + 1);
      int rl = (frame & 1) ? 20 : 10;
      cv->drawLine(cx - rl, cy - 9, cx + rl, cy - 9);
      cv->drawLine(cx, cy - 9, cx, cy - 6);
      cv->drawLine(cx - 9, cy + 8, cx + 9, cy + 8);
      cv->setBrushColor(T.win);
      cv->fillRectangle(imin(cx + d * 3, cx + d * 8), cy - 3, imax(cx + d * 3, cx + d * 8), cy);
      if (crash && (frame & 2)) { cv->setBrushColor(T.face); cv->fillEllipse(cx - d * 6, cy - 12, 12, 10); }
    }
    // stuntman: arms up while falling
    if (manOut) {
      int sx = ox + (int)mx, sy = oy + (int)my;
      cv->setPenColor(T.text); cv->setBrushColor(T.text);
      cv->fillEllipse(sx, sy - 7, 5, 5);
      cv->drawLine(sx, sy - 5, sx, sy + 3);
      cv->drawLine(sx, sy - 3, sx - 4, sy - 8); cv->drawLine(sx, sy - 3, sx + 4, sy - 8);
      cv->drawLine(sx, sy + 3, sx - 3, sy + 8); cv->drawLine(sx, sy + 3, sx + 3, sy + 8);
    }
    if (splat) { cv->setPenColor(T.text); cv->setBrushColor(T.win); cv->drawText(&FONT_std_14, ox + W / 2 - 30, oy + GY - 60, "OUCH!"); }
    for (auto & b : birds) {
      int bx = ox + (int)b.x, by = oy + (int)b.y, f = (frame >> 1) & 1;
      cv->setPenColor(T.text);
      cv->drawLine(bx - 7, by - 3 + f * 5, bx, by); cv->drawLine(bx, by, bx + 7, by - 3 + f * 5);
    }
    for (auto & u : bullets) { cv->setPenColor(T.text); cv->drawLine(ox + (int)u.x - 3, oy + (int)u.y, ox + (int)u.x + 3, oy + (int)u.y); }
    // the drop height marker and score
    char t[48];
    cv->setPenColor(T.text); cv->setBrushColor(T.win);
    snprintf(t, sizeof(t), "%d", score); cv->drawText(&FONT_std_16, ox + 8, oy + 6, t);
    snprintf(t, sizeof(t), "Jumpers %d   Level %d%s", jumpers, level, level >= 5 ? "   F = fire" : "");
    cv->drawText(&FONT_std_12, ox + W - 210, oy + 8, t);
    if (over) { cv->drawText(&FONT_std_24, ox + W / 2 - 64, oy + 110, "GAME OVER"); cv->drawText(&FONT_std_14, ox + W / 2 - 74, oy + 144, "Click to play again"); }
  }
};

// ====================================================================== Checkers (you: white, from the bottom; TidyOS: black)
class CheckersBoard : public Board {
public:
  static const int CS = 46;
  CheckersBoard(uiWindow * p, Point pos) : Board(p, pos, Size(8 * CS + 2, 8 * CS + 2)) { newGame(); }

  // 0 empty, 1 your man, 2 your king, 3 my man, 4 my king
  struct Pos { int8_t b[64]; };
  struct Move { int8_t from, n; int8_t to[10]; int8_t cap[10]; };
  Pos pos;
  int sel = -1, nPath = 0;
  int8_t path[10];
  bool myTurn = false, over = false;
  std::vector<Move> legal;

  static bool mine(int v, bool cpu) { return cpu ? (v == 3 || v == 4) : (v == 1 || v == 2); }
  static bool king(int v) { return v == 2 || v == 4; }

  void newGame() override {
    memset(pos.b, 0, 64);
    for (int s = 0; s < 64; ++s) {
      int x = s % 8, y = s / 8;
      if ((x + y) % 2 == 0) continue;
      if (y < 3) pos.b[s] = 3; else if (y > 4) pos.b[s] = 1;
    }
    sel = -1; nPath = 0; myTurn = false; over = false;
    stopTimer();
    gen(pos, false, legal);
    repaint();
    say("Your move (white). Click a piece, then where it goes. Jumps are a must.");
  }
  void say(const char * m) { if (onScore) onScore(0, m); }

  // jumps (with every continuation) for one piece
  static void jumps(const Pos & p, int s, bool cpu, Move & m, std::vector<Move> & out) {
    int v = p.b[s], x = s % 8, y = s / 8;
    bool any = false;
    for (int dy = -1; dy <= 1; dy += 2) {
      if (!king(v) && (cpu ? dy < 0 : dy > 0)) continue;
      for (int dx = -1; dx <= 1; dx += 2) {
        int mx = x + dx, my = y + dy, lx = x + 2 * dx, ly = y + 2 * dy;
        if (lx < 0 || lx > 7 || ly < 0 || ly > 7) continue;
        int ms = my * 8 + mx, ls = ly * 8 + lx;
        int mv = p.b[ms];
        if (!mv || mine(mv, cpu) || p.b[ls] || m.n >= 9) continue;
        Pos q = p;
        q.b[ls] = v; q.b[s] = 0; q.b[ms] = 0;
        bool crowned = false;
        if (!king(v) && (cpu ? ly == 7 : ly == 0)) { q.b[ls] = cpu ? 4 : 2; crowned = true; }
        m.to[m.n] = ls; m.cap[m.n] = ms; m.n++;
        any = true;
        if (crowned) out.push_back(m);              // being crowned ends the move
        else jumps(q, ls, cpu, m, out);
        m.n--;
      }
    }
    if (!any && m.n > 0) out.push_back(m);
  }
  static void gen(const Pos & p, bool cpu, std::vector<Move> & out) {
    out.clear();
    for (int s = 0; s < 64; ++s) if (mine(p.b[s], cpu)) { Move m; m.from = s; m.n = 0; jumps(p, s, cpu, m, out); }
    if (!out.empty()) return;                      // a jump is there: it must be taken
    for (int s = 0; s < 64; ++s) {
      int v = p.b[s];
      if (!mine(v, cpu)) continue;
      int x = s % 8, y = s / 8;
      for (int dy = -1; dy <= 1; dy += 2) {
        if (!king(v) && (cpu ? dy < 0 : dy > 0)) continue;
        for (int dx = -1; dx <= 1; dx += 2) {
          int nx = x + dx, ny = y + dy;
          if (nx < 0 || nx > 7 || ny < 0 || ny > 7 || p.b[ny * 8 + nx]) continue;
          Move m; m.from = s; m.n = 1; m.to[0] = ny * 8 + nx; m.cap[0] = -1;
          out.push_back(m);
        }
      }
    }
  }
  static void apply(Pos & p, const Move & m) {
    int v = p.b[m.from];
    p.b[m.from] = 0;
    for (int i = 0; i < m.n; ++i) if (m.cap[i] >= 0) p.b[m.cap[i]] = 0;
    int t = m.to[m.n - 1];
    if (v == 1 && t / 8 == 0) v = 2;
    if (v == 3 && t / 8 == 7) v = 4;
    p.b[t] = v;
  }
  static int eval(const Pos & p) {                  // good for the computer = positive
    int e = 0;
    for (int s = 0; s < 64; ++s) {
      switch (p.b[s]) {
        case 1: e -= 100 + (7 - s / 8) * 3; break;
        case 2: e -= 165; break;
        case 3: e += 100 + (s / 8) * 3; break;
        case 4: e += 165; break;
      }
    }
    return e;
  }
  static int search(const Pos & p, int depth, bool cpu, int a, int b) {
    std::vector<Move> ms;
    gen(p, cpu, ms);
    if (ms.empty()) return cpu ? -100000 : 100000;     // no moves: that side has lost
    if (depth == 0) return eval(p);
    for (auto & m : ms) {
      Pos q = p; apply(q, m);
      int v = search(q, depth - 1, !cpu, a, b);
      if (cpu) { if (v > a) a = v; } else { if (v < b) b = v; }
      if (a >= b) break;
    }
    return cpu ? a : b;
  }
  void cpuMove() {
    std::vector<Move> ms;
    gen(pos, true, ms);
    if (ms.empty()) { over = true; say("You win! I'm out of moves. New game to play again."); repaint(); return; }
    int best = -1000000, pick = 0;
    for (size_t i = 0; i < ms.size(); ++i) {
      Pos q = pos; apply(q, ms[i]);
      int v = search(q, 3, false, -1000000, 1000000) + (int)(esp_random() % 7);   // a little variety
      if (v > best) { best = v; pick = i; }
    }
    apply(pos, ms[pick]);
    myTurn = false;
    gen(pos, false, legal);
    repaint();
    if (legal.empty()) { over = true; say("I win this one. New game to try again."); }
    else say(legal[0].cap[0] >= 0 ? "Your move - you have a jump to take." : "Your move.");
  }

  void tick() override { stopTimer(); if (myTurn && !over) cpuMove(); }

  void click(int px, int py, int) override {
    if (over || myTurn) return;
    int x = (px - 1) / CS, y = (py - 1) / CS;
    if (x < 0 || x > 7 || y < 0 || y > 7) return;
    int s = y * 8 + x;
    if (mine(pos.b[s], false)) {                     // pick (or re-pick) a piece
      bool can = false;
      for (auto & m : legal) if (m.from == s) can = true;
      if (!can) { say(legal.empty() ? "No moves." : "That piece can't move now (a jump elsewhere is a must)."); return; }
      sel = s; nPath = 0; repaint(); return;
    }
    if (sel < 0) return;
    path[nPath] = s;
    const Move * done = nullptr; bool prefix = false;
    for (auto & m : legal) {
      if (m.from != sel || m.n < nPath + 1) continue;
      bool ok = true;
      for (int i = 0; i <= nPath; ++i) if (m.to[i] != path[i]) ok = false;
      if (!ok) continue;
      if (m.n == nPath + 1) done = &m; else prefix = true;
    }
    if (done) {
      apply(pos, *done);
      sel = -1; nPath = 0; myTurn = true;
      repaint();
      say("Thinking...");
      startTimer(350);                               // let the move show first
    } else if (prefix) { ++nPath; repaint(); say("Keep jumping: click the next square."); }
  }

  void paint(Rect const & clip) override {
    auto cv = canvas();
    Rect r = cr();
    for (int s = 0; s < 64; ++s) {
      int x = s % 8, y = s / 8;
      int px = r.X1 + 1 + x * CS, py = r.Y1 + 1 + y * CS;
      bool dark = (x + y) % 2;
      cv->setBrushColor(dark ? T.accent : T.win);
      cv->fillRectangle(px, py, px + CS - 1, py + CS - 1);
      bool hl = s == sel;
      for (int i = 0; i < nPath; ++i) if (path[i] == s) hl = true;
      if (hl) { cv->setPenColor(T.warn); cv->drawRectangle(px + 1, py + 1, px + CS - 2, py + CS - 2); cv->drawRectangle(px + 2, py + 2, px + CS - 3, py + CS - 3); }
      int v = pos.b[s];
      if (!v) continue;
      int cx = px + CS / 2, cy = py + CS / 2, d = CS - 12;
      cv->setBrushColor(T.text);
      cv->fillEllipse(cx + 1, cy + 2, d, d);         // edge / shadow
      cv->setBrushColor(v <= 2 ? T.win : T.text);
      cv->fillEllipse(cx, cy, d - 2, d - 2);
      cv->setPenColor(v <= 2 ? T.text : T.face);
      cv->drawEllipse(cx, cy, d - 12, d - 12);
      if (king(v)) {                                  // a little crown
        cv->setBrushColor(v <= 2 ? T.text : T.win);
        cv->fillRectangle(cx - 8, cy, cx + 8, cy + 3);
        for (int k = -8; k <= 8; k += 8) cv->fillRectangle(cx + k - 1, cy - 6, cx + k + 1, cy);
      }
    }
  }
};
