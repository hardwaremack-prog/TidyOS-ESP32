// ====================================================================== Latch (password safe)
// Opens and saves the same .latch vault files as the Latch web app: a JSON envelope with
// PBKDF2-SHA256 key derivation and AES-256-GCM encryption. (included at the end of apps.cpp)
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/gcm.h"
#include "mbedtls/base64.h"
#include "esp_random.h"

namespace lj {                                   // a small JSON reader / writer
struct P { const char * s; const char * e; };
static void ws(P & p) { while (p.s < p.e && (*p.s == ' ' || *p.s == '\n' || *p.s == '\r' || *p.s == '\t')) ++p.s; }
static void utf8(std::string & o, unsigned cp)
{
  if (cp < 0x80) o += (char)cp;
  else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
  else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
  else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
}
static unsigned hex4(const char * s) { char b[5] = { s[0], s[1], s[2], s[3], 0 }; return strtoul(b, nullptr, 16); }
static bool str(P & p, std::string & out)
{
  ws(p);
  if (p.s >= p.e || *p.s != '"') return false;
  ++p.s; out.clear();
  while (p.s < p.e && *p.s != '"') {
    char c = *p.s++;
    if (c != '\\' || p.s >= p.e) { out += c; continue; }
    char d = *p.s++;
    switch (d) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'u': {
        if (p.e - p.s < 4) return false;
        unsigned cp = hex4(p.s); p.s += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && p.e - p.s >= 6 && p.s[0] == '\\' && p.s[1] == 'u') {
          unsigned lo = hex4(p.s + 2); p.s += 6;
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        }
        utf8(out, cp);
        break;
      }
      default: out += d;
    }
  }
  if (p.s >= p.e) return false;
  ++p.s;
  return true;
}
static bool skip(P & p)
{
  ws(p);
  if (p.s >= p.e) return false;
  char c = *p.s;
  if (c == '"') { std::string t; return str(p, t); }
  if (c == '{' || c == '[') {
    char close = c == '{' ? '}' : ']';
    ++p.s; ws(p);
    if (p.s < p.e && *p.s == close) { ++p.s; return true; }
    for (;;) {
      if (c == '{') { std::string k; if (!str(p, k)) return false; ws(p); if (p.s >= p.e || *p.s != ':') return false; ++p.s; }
      if (!skip(p)) return false;
      ws(p);
      if (p.s >= p.e) return false;
      if (*p.s == ',') { ++p.s; continue; }
      if (*p.s == close) { ++p.s; return true; }
      return false;
    }
  }
  while (p.s < p.e && !strchr(",}] \n\r\t", *p.s)) ++p.s;
  return true;
}
typedef std::function<void(const std::string &, const char *, const char *)> MemberFn;
static bool obj(P p, const MemberFn & f)
{
  ws(p);
  if (p.s >= p.e || *p.s != '{') return false;
  ++p.s; ws(p);
  if (p.s < p.e && *p.s == '}') return true;
  for (;;) {
    std::string k;
    if (!str(p, k)) return false;
    ws(p);
    if (p.s >= p.e || *p.s != ':') return false;
    ++p.s; ws(p);
    const char * a = p.s;
    if (!skip(p)) return false;
    f(k, a, p.s);
    ws(p);
    if (p.s < p.e && *p.s == ',') { ++p.s; continue; }
    return p.s < p.e && *p.s == '}';
  }
}
static bool arr(P p, const std::function<void(const char *, const char *)> & f)
{
  ws(p);
  if (p.s >= p.e || *p.s != '[') return false;
  ++p.s; ws(p);
  if (p.s < p.e && *p.s == ']') return true;
  for (;;) {
    ws(p);
    const char * a = p.s;
    if (!skip(p)) return false;
    f(a, p.s);
    ws(p);
    if (p.s < p.e && *p.s == ',') { ++p.s; continue; }
    return p.s < p.e && *p.s == ']';
  }
}
static std::string sval(const char * a, const char * b) { P p = { a, b }; std::string o; return str(p, o) ? o : std::string(a, b); }
static std::string q(const std::string & s)
{
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"') o += "\\\""; else if (c == '\\') o += "\\\\"; else if (c == '\n') o += "\\n"; else if (c == '\r') o += "\\r"; else if (c == '\t') o += "\\t";
    else if (c < 32) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o + "\"";
}
} // namespace lj

