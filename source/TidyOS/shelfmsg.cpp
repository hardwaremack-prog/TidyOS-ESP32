// TidyOS <-> The Shelf messaging (see shelfmsg.h)
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <mutex>
#include <map>
#include <algorithm>
#include <time.h>
#include "stream.h"
#include "shelfmsg.h"
#include "core.h"

// ====================================================================== shared state

static std::recursive_mutex s_lock;
#define LOCK std::lock_guard<std::recursive_mutex> lockGuard_(s_lock)
static volatile uint32_t s_version = 1;
static void bump() { ++s_version; }

static Preferences s_mp;
static std::string s_myId, s_myName, s_instance;
static const uint16_t DISCOVERY_PORT = 41234, MY_PORT = 8080;

static uint64_t nowMs()
{
  time_t t = time(nullptr);
  return t > 1600000000 ? (uint64_t)t * 1000ULL + (millis() % 1000) : 0;
}

// ---------------------------------------------------------------- tiny JSON helpers (flat objects)
static std::string jsonEsc(const std::string & s)
{
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if (c < 32) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
    else o += c;
  }
  return o;
}

// value of "key" in a flat JSON object: strings are unescaped (non-ASCII -> close ASCII or '?'),
// numbers / true / false returned as text. false when missing.
static bool jsonGet(const std::string & j, const char * key, std::string & out)
{
  std::string k = std::string("\"") + key + "\"";
  size_t p = 0;
  while ((p = j.find(k, p)) != std::string::npos) {
    size_t q = p + k.size();
    while (q < j.size() && isspace((unsigned char)j[q])) ++q;
    if (q < j.size() && j[q] == ':') { p = q + 1; break; }
    p = q;
  }
  if (p == std::string::npos) return false;
  while (p < j.size() && isspace((unsigned char)j[p])) ++p;
  if (p >= j.size()) return false;
  out.clear();
  if (j[p] != '"') {
    size_t e = p;
    while (e < j.size() && j[e] != ',' && j[e] != '}' && !isspace((unsigned char)j[e])) ++e;
    out = j.substr(p, e - p);
    return out != "null";
  }
  for (size_t i = p + 1; i < j.size(); ++i) {
    char c = j[i];
    if (c == '"') return true;
    if (c != '\\') { out += c; continue; }
    if (++i >= j.size()) break;
    char e = j[i];
    if (e == 'n') out += '\n';
    else if (e == 't') out += ' ';
    else if (e == 'r') {}
    else if (e == 'u' && i + 4 < j.size()) {
      unsigned v = strtoul(j.substr(i + 1, 4).c_str(), nullptr, 16);
      i += 4;
      if (v < 128) out += (char)v;
      else if (v == 0x2018 || v == 0x2019) out += '\'';
      else if (v == 0x201C || v == 0x201D) out += '"';
      else if (v == 0x2013 || v == 0x2014) out += '-';
      else if (v >= 0xD800 && v <= 0xDBFF) { if (i + 6 < j.size() && j[i + 1] == '\\') i += 6; out += '?'; }   // emoji
      else out += '?';
    } else out += e;
  }
  return true;
}

static std::string toAscii(const std::string & s)
{
  // UTF-8 from other Shelves -> ASCII for the VGA font
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = s[i];
    if (c < 128) { o += (char)c; continue; }
    int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
    uint32_t cp = c & (0x3F >> extra);
    for (int k = 0; k < extra && i + 1 < s.size(); ++k) cp = (cp << 6) | (s[++i] & 0x3F);
    if (cp == 0x2018 || cp == 0x2019) o += '\'';
    else if (cp == 0x201C || cp == 0x201D) o += '"';
    else if (cp == 0x2013 || cp == 0x2014) o += '-';
    else if (cp == 0x2026) o += "...";
    else if (cp >= 0x1F300) o += ":)";
    else o += '?';
  }
  return o;
}

static bool validId(const std::string & id)
{
  if (id.empty() || id.size() > 100) return false;
  for (char c : id) if (!isalnum((unsigned char)c) && c != '-') return false;
  return true;
}

