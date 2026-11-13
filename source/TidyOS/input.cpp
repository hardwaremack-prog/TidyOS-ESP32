// TidyOS input
//  - Bluetooth LE HID host (keyboards + mice) using NimBLE
//  - HID report descriptor parser so most BLE keyboards/mice "just work"
//  - software key repeat (BLE keyboards don't repeat on their own)
//  - USB serial keyboard for testing (type in the Serial Monitor at 115200)

#include <NimBLEDevice.h>
#include <mutex>
#include "input.h"
#include "core.h"

using namespace fabgl;

// ============================================================ HID report descriptor parsing

struct KbdFmt {
  bool     valid    = false;
  bool     hasMod   = false;
  uint16_t modOff   = 0;
  uint16_t keysOff  = 0;
  uint8_t  keysSize = 0;
  uint8_t  keysCount= 0;
  bool     bitmap   = false;   // NKRO style keyboards
  uint16_t bmOff    = 0;
  uint16_t bmMin    = 0;
  uint16_t bmCount  = 0;
};

struct MouseFmt {
  bool     valid    = false;
  bool     hasBtn   = false;
  uint16_t btnOff   = 0;
  uint8_t  btnCount = 0;
  uint16_t xOff = 0, yOff = 0, wOff = 0;
  uint8_t  xSize = 0, ySize = 0, wSize = 0;
  bool     hasWheel = false;
};

struct ReportFmt {
  uint8_t  id = 0;
  KbdFmt   kb;
  MouseFmt ms;
};

struct HidLayout {
  ReportFmt r[10];
  int n = 0;
  ReportFmt * get(uint8_t id, bool create) {
    for (int i = 0; i < n; ++i)
      if (r[i].id == id)
        return &r[i];
    if (!create || n >= 10)
      return nullptr;
    r[n].id = id;
    return &r[n++];
  }
};

static void hidParse(const uint8_t * d, size_t len, HidLayout & L)
{
  struct Glob { uint16_t page; uint32_t rsize, rcount; uint8_t rid; };
  Glob g = { 0, 0, 0, 0 };
  Glob stack[4];
  int  sp = 0;
  uint32_t usages[24];
  int      nus = 0;
  uint32_t umin = 0, umax = 0;
  bool     range = false;
  static uint16_t bitpos[256];
  memset(bitpos, 0, sizeof(bitpos));

  size_t i = 0;
  while (i < len) {
    uint8_t p = d[i++];
    if (p == 0xFE) {               // long item (never used by keyboards/mice)
      if (i + 1 >= len) break;
      i += 2 + d[i];
      continue;
    }
    int sz = p & 3;
    if (sz == 3) sz = 4;
    if (i + sz > len) break;
    uint32_t v = 0;
    for (int k = 0; k < sz; ++k)
      v |= (uint32_t)d[i + k] << (8 * k);
    i += sz;
    uint8_t type = (p >> 2) & 3;
    uint8_t tag  = p >> 4;

    if (type == 1) {               // global
      switch (tag) {
        case 0:  g.page = v; break;
        case 7:  g.rsize = v; break;
        case 8:  g.rid = v; break;
        case 9:  g.rcount = v; break;
        case 10: if (sp < 4) stack[sp++] = g; break;
        case 11: if (sp > 0) g = stack[--sp]; break;
        default: break;
      }
    } else if (type == 2) {        // local
      switch (tag) {
        case 0:
          if (nus < 24) usages[nus++] = (sz == 4) ? v : (((uint32_t)g.page << 16) | v);
          break;
        case 1: umin = v; range = true; break;
        case 2: umax = v; break;
        default: break;
      }
    } else if (type == 0) {        // main
      if (tag == 8) {              // INPUT
        uint16_t off   = bitpos[g.rid];
        bool isConst   = v & 1;
        bool isVar     = v & 2;
        if (!isConst && g.rsize > 0) {
          ReportFmt * R = L.get(g.rid, true);
          if (R) {
            // first usage of this item
            uint32_t first = range ? umin : (nus ? (usages[0] & 0xFFFF) : 0);
            uint16_t page  = (!range && nus && (usages[0] >> 16)) ? (usages[0] >> 16) : g.page;
            if (page == 0x07) {                                   // keyboard
              if (isVar) {
                if (first == 0xE0 && g.rsize == 1 && g.rcount <= 8) {
                  R->kb.hasMod = true; R->kb.modOff = off; R->kb.valid = true;
                } else if (g.rsize == 1 && g.rcount > 8) {
                  R->kb.bitmap = true; R->kb.bmOff = off; R->kb.bmMin = first; R->kb.bmCount = g.rcount; R->kb.valid = true;
                }
              } else {
                R->kb.keysOff = off; R->kb.keysSize = g.rsize; R->kb.keysCount = g.rcount; R->kb.valid = true;
              }
            } else if (page == 0x09 && isVar) {                     // buttons
              if (!R->ms.hasBtn) {
                R->ms.hasBtn = true; R->ms.btnOff = off; R->ms.btnCount = g.rcount;
              }
            } else if (page == 0x01 && isVar) {                     // generic desktop
              for (uint32_t j = 0; j < g.rcount; ++j) {
                uint32_t u = range ? (umin + j) : (nus ? (usages[j < (uint32_t)nus ? j : nus - 1] & 0xFFFF) : 0);
                uint16_t fo = off + j * g.rsize;
                if (u == 0x30)      { R->ms.xOff = fo; R->ms.xSize = g.rsize; R->ms.valid = true; }
                else if (u == 0x31) { R->ms.yOff = fo; R->ms.ySize = g.rsize; R->ms.valid = true; }
                else if (u == 0x38) { R->ms.wOff = fo; R->ms.wSize = g.rsize; R->ms.hasWheel = true; }
              }
            }
          }
        }
        bitpos[g.rid] += g.rsize * g.rcount;
      }
      nus = 0; range = false; umin = umax = 0;
    }
  }
}