static std::string b64enc(const uint8_t * d, size_t n)
{
  size_t olen = 0;
  mbedtls_base64_encode(nullptr, 0, &olen, d, n);
  std::string o(olen, 0);
  if (mbedtls_base64_encode((unsigned char *)&o[0], olen, &olen, d, n) != 0) return "";
  o.resize(olen);
  return o;
}
static bool b64dec(const std::string & s, std::vector<uint8_t> & out)
{
  size_t olen = 0;
  mbedtls_base64_decode(nullptr, 0, &olen, (const unsigned char *)s.data(), s.size());
  out.resize(olen);
  if (mbedtls_base64_decode(out.data(), olen, &olen, (const unsigned char *)s.data(), s.size()) != 0) return false;
  out.resize(olen);
  return true;
}
static std::string isoNow()
{
  time_t now = time(nullptr); struct tm t; gmtime_r(&now, &t);
  char b[32]; snprintf(b, sizeof(b), "%04d-%02d-%02dT%02d:%02d:%02d.000Z", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  return b;
}
static std::string latchUid()
{
  uint8_t r[9]; esp_fill_random(r, sizeof(r));
  std::string s = b64enc(r, 9);
  for (auto & c : s) if (c == '+' || c == '/' || c == '=') c = 'x';
  return s;
}

struct LGroup { std::string id, name, icon; bool bin = false; std::string extra; };
struct LEntry {
  std::string id, groupId, title, username, password, url, notes, created, modified, expires;
  std::string history = "[]";      // kept as JSON text
  std::string extra;               // other fields, kept as-is
};
struct LVault {
  std::string name, created, settings = "{}", extra;
  std::vector<LGroup> groups;
  std::vector<LEntry> entries;
  uint8_t key[32]; uint8_t salt[16]; uint32_t iter = 0;
  bool open = false;
};

static bool parseVault(const std::string & js, LVault & v)
{
  v.groups.clear(); v.entries.clear(); v.extra.clear();
  lj::P p = { js.data(), js.data() + js.size() };
  return lj::obj(p, [&](const std::string & k, const char * a, const char * b) {
    if (k == "name") v.name = lj::sval(a, b);
    else if (k == "created") v.created = lj::sval(a, b);
    else if (k == "settings") v.settings.assign(a, b);
    else if (k == "groups") lj::arr({ a, b }, [&](const char * ga, const char * gb) {
      LGroup g;
      lj::obj({ ga, gb }, [&](const std::string & gk, const char * x, const char * y) {
        if (gk == "id") g.id = lj::sval(x, y); else if (gk == "name") g.name = lj::sval(x, y);
        else if (gk == "icon") g.icon = lj::sval(x, y); else if (gk == "bin") g.bin = (y - x == 4 && !strncmp(x, "true", 4));
        else { if (!g.extra.empty()) g.extra += ','; g.extra += lj::q(gk) + ":" + std::string(x, y); }
      });
      v.groups.push_back(g);
    });
    else if (k == "entries") lj::arr({ a, b }, [&](const char * ea, const char * eb) {
      LEntry e;
      lj::obj({ ea, eb }, [&](const std::string & ek, const char * x, const char * y) {
        std::string s;
        if (ek == "history") { e.history.assign(x, y); return; }
        bool known = true;
        if (ek == "id") e.id = lj::sval(x, y); else if (ek == "groupId") e.groupId = lj::sval(x, y);
        else if (ek == "title") e.title = lj::sval(x, y); else if (ek == "username") e.username = lj::sval(x, y);
        else if (ek == "password") e.password = lj::sval(x, y); else if (ek == "url") e.url = lj::sval(x, y);
        else if (ek == "notes") e.notes = lj::sval(x, y); else if (ek == "created") e.created = lj::sval(x, y);
        else if (ek == "modified") e.modified = lj::sval(x, y);
        else if (ek == "expires") { e.expires = (y - x == 4 && !strncmp(x, "null", 4)) ? "" : lj::sval(x, y); }
        else known = false;
        if (!known) { if (!e.extra.empty()) e.extra += ','; e.extra += lj::q(ek) + ":" + std::string(x, y); }
      });
      v.entries.push_back(e);
    });
    else { if (!v.extra.empty()) v.extra += ','; v.extra += lj::q(k) + ":" + std::string(a, b); }
  });
}

static std::string vaultJson(const LVault & v)
{
  std::string o = "{\"name\":" + lj::q(v.name) + ",\"groups\":[";
  for (size_t i = 0; i < v.groups.size(); ++i) {
    auto & g = v.groups[i];
    if (i) o += ',';
    o += "{\"id\":" + lj::q(g.id) + ",\"name\":" + lj::q(g.name) + ",\"icon\":" + lj::q(g.icon);
    if (g.bin) o += ",\"bin\":true";
    if (!g.extra.empty()) o += "," + g.extra;
    o += '}';
  }
  o += "],\"entries\":[";
  for (size_t i = 0; i < v.entries.size(); ++i) {
    auto & e = v.entries[i];
    if (i) o += ',';
    o += "{\"id\":" + lj::q(e.id) + ",\"groupId\":" + lj::q(e.groupId) + ",\"title\":" + lj::q(e.title) + ",\"username\":" + lj::q(e.username) +
         ",\"password\":" + lj::q(e.password) + ",\"url\":" + lj::q(e.url) + ",\"notes\":" + lj::q(e.notes) +
         ",\"expires\":" + (e.expires.empty() ? std::string("null") : lj::q(e.expires)) + ",\"created\":" + lj::q(e.created) +
         ",\"modified\":" + lj::q(e.modified) + ",\"history\":" + e.history;
    if (!e.extra.empty()) o += "," + e.extra;
    o += '}';
  }
  o += "],\"settings\":" + v.settings + ",\"created\":" + lj::q(v.created);
  if (!v.extra.empty()) o += "," + v.extra;
  return o + "}";
}

// ---- crypto (the slow part, PBKDF2, runs on its own task so the screen keeps working)
struct KdfJob { std::string pw; uint8_t salt[16]; size_t saltLen; uint32_t iter; uint8_t key[32]; volatile int done; int err; };
static void kdfTaskBody(void * arg)
{
  auto j = (KdfJob *)arg;
  mbedtls_md_context_t md;
  mbedtls_md_init(&md);
  j->err = mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  if (!j->err) j->err = mbedtls_pkcs5_pbkdf2_hmac(&md, (const unsigned char *)j->pw.data(), j->pw.size(), j->salt, j->saltLen, j->iter, 32, j->key);
  mbedtls_md_free(&md);
  j->done = 1;
  return;
}

// runs the task, then ends it - after the body's locals are destroyed (vTaskDelete
// inside the body would skip their destructors and leak their memory)
static void kdfTask(void * arg) { kdfTaskBody(arg); vTaskDelete(nullptr); }

// derive the key with a small "please wait" window; false if it failed or memory ran out
static bool deriveKeyUI(const std::string & pw, const uint8_t * salt, size_t saltLen, uint32_t iter, uint8_t * key)
{
  KdfJob * j = new (std::nothrow) KdfJob();
  if (!j) return false;
  j->pw = pw; memcpy(j->salt, salt, saltLen); j->saltLen = saltLen; j->iter = iter; j->done = 0; j->err = 0;
  // priority 0: shares the CPU with the idle task, so the watchdog stays happy and the screen stays responsive
  if (xTaskCreatePinnedToCore(kdfTask, "latchkdf", 4096, j, 0, nullptr, 1) != pdPASS) { delete j; return false; }
  const int W = 360, H = 110;
  auto f = new uiFrame(gApp->rootWindow(), "Latch", Point((SCREEN_W - W) / 2, (SCREEN_H - 22 - H) / 2), Size(W, H), false);
  applyStyle(f);
  f->frameProps().resizeable = false; f->frameProps().hasMaximizeButton = false; f->frameProps().hasMinimizeButton = false; f->frameProps().hasCloseButton = false;
  Point o = clientOrigin(f);
  auto l = S(new uiLabel(f, "Working out the key...", Point(o.X + 10, o.Y + 10), Size(W - 30, 18)));
  auto l2 = S(new uiLabel(f, "", Point(o.X + 10, o.Y + 34), Size(W - 30, 18)));
  l2->labelStyle().textFont = &FONT_std_12;
  uint32_t t0 = millis();
  f->onTimer = [&](uiTimerHandle) {
    char b[80];
    snprintf(b, sizeof(b), "%u s  (%u rounds - the ESP32 takes a while)", (unsigned)((millis() - t0) / 1000), (unsigned)iter);
    l2->setText(b); l2->repaint();
    if (j->done) f->exitModal(1);
  };
  auto t = gApp->setTimer(f, 500);
  (void)l;
  gApp->showModalWindow(f);
  gApp->killTimer(t);
  gApp->destroyWindow(f);
  while (!j->done) delay(20);
  bool ok = j->err == 0;
  if (ok) memcpy(key, j->key, 32);
  memset(j->key, 0, 32);
  for (auto & c : j->pw) c = 0;
  delete j;
  return ok;
}

static bool gcmRun(bool enc, const uint8_t * key, const uint8_t * iv, const uint8_t * in, size_t n, uint8_t * out, uint8_t * tag)
{
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  int r = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (!r) r = enc ? mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, n, iv, 12, nullptr, 0, in, out, 16, tag)
                  : mbedtls_gcm_auth_decrypt(&g, n, iv, 12, nullptr, 0, tag, 16, in, out);
  mbedtls_gcm_free(&g);
  return r == 0;
}

