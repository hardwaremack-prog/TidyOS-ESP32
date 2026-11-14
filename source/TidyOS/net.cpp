// TidyOS networking
#include "screen.h"
#include "shelfmsg.h"
#include "lwip/dns.h"
#include <sys/time.h>
#include <WiFi.h>
#include <time.h>
#include "net.h"
#include "core.h"

// ============================================================ Wi-Fi

void netBegin()
{
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname("tidyos");
#ifdef TIDYOS_HTML5
  WiFi.setSleep(false);       // power saving makes the hotspot drop joins (and the browser view laggy)
#endif
  if (gSet.ssid.length())
    WiFi.begin(gSet.ssid.c_str(), gSet.pass.c_str());
  netApplyClock();
}

void netConnect(const char * ssid, const char * pass)
{
  gSet.ssid = ssid;
  gSet.pass = pass;
  settingsSave();
  WiFi.disconnect();
  delay(50);
  WiFi.begin(ssid, pass);
}

bool netConnected() { return WiFi.status() == WL_CONNECTED; }

std::string netStatus()
{
  if (screenApOn() && WiFi.status() != WL_CONNECTED)
    return "Hotspot " SCREEN_AP_NAME " - " + screenUrl();
  if (!gSet.ssid.length())
    return "Wi-Fi not set up";
  if (WiFi.status() == WL_CONNECTED) {
    String s = WiFi.SSID() + "  " + WiFi.localIP().toString();
    return s.c_str();
  }
  return std::string("Connecting to ") + gSet.ssid.c_str() + "...";
}

void netScanStart()
{
  WiFi.scanDelete();
  WiFi.scanNetworks(true);
}

int netScanResults(std::vector<std::string> & out)
{
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return -1;
  if (n < 0) return 0;
  out.clear();
  for (int i = 0; i < n; ++i) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (auto & e : out) if (e == s.c_str()) dup = true;
    if (!dup) out.push_back(s.c_str());
  }
  WiFi.scanDelete();
  return out.size();
}

void netApplyClock()
{
  long off = (long)gSet.tzHours10 * 360;   // tenths of an hour -> seconds
  // the last server is a plain IP address (time.google.com), so the clock still sets if DNS is broken
  configTime(off, gSet.dst ? 3600 : 0, "pool.ntp.org", "time.google.com", "216.239.35.0");
}

bool netClockSet()
{
  time_t now = time(nullptr);
  return now > 1700000000;     // after Nov 2023
}

// Make sure the Wi-Fi connection has working DNS servers. Starting or stopping the setup hotspot
// can wipe them, which silently breaks the clock, the browser and Shelf lookups.
void netFixDns()
{
  if (WiFi.status() != WL_CONNECTED) return;
  const ip_addr_t * d = dns_getserver(0);
  uint32_t a = (d && IP_IS_V4(d)) ? ip4_addr_get_u32(ip_2_ip4(d)) : 0;
  bool bad = a == 0 || (a & 0x00FFFFFF) == 0x0004A8C0;      // 0.0.0.0 or 192.168.4.x (our own hotspot)
  if (bad) {
    ip_addr_t gw, g8;
    IPAddress r = WiFi.gatewayIP();
    IP_ADDR4(&gw, r[0], r[1], r[2], r[3]);
    IP_ADDR4(&g8, 8, 8, 8, 8);
    dns_setserver(0, &gw);
    dns_setserver(1, &g8);
    Serial.println("[TidyOS] DNS servers were missing - using the router and 8.8.8.8");
  }
}

// called once a second from the desktop: re-sync the clock whenever Wi-Fi (re)connects
void netTick()
{
  static bool was = false;
  static uint32_t lastTry = 0;
  bool now = WiFi.status() == WL_CONNECTED;
  if (now && !was) { netFixDns(); netApplyClock(); lastTry = millis(); if (msgWanted()) msgBegin(); }
  else if (now && !netClockSet() && millis() - lastTry > 30000) { netFixDns(); netApplyClock(); lastTry = millis(); }
  was = now;
}

void netSetClockFrom(uint32_t epoch)
{
  if (netClockSet() || epoch < 1700000000) return;
  struct timeval tv = { (time_t)epoch, 0 };
  settimeofday(&tv, nullptr);
  netApplyClock();          // keeps the time zone, and NTP still corrects it later
  Serial.println("[TidyOS] clock set from the browser");
}