static int32_t getBits(const uint8_t * d, size_t len, uint32_t off, uint8_t size, bool sgn)
{
  uint32_t v = 0;
  for (int b = 0; b < size && b < 32; ++b) {
    uint32_t bit = off + b;
    if ((bit >> 3) >= len)
      break;
    if ((d[bit >> 3] >> (bit & 7)) & 1)
      v |= 1u << b;
  }
  if (sgn && size > 0 && size < 32 && ((v >> (size - 1)) & 1))
    v |= ~0u << size;
  return (int32_t) v;
}

// ============================================================ key translation (HID usage -> FabGL)

struct KeyDef { VirtualKey vk, svk; uint8_t a, sa; };

static bool usageToKey(uint8_t u, bool shift, bool caps, bool ctrl, VirtualKey & vk, uint8_t & ascii)
{
  ascii = 0;
  if (u >= 0x04 && u <= 0x1D) {                      // letters
    bool up = shift ^ caps;
    vk    = (VirtualKey)((up ? VK_A : VK_a) + (u - 0x04));
    ascii = (up ? 'A' : 'a') + (u - 0x04);
    if (ctrl) ascii = (u - 0x04) + 1;                 // ctrl codes
    return true;
  }
  static const KeyDef digits[10] = {
    { VK_1, VK_EXCLAIM, '1', '!' }, { VK_2, VK_AT, '2', '@' }, { VK_3, VK_HASH, '3', '#' },
    { VK_4, VK_DOLLAR, '4', '$' },  { VK_5, VK_PERCENT, '5', '%' }, { VK_6, VK_CARET, '6', '^' },
    { VK_7, VK_AMPERSAND, '7', '&' }, { VK_8, VK_ASTERISK, '8', '*' }, { VK_9, VK_LEFTPAREN, '9', '(' },
    { VK_0, VK_RIGHTPAREN, '0', ')' },
  };
  if (u >= 0x1E && u <= 0x27) {
    auto & k = digits[u - 0x1E];
    vk = shift ? k.svk : k.vk; ascii = shift ? k.sa : k.a;
    return true;
  }
  static const KeyDef punct[] = {   // 0x2D .. 0x38
    { VK_MINUS, VK_UNDERSCORE, '-', '_' },         { VK_EQUALS, VK_PLUS, '=', '+' },
    { VK_LEFTBRACKET, VK_LEFTBRACE, '[', '{' },    { VK_RIGHTBRACKET, VK_RIGHTBRACE, ']', '}' },
    { VK_BACKSLASH, VK_VERTICALBAR, '\\', '|' },   { VK_BACKSLASH, VK_VERTICALBAR, '\\', '|' },
    { VK_SEMICOLON, VK_COLON, ';', ':' },          { VK_QUOTE, VK_QUOTEDBL, '\'', '"' },
    { VK_GRAVEACCENT, VK_TILDE, '`', '~' },        { VK_COMMA, VK_LESS, ',', '<' },
    { VK_PERIOD, VK_GREATER, '.', '>' },           { VK_SLASH, VK_QUESTION, '/', '?' },
  };
  if (u >= 0x2D && u <= 0x38) {
    auto & k = punct[u - 0x2D];
    vk = shift ? k.svk : k.vk; ascii = shift ? k.sa : k.a;
    return true;
  }
  if (u >= 0x3A && u <= 0x45) { vk = (VirtualKey)(VK_F1 + (u - 0x3A)); return true; }
  if (u >= 0x59 && u <= 0x61) { vk = (VirtualKey)(VK_KP_1 + (u - 0x59)); ascii = '1' + (u - 0x59); return true; }
  switch (u) {
    case 0x28: vk = VK_RETURN;    ascii = '\r'; return true;
    case 0x29: vk = VK_ESCAPE;    ascii = 0x1B; return true;
    case 0x2A: vk = VK_BACKSPACE; ascii = 0x08; return true;
    case 0x2B: vk = VK_TAB;       ascii = '\t'; return true;
    case 0x2C: vk = VK_SPACE;     ascii = ' ';  return true;
    case 0x39: vk = VK_CAPSLOCK;  return true;
    case 0x46: vk = VK_PRINTSCREEN; return true;
    case 0x47: vk = VK_SCROLLLOCK; return true;
    case 0x48: vk = VK_PAUSE;     return true;
    case 0x49: vk = VK_INSERT;    return true;
    case 0x4A: vk = VK_HOME;      return true;
    case 0x4B: vk = VK_PAGEUP;    return true;
    case 0x4C: vk = VK_DELETE;    ascii = 0x7F; return true;
    case 0x4D: vk = VK_END;       return true;
    case 0x4E: vk = VK_PAGEDOWN;  return true;
    case 0x4F: vk = VK_RIGHT;     return true;
    case 0x50: vk = VK_LEFT;      return true;
    case 0x51: vk = VK_DOWN;      return true;
    case 0x52: vk = VK_UP;        return true;
    case 0x53: vk = VK_NUMLOCK;   return true;
    case 0x54: vk = VK_KP_DIVIDE;   ascii = '/'; return true;
    case 0x55: vk = VK_KP_MULTIPLY; ascii = '*'; return true;
    case 0x56: vk = VK_KP_MINUS;    ascii = '-'; return true;
    case 0x57: vk = VK_KP_PLUS;     ascii = '+'; return true;
    case 0x58: vk = VK_KP_ENTER;    ascii = '\r'; return true;
    case 0x62: vk = VK_KP_0;        ascii = '0'; return true;
    case 0x63: vk = VK_KP_PERIOD;   ascii = '.'; return true;
    case 0x65: vk = VK_APPLICATION; return true;
    default: return false;
  }
}