static std::string uuid4()
{
  uint8_t b[16];
  for (int i = 0; i < 16; i += 4) { uint32_t r = esp_random(); memcpy(b + i, &r, 4); }
  b[6] = (b[6] & 0x0F) | 0x40; b[8] = (b[8] & 0x3F) | 0x80;
  char s[40];
  snprintf(s, sizeof(s), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return s;
}

// ====================================================================== Shelf network: peers + conversations

struct Peer {
  std::string id, name, ip, status = "online";
  int port = 8080;
  uint32_t seenMs = 0;
  bool live = false;
  int unread = 0;
};
static std::vector<Peer> s_peers;
static bool s_peersDirty = false;
static std::string s_notice, s_status = "Starting...";
static void setStatus(const std::string & st) { LOCK; s_status = st; }

static bool online(const Peer & p) { return p.live && millis() - p.seenMs < 15000; }

static std::string presenceJson(const char * type)
{
  LOCK;
  return std::string("{\"type\":\"") + type + "\",\"id\":\"" + s_myId + "\",\"name\":\"" + jsonEsc(s_myName) +
         "\",\"status\":\"online\",\"httpPort\":" + std::to_string(MY_PORT) + ",\"instance\":\"" + s_instance + "\"}";
}

static Peer * findPeer(const std::string & id)
{
  for (auto & p : s_peers) if (p.id == id) return &p;
  return nullptr;
}

static void savePeers()
{
  std::string s;
  {
    LOCK;
    for (auto & p : s_peers) s += p.id + "|" + p.name + "|" + p.ip + "|" + std::to_string(p.port) + "\n";
    s_peersDirty = false;
  }
  writeFileStr(dataPath("msg-peers.txt").c_str(), s);
}

static void loadPeers()
{
  std::string s;
  if (!readFileStr(dataPath("msg-peers.txt").c_str(), s)) return;
  size_t st = 0;
  for (size_t i = 0; i <= s.size(); ++i)
    if (i == s.size() || s[i] == '\n') {
      std::string l = s.substr(st, i - st);
      st = i + 1;
      size_t a = l.find('|'), b = l.find('|', a + 1), c = l.find('|', b + 1);
      if (a == std::string::npos || b == std::string::npos || c == std::string::npos) continue;
      Peer p;
      p.id = l.substr(0, a); p.name = l.substr(a + 1, b - a - 1); p.ip = l.substr(b + 1, c - b - 1); p.port = atoi(l.c_str() + c + 1);
      if (validId(p.id) && p.id != s_myId && s_peers.size() < 40) s_peers.push_back(p);
    }
}

static void notePeer(const std::string & json, const std::string & ip)
{
  std::string id, name, status, port, inst;
  if (!jsonGet(json, "id", id) || !validId(id) || id == s_myId) return;
  if (jsonGet(json, "instance", inst) && inst == s_instance) return;
  jsonGet(json, "name", name);
  jsonGet(json, "status", status);
  int prt = jsonGet(json, "httpPort", port) ? atoi(port.c_str()) : 8080;
  if (prt <= 0 || prt > 65535) prt = 8080;
  LOCK;
  Peer * p = findPeer(id);
  if (!p) {
    if (s_peers.size() >= 40) s_peers.erase(s_peers.begin());
    s_peers.push_back(Peer());
    p = &s_peers.back();
    p->id = id;
    s_peersDirty = true;
  }
  std::string nm = toAscii(name.empty() ? "Unknown" : name).substr(0, 60);
  if (p->name != nm || p->ip != ip || p->port != prt) s_peersDirty = true;
  bool wasOnline = online(*p);
  p->name = nm; p->ip = ip; p->port = prt;
  p->status = status.empty() ? "online" : status;
  p->seenMs = millis();
  p->live = p->status != "offline";
  if (!wasOnline) bump();
}

static std::string convPath(const std::string & id) { return dataPath(("msg-" + id.substr(0, 36) + ".txt").c_str()); }

static void appendConv(const std::string & id, char kind, uint64_t t, const std::string & text)
{
  std::string line;
  line += kind; line += '|'; line += std::to_string((unsigned long long)t); line += '|';
  for (char c : text) line += (c == '\n') ? '\x1F' : c;            // keep one message per line
  line += '\n';
  FILE * f = fopen(convPath(id).c_str(), "ab");
  if (f) { fwrite(line.data(), 1, line.size(), f); fclose(f); }
}

// ---------------------------------------------------------------- outgoing queue (sent by the background task)
struct Outgoing { std::string peerId, text; uint64_t t; };
static std::vector<Outgoing> s_out;

static bool httpPost(const std::string & ip, int port, const char * path, const std::string & body, std::string * reply, uint32_t timeoutMs)
{
  WiFiClient c;
  if (!c.connect(ip.c_str(), port, timeoutMs)) return false;
  std::string req = std::string("POST ") + path + " HTTP/1.1\r\nHost: " + ip + ":" + std::to_string(port) +
                    "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  c.write((const uint8_t *)req.data(), req.size());
  gNetTx += req.size();
  std::string resp;
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs + 2000 && (c.connected() || c.available())) {
    while (c.available() && resp.size() < 3000) resp += (char)c.read();
    delay(5);
  }
  c.stop();
  gNetRx += resp.size();
  int code = 0;
  size_t sp = resp.find(' ');
  if (sp != std::string::npos) code = atoi(resp.c_str() + sp + 1);
  if (reply) {
    size_t he = resp.find("\r\n\r\n");
    *reply = he == std::string::npos ? "" : resp.substr(he + 4);
  }
  return code >= 200 && code < 300;
}

static bool probe(const std::string & ip, int port, uint32_t timeoutMs)
{
  std::string reply;
  if (!httpPost(ip, port, "/api/messaging/hello", presenceJson("shelf-presence"), &reply, timeoutMs)) return false;
  // the reply may be chunked: find the JSON object
  size_t a = reply.find('{'), b = reply.rfind('}');
  if (a == std::string::npos || b == std::string::npos) return false;
  std::string j = reply.substr(a, b - a + 1), type;
  if (!jsonGet(j, "type", type) || type != "shelf-hello") return false;
  notePeer(j, ip);
  return true;
}