// a password box (shows dots); returns false on Cancel
static bool askSecret(const char * title, const char * prompt, std::string & out, bool twice)
{
  const int W = 380, H = twice ? 170 : 130;
  auto f = new uiFrame(gApp->rootWindow(), title, Point((SCREEN_W - W) / 2, (SCREEN_H - 22 - H) / 2), Size(W, H), false);
  applyStyle(f);
  f->frameProps().resizeable = false; f->frameProps().hasMaximizeButton = false; f->frameProps().hasMinimizeButton = false;
  Point o = clientOrigin(f);
  int x = o.X + 10, y = o.Y + 8;
  S(new uiLabel(f, prompt, Point(x, y), Size(W - 30, 18)));
  y += 22;
  auto e1 = S(new uiTextEdit(f, "", Point(x, y), Size(W - 30, 24)));
  e1->textEditProps().passwordMode = true;
  uiTextEdit * e2 = nullptr;
  if (twice) {
    y += 30;
    S(new uiLabel(f, "Type it again:", Point(x, y + 4), Size(100, 18)));
    e2 = S(new uiTextEdit(f, "", Point(x + 104, y), Size(W - 134, 24)));
    e2->textEditProps().passwordMode = true;
  }
  y += 34;
  auto ok = S(new uiButton(f, "OK", Point(x + W - 200, y), Size(80, 26)));
  auto cancel = S(new uiButton(f, "Cancel", Point(x + W - 112, y), Size(80, 26)));
  ok->onClick = [&]() { f->exitModal(1); };
  cancel->onClick = [&]() { f->exitModal(0); };
  f->onKeyUp = [&](uiKeyEventInfo const & k) {
    if (k.VK == VK_ESCAPE) f->exitModal(0);
    else if (k.VK == VK_RETURN || k.VK == VK_KP_ENTER) f->exitModal(1);
  };
  gApp->setFocusedWindow(e1);
  int r = gApp->showModalWindow(f);
  std::string a = e1->text() ? e1->text() : "", b = e2 && e2->text() ? e2->text() : "";
  gApp->destroyWindow(f);
  if (r != 1) return false;
  if (twice && a != b) { gApp->messageBox("Latch", "The two passwords don't match.", "OK", nullptr, nullptr, uiMessageBoxIcon::Warning); return false; }
  out = a;
  return true;
}