static const VirtualKey s_modVK[8] = { VK_LCTRL, VK_LSHIFT, VK_LALT, VK_LGUI, VK_RCTRL, VK_RSHIFT, VK_RALT, VK_RGUI };

// ============================================================ posting events to the UI

static uiApp *                     s_app  = nullptr;
static Keyboard *                  s_kbd  = nullptr;
static BitmappedDisplayController *s_dc   = nullptr;
static volatile uint8_t            s_mods = 0;      // HID modifier bits (merged from all keyboards)
static volatile bool               s_caps = false;
static MouseStatus                 s_mouse;
static portMUX_TYPE                s_mux = portMUX_INITIALIZER_UNLOCKED;

// key repeat state
static volatile uint8_t  s_repUsage = 0;
static volatile int64_t  s_repNextUS = 0;

static void postKey(VirtualKey vk, uint8_t ascii, bool down)
{
  if (!s_app || !s_kbd)
    return;
  uint8_t m = s_mods;
  VirtualKeyItem item = {};
  item.vk       = vk;
  item.down     = down;
  item.ASCII    = ascii;
  item.CTRL     = (m & 0x11) != 0;
  item.LALT     = (m & 0x04) != 0;
  item.RALT     = (m & 0x40) != 0;
  item.SHIFT    = (m & 0x22) != 0;
  item.GUI      = (m & 0x88) != 0;
  item.CAPSLOCK = s_caps;
  s_kbd->injectVirtualKey(item, false);    // keeps Keyboard::isVKDown() correct (used by list boxes)

  uiEvent evt = uiEvent(nullptr, down ? UIEVT_KEYDOWN : UIEVT_KEYUP);
  evt.params.key.VK    = vk;
  evt.params.key.ASCII = ascii;
  evt.params.key.LALT  = item.LALT;
  evt.params.key.RALT  = item.RALT;
  evt.params.key.CTRL  = item.CTRL;
  evt.params.key.SHIFT = item.SHIFT;
  evt.params.key.GUI   = item.GUI;
  s_app->postEvent(&evt);
}

static void postUsage(uint8_t usage, bool down)
{
  VirtualKey vk;
  uint8_t ascii;
  uint8_t m = s_mods;
  if (!usageToKey(usage, (m & 0x22) != 0, s_caps, (m & 0x11) != 0, vk, ascii))
    return;
  if (usage == 0x39 && down)
    s_caps = !s_caps;
  if (down && usage == 0x29 && (m & 0x11) && (m & 0x22)) { gOpenTaskMgr = true; return; }   // Ctrl+Shift+Esc
  if (down && usage == 0x4C && (m & 0x11) && (m & 0x44)) { gOpenTaskMgr = true; return; }   // Ctrl+Alt+Del
  postKey(vk, ascii, down);
  if (down) {
    if (usage != 0x39) {
      s_repUsage  = usage;
      s_repNextUS = esp_timer_get_time() + 450000;
    }
  } else if (s_repUsage == usage) {
    s_repUsage = 0;
  }
}