// ---------------------------------------------------------------- tiny HTTP server (port 8080)
// Reads a request body but leaves out any "image" value, so a big picture sent from a
// Shelf can't use up the ESP32's memory. The text is kept and "[picture]" is noted.
static bool readBodyNoImages(WiFiClient & c, long len, std::string & body, bool & hadImage)
{
  hadImage = false;
  enum { NORMAL, IN_STR, IN_STR_ESC, SKIP_STR, SKIP_STR_ESC } st = NORMAL;
  std::string lastStr, cur;
  bool expectValueOfImage = false;
  uint32_t t0 = millis();
  long got = 0;
  while (got < len && millis() - t0 < 8000) {
    int ch = c.read();
    if (ch < 0) { delay(2); continue; }
    ++got;
    char x = (char)ch;
    switch (st) {
      case NORMAL:
        if (x == '"') {
          if (expectValueOfImage) { st = SKIP_STR; hadImage = true; body += "\"[picture]"; break; }
          st = IN_STR; cur.clear();
        } else if (x == ':') {
          expectValueOfImage = (lastStr == "image");
        } else if (!isspace((unsigned char)x)) {
          expectValueOfImage = false;
        }
        if (st != SKIP_STR && body.size() < 12000) body += x;
        break;
      case IN_STR:
        if (body.size() < 12000) body += x;
        if (x == '\\') st = IN_STR_ESC;
        else if (x == '"') { st = NORMAL; lastStr = cur; }
        else if (cur.size() < 16) cur += x;
        break;
      case IN_STR_ESC:
        if (body.size() < 12000) body += x;
        st = IN_STR;
        break;
      case SKIP_STR:
        if (x == '\\') st = SKIP_STR_ESC;
        else if (x == '"') { st = NORMAL; body += '"'; expectValueOfImage = false; lastStr.clear(); }
        break;
      case SKIP_STR_ESC:
        st = SKIP_STR;
        break;
    }
  }
  return got >= len;
}

static void reply(WiFiClient & c, int code, const std::string & json)
{
  std::string r = "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK" : " Error") +
                  "\r\nContent-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nContent-Length: " + std::to_string(json.size()) +
                  "\r\nConnection: close\r\n\r\n" + json;
  c.write((const uint8_t *)r.data(), r.size());
}

static WiFiServer s_server(MY_PORT);
static bool s_serverUp = false;

static void handleHttp()
{
  WiFiClient c = s_server.available();
  if (!c) return;
  c.setTimeout(3);
  std::string line, method, path;
  long len = 0;
  uint32_t t0 = millis();
  bool first = true;
  // request line + headers
  while (millis() - t0 < 3000) {
    if (!c.available()) { if (!c.connected()) break; delay(2); continue; }
    char ch = c.read();
    if (ch == '\r') continue;
    if (ch != '\n') { if (line.size() < 300) line += ch; continue; }
    if (first) {
      size_t a = line.find(' '), b = line.find(' ', a + 1);
      if (a != std::string::npos) { method = line.substr(0, a); path = line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1); }
      first = false;
    } else if (line.empty()) {
      break;
    } else if (strncasecmp(line.c_str(), "content-length:", 15) == 0) {
      len = atol(line.c_str() + 15);
    }
    line.clear();
  }
  std::string ip = c.remoteIP().toString().c_str();
  std::string body;
  bool hadImage = false;
  if (len > 0) readBodyNoImages(c, std::min(len, 4000000L), body, hadImage);
  gNetRx += len + line.size();

  if (method == "OPTIONS") { reply(c, 200, "{}"); }
  else if (path == "/api/messaging/identity") {
    reply(c, 200, "{\"id\":\"" + s_myId + "\",\"name\":\"" + jsonEsc(s_myName) + "\",\"status\":\"online\"}");
  } else if (path == "/api/messaging/hello" && method == "POST") {
    std::string type;
    if (jsonGet(body, "type", type) && type == "shelf-presence") notePeer(body, ip);
    reply(c, 200, presenceJson("shelf-hello"));
  } else if (path == "/api/messaging/receive" && method == "POST") {
    std::string from, fromName, fromPort, text;
    jsonGet(body, "fromId", from);
    jsonGet(body, "fromName", fromName);
    bool hasText = jsonGet(body, "text", text) && !text.empty();
    if (!validId(from) || (!hasText && !hadImage)) { reply(c, 400, "{\"error\":\"Invalid message\"}"); }
    else {
      text = toAscii(text).substr(0, 5000);
      if (hadImage) text = text.empty() ? "[picture - open The Shelf to see it]" : text + "  [picture]";
      std::string tsS;
      uint64_t t = jsonGet(body, "timestamp", tsS) ? strtoull(tsS.c_str(), nullptr, 10) : nowMs();
      if (jsonGet(body, "fromPort", fromPort)) {
        std::string info = "{\"id\":\"" + from + "\",\"name\":\"" + jsonEsc(fromName) + "\",\"httpPort\":" + std::to_string(atoi(fromPort.c_str())) + "}";
        notePeer(info, ip);
      }
      appendConv(from, 'I', t, text);
      {
        LOCK;
        Peer * p = findPeer(from);
        if (!p) { s_peers.push_back(Peer()); p = &s_peers.back(); p->id = from; p->name = toAscii(fromName.empty() ? "Unknown" : fromName); s_peersDirty = true; }
        p->unread++;
        s_notice = "New message from " + p->name;
      }
      bump();
      reply(c, 200, "{\"ok\":true}");
    }
  } else if (path == "/" || path == "/index.html") {
    std::string h = "<html><body style='font-family:sans-serif'><h2>TidyOS on ESP32</h2><p>" + s_myName +
                    " is online for The Shelf Messages.</p></body></html>";
    std::string r = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + std::to_string(h.size()) + "\r\nConnection: close\r\n\r\n" + h;
    c.write((const uint8_t *)r.data(), r.size());
  } else {
    reply(c, 404, "{\"error\":\"Not found\"}");
  }
  c.flush();
  c.stop();
}