class LatchApp : public AppFrame {
public:
  static AppFrame * inst;
  LVault v;
  std::string path;
  bool dirty = false;
  int group = -1;                       // -1 = all (not the bin)
  std::vector<int> shown;               // list row -> entry index
  int sel = -1;                         // entry index
  uint32_t lastUse = 0;
  uiListBox * list;
  uiTextEdit * fTitle, * fUser, * fPass, * fUrl, * fNotes, * fExp, * search;
  uiButton * groupBtn, * showBtn, * entryGroupBtn;
  uiLabel * status, * nameLbl;
  int entryGroup = 0;

  LatchApp() : AppFrame("Latch", Point(4, 4), Size(632, 452), &inst) {
    button("Open", cx, cy, 50)->onClick = [this]() { openVault(); };
    button("New vault", cx + 54, cy, 76)->onClick = [this]() { newVault(); };
    button("Save", cx + 134, cy, 50)->onClick = [this]() { save(); };
    button("Lock", cx + 188, cy, 50)->onClick = [this]() { lock(); };
    nameLbl = label("No vault open", cx + 246, cy + 3, 360, 18);
    int y = cy + 30;
    groupBtn = button("All entries", cx, y, 160);
    groupBtn->onClick = [this]() { nextGroup(); };
    search = edit("", cx + 166, y, 80);
    search->onChange = [this]() { fill(); };
    list = S(new uiListBox(this, Point(cx, y + 28), Size(246, 300)));
    list->items().takeStrings();          // keep copies: the titles are built on the fly
    list->onChange = [this]() { int i = list->firstSelectedItem(); if (i >= 0 && i < (int)shown.size()) pick(shown[i]); };
    int rx = cx + 256, lw = 70, fw = 280;
    auto row = [&](const char * t, int yy) { label(t, rx, yy + 3, lw, 18); };
    row("Title", y); fTitle = edit("", rx + lw, y, fw);
    row("User", y + 30); fUser = edit("", rx + lw, y + 30, fw);
    row("Password", y + 60); fPass = edit("", rx + lw, y + 60, fw - 120);
    fPass->textEditProps().passwordMode = true;
    showBtn = button("Show", rx + lw + fw - 116, y + 60, 54);
    showBtn->onClick = [this]() { fPass->textEditProps().passwordMode = !fPass->textEditProps().passwordMode; showBtn->setText(fPass->textEditProps().passwordMode ? "Show" : "Hide"); showBtn->repaint(); fPass->repaint(); };
    button("New", rx + lw + fw - 58, y + 60, 58)->onClick = [this]() { makePassword(); };
    row("Website", y + 90); fUrl = edit("", rx + lw, y + 90, fw);
    row("Notes", y + 120); fNotes = edit("", rx + lw, y + 120, fw);
    row("Expires", y + 150); fExp = edit("", rx + lw, y + 150, 110);
    label("YYYY-MM-DD", rx + lw + 116, y + 154, 100, 16)->labelStyle().textFont = &FONT_std_12;
    row("Group", y + 180);
    entryGroupBtn = button("", rx + lw, y + 180, 160);
    entryGroupBtn->onClick = [this]() { if (!v.groups.empty()) { entryGroup = (entryGroup + 1) % v.groups.size(); showEntryGroup(); } };
    button("Add new", rx, y + 216, 80, 26)->onClick = [this]() { newEntry(); };
    button("Keep changes", rx + 86, y + 216, 106, 26)->onClick = [this]() { apply(); };
    button("Delete", rx + 198, y + 216, 70, 26)->onClick = [this]() { del(); };
    status = label("Open a .latch vault from your Latch app, or make a new one.", cx, cy + 394, 612, 16);
    status->labelStyle().textFont = &FONT_std_12;
    onTimer = [this](uiTimerHandle) { if (v.open && millis() - lastUse > 5 * 60 * 1000) { lock(); say("Locked after 5 minutes without use."); } };
    startTimer(5000);
    clearFields();
  }