// ============================================================ URL helpers

static std::string urlEncode(const std::string & s)
{
  static const char * hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += c;
    else if (c == ' ') o += '+';
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

std::string urlFromInput(const std::string & typed)
{
  std::string t = typed;
  while (!t.empty() && t[0] == ' ') t.erase(0, 1);
  while (!t.empty() && t.back() == ' ') t.pop_back();
  if (t.empty()) return t;
  if (t.find("://") != std::string::npos) return t;
  bool looksLikeHost = t.find(' ') == std::string::npos && (t.find('.') != std::string::npos || t.find(':') != std::string::npos);
  if (looksLikeHost) return "http://" + t;
  if (gSet.proxy.length())
    return "https://html.duckduckgo.com/html/?q=" + urlEncode(t);
  return "http://frogfind.com/?q=" + urlEncode(t);      // FrogFind: plain-HTTP search for vintage computers
}

static std::string resolveUrl(const std::string & base, const std::string & href)
{
  if (href.empty() || href[0] == '#') return "";
  if (href.compare(0, 7, "mailto:") == 0 || href.compare(0, 11, "javascript:") == 0 || href.compare(0, 4, "tel:") == 0) return "";
  // absolute? (scheme letters followed by ://)
  size_t sch = href.find("://");
  if (sch != std::string::npos && sch > 0) {
    bool scheme = true;
    for (size_t i = 0; i < sch; ++i)
      if (!isalnum((unsigned char)href[i]) && href[i] != '+' && href[i] != '-' && href[i] != '.') scheme = false;
    if (scheme) return href;
  }
  size_t se = base.find("://");
  if (se == std::string::npos) return href;
  std::string scheme = base.substr(0, se);
  size_t hs = se + 3;
  size_t pe = base.find('/', hs);
  std::string origin = pe == std::string::npos ? base : base.substr(0, pe);
  if (href.compare(0, 2, "//") == 0) return scheme + ":" + href;
  if (href[0] == '/') return origin + href;
  std::string dir = pe == std::string::npos ? origin + "/" : base.substr(0, base.rfind('/') + 1);
  size_t q = dir.find('?');
  if (q != std::string::npos) dir = dir.substr(0, dir.rfind('/', q) + 1);
  return dir + href;
}

// ============================================================ HTML -> text (streaming)

class HtmlText {
public:
  std::string text, title, base;
  std::vector<std::string> links;
  size_t maxText = 16000;

  void feed(const char * p, size_t n) { for (size_t i = 0; i < n; ++i) put((unsigned char)p[i]); }

  std::string result() {          // builds the result inside `text` (no second copy of the page)
    text.insert(0, "TITLE: " + title + "\nURL: " + base + "\n---\n");
    text += "\n#LINKS#\n";
    for (size_t i = 0; i < links.size(); ++i) {
      text += std::to_string(i + 1) + " " + links[i] + "\n";
      std::string().swap(links[i]);
    }
    std::vector<std::string>().swap(links);
    return std::move(text);
  }

private:
  enum { S_TEXT, S_TAG, S_COMMENT, S_ENT } st = S_TEXT;
  std::string tag, ent, skipUntil;
  bool inTitle = false, pre = false, lastSpace = true;
  int  nl = 2, curLink = 0, utf = 0;
  uint32_t ucode = 0;

  void outc(char c) {
    if (text.size() >= maxText) return;
    if (c == '\n') {
      if (nl >= 2) return;
      while (!text.empty() && text.back() == ' ') text.pop_back();
      text += '\n'; ++nl; lastSpace = true; return;
    }
    if (c == ' ' && !pre) {
      if (lastSpace) return;
      text += ' '; lastSpace = true; return;
    }
    text += c; nl = 0; lastSpace = false;
  }
  void outs(const char * s) { while (*s) outc(*s++); }

  void emitChar(uint32_t cp) {
    if (!skipUntil.empty()) return;
    if (inTitle) {
      if (title.size() < 120) {
        if (cp == '\n' || cp == '\t' || cp == 0xA0) cp = ' ';
        else if (cp == 0x2013 || cp == 0x2014) cp = '-';
        else if (cp == 0x2018 || cp == 0x2019) cp = '\'';
        if (cp < 128 && !(cp == ' ' && !title.empty() && title.back() == ' ')) title += (char)cp;
      }
      return;
    }
    if (cp == '\r') return;
    if (cp == '\t' || cp == '\n') { if (pre && cp == '\n') outc('\n'); else outc(' '); return; }
    if (cp < 128) { outc((char)cp); return; }
    switch (cp) {
      case 0xA0: outc(' '); break;
      case 0x2018: case 0x2019: outc('\''); break;
      case 0x201C: case 0x201D: outc('"'); break;
      case 0x2013: case 0x2014: outc('-'); break;
      case 0x2026: outs("..."); break;
      case 0x2022: outc('*'); break;
      case 0xA9: outs("(c)"); break;
      case 0xE9: case 0xE8: case 0xEA: outc('e'); break;
      case 0xE1: case 0xE0: case 0xE2: outc('a'); break;
      case 0xF3: case 0xF6: outc('o'); break;
      case 0xFC: case 0xFA: outc('u'); break;
      case 0xED: outc('i'); break;
      case 0xF1: outc('n'); break;
      default: break;
    }
  }

  void put(unsigned char c) {
    switch (st) {
      case S_TEXT:
        if (c == '<') { st = S_TAG; tag.clear(); return; }
        if (c == '&') { st = S_ENT; ent.clear(); return; }
        utf8(c);
        return;
      case S_ENT:
        if (c == ';' || ent.size() > 8 || c == ' ' || c == '<') {
          entity();
          st = S_TEXT;
          if (c == '<') { st = S_TAG; tag.clear(); }
          else if (c == ' ') emitChar(' ');
          return;
        }
        ent += (char)c;
        return;
      case S_TAG:
        if (tag.size() == 3 && tag == "!--") { st = S_COMMENT; tag.clear(); }
        if (c == '>') { st = S_TEXT; doTag(); return; }
        if (tag.size() < 600) tag += (char)c;
        return;
      case S_COMMENT:
        tag += (char)c;
        if (tag.size() > 3) tag.erase(0, tag.size() - 3);
        if (tag == "-->") st = S_TEXT;
        return;
    }
  }

  void utf8(unsigned char c) {
    if (utf) {
      if ((c & 0xC0) == 0x80) { ucode = (ucode << 6) | (c & 0x3F); if (--utf == 0) emitChar(ucode); return; }
      utf = 0;
    }
    if (c < 0x80) { emitChar(c); return; }
    if ((c & 0xE0) == 0xC0) { utf = 1; ucode = c & 0x1F; return; }
    if ((c & 0xF0) == 0xE0) { utf = 2; ucode = c & 0x0F; return; }
    if ((c & 0xF8) == 0xF0) { utf = 3; ucode = c & 0x07; return; }
    emitChar(c);   // latin-1 fallback
  }

  void entity() {
    if (ent == "amp") emitChar('&');
    else if (ent == "lt") emitChar('<');
    else if (ent == "gt") emitChar('>');
    else if (ent == "quot") emitChar('"');
    else if (ent == "apos" || ent == "#39") emitChar('\'');
    else if (ent == "nbsp") emitChar(' ');
    else if (ent == "copy") emitChar(0xA9);
    else if (ent == "mdash" || ent == "ndash") emitChar('-');
    else if (ent == "hellip") emitChar(0x2026);
    else if (ent == "rsquo" || ent == "lsquo") emitChar('\'');
    else if (ent == "rdquo" || ent == "ldquo") emitChar('"');
    else if (ent.size() > 1 && ent[0] == '#') {
      uint32_t v = (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent.c_str() + 2, nullptr, 16) : strtoul(ent.c_str() + 1, nullptr, 10);
      emitChar(v);
    } else {
      emitChar('&');
      for (char ch : ent) emitChar(ch);
    }
  }

  static std::string attr(const std::string & t, const char * name) {
    std::string low = t;
    for (auto & ch : low) ch = tolower(ch);
    std::string key = std::string(name) + "=";
    size_t p = 0;
    while ((p = low.find(key, p)) != std::string::npos) {
      if (p == 0 || isspace((unsigned char)low[p - 1])) break;
      p += key.size();
    }
    if (p == std::string::npos) return "";
    p += key.size();
    if (p >= t.size()) return "";
    char q = t[p];
    if (q == '"' || q == '\'') {
      size_t e = t.find(q, p + 1);
      return t.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
    }
    size_t e = p;
    while (e < t.size() && !isspace((unsigned char)t[e])) ++e;
    return t.substr(p, e - p);
  }

  void doTag() {
    if (tag.empty() || tag[0] == '!' || tag[0] == '?') return;
    bool close = tag[0] == '/';
    size_t s = close ? 1 : 0, e = s;
    while (e < tag.size() && !isspace((unsigned char)tag[e]) && tag[e] != '/') ++e;
    std::string name = tag.substr(s, e - s);
    for (auto & ch : name) ch = tolower(ch);

    if (!skipUntil.empty()) {
      if (close && name == skipUntil) skipUntil.clear();
      return;
    }
    if (!close && (name == "script" || name == "style" || name == "noscript" || name == "svg" || name == "template" || name == "select")) {
      skipUntil = name;
      return;
    }
    if (name == "title") { inTitle = !close; return; }
    if (name == "a") {
      if (!close) {
        std::string u = resolveUrl(base, attr(tag, "href"));
        if (!u.empty() && links.size() < 250) { links.push_back(u); curLink = links.size(); }
        else curLink = 0;
      } else if (curLink) {
        char b[12];
        snprintf(b, sizeof(b), "[%d]", curLink);
        outs(b);
        curLink = 0;
      }
      return;
    }
    if (name == "pre") { pre = !close; outc('\n'); return; }
    if (name == "br") { outc('\n'); nl = 0; return; }
    if (name == "li") { if (!close) { outc('\n'); outs(" * "); } return; }
    if (name == "hr") { outc('\n'); outs("----------------------------------------"); outc('\n'); return; }
    if (name == "td" || name == "th") { if (!close) outs("  "); return; }
    if (name == "img" && !close) {
      std::string alt = attr(tag, "alt");
      if (!alt.empty() && alt.size() < 60) { outs("[img: "); outs(alt.c_str()); outs("] "); }
      return;
    }
    if (name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6') {
      outc('\n'); outc('\n');
      if (!close && name[1] <= '2') outs("== ");
      return;
    }
    static const char * blocks[] = { "p", "div", "ul", "ol", "tr", "table", "section", "article", "header", "footer",
                                     "nav", "blockquote", "form", "dl", "dt", "dd", "main", "aside", "figure", "h", nullptr };
    for (int i = 0; blocks[i]; ++i)
      if (name == blocks[i]) { outc('\n'); if (name == "p") outc('\n'); return; }
  }
};

// ============================================================ fetch task

static volatile FetchState s_state = FETCH_IDLE;
static std::string s_url, s_result;

// Small HTTP/1.0 client (smaller than HTTPClient and easy on RAM)
static bool readLine(WiFiClient & c, std::string & line, uint32_t timeoutMs)
{
  line.clear();
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    while (c.available()) {
      char ch = c.read();
      if (ch == '\n') { if (!line.empty() && line.back() == '\r') line.pop_back(); return true; }
      if (line.size() < 1024) line += ch;
    }
    if (!c.connected()) return !line.empty();
    delay(2);
  }
  return false;
}