// ====================================================================== Coms mesh (ESP-NOW, same packets as the Shelf boards)

static const uint8_t MAGIC0 = 'S', MAGIC1 = '9';
static const uint8_t T_TEXT = 1, T_SYNC_REQ = 2, T_REPLAY = 3, T_RECEIPT = 4, T_HELLO = 5, T_WHO = 8;
static const size_t  MAX_TEXT = 180, MAX_NAME = 16;
static const uint8_t HOPS = 255;
static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static bool      s_nowUp = false;
static uint32_t  s_myNode = 0;
static uint32_t  s_seen[512];
static int       s_seenPos = 0;
static const size_t COMS_KEEP = 40;          // mesh lines kept in RAM (all of them stay in coms-log.txt)
static std::vector<ChatLine> s_coms;
static std::vector<uint32_t> s_comsIds;     // message id for each s_coms entry
static int       s_comsUnread = 0;
static volatile int s_nowSent = 0, s_nowHeard = 0;
struct Member { uint32_t node; std::string name; uint32_t atMs; int hops; };
static std::vector<Member> s_members;
static uint32_t  s_lastWhoAnswer = 0, s_lastSyncAsk = 0, s_lastHello = 0, s_bootMs = 0;

struct NowPkt { uint8_t len; uint8_t buf[250]; };
static NowPkt s_ring[8];
static volatile uint8_t s_rw = 0, s_rr = 0;
static portMUX_TYPE s_ringMux = portMUX_INITIALIZER_UNLOCKED;

struct Tx { uint8_t buf[250]; uint8_t len; uint32_t due; };
static std::vector<Tx> s_tx;

static bool wasSeen(uint32_t id) { for (uint32_t s : s_seen) if (s == id) return true; return false; }
static void markSeen(uint32_t id) { s_seen[s_seenPos] = id; s_seenPos = (s_seenPos + 1) % 512; }

static void onNowRecv(const uint8_t *, const uint8_t * data, int len)
{
  if (len < 8 || len > 250 || data[0] != MAGIC0 || data[1] != MAGIC1) return;
  portENTER_CRITICAL(&s_ringMux);
  if ((uint8_t)(s_rw - s_rr) < 8) { NowPkt & p = s_ring[s_rw % 8]; p.len = len; memcpy(p.buf, data, len); s_rw++; }
  portEXIT_CRITICAL(&s_ringMux);
}

static void queueTx(const uint8_t * b, size_t len, uint32_t delayMs)
{
  if (len > 250) return;
  Tx t;
  memcpy(t.buf, b, len);
  t.len = len;
  t.due = millis() + delayMs;
  LOCK;
  if (s_tx.size() < 16) s_tx.push_back(t);
}

static size_t buildText(uint8_t * b, uint32_t id, uint32_t origin, uint8_t hopsLeft, uint8_t hopsTaken, const std::string & name, const std::string & text)
{
  size_t n = 0;
  b[n++] = MAGIC0; b[n++] = MAGIC1; b[n++] = T_TEXT; b[n++] = hopsLeft; b[n++] = hopsTaken;
  memcpy(b + n, &id, 4); n += 4; memcpy(b + n, &origin, 4); n += 4; memcpy(b + n, &s_myNode, 4); n += 4;
  size_t nl = std::min(name.size(), MAX_NAME); b[n++] = nl; memcpy(b + n, name.data(), nl); n += nl;
  size_t tl = std::min(text.size(), MAX_TEXT); memcpy(b + n, text.data(), tl); n += tl;
  return n;
}

static size_t buildReceipt(uint8_t * b, uint32_t rid, uint32_t receiver, uint8_t hopsLeft, uint8_t hopsTaken, uint32_t msg, const std::string & name)
{
  size_t n = 0;
  b[n++] = MAGIC0; b[n++] = MAGIC1; b[n++] = T_RECEIPT; b[n++] = hopsLeft; b[n++] = hopsTaken;
  memcpy(b + n, &rid, 4); n += 4; memcpy(b + n, &receiver, 4); n += 4; memcpy(b + n, &s_myNode, 4); n += 4;
  memcpy(b + n, &msg, 4); n += 4;
  size_t nl = std::min(name.size(), MAX_NAME); b[n++] = nl; memcpy(b + n, name.data(), nl); n += nl;
  return n;
}