  void say(const char * m) { status->setText(m); status->repaint(); }
  void touch() { lastUse = millis(); }
  bool inBin(const LEntry & e) { for (auto & g : v.groups) if (g.id == e.groupId) return g.bin; return false; }
  int binIndex() { for (size_t i = 0; i < v.groups.size(); ++i) if (v.groups[i].bin) return i; return -1; }

  void clearFields() {
    for (auto e : { fTitle, fUser, fPass, fUrl, fNotes, fExp }) { e->setText(""); e->repaint(); }
    sel = -1;
  }
  void wipe() {
    for (auto & e : v.entries) { for (auto & c : e.password) c = 0; }
    v = LVault();
    memset(v.key, 0, 32);
    shown.clear();
    list->items().clear(); list->repaint();
    clearFields();
  }
  void lock() {
    if (v.open && dirty) {
      auto r = gApp->messageBox("Latch", "Save your changes before locking?", "Save", "Don't save", "Cancel", uiMessageBoxIcon::Question);
      if (r == uiMessageBoxResult::Button3) return;
      if (r == uiMessageBoxResult::Button1 && !save()) return;
    }
    wipe(); dirty = false;
    nameLbl->setText(path.empty() ? "No vault open" : ("Locked: " + baseName(path)).c_str()); nameLbl->repaint();
    say("Locked. Open the vault again with your master password.");
  }