static bool httpGet(const std::string & url0, std::function<bool(const char *, size_t)> sink, std::string & finalUrl, std::string & err)
{
  std::string url = url0;
  for (int hop = 0; hop < 6; ++hop) {
    if (url.compare(0, 7, "http://") != 0) {
      err = url.compare(0, 8, "https://") == 0 ? "Secure (https) pages need FrogFind or the browser helper." : "Only http:// addresses are supported.";
      return false;
    }
    size_t hs = 7, pe = url.find('/', hs);
    std::string hostport = url.substr(hs, pe == std::string::npos ? std::string::npos : pe - hs);
    std::string path = pe == std::string::npos ? "/" : url.substr(pe);
    size_t hash = path.find('#');
    if (hash != std::string::npos) path.erase(hash);
    std::string host = hostport;
    int port = 80;
    size_t colon = hostport.find(':');
    if (colon != std::string::npos) { host = hostport.substr(0, colon); port = atoi(hostport.c_str() + colon + 1); }

    WiFiClient c;
    if (!c.connect(host.c_str(), port, 8000)) { err = "Could not reach " + host; return false; }
    std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + hostport +
                      "\r\nUser-Agent: Mozilla/5.0 (TidyOS ESP32; text browser)\r\nAccept: text/html,text/plain;q=0.9,*/*;q=0.5\r\n"
                      "Accept-Encoding: identity\r\nConnection: close\r\n\r\n";
    c.write((const uint8_t *)req.data(), req.size());
    gNetTx += req.size();

    std::string line;
    if (!readLine(c, line, 12000)) { err = "No answer from " + host; c.stop(); return false; }
    int code = 0;
    size_t sp = line.find(' ');
    if (sp != std::string::npos) code = atoi(line.c_str() + sp + 1);
    std::string location;
    while (readLine(c, line, 8000) && !line.empty()) {
      if (line.size() > 9 && strncasecmp(line.c_str(), "location:", 9) == 0) {
        location = line.substr(9);
        while (!location.empty() && location[0] == ' ') location.erase(0, 1);
      }
    }
    if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308) && !location.empty()) {
      c.stop();
      url = resolveUrl(url, location);
      if (url.compare(0, 8, "https://") == 0 && !gSet.proxy.length())
        url = "http://frogfind.com/read.php?a=" + url;
      continue;
    }
    if (code != 200) { err = "The site answered with error " + std::to_string(code); c.stop(); return false; }

    finalUrl = url;
    char buf[512];
    size_t total = 0;
    uint32_t lastData = millis();
    while (true) {
      int av = c.available();
      if (av > 0) {
        int r = c.read((uint8_t *)buf, av > (int)sizeof(buf) ? sizeof(buf) : av);
        if (r <= 0) break;
        total += r;
        gNetRx += r;
        lastData = millis();
        if (!sink(buf, r) || total > 400000) break;
      } else {
        if (!c.connected() || millis() - lastData > 8000) break;
        delay(5);
      }
    }
    c.stop();
    return true;
  }
  err = "Too many redirects";
  return false;
}