static void sendHello()
{
  uint8_t b[64];
  uint32_t id = esp_random();
  markSeen(id);
  size_t n = 0;
  b[n++] = MAGIC0; b[n++] = MAGIC1; b[n++] = T_HELLO; b[n++] = HOPS; b[n++] = 0;
  memcpy(b + n, &id, 4); n += 4; memcpy(b + n, &s_myNode, 4); n += 4; memcpy(b + n, &s_myNode, 4); n += 4;
  b[n++] = 32;      // board type: plain ESP32
  b[n++] = 255;     // battery unknown
  b[n++] = 0;       // flags
  std::string nm = s_myName.substr(0, MAX_NAME);
  b[n++] = nm.size(); memcpy(b + n, nm.data(), nm.size()); n += nm.size();
  queueTx(b, n, 0);
  s_lastHello = millis();
}

static void askCatchUp()
{
  uint8_t b[250];
  size_t n = 0;
  b[n++] = MAGIC0; b[n++] = MAGIC1; b[n++] = T_SYNC_REQ;
  memcpy(b + n, &s_myNode, 4); n += 4;
  LOCK;
  uint8_t k = std::min((size_t)50, s_comsIds.size());
  b[n++] = k;
  for (size_t i = s_comsIds.size() - k; i < s_comsIds.size(); ++i) { memcpy(b + n, &s_comsIds[i], 4); n += 4; }
  queueTx(b, n, 0);
  s_lastSyncAsk = millis();
}

static void noteMember(uint32_t node, const std::string & name, int hops)
{
  LOCK;
  for (auto & m : s_members) if (m.node == node) { if (!name.empty()) m.name = name; m.atMs = millis(); m.hops = hops; return; }
  if (s_members.size() >= 40) s_members.erase(s_members.begin());
  s_members.push_back({ node, name, millis(), hops });
  bump();
  // someone new in range: ask for anything we missed (at most once a minute)
  if (millis() - s_lastSyncAsk > 60000) askCatchUp();
}

static void addComs(uint32_t id, const ChatLine & cl, bool save)
{
  LOCK;
  s_coms.push_back(cl);
  s_comsIds.push_back(id);
  if (s_coms.size() > COMS_KEEP) { s_coms.erase(s_coms.begin()); s_comsIds.erase(s_comsIds.begin()); }
  if (cl.in) s_comsUnread++;
  bump();
  if (save) {
    FILE * f = fopen(dataPath("coms-log.txt").c_str(), "ab");
    if (f) {
      std::string t = cl.text;
      for (auto & c : t) if (c == '\n' || c == '|') c = ' ';
      fprintf(f, "%c|%08x|%llu|%d|%s|%s\n", cl.in ? 'I' : 'O', (unsigned)id, (unsigned long long)cl.t, cl.hops, cl.who.c_str(), t.c_str());
      fclose(f);
    }
  }
}

// Keeps only the newest COMS_KEEP lines in memory; the whole history stays in coms-log.txt on
// the card. Reads just the end of the file, one line at a time (no big buffers).
static bool s_comsLoaded = false;

static void loadComs()
{
  if (s_comsLoaded) return;
  s_comsLoaded = true;
  FILE * f = fopen(dataPath("coms-log.txt").c_str(), "rb");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  long from = size > 12 * 1024 ? size - 12 * 1024 : 0;
  fseek(f, from, SEEK_SET);
  char l[320];
  if (from > 0) fgets(l, sizeof(l), f);            // skip the partial first line
  LOCK;
  while (fgets(l, sizeof(l), f)) {
    size_t n = strlen(l);
    while (n && (l[n - 1] == '\n' || l[n - 1] == '\r')) l[--n] = 0;
    // I|id|t|hops|who|text
    char * p[5]; char * q = l; bool ok = true;
    for (int k = 0; k < 5; ++k) { p[k] = strchr(q, '|'); if (!p[k]) { ok = false; break; } q = p[k] + 1; }
    if (!ok) continue;
    ChatLine cl;
    cl.in   = l[0] == 'I';
    uint32_t id = strtoul(p[0] + 1, nullptr, 16);
    cl.t    = strtoull(p[1] + 1, nullptr, 10);
    cl.hops = atoi(p[2] + 1);
    cl.who.assign(p[3] + 1, p[4] - p[3] - 1);
    cl.text = p[4] + 1;
    s_coms.push_back(cl);
    s_comsIds.push_back(id);
    if (s_coms.size() > COMS_KEEP) { s_coms.erase(s_coms.begin()); s_comsIds.erase(s_comsIds.begin()); }
    markSeen(id);
  }
  fclose(f);
}