  void openVault() {
    if (v.open) lock();
    if (v.open) return;
    std::string p = askFile("Open vault (.latch)", "Open", "", path.empty() ? documentsDir() : dirOf(path));
    if (p.empty()) return;
    long sz = fileSize(p.c_str());
    if (sz <= 0 || (size_t)sz * 3 + 30000 > ramBiggest()) { gApp->messageBox("Latch", "That vault is too big for the free memory (close other apps).", "OK", nullptr, nullptr, uiMessageBoxIcon::Error); return; }
    std::string file;
    if (!readFileStr(p.c_str(), file, sz + 16)) { gApp->messageBox("Latch", "Couldn't read that file.", "OK", nullptr, nullptr, uiMessageBoxIcon::Error); return; }
    std::string fmt, saltB, ivB, dataB; long iter = 0;
    lj::obj({ file.data(), file.data() + file.size() }, [&](const std::string & k, const char * a, const char * b) {
      if (k == "format") fmt = lj::sval(a, b);
      else if (k == "kdf") lj::obj({ a, b }, [&](const std::string & kk, const char * x, const char * y) { if (kk == "iterations") iter = atol(std::string(x, y).c_str()); else if (kk == "salt") saltB = lj::sval(x, y); });
      else if (k == "cipher") lj::obj({ a, b }, [&](const std::string & kk, const char * x, const char * y) { if (kk == "iv") ivB = lj::sval(x, y); });
      else if (k == "data") dataB = lj::sval(a, b);
    });
    file.clear(); file.shrink_to_fit();
    std::vector<uint8_t> salt, iv, data;
    if (fmt != "latch-vault" || iter <= 0 || !b64dec(saltB, salt) || !b64dec(ivB, iv) || !b64dec(dataB, data) || iv.size() != 12 || salt.size() > 16 || data.size() < 16) {
      gApp->messageBox("Latch", "That isn't a Latch vault file.", "OK", nullptr, nullptr, uiMessageBoxIcon::Error); return;
    }
    dataB.clear(); dataB.shrink_to_fit();
    std::string pw;
    if (!askSecret("Unlock vault", ("Master password for " + baseName(p) + ":").c_str(), pw, false)) return;
    uint8_t key[32];
    if (!deriveKeyUI(pw, salt.data(), salt.size(), iter, key)) { gApp->messageBox("Latch", "Not enough memory to unlock.", "OK", nullptr, nullptr, uiMessageBoxIcon::Error); return; }
    for (auto & c : pw) c = 0;
    size_t n = data.size() - 16;
    std::string pt(n, 0);
    bool ok = gcmRun(false, key, iv.data(), data.data(), n, (uint8_t *)&pt[0], data.data() + n);
    data.clear(); data.shrink_to_fit();
    if (!ok) { memset(key, 0, 32); gApp->messageBox("Latch", "Wrong master password (or the file is damaged).", "OK", nullptr, nullptr, uiMessageBoxIcon::Error); return; }
    LVault nv;
    if (!parseVault(pt, nv)) { memset(key, 0, 32); gApp->messageBox("Latch", "The vault opened but its contents couldn't be read.", "OK", nullptr, nullptr, uiMessageBoxIcon::Error); return; }
    for (auto & c : pt) c = 0;
    v = std::move(nv);
    memcpy(v.key, key, 32); memset(key, 0, 32);
    memset(v.salt, 0, 16); memcpy(v.salt, salt.data(), salt.size());
    v.iter = iter; v.open = true;
    saltLen = salt.size();
    path = p; dirty = false; group = -1;
    opened();
  }
  size_t saltLen = 16;

  void opened() {
    touch();
    nameLbl->setText((v.name + "  (" + baseName(path.empty() ? std::string("not saved yet") : path) + ")").c_str()); nameLbl->repaint();
    groupBtn->setText("All entries"); groupBtn->repaint();
    fill();
    clearFields();
    char b[64]; snprintf(b, sizeof(b), "%d entries. Locks itself after 5 minutes without use.", (int)v.entries.size());
    say(b);
  }