static void postMouse(int dx, int dy, int wheel, uint8_t buttons)
{
  if (!s_app)
    return;
  MouseStatus prev, cur;
  portENTER_CRITICAL(&s_mux);
  prev = s_mouse;
  int x = s_mouse.X + dx;
  int y = s_mouse.Y + dy;
  s_mouse.X = x < 0 ? 0 : (x >= SCREEN_W ? SCREEN_W - 1 : x);
  s_mouse.Y = y < 0 ? 0 : (y >= SCREEN_H ? SCREEN_H - 1 : y);
  s_mouse.wheelDelta = wheel;
  s_mouse.buttons.left   = (buttons & 1) != 0;
  s_mouse.buttons.right  = (buttons & 2) != 0;
  s_mouse.buttons.middle = (buttons & 4) != 0;
  cur = s_mouse;
  portEXIT_CRITICAL(&s_mux);

#ifndef TIDYOS_HTML5      // HTML5: the browser draws its own pointer
  if (s_dc && (cur.X != prev.X || cur.Y != prev.Y))
    s_dc->setMouseCursorPos(cur.X, cur.Y);
#endif

  auto send = [&](uiEventID id, int changed) {
    uiEvent evt = uiEvent(nullptr, id);
    evt.params.mouse.status        = cur;
    evt.params.mouse.changedButton = changed;
    s_app->postEvent(&evt);
  };
  if (cur.X != prev.X || cur.Y != prev.Y) send(UIEVT_MOUSEMOVE, 0);
  if (wheel != 0)                           send(UIEVT_MOUSEWHEEL, 0);
  if (cur.buttons.left   != prev.buttons.left)   send(cur.buttons.left   ? UIEVT_MOUSEBUTTONDOWN : UIEVT_MOUSEBUTTONUP, 1);
  if (cur.buttons.middle != prev.buttons.middle) send(cur.buttons.middle ? UIEVT_MOUSEBUTTONDOWN : UIEVT_MOUSEBUTTONUP, 2);
  if (cur.buttons.right  != prev.buttons.right)  send(cur.buttons.right  ? UIEVT_MOUSEBUTTONDOWN : UIEVT_MOUSEBUTTONUP, 3);
}

// ============================================================ connections

struct Conn {
  NimBLEClient * client = nullptr;
  HidLayout      layout;
  bool           kbd = false, mouse = false;
  uint8_t        keys[16] = {0};
  int            nkeys = 0;
  uint8_t        mods = 0;
  volatile bool  dead = false;
  std::string    name;
};

static std::vector<Conn*> s_conns;
static std::mutex         s_lock;

struct Found {
  NimBLEAddress addr;
  std::string   name;
  bool kbd, mouse, bonded;
};
static std::vector<Found> s_found;
static volatile bool      s_pairMode = false;
static volatile bool      s_scanning = false;
static volatile int       s_pendingIdx = -1;     // index into s_found to connect to
static NimBLEAddress      s_pendingAddr;
static volatile bool      s_havePending = false;
static std::string        s_status = "No keyboard or mouse yet";
static int64_t            s_lastScanEndUS = 0;

static int countKind(bool kbd)
{
  std::lock_guard<std::mutex> lk(s_lock);
  int n = 0;
  for (auto c : s_conns) if (!c->dead && (kbd ? c->kbd : c->mouse)) ++n;
  return n;
}

// Accept brand-new keyboards/mice automatically when there is no keyboard yet
// (otherwise there would be no way to reach Settings), and for the first
// 5 minutes after boot while there is no mouse.
static bool autoAcceptNew()
{
  if (countKind(true) == 0) return true;
  if (countKind(false) == 0 && esp_timer_get_time() < 300000000LL) return true;
  return false;
}

static bool knownName(const std::string & n)
{
  if (n.empty()) return false;
  String names = ";" + gSet.btNames + ";";
  return names.indexOf((";" + String(n.c_str()) + ";")) >= 0;
}

static void rememberName(const std::string & n)
{
  if (n.empty() || knownName(n)) return;
  if (gSet.btNames.length()) gSet.btNames += ";";
  gSet.btNames += n.c_str();
  settingsSave();
}

static bool isConnectedTo(const NimBLEAddress & a)
{
  std::lock_guard<std::mutex> lk(s_lock);
  for (auto c : s_conns)
    if (!c->dead && c->client && c->client->getPeerAddress() == a)
      return true;
  return false;
}

static void handleKeyboard(Conn * c, const uint8_t * d, size_t len, const KbdFmt & f)
{
  uint8_t mods = f.hasMod ? (uint8_t)getBits(d, len, f.modOff, 8, false) : 0;
  uint8_t keys[16];
  int n = 0;
  if (f.bitmap) {
    for (uint16_t i = 0; i < f.bmCount && n < 16; ++i)
      if (getBits(d, len, f.bmOff + i, 1, false)) {
        uint16_t u = f.bmMin + i;
        if (u >= 4 && u < 0xE0) keys[n++] = u;
      }
  }
  for (int i = 0; i < f.keysCount && n < 16; ++i) {
    uint32_t u = getBits(d, len, f.keysOff + i * f.keysSize, f.keysSize, false);
    if (u == 1) return;                     // ErrorRollOver - ignore whole report
    if (u >= 4 && u < 0xE0) keys[n++] = (uint8_t)u;
  }
  // modifiers
  uint8_t changed = mods ^ c->mods;
  for (int b = 0; b < 8; ++b)
    if (changed & (1 << b)) {
      if (mods & (1 << b)) s_mods |= (1 << b); else s_mods &= ~(1 << b);
      postKey(s_modVK[b], 0, (mods >> b) & 1);
    }
  c->mods = mods;
  // released keys
  for (int i = 0; i < c->nkeys; ++i) {
    bool still = false;
    for (int j = 0; j < n; ++j) if (keys[j] == c->keys[i]) still = true;
    if (!still) postUsage(c->keys[i], false);
  }
  // pressed keys
  for (int j = 0; j < n; ++j) {
    bool was = false;
    for (int i = 0; i < c->nkeys; ++i) if (keys[j] == c->keys[i]) was = true;
    if (!was) postUsage(keys[j], true);
  }
  memcpy(c->keys, keys, n);
  c->nkeys = n;
}