static void handleNowPacket(const uint8_t * b, size_t len)
{
  uint8_t type = b[2];
  if (type == T_SYNC_REQ) return;                          // the Shelf boards answer these
  if (len < 17) return;
  uint32_t id, origin, sender;
  memcpy(&id, b + 5, 4); memcpy(&origin, b + 9, 4); memcpy(&sender, b + 13, 4);
  uint8_t hopsLeft = b[3], hopsTaken = b[4];

  if (type == T_RECEIPT) {
    if (len < 22) return;
    uint32_t receiver, msg;
    memcpy(&receiver, b + 9, 4); memcpy(&msg, b + 17, 4);
    if (receiver == s_myNode || wasSeen(id)) return;
    markSeen(id);
    size_t nl = b[21];
    std::string name((const char *)b + 22, std::min(nl, len - 22));
    bool mine = false;
    {
      LOCK;
      for (size_t i = 0; i < s_comsIds.size(); ++i)
        if (s_comsIds[i] == msg && !s_coms[i].in) { s_coms[i].receipts++; mine = true; bump(); }
    }
    if (!mine && hopsLeft > 0) {
      uint8_t out[64];
      size_t n = buildReceipt(out, id, receiver, hopsLeft - 1, hopsTaken + 1, msg, name);
      queueTx(out, n, 600 + esp_random() % 2000);
    }
    return;
  }

  if (type >= T_HELLO && type <= T_WHO) {                  // hello / remote admin / who: note + pass on
    if (origin == s_myNode || wasSeen(id)) return;
    markSeen(id);
    if (type == T_HELLO && len >= 21) {
      size_t nl = b[20];
      if (21 + nl <= len) noteMember(origin, std::string((const char *)b + 21, nl), hopsTaken);
    }
    if (type == T_WHO && millis() - s_lastWhoAnswer > 60000) { s_lastWhoAnswer = millis(); sendHello(); }
    if (hopsLeft > 0) {
      uint8_t out[250];
      memcpy(out, b, len);
      out[3]--; out[4]++; memcpy(out + 13, &s_myNode, 4);
      queueTx(out, len, 400 + esp_random() % 1600);
    }
    return;
  }

  if (type != T_TEXT && type != T_REPLAY) return;
  size_t hdr = type == T_REPLAY ? 22 : 18;
  if (len < hdr) return;
  uint32_t ageS = 0;
  if (type == T_REPLAY) memcpy(&ageS, b + 17, 4);
  size_t nl = b[hdr - 1];
  if (hdr + nl > len || nl > MAX_NAME) return;
  std::string name((const char *)b + hdr, nl);
  std::string text((const char *)b + hdr + nl, len - hdr - nl);
  if (origin == s_myNode || wasSeen(id)) return;
  markSeen(id);
  if (type == T_REPLAY && ageS > 12 * 3600) return;
  noteMember(origin, name, hopsTaken);
  ChatLine cl;
  cl.in = true;
  cl.who = toAscii(name);
  cl.text = toAscii(text);
  cl.hops = hopsTaken;
  uint64_t now = nowMs();
  cl.t = (type == T_REPLAY && now > (uint64_t)ageS * 1000) ? now - (uint64_t)ageS * 1000 : now;
  addComs(id, cl, true);
  if (type == T_TEXT) {
    if (hopsLeft > 0) {                                    // every member is a relay
      uint8_t out[250];
      size_t n = buildText(out, id, origin, hopsLeft - 1, hopsTaken + 1, name, text);
      queueTx(out, n, 400 + esp_random() % 1600);
    }
    uint8_t r[64];                                         // "received" receipt back to the sender
    size_t n = buildReceipt(r, esp_random(), s_myNode, HOPS, 0, id, s_myName.substr(0, MAX_NAME));
    queueTx(r, n, 2200 + esp_random() % 2500);
  }
}

static wifi_interface_t s_nowIf = WIFI_IF_STA;

static bool nowStart()
{
  if (esp_now_init() != ESP_OK) return false;
  esp_now_register_recv_cb(onNowRecv);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BCAST, 6);
  peer.channel = 0;
  peer.encrypt = false;
  peer.ifidx = s_nowIf;
  if (esp_now_add_peer(&peer) != ESP_OK) { esp_now_deinit(); return false; }
  return true;
}

static void comsPoll()
{
  // received frames
  while (s_rr != s_rw) {
    NowPkt p;
    portENTER_CRITICAL(&s_ringMux);
    p = s_ring[s_rr % 8];
    s_rr++;
    portEXIT_CRITICAL(&s_ringMux);
    s_nowHeard++;
    handleNowPacket(p.buf, p.len);
  }
  // due transmissions
  std::vector<Tx> due;
  {
    LOCK;
    uint32_t now = millis();
    for (auto it = s_tx.begin(); it != s_tx.end();) {
      if ((int32_t)(now - it->due) >= 0) { due.push_back(*it); it = s_tx.erase(it); }
      else ++it;
    }
  }
  for (auto & t : due) if (esp_now_send(BCAST, t.buf, t.len) == ESP_OK) s_nowSent++;
  // periodic hello + first catch-up
  if (millis() - s_lastHello > 15UL * 60 * 1000 || (s_lastHello == 0 && millis() - s_bootMs > 4000)) sendHello();
  if (s_lastSyncAsk == 0 && millis() - s_bootMs > 6000) askCatchUp();
}

// ====================================================================== background task

static WiFiUDP s_udp;
static bool s_udpUp = false;