  void newVault() {
    if (v.open) lock();
    if (v.open) return;
    std::string pw;
    if (!askSecret("New vault", "Pick a master password (it can't be recovered):", pw, true)) return;
    if (pw.size() < 8) { gApp->messageBox("Latch", "Use at least 8 characters.", "OK", nullptr, nullptr, uiMessageBoxIcon::Warning); return; }
    esp_fill_random(v.salt, 16); saltLen = 16;
    v.iter = 100000;                     // a new vault made here: 100,000 rounds (the PC Latch uses 600,000; it opens either)
    if (!deriveKeyUI(pw, v.salt, 16, v.iter, v.key)) { gApp->messageBox("Latch", "Not enough memory.", "OK", nullptr, nullptr, uiMessageBoxIcon::Error); return; }
    for (auto & c : pw) c = 0;
    v.name = "My Vault"; v.created = isoNow();
    v.settings = "{\"autoLockMin\":5,\"clipSec\":12,\"gen\":{\"length\":20,\"upper\":true,\"lower\":true,\"digits\":true,\"symbols\":true,\"similar\":false}}";
    const char * G[6][2] = { { "General", "\xF0\x9F\x93\x81" }, { "Email", "\xE2\x9C\x89\xEF\xB8\x8F" }, { "Banking", "\xF0\x9F\x8F\xA6" },
                             { "Internet", "\xF0\x9F\x8C\x90" }, { "Wi-Fi & Network", "\xF0\x9F\x93\xB6" }, { "Computers", "\xF0\x9F\x92\xBB" } };
    for (auto & g : G) { LGroup x; x.id = latchUid(); x.name = g[0]; x.icon = g[1]; v.groups.push_back(x); }
    LGroup bin; bin.id = latchUid(); bin.name = "Recycle Bin"; bin.icon = "\xF0\x9F\x97\x91\xEF\xB8\x8F"; bin.bin = true; v.groups.push_back(bin);
    v.open = true; path.clear(); dirty = true;
    opened();
    say("New vault. Add entries, then Save it (pick a name ending in .latch).");
  }

  bool save() {
    if (!v.open) return false;
    apply(true);
    if (path.empty()) {
      std::string p = askFile("Save vault", "Save", "vault.latch", documentsDir());
      if (p.empty()) return false;
      if (strcmp(fileExt(p.c_str()), "latch")) p += ".latch";
      path = p;
    }
    std::string pt = vaultJson(v);
    std::vector<uint8_t> ct(pt.size() + 16);
    uint8_t iv[12]; esp_fill_random(iv, 12);
    bool ok = gcmRun(true, v.key, iv, (const uint8_t *)pt.data(), pt.size(), ct.data(), ct.data() + pt.size());
    for (auto & c : pt) c = 0;
    pt.clear(); pt.shrink_to_fit();
    if (!ok) { gApp->messageBox("Latch", "Couldn't encrypt the vault.", "OK", nullptr, nullptr, uiMessageBoxIcon::Error); return false; }
    std::string data = b64enc(ct.data(), ct.size());
    ct.clear(); ct.shrink_to_fit();
    std::string head = "{\"format\":\"latch-vault\",\"version\":1,\"kdf\":{\"name\":\"PBKDF2-SHA256\",\"iterations\":" + std::to_string(v.iter) +
                       ",\"salt\":\"" + b64enc(v.salt, saltLen) + "\"},\"cipher\":{\"name\":\"AES-256-GCM\",\"iv\":\"" + b64enc(iv, 12) + "\"},\"data\":\"";
    ok = writeFileWith(path.c_str(), [&](RSink & o) { o.put(head); o.put(data); o.put("\"}"); });
    if (!ok) { gApp->messageBox("Latch", "Couldn't save. Is the SD card in?", "OK", nullptr, nullptr, uiMessageBoxIcon::Error); return false; }
    dirty = false;
    nameLbl->setText((v.name + "  (" + baseName(path) + ")").c_str()); nameLbl->repaint();
    say("Saved. The PC Latch app opens this file too.");
    return true;
  }

  void nextGroup() {
    if (!v.open) return;
    touch();
    group = group + 1 >= (int)v.groups.size() ? -1 : group + 1;
    groupBtn->setText(group < 0 ? "All entries" : v.groups[group].name.c_str()); groupBtn->repaint();
    fill();
  }

  static std::string lower(std::string s) { for (auto & c : s) c = tolower((unsigned char)c); return s; }
  void fill() {
    shown.clear();
    list->items().clear();
    if (!v.open) { list->repaint(); return; }
    std::string q = lower(search->text() ? search->text() : "");
    for (size_t i = 0; i < v.entries.size(); ++i) {
      auto & e = v.entries[i];
      if (group < 0 ? inBin(e) : e.groupId != v.groups[group].id) continue;
      if (!q.empty() && lower(e.title + " " + e.username + " " + e.url + " " + e.notes).find(q) == std::string::npos) continue;
      shown.push_back(i);
      std::string t = e.title.empty() ? "(no title)" : e.title;
      if (!e.expires.empty() && e.expires < isoNow().substr(0, 10)) t += "  (expired)";
      list->items().append(t.c_str());
    }
    list->repaint();
  }