static void handleMouse(const uint8_t * d, size_t len, const MouseFmt & f)
{
  uint8_t btn = f.hasBtn ? (uint8_t)getBits(d, len, f.btnOff, f.btnCount > 8 ? 8 : f.btnCount, false) : 0;
  int dx = getBits(d, len, f.xOff, f.xSize, true);
  int dy = getBits(d, len, f.yOff, f.ySize, true);
  int w  = f.hasWheel ? getBits(d, len, f.wOff, f.wSize, true) : 0;
  postMouse(dx, dy, -w, btn);
}

static void onReport(Conn * c, uint8_t rid, uint8_t * d, size_t len, int bootKind)
{
  if (bootKind == 1) {                // boot keyboard
    KbdFmt f; f.valid = true; f.hasMod = true; f.modOff = 0; f.keysOff = 16; f.keysSize = 8; f.keysCount = 6;
    handleKeyboard(c, d, len, f);
    return;
  }
  if (bootKind == 2) {                // boot mouse
    MouseFmt f; f.hasBtn = true; f.btnCount = 3; f.xOff = 8; f.xSize = 8; f.yOff = 16; f.ySize = 8;
    f.hasWheel = len >= 4; f.wOff = 24; f.wSize = 8;
    handleMouse(d, len, f);
    return;
  }
  ReportFmt * R = c->layout.get(rid, false);
  if (!R && c->layout.n == 1)
    R = &c->layout.r[0];
  if (R) {
    if (R->kb.valid) { handleKeyboard(c, d, len, R->kb); return; }
    if (R->ms.valid) { handleMouse(d, len, R->ms); return; }
    return;
  }
  // unknown layout: guess
  if (len == 8 && d[1] == 0) {
    onReport(c, rid, d, len, 1);
  } else if (len >= 3 && len <= 5) {
    onReport(c, rid, d, len, 2);
  }
}

class ClientCB : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient * client) override {
    std::lock_guard<std::mutex> lk(s_lock);
    for (auto c : s_conns)
      if (c->client == client)
        c->dead = true;
  }
  bool onConnParamsUpdateRequest(NimBLEClient *, const ble_gap_upd_params *) override { return true; }
};
static ClientCB s_clientCB;