static void msgTask(void *)
{
  uint32_t lastBeat = 0, lastCheck = 0, lastSave = 0;
  size_t checkIdx = 0;
  for (;;) {
    bool wifi = WiFi.status() == WL_CONNECTED;
    if (s_nowUp) comsPoll();

    if (!wifi) {
      if (s_udpUp) { s_udp.stop(); s_udpUp = false; }
      setStatus(gSet.ssid.length() ? "Waiting for Wi-Fi..." : "Set up Wi-Fi in Settings to reach your Shelves.");
      vTaskDelay(50 / portTICK_PERIOD_MS);
      continue;
    }
    if (!s_udpUp) s_udpUp = s_udp.begin(DISCOVERY_PORT);
    if (!s_serverUp) { s_server.begin(); s_serverUp = true; }

    // presence from other Shelves
    int sz;
    while (s_udpUp && (sz = s_udp.parsePacket()) > 0) {
      char buf[600];
      int n = s_udp.read(buf, std::min(sz, (int)sizeof(buf) - 1));
      if (n <= 0) break;
      buf[n] = 0;
      std::string j(buf), type;
      if (jsonGet(j, "type", type) && type == "shelf-presence") notePeer(j, s_udp.remoteIP().toString().c_str());
    }
    uint32_t now = millis();
    if (now - lastBeat > 3000) {
      std::string p = presenceJson("shelf-presence");
      IPAddress targets[2] = { IPAddress(255, 255, 255, 255), WiFi.broadcastIP() };
      for (auto & t : targets) { s_udp.beginPacket(t, DISCOVERY_PORT); s_udp.write((const uint8_t *)p.data(), p.size()); s_udp.endPacket(); }
      lastBeat = now;
      int on = 0;
      { LOCK; for (auto & x : s_peers) if (online(x)) ++on; }
      setStatus("Online as \"" + s_myName + "\" at " + WiFi.localIP().toString().c_str() + ":8080  -  " + std::to_string(on) + " Shelf" + (on == 1 ? "" : "s") + " online");
    }

    handleHttp();

    // send queued messages
    Outgoing o;
    bool have = false;
    { LOCK; if (!s_out.empty()) { o = s_out.front(); s_out.erase(s_out.begin()); have = true; } }
    if (have) {
      std::string ip; int port = 8080;
      { LOCK; Peer * p = findPeer(o.peerId); if (p) { ip = p->ip; port = p->port; } }
      std::string body = "{\"fromId\":\"" + s_myId + "\",\"fromName\":\"" + jsonEsc(s_myName) + "\",\"fromPort\":" + std::to_string(MY_PORT) +
                         ",\"text\":\"" + jsonEsc(o.text) + "\"" + (o.t ? ",\"timestamp\":" + std::to_string((unsigned long long)o.t) : std::string("")) + "}";
      bool ok = !ip.empty() && httpPost(ip, port, "/api/messaging/receive", body, nullptr, 5000);
      appendConv(o.peerId, ok ? 'O' : 'F', o.t, o.text);
      bump();
    }

    // check in with known Shelves that went quiet (broadcasts don't cross every router)
    if (now - lastCheck > 4000) {
      std::string ip; int port = 0;
      {
        LOCK;
        if (!s_peers.empty()) {
          checkIdx = (checkIdx + 1) % s_peers.size();
          Peer & p = s_peers[checkIdx];
          if (!p.ip.empty() && !(online(p) && millis() - p.seenMs < 8000)) { ip = p.ip; port = p.port; }
        }
      }
      if (!ip.empty()) probe(ip, port, 1200);
      lastCheck = millis();
    }
    if (s_peersDirty && millis() - lastSave > 30000) { savePeers(); lastSave = millis(); }
    vTaskDelay(20 / portTICK_PERIOD_MS);
  }
}

// ====================================================================== public API

static bool s_msgStarted = false;
bool msgStarted() { return s_msgStarted; }

void msgBegin()
{
  if (s_msgStarted) return;
  s_msgStarted = true;
  s_mp.begin("tidymsg", false);
  s_myId   = s_mp.getString("id", "").c_str();
  if (!validId(s_myId)) { s_myId = uuid4(); s_mp.putString("id", s_myId.c_str()); }
  s_myName = s_mp.getString("name", "TidyOS ESP32").c_str();
  char t[17];
  snprintf(t, sizeof(t), "%08x%08x", (unsigned)esp_random(), (unsigned)esp_random());
  s_instance = t;
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  s_myNode = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
  s_bootMs = millis();
  loadPeers();
  if (gSet.meshOn) loadComs();            // mesh history only matters (and only costs RAM) with the mesh on
  s_comsUnread = 0;
  s_nowUp = gSet.meshOn && nowStart();
  xTaskCreatePinnedToCore(msgTask, "shelfmsg", 8192, nullptr, 1, nullptr, 0);
}

uint32_t msgVersion() { return s_version; }
std::string msgMyName() { LOCK; return s_myName; }

void msgSetName(const std::string & n)
{
  std::string nm = n.substr(0, 60);
  if (nm.empty()) return;
  { LOCK; s_myName = nm; }
  s_mp.putString("name", nm.c_str());
  bump();
}

void msgPeers(std::vector<MsgPeer> & out)
{
  LOCK;
  out.clear();
  for (auto & p : s_peers) {
    MsgPeer m;
    m.id = p.id; m.name = p.name; m.ip = p.ip; m.port = p.port; m.online = online(p); m.unread = p.unread;
    out.push_back(m);
  }
  std::stable_sort(out.begin(), out.end(), [](const MsgPeer & a, const MsgPeer & b) { return a.online > b.online; });
}