  void showEntryGroup() {
    entryGroupBtn->setText(v.groups.empty() ? "" : v.groups[entryGroup].name.c_str()); entryGroupBtn->repaint();
  }
  void pick(int i) {
    touch();
    sel = i;
    auto & e = v.entries[i];
    fTitle->setText(e.title.c_str()); fUser->setText(e.username.c_str()); fPass->setText(e.password.c_str());
    fUrl->setText(e.url.c_str()); fNotes->setText(e.notes.c_str()); fExp->setText(e.expires.c_str());
    for (auto ed : { fTitle, fUser, fPass, fUrl, fNotes, fExp }) ed->repaint();
    entryGroup = 0;
    for (size_t g = 0; g < v.groups.size(); ++g) if (v.groups[g].id == e.groupId) entryGroup = g;
    showEntryGroup();
  }
  static std::string txt(uiTextEdit * e) { return e->text() ? e->text() : ""; }

  void newEntry() {
    if (!v.open) return;
    touch();
    clearFields();
    entryGroup = group >= 0 && !v.groups[group].bin ? group : 0;
    showEntryGroup();
    gApp->setFocusedWindow(fTitle);
    say("Fill in the boxes, then Keep changes.");
  }
  // keep what's in the boxes (quiet = from Save: only if something was typed)
  void apply(bool quiet = false) {
    if (!v.open) return;
    touch();
    std::string t = txt(fTitle), u = txt(fUser), pw = txt(fPass), url = txt(fUrl), n = txt(fNotes), ex = txt(fExp);
    if (sel < 0 && t.empty() && u.empty() && pw.empty()) { if (!quiet) say("Type a title first."); return; }
    std::string gid = v.groups.empty() ? "" : v.groups[entryGroup].id;
    if (sel >= 0) {
      auto & e = v.entries[sel];
      if (e.title == t && e.username == u && e.password == pw && e.url == url && e.notes == n && e.expires == ex && e.groupId == gid) return;
      if (!e.password.empty() && e.password != pw) {         // old password goes into the history, like the PC app
        std::string item = "{\"password\":" + lj::q(e.password) + ",\"changed\":" + lj::q(isoNow()) + "}";
        size_t b = e.history.find('[');
        bool empty = e.history.find_first_not_of(" \n\r\t[]") == std::string::npos;
        e.history = b == std::string::npos ? "[" + item + "]" : "[" + item + (empty ? "" : ",") + e.history.substr(b + 1);
        if (empty) e.history = "[" + item + "]";
      }
      e.title = t; e.username = u; e.password = pw; e.url = url; e.notes = n; e.expires = ex; e.groupId = gid; e.modified = isoNow();
    } else {
      LEntry e;
      e.id = latchUid(); e.groupId = gid; e.title = t; e.username = u; e.password = pw; e.url = url; e.notes = n; e.expires = ex;
      e.created = e.modified = isoNow();
      v.entries.push_back(e);
      sel = v.entries.size() - 1;
    }
    dirty = true;
    fill();
    if (!quiet) say("Kept. Save writes it to the vault file.");
  }

  void del() {
    if (!v.open || sel < 0) return;
    touch();
    auto & e = v.entries[sel];
    int bin = binIndex();
    if (bin >= 0 && e.groupId != v.groups[bin].id) {
      e.groupId = v.groups[bin].id; e.modified = isoNow();
      say("Moved to the Recycle Bin.");
    } else {
      if (gApp->messageBox("Latch", "Delete this entry for good?", "Delete", "Cancel", nullptr, uiMessageBoxIcon::Warning) != uiMessageBoxResult::Button1) return;
      for (auto & c : e.password) c = 0;
      v.entries.erase(v.entries.begin() + sel);
      say("Deleted.");
    }
    dirty = true;
    clearFields();
    fill();
  }

  void makePassword() {
    touch();
    static const char * SETS = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789!@#$%^&*-_=+?";
    int n = strlen(SETS);
    std::string p;
    for (int i = 0; i < 20; ++i) p += SETS[esp_random() % n];
    fPass->setText(p.c_str()); fPass->repaint();
    say("New 20-character password. Keep changes to use it.");
  }

  bool canClose() override {
    if (!v.open || !dirty) { wipe(); return true; }
    auto r = gApp->messageBox("Latch", "Save your changes first?", "Save", "Don't save", "Cancel", uiMessageBoxIcon::Question);
    if (r == uiMessageBoxResult::Button3) return false;
    if (r == uiMessageBoxResult::Button1 && !save()) return false;
    wipe();
    return true;
  }
};
AppFrame * LatchApp::inst = nullptr;

void openLatch()
{
  if (!LatchApp::inst) new LatchApp();
  LatchApp::inst->open();
}