static void connectTo(NimBLEAddress addr, std::string name)
{
  s_status = "Connecting to " + (name.empty() ? addr.toString() : name) + "...";
  NimBLEClient * cl = NimBLEDevice::createClient();
  if (!cl) { s_status = "Too many Bluetooth devices"; return; }
  cl->setClientCallbacks(&s_clientCB, false);
  cl->setConnectionParams(6, 12, 0, 400);
  cl->setConnectTimeout(8);
  if (!cl->connect(addr)) {
    NimBLEDevice::deleteClient(cl);
    s_status = "Could not connect - is it in pairing mode?";
    return;
  }
  cl->secureConnection();        // pair / encrypt (Just Works). Ignore failure: some devices don't need it.

  NimBLERemoteService * svc = cl->getService(NimBLEUUID((uint16_t)0x1812));
  if (!svc) {
    cl->disconnect();
    NimBLEDevice::deleteClient(cl);
    s_status = "That device is not a keyboard or mouse";
    return;
  }

  Conn * c = new Conn;
  c->client = cl;
  c->name   = name;

  NimBLERemoteCharacteristic * rmap = svc->getCharacteristic(NimBLEUUID((uint16_t)0x2A4B));
  if (rmap) {
    NimBLEAttValue v = rmap->readValue();
    if (v.length())
      hidParse(v.data(), v.length(), c->layout);
  }

  bool any = false;
  auto chars = svc->getCharacteristics(true);
  if (chars) {
    for (auto ch : *chars) {
      if (!(ch->getUUID() == NimBLEUUID((uint16_t)0x2A4D)) || !ch->canNotify())
        continue;
      uint8_t rid = 0, rtype = 1;
      NimBLERemoteDescriptor * ref = ch->getDescriptor(NimBLEUUID((uint16_t)0x2908));
      if (ref) {
        NimBLEAttValue dv = ref->readValue();
        if (dv.length() >= 2) { rid = dv.data()[0]; rtype = dv.data()[1]; }
      }
      if (rtype != 1)
        continue;
      ReportFmt * R = c->layout.get(rid, false);
      if (!R && c->layout.n == 1) R = &c->layout.r[0];
      if (R && !R->kb.valid && !R->ms.valid)
        continue;                                     // consumer keys etc.
      if (R && R->kb.valid) c->kbd = true;
      if (R && R->ms.valid) c->mouse = true;
      if (ch->subscribe(true, [c, rid](NimBLERemoteCharacteristic *, uint8_t * d, size_t l, bool) { onReport(c, rid, d, l, 0); }, true))
        any = true;
    }
  }
  if (!any) {
    // fall back to boot protocol
    NimBLERemoteCharacteristic * pm = svc->getCharacteristic(NimBLEUUID((uint16_t)0x2A4E));
    if (pm) { uint8_t zero = 0; pm->writeValue(&zero, 1, false); }
    NimBLERemoteCharacteristic * bk = svc->getCharacteristic(NimBLEUUID((uint16_t)0x2A22));
    NimBLERemoteCharacteristic * bm = svc->getCharacteristic(NimBLEUUID((uint16_t)0x2A33));
    if (bk && bk->canNotify() && bk->subscribe(true, [c](NimBLERemoteCharacteristic *, uint8_t * d, size_t l, bool) { onReport(c, 0, d, l, 1); }, true)) { any = true; c->kbd = true; }
    if (bm && bm->canNotify() && bm->subscribe(true, [c](NimBLERemoteCharacteristic *, uint8_t * d, size_t l, bool) { onReport(c, 0, d, l, 2); }, true)) { any = true; c->mouse = true; }
  }
  if (!any) {
    cl->disconnect();
    NimBLEDevice::deleteClient(cl);
    delete c;
    s_status = "Connected, but no key/mouse data found";
    return;
  }
  {
    std::lock_guard<std::mutex> lk(s_lock);
    s_conns.push_back(c);
  }
  rememberName(name);
  s_status = "Connected: " + (name.empty() ? std::string(c->kbd ? "keyboard" : "mouse") : name);
}

// ============================================================ scanning

class ScanCB : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice * dev) override {
    bool hidSvc = dev->isAdvertisingService(NimBLEUUID((uint16_t)0x1812));
    uint16_t ap = dev->haveAppearance() ? dev->getAppearance() : 0;
    bool hidAp  = (ap >= 0x03C0 && ap <= 0x03CF);
    NimBLEAddress a = dev->getAddress();
    bool bonded = NimBLEDevice::isBonded(a);
    std::string name = dev->haveName() ? dev->getName() : std::string();
    if (!hidSvc && !hidAp && !bonded)
      return;
    bool kbd   = ap == 0x03C1 || (name.find("eyboard") != std::string::npos) || (name.find("Keys") != std::string::npos);
    bool mouse = ap == 0x03C2 || (name.find("ouse") != std::string::npos);
    {
      std::lock_guard<std::mutex> lk(s_lock);
      bool found = false;
      for (auto & f : s_found)
        if (f.addr == a) {
          if (!name.empty()) f.name = name;
          found = true;
        }
      if (!found && s_found.size() < 16)
        s_found.push_back({ a, name, kbd, mouse, bonded });
    }
    bool want = bonded || knownName(name) || (hidSvc && autoAcceptNew());
    if (!s_pairMode && !s_havePending && want && !isConnectedTo(a)) {
      s_pendingAddr = a;
      s_havePending = true;
      NimBLEDevice::getScan()->stop();
    }
  }
};
static ScanCB s_scanCB;

static void scanEnded(NimBLEScanResults)
{
  s_scanning = false;
  s_pairMode = false;
  s_lastScanEndUS = esp_timer_get_time();
}

static void startScan(int seconds, bool pair)
{
  NimBLEScan * sc = NimBLEDevice::getScan();
  if (sc->isScanning()) sc->stop();     // fires scanEnded(), which resets the flags
  sc->clearResults();
  s_pairMode = pair;
  s_scanning = true;
  if (!sc->start(seconds, scanEnded, false)) {
    s_scanning = false;
    s_pairMode = false;
  }
}

// ============================================================ manager task

static void cleanupDead()
{
  std::vector<Conn*> dead;
  {
    std::lock_guard<std::mutex> lk(s_lock);
    for (auto it = s_conns.begin(); it != s_conns.end();) {
      if ((*it)->dead) { dead.push_back(*it); it = s_conns.erase(it); }
      else ++it;
    }
  }
  for (auto c : dead) {
    // release anything still held
    for (int i = 0; i < c->nkeys; ++i) postUsage(c->keys[i], false);
    for (int b = 0; b < 8; ++b) if (c->mods & (1 << b)) { s_mods &= ~(1 << b); postKey(s_modVK[b], 0, false); }
    s_repUsage = 0;
    NimBLEDevice::deleteClient(c->client);
    s_status = "Disconnected: " + (c->name.empty() ? std::string("device") : c->name);
    delete c;
  }
}