void msgConversation(const std::string & peerId, std::vector<ChatLine> & out)
{
  out.clear();
  std::string name;
  {
    LOCK;
    Peer * p = findPeer(peerId);
    if (p) { name = p->name; p->unread = 0; }
  }
  // Stream the last 48 KB of the log line by line instead of loading it whole.
  FILE * f = fopen(convPath(peerId).c_str(), "rb");
  if (!f) return;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  long from = size > 48 * 1024 ? size - 48 * 1024 : 0;
  fseek(f, from, SEEK_SET);
  FileSource in(f);
  if (from > 0) { int ch; while ((ch = in.peek()) >= 0) { in.skip(); if (ch == '\n') break; } }   // partial first line
  auto addLine = [&](const std::string & l) {
    size_t a = l.find('|'), b = l.find('|', a + 1);
    if (l.size() < 3 || a == std::string::npos || b == std::string::npos) return;
    ChatLine c;
    c.in = l[0] == 'I';
    c.failed = l[0] == 'F';
    c.t = strtoull(l.c_str() + a + 1, nullptr, 10);
    c.who = c.in ? name : "Me";
    c.text = l.substr(b + 1);
    for (auto & ch : c.text) if (ch == '\x1F') ch = '\n';
    out.push_back(c);
    if (out.size() >= 300) out.erase(out.begin(), out.end() - 150);
  };
  std::string l;
  for (;;) {
    int ch = in.peek();
    if (ch >= 0) in.skip();
    if (ch >= 0 && ch != '\n') { if (l.size() < 2200) l += (char)ch; continue; }
    if (!l.empty()) addLine(l);
    l.clear();
    if (ch < 0) break;
  }
  fclose(f);
  if (out.size() > 150) out.erase(out.begin(), out.end() - 150);
}

void msgSend(const std::string & peerId, const std::string & text)
{
  if (text.empty()) return;
  LOCK;
  if (s_out.size() < 10) s_out.push_back({ peerId, text.substr(0, 2000), nowMs() });
}

void msgAddAddress(const std::string & address)
{
  std::string a = address;
  if (a.compare(0, 7, "http://") == 0) a = a.substr(7);
  size_t sl = a.find('/');
  if (sl != std::string::npos) a = a.substr(0, sl);
  std::string host = a;
  int port = 8080;
  size_t c = a.rfind(':');
  if (c != std::string::npos) { host = a.substr(0, c); port = atoi(a.c_str() + c + 1); }
  if (host.empty()) return;
  // probe in the background so the screen doesn't freeze
  struct Job { std::string host; int port; };
  Job * j = new Job{ host, port };
  xTaskCreatePinnedToCore([](void * arg) {
    Job * j = (Job *)arg;
    IPAddress ip;
    std::string addr = j->host;
    if (WiFi.hostByName(j->host.c_str(), ip) == 1) addr = ip.toString().c_str();
    bool ok = probe(addr, j->port, 4000);
    setStatus(ok ? "Found a Shelf at " + addr + ":" + std::to_string(j->port)
                  : "Nothing answered at " + addr + ":" + std::to_string(j->port) + " - is The Shelf running there?");
    bump();
    delete j;
    vTaskDelete(nullptr);
  }, "msgadd", 6144, j, 1, nullptr, 0);
}

std::string msgStatus() { LOCK; return s_status; }

int msgUnreadTotal()
{
  LOCK;
  int n = 0;
  for (auto & p : s_peers) n += p.unread;
  return n;
}

std::string msgLastNotice() { LOCK; return msgUnreadTotal() ? s_notice : std::string(); }

void comsLog(std::vector<ChatLine> & out) { LOCK; out = s_coms; }

void comsSend(const std::string & text)
{
  if (text.empty() || !s_nowUp) return;
  std::string t = text.substr(0, MAX_TEXT);
  uint32_t id = esp_random();
  markSeen(id);
  uint8_t b[250];
  size_t n = buildText(b, id, s_myNode, HOPS, 0, s_myName.substr(0, MAX_NAME), t);
  queueTx(b, n, 0);
  ChatLine cl;
  cl.in = false;
  cl.who = s_myName;
  cl.text = t;
  cl.t = nowMs();
  addComs(id, cl, true);
}

void comsMembers(std::vector<std::string> & out)
{
  LOCK;
  out.clear();
  for (auto & m : s_members) {
    uint32_t ago = (millis() - m.atMs) / 60000;
    char b[96];
    snprintf(b, sizeof(b), "%s  (%s, %s)", m.name.empty() ? "?" : m.name.c_str(),
             m.hops == 0 ? "in range" : (std::to_string(m.hops) + " hops").c_str(),
             ago == 0 ? "now" : (std::to_string(ago) + " min").c_str());
    out.push_back(b);
  }
}

bool comsUp() { return s_nowUp; }

void comsSetOn(bool on)
{
  if (on && s_msgStarted) loadComs();
  if (on && !s_nowUp && s_msgStarted) s_nowUp = nowStart();
  if (!on && s_nowUp) { s_nowUp = false; delay(50); esp_now_deinit(); }
}

int comsChannel()
{
  uint8_t ch = 0;
  wifi_second_chan_t sc;
  esp_wifi_get_channel(&ch, &sc);
  return ch;
}

int  comsUnread()   { return s_comsUnread; }
void comsCounts(int & sent, int & heard) { sent = s_nowSent; heard = s_nowHeard; }
void comsMarkRead() { s_comsUnread = 0; }