static void fetchTask(void *)
{
  std::string url = s_url, err, finalUrl;
  std::string out;
  if (!netConnected()) {
    err = "Not connected to Wi-Fi. Open Settings to connect.";
  } else if (gSet.proxy.length()) {
    // Browser helper on The Shelf / a Pi does the heavy lifting (https, big pages)
    std::string p = std::string("http://") + gSet.proxy.c_str() + "/fetch?w=76&url=" + urlEncode(url);
    out.reserve(8192);
    bool ok = httpGet(p, [&](const char * d, size_t n) { if (out.size() + n > 32000) return false; out.append(d, n); return true; }, finalUrl, err);
    if (!ok) err = "Browser helper (" + std::string(gSet.proxy.c_str()) + "): " + err;
  } else {
    // No helper: secure (https) pages go through FrogFind's plain-HTTP reader,
    // so the ESP32 never has to do heavy TLS encryption.
    std::string real = url;
    if (real.compare(0, 8, "https://") == 0)
      real = "http://frogfind.com/read.php?a=" + url;
    HtmlText * h = new HtmlText;
    h->base = real;
    bool ok = httpGet(real, [&](const char * d, size_t n) { h->feed(d, n); return h->text.size() < h->maxText; }, finalUrl, err);
    if (ok) out = h->result();
    delete h;
  }
  if (out.empty() && err.empty()) err = "The page was empty.";
  if (err.empty()) s_result = std::move(out); else s_result = err;
  s_state  = err.empty() ? FETCH_DONE : FETCH_ERROR;
  vTaskDelete(nullptr);
}

void fetchStart(const std::string & url)
{
  if (s_state == FETCH_BUSY) return;
  s_url    = url;
  s_result.clear();
  s_state  = FETCH_BUSY;
  if (xTaskCreatePinnedToCore(fetchTask, "fetch", 8192, nullptr, 2, nullptr, 0) != pdPASS) {
    s_result = "Out of memory";
    s_state  = FETCH_ERROR;
  }
}

FetchState fetchState() { return s_state; }

std::string fetchTake()
{
  std::string r;
  r.swap(s_result);
  s_state = FETCH_IDLE;
  return r;
}