static int knownDevices()
{
  int names = 0;
  if (gSet.btNames.length()) {
    names = 1;
    for (int i = 0; i < (int)gSet.btNames.length(); ++i) if (gSet.btNames[i] == ';') ++names;
  }
  int b = NimBLEDevice::getNumBonds();
  return names > b ? names : b;
}

static void bleTask(void *)
{
  vTaskDelay(1500 / portTICK_PERIOD_MS);
  for (;;) {
    cleanupDead();

    if (s_pendingIdx >= 0) {
      int idx = s_pendingIdx;
      s_pendingIdx = -1;
      NimBLEAddress a;
      std::string n;
      bool ok = false;
      {
        std::lock_guard<std::mutex> lk(s_lock);
        if (idx < (int)s_found.size()) { a = s_found[idx].addr; n = s_found[idx].name; ok = true; }
      }
      if (ok && !isConnectedTo(a)) {
        NimBLEDevice::getScan()->stop();
        s_pairMode = false;
        connectTo(a, n);
      }
    }

    if (s_havePending) {
      NimBLEAddress a = s_pendingAddr;
      std::string n;
      {
        std::lock_guard<std::mutex> lk(s_lock);
        for (auto & f : s_found) if (f.addr == a) n = f.name;
      }
      s_havePending = false;
      vTaskDelay(150 / portTICK_PERIOD_MS);
      if (!isConnectedTo(a))
        connectTo(a, n);
    }

    // automatic reconnect of known devices
    int connected;
    {
      std::lock_guard<std::mutex> lk(s_lock);
      connected = s_conns.size();
    }
    if (!s_scanning && !s_pairMode && (connected < knownDevices() || autoAcceptNew()) && connected < 3 &&
        esp_timer_get_time() - s_lastScanEndUS > 6000000LL) {
      startScan(5, false);
    }
    vTaskDelay(200 / portTICK_PERIOD_MS);
  }
}

// key repeat task
static void repeatTask(void *)
{
  for (;;) {
    uint8_t u = s_repUsage;
    if (u && esp_timer_get_time() >= s_repNextUS) {
      VirtualKey vk; uint8_t ascii;
      uint8_t m = s_mods;
      if (usageToKey(u, (m & 0x22) != 0, s_caps, (m & 0x11) != 0, vk, ascii))
        postKey(vk, ascii, true);
      s_repNextUS = esp_timer_get_time() + 45000;
    }
    vTaskDelay(15 / portTICK_PERIOD_MS);
  }
}

// ============================================================ serial keyboard (for testing)

static void serialKey(VirtualKey vk, uint8_t ascii)
{
  postKey(vk, ascii, true);
  postKey(vk, ascii, false);
}

static void serialTask(void *)
{
  int esc = 0;
  for (;;) {
    while (Serial.available()) {
      int ch = Serial.read();
      if (esc == 1) { esc = (ch == '[') ? 2 : 0; continue; }
      if (esc == 2) {
        esc = 0;
        if (ch == 'A') serialKey(VK_UP, 0);
        else if (ch == 'B') serialKey(VK_DOWN, 0);
        else if (ch == 'C') serialKey(VK_RIGHT, 0);
        else if (ch == 'D') serialKey(VK_LEFT, 0);
        continue;
      }
      if (ch == 0x1B) { esc = 1; continue; }
      if (ch == '\n') continue;
      if (ch == '\r') { serialKey(VK_RETURN, '\r'); continue; }
      if (ch == 0x7F || ch == 0x08) { serialKey(VK_BACKSPACE, 0x08); continue; }
      if (ch == '\t') { serialKey(VK_TAB, '\t'); continue; }
      if (ch >= 'a' && ch <= 'z') { serialKey((VirtualKey)(VK_a + ch - 'a'), ch); continue; }
      if (ch >= 'A' && ch <= 'Z') { serialKey((VirtualKey)(VK_A + ch - 'A'), ch); continue; }
      if (ch >= '0' && ch <= '9') { serialKey((VirtualKey)(VK_0 + ch - '0'), ch); continue; }
      static const char  * pc  = " !@#$%^&*()-_=+[]{};:'\",.<>/?\\|`~";
      static const VirtualKey pvk[] = { VK_SPACE, VK_EXCLAIM, VK_AT, VK_HASH, VK_DOLLAR, VK_PERCENT, VK_CARET, VK_AMPERSAND,
        VK_ASTERISK, VK_LEFTPAREN, VK_RIGHTPAREN, VK_MINUS, VK_UNDERSCORE, VK_EQUALS, VK_PLUS, VK_LEFTBRACKET, VK_RIGHTBRACKET,
        VK_LEFTBRACE, VK_RIGHTBRACE, VK_SEMICOLON, VK_COLON, VK_QUOTE, VK_QUOTEDBL, VK_COMMA, VK_PERIOD, VK_LESS, VK_GREATER,
        VK_SLASH, VK_QUESTION, VK_BACKSLASH, VK_VERTICALBAR, VK_GRAVEACCENT, VK_TILDE };
      const char * p = strchr(pc, ch);
      if (p && ch) serialKey(pvk[p - pc], ch);
    }
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

// ============================================================ public API

// ============================================================ browser keyboard + mouse (HTML5 edition)

static volatile uint8_t s_webMods = 0;

void inputWebKey(uint8_t usage, uint8_t mods, bool down)
{
  uint8_t changed = mods ^ s_webMods;
  for (int b = 0; b < 8; ++b)
    if (changed & (1 << b)) {
      if (mods & (1 << b)) s_mods |= (1 << b); else s_mods &= ~(1 << b);
      postKey(s_modVK[b], 0, (mods >> b) & 1);
    }
  s_webMods = mods;
  if (usage >= 4 && usage < 0xE0) postUsage(usage, down);
}

void inputWebMouse(int x, int y, int wheel, uint8_t buttons)
{
  postMouse(x - s_mouse.X, y - s_mouse.Y, wheel, buttons);
}

void inputWebReleaseAll()
{
  inputWebKey(0, 0, false);
  s_repUsage = 0;
  if (s_mouse.buttons.left || s_mouse.buttons.right || s_mouse.buttons.middle) postMouse(0, 0, 0, 0);
}

#ifdef TIDYOS_HTML5
static const bool s_bleOn = false;
#else
static const bool s_bleOn = true;
#endif

void inputBegin()
{
  s_mouse.X = SCREEN_W / 2;
  s_mouse.Y = SCREEN_H / 2;
  if (!s_bleOn) { s_status = "Keyboard and mouse: your web browser"; return; }
  NimBLEDevice::init("TidyOS");
  NimBLEDevice::setSecurityAuth(true, false, true);              // bond, no MITM, secure connections
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);       // "Just Works" pairing
  NimBLEScan * sc = NimBLEDevice::getScan();
  sc->setAdvertisedDeviceCallbacks(&s_scanCB, true);
  sc->setActiveScan(true);
  sc->setInterval(100);
  sc->setWindow(60);
  s_mouse.X = SCREEN_W / 2;
  s_mouse.Y = SCREEN_H / 2;
}

void inputAttach(uiApp * app, Keyboard * kbd, BitmappedDisplayController * dc)
{
  s_kbd = kbd;
  s_dc  = dc;
  s_app = app;
  static bool started = false;
  if (!started) {
    started = true;
    if (s_bleOn) xTaskCreatePinnedToCore(bleTask, "ble", 6144, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(repeatTask, "keyrep", 2048, nullptr, 4, nullptr, 0);
    xTaskCreatePinnedToCore(serialTask, "serkbd", 2048, nullptr, 2, nullptr, 0);
  }
#ifdef TIDYOS_HTML5
  if (dc) dc->setMouseCursorPos(-100, -100);       // keep FabGL's pointer off screen
#else
  if (dc) dc->setMouseCursorPos(s_mouse.X, s_mouse.Y);
#endif
}

void inputStartPairScan(int seconds)
{
  if (!s_bleOn) { s_status = "Bluetooth is off in the HTML5 edition - your browser is the keyboard and mouse"; return; }
  {
    std::lock_guard<std::mutex> lk(s_lock);
    s_found.clear();
  }
  s_status = "Looking for keyboards and mice...";
  startScan(seconds, true);
}

bool inputScanning() { return s_scanning; }

int inputFoundCount()
{
  std::lock_guard<std::mutex> lk(s_lock);
  return s_found.size();
}

bool inputGetFound(int i, FoundDev & out)
{
  std::lock_guard<std::mutex> lk(s_lock);
  if (i < 0 || i >= (int)s_found.size()) return false;
  auto & f = s_found[i];
  out.name   = f.name.empty() ? f.addr.toString() : f.name;
  out.addr   = f.addr.toString();
  out.isKbd  = f.kbd;
  out.isMouse= f.mouse;
  out.bonded = f.bonded;
  return true;
}

void inputPair(int i) { if (s_bleOn) s_pendingIdx = i; }

void inputForgetAll()
{
  if (!s_bleOn) { s_status = "Bluetooth is off in the HTML5 edition - your browser is the keyboard and mouse"; return; }
  {
    std::lock_guard<std::mutex> lk(s_lock);
    for (auto c : s_conns) if (c->client) c->client->disconnect();
  }
  NimBLEDevice::deleteAllBonds();
  gSet.btNames = "";
  settingsSave();
  s_status = "Forgot all Bluetooth devices";
}

#include "screen.h"
int inputKeyboards() { return s_bleOn ? countKind(true) : (screenViewer() ? 1 : 0); }
int inputMice()      { return s_bleOn ? countKind(false) : (screenViewer() ? 1 : 0); }

std::string inputStatus()
{
  if (!s_bleOn) {
    if (screenViewer()) return "Browser connected";
    std::string u = screenUrl();
    return u.empty() ? std::string("Waiting for Wi-Fi...") : "Open " + u + " in a web browser";
  }
  return s_status;
}
