// TidyOS in-app update from GitHub.
//
// A normal TidyOS has neither the flash space for a second copy of itself nor the RAM for an
// HTTPS connection, so updating takes three steps:
//  1. "Check for updates" restarts TidyOS into update mode: screen + Wi-Fi only, no apps,
//     which leaves plenty of RAM for HTTPS.
//  2. Update mode reads the web flasher's manifest on GitHub Pages; if it lists a newer
//     version, it downloads the app to the SD card (/tidyos/update.bin), checking SHA-256.
//  3. It restarts into the loader (a small separate program in its own flash slot), which
//     checks the file again, writes it over the app and starts the new TidyOS.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <sys/stat.h>
#include "mbedtls/sha256.h"
#include "config.h"
#include "core.h"
#include "update.h"
#include "update_roots_der.h"
#include <memory>
#include <new>
#include "mbedtls/x509_crt.h"

static char s_lastErr[100] = "";
#include <stdarg.h>
static void failMsg(const char * fmt, ...)
{
  char m[70];
  va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
  snprintf(s_lastErr, sizeof(s_lastErr), "%s (%u KB free)", m, (unsigned)(ramFree() / 1024));
  Serial.printf("[update] %s\n", s_lastErr);
}

// Certificate check, done with the TLS connection CLOSED: a TLS session holds ~25 KB, and
// with the screen running there isn't enough left to check certificates at the same time.
// So: connect, copy the server's certificates (~5 KB), hang up, check them against our roots
// (stored in flash as DER, used in place), and remember the checked server certificate.
// Every later connection just has to present that exact certificate.
static std::string s_okHost;
static std::vector<uint8_t> s_okLeaf;

static bool peerIsTrusted(WiFiClientSecure & c, const std::string & host)
{
  const mbedtls_x509_crt * peer = c.getPeerCertificate();
  return peer && host == s_okHost && peer->raw.len == s_okLeaf.size() && memcmp(peer->raw.p, s_okLeaf.data(), s_okLeaf.size()) == 0;
}

static bool checkChain(std::vector<std::vector<uint8_t>> & certs, const std::string & host)
{
  mbedtls_x509_crt chain;
  mbedtls_x509_crt_init(&chain);
  for (auto & c : certs)
    if (mbedtls_x509_crt_parse_der_nocopy(&chain, c.data(), c.size()) != 0) {
      mbedtls_x509_crt_free(&chain);
      snprintf(s_lastErr, sizeof(s_lastErr), "unreadable certificate from %s", host.c_str());
      return false;
    }
  uint32_t flags = 0, bestFlags = 0;
  bool haveBest = false, ok = false;
  for (auto & r : UPDATE_ROOTS_DER) {
    mbedtls_x509_crt ca;
    mbedtls_x509_crt_init(&ca);
    int pr = mbedtls_x509_crt_parse_der_nocopy(&ca, r.der, r.len);
    if (pr == 0) {
      flags = 0;
      int v = mbedtls_x509_crt_verify(&chain, &ca, nullptr, host.c_str(), &flags, nullptr, nullptr);
      if (v == 0) ok = true;
      else if (!haveBest || !(flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)) { bestFlags = flags; haveBest = true; }
    }
    mbedtls_x509_crt_free(&ca);
    if (ok) break;
    if (pr == MBEDTLS_ERR_X509_ALLOC_FAILED) { snprintf(s_lastErr, sizeof(s_lastErr), "out of memory checking the certificate (%u KB free)", (unsigned)(ramFree() / 1024)); break; }
  }
  if (!ok && haveBest) {
    char info[160];
    mbedtls_x509_crt_verify_info(info, sizeof(info), "", bestFlags);
    for (char * q = info; *q; ++q) if (*q == '\n') *q = ' ';
    // name the top certificate's issuer and the free RAM, so a screenshot says what went wrong
    const mbedtls_x509_crt * top = &chain;
    while (top->next && top->next->raw.len) top = top->next;
    char iss[100] = "";
    mbedtls_x509_dn_gets(iss, sizeof(iss), &top->issuer);
    const char * cn = strstr(iss, "CN=");
    snprintf(s_lastErr, sizeof(s_lastErr), "not trusted: %.40s (%u KB free)", cn ? cn + 3 : iss, (unsigned)(ramFree() / 1024));
    Serial.printf("[update] %s\n", info);
    for (const mbedtls_x509_crt * x = &chain; x; x = x->next) {
      char sub[110], iss[110];
      mbedtls_x509_dn_gets(sub, sizeof(sub), &x->subject);
      mbedtls_x509_dn_gets(iss, sizeof(iss), &x->issuer);
      Serial.printf("[update]   chain: %s\n[update]       issued by %s\n", sub, iss);
    }
  }
  mbedtls_x509_crt_free(&chain);
  Serial.printf("[update] %s certificate %s (free RAM %u)\n", host.c_str(), ok ? "checked OK" : "NOT trusted", (unsigned)ramFree());
  return ok;
}

// connected: make sure we're talking to the real server. May hang up and reconnect.
static bool verifyPeer(WiFiClientSecure & c, const std::string & host)
{
  if (peerIsTrusted(c, host)) return true;
  const mbedtls_x509_crt * peer = c.getPeerCertificate();
  if (!peer) { snprintf(s_lastErr, sizeof(s_lastErr), "no certificate from %s", host.c_str()); return false; }
  std::vector<std::vector<uint8_t>> certs;
  for (const mbedtls_x509_crt * x = peer; x && certs.size() < 4; x = x->next)
    certs.emplace_back(x->raw.p, x->raw.p + x->raw.len);
  c.stop();                                     // frees the TLS session's memory
  delay(50);
  if (!checkChain(certs, host)) return false;
  s_okHost = host;
  s_okLeaf = certs[0];
  return false;                                 // checked: the caller connects again with a fresh client
}

#define UPD_HOST "hardwaremack-prog.github.io"
#define UPD_BASE "/TidyOS-ESP32/"
#ifdef TIDYOS_HTML5
  #define UPD_MANIFEST "manifest-html5.json"
#else
  #define UPD_MANIFEST "manifest.json"
#endif

bool sdMountForUpdate();     // core.cpp

bool updateLoaderPresent()
{
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr) != nullptr;
}

void updateRequest()
{
  Preferences p;
  p.begin("tidyupd", false);
  p.putBool("go", true);
  p.end();
  delay(200);
  ESP.restart();
}

bool updateModeRequested()
{
  Preferences p;
  p.begin("tidyupd", false);
  bool go = p.getBool("go", false);
  if (go) p.putBool("go", false);
  p.end();
  return go;
}

// ---------------------------------------------------------------- HTTPS GET
// sink gets the body in pieces; returns the HTTP status (or -1)
static int httpsGet1(std::string host, std::string path, const std::function<bool(const uint8_t *, size_t, size_t)> & sink, int redirects);

// A closed WiFiClientSecure can't be reused (its TLS state is wiped), so the certificate
// check (which hangs up) is followed by a second try with a brand-new client.
static int httpsGet(std::string host, std::string path, const std::function<bool(const uint8_t *, size_t, size_t)> & sink, int redirects = 3)
{
  s_lastErr[0] = 0;
  int code = httpsGet1(host, path, sink, redirects);
  if (code == -3) {
    delay(100);
    code = httpsGet1(host, path, sink, redirects);
    if (code == -3) { snprintf(s_lastErr, sizeof(s_lastErr), "%s changed its certificate - try again", host.c_str()); code = -1; }
  }
  return code;
}

static int httpsGet1(std::string host, std::string path, const std::function<bool(const uint8_t *, size_t, size_t)> & sink, int redirects)
{
  WiFiClientSecure c;
  c.setInsecure();       // the handshake doesn't check; verifyPeer() below does, before anything is used
  c.setTimeout(20);
  static_cast<Stream &>(c).setTimeout(8000);     // header lines: wait for a slow server
  if (!c.connect(host.c_str(), 443)) {
    char e[80] = "";
    int code = c.lastError(e, sizeof(e));
    if (!e[0]) {
      IPAddress ip;
      snprintf(e, sizeof(e), WiFi.hostByName(host.c_str(), ip) ? "no answer from %s" : "can't look up %s", host.c_str());
    }
    snprintf(s_lastErr, sizeof(s_lastErr), "%s", e);
    Serial.printf("[update] can't connect to %s: %s (%d), free RAM %u\n", host.c_str(), e, code, (unsigned)ramFree());
    return -1;
  }
  if (!verifyPeer(c, host)) {
    c.stop();
    // just checked and trusted (no error set): connect again
    bool checked = !s_lastErr[0] && host == s_okHost && !s_okLeaf.empty();
    return checked ? -3 : -1;
  }
  std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nUser-Agent: TidyOS/" TIDYOS_VERSION "\r\nConnection: close\r\n\r\n";
  c.write((const uint8_t *)req.data(), req.size());
  int code = -1;
  long len = -1;
  std::string location;
  bool first = true;
  uint32_t t0 = millis();
  for (;;) {
    if (!c.connected() && !c.available()) { failMsg("%s hung up before answering", host.c_str()); return -1; }
    if (millis() - t0 > 20000) { failMsg("no answer from %s", host.c_str()); return -1; }
    String l = c.readStringUntil('\n');
    l.trim();
    if (first) { first = false; int sp = l.indexOf(' '); if (sp > 0) code = l.substring(sp + 1).toInt(); continue; }
    if (l.length() == 0) break;
    String low = l; low.toLowerCase();
    if (low.startsWith("content-length:")) len = l.substring(15).toInt();
    if (low.startsWith("location:")) { String v = l.substring(9); v.trim(); location = v.c_str(); }
  }
  if ((code == 301 || code == 302 || code == 307 || code == 308) && redirects > 0 && location.rfind("https://", 0) == 0) {
    c.stop();
    std::string rest = location.substr(8);
    size_t sl = rest.find('/');
    return httpsGet(rest.substr(0, sl), sl == std::string::npos ? "/" : rest.substr(sl), sink, redirects - 1);
  }
  if (code != 200) return code;
  uint8_t buf[512];                 // on the stack: needs no free block of RAM
  size_t got = 0;
  t0 = millis();
  while (len < 0 || (long)got < len) {
    int a = c.available();
    if (a <= 0) {
      if (!c.connected()) break;
      if (millis() - t0 > 20000) { failMsg("download stalled at %u KB", (unsigned)(got / 1024)); return -1; }
      delay(2);
      continue;
    }
    int r = c.read(buf, std::min((size_t)a, sizeof(buf)));
    if (r <= 0) continue;
    t0 = millis();
    got += r;
    if (!sink(buf, r, len < 0 ? 0 : (size_t)len)) return -2;
  }
  if (len >= 0 && (long)got != len) { failMsg("connection dropped at %u of %ld KB", (unsigned)(got / 1024), len / 1024); return -1; }
  return 200;
}

// "key": "value" or "key": number, first match after `from`
static std::string field(const std::string & js, const char * key, size_t from = 0)
{
  std::string k = std::string("\"") + key + "\"";
  size_t p = js.find(k, from);
  if (p == std::string::npos) return "";
  p = js.find(':', p);
  if (p == std::string::npos) return "";
  ++p;
  while (p < js.size() && (js[p] == ' ' || js[p] == '\n' || js[p] == '\r' || js[p] == '\t')) ++p;
  if (p < js.size() && js[p] == '"') { size_t e = js.find('"', p + 1); return e == std::string::npos ? "" : js.substr(p + 1, e - p - 1); }
  size_t e = p;
  while (e < js.size() && (isdigit((unsigned char)js[e]) || js[e] == '.')) ++e;
  return js.substr(p, e - p);
}

// -1 a older than b, 0 same, 1 newer
static int versionCmp(const std::string & a, const std::string & b)
{
  size_t i = 0, j = 0;
  while (i < a.size() || j < b.size()) {
    long x = 0, y = 0;
    while (i < a.size() && a[i] != '.') x = x * 10 + (isdigit((unsigned char)a[i]) ? a[i] - '0' : 0), ++i;
    while (j < b.size() && b[j] != '.') y = y * 10 + (isdigit((unsigned char)b[j]) ? b[j] - '0' : 0), ++j;
    if (x != y) return x < y ? -1 : 1;
    ++i; ++j;
  }
  return 0;
}

static std::string hexOf(const uint8_t * h)
{
  char b[65];
  for (int i = 0; i < 32; ++i) sprintf(b + i * 2, "%02x", h[i]);
  return std::string(b, 64);
}

static void finish(void (*status)(const char *, int), const char * msg, int pct, int waitMs)
{
  Serial.printf("[update] %s\n", msg);
  status(msg, pct);
  delay(waitMs);
  ESP.restart();
}

void updateRun(void (*status)(const char * msg, int percent))
{
  char b[96];
  if (!updateLoaderPresent())
    finish(status, "Install once from the web flasher to enable updates", 0, 6000);

  status("Joining Wi-Fi...", 5);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) delay(200);
  if (WiFi.status() != WL_CONNECTED) finish(status, "No Wi-Fi - set it up in Settings first", 0, 6000);

  status("Checking GitHub for a new version...", 10);
  std::string js;
  int code = httpsGet(UPD_HOST, UPD_BASE UPD_MANIFEST, [&](const uint8_t * d, size_t n, size_t) {
    if (js.size() + n > 8192) return false;
    js.append((const char *)d, n);
    return true;
  });
  if (code != 200) {
    if (code == -1 && s_lastErr[0]) snprintf(b, sizeof(b), "GitHub: %.80s", s_lastErr);
    else snprintf(b, sizeof(b), "Couldn't reach GitHub (%d) - try later", code);
    finish(status, b, 0, 9000);
  }
  size_t sec = js.find("\"tidyos\"");
  std::string ver = field(js, "version", sec == std::string::npos ? 0 : sec);
  std::string app = field(js, "app", sec == std::string::npos ? 0 : sec);
  std::string sha = field(js, "sha256", sec == std::string::npos ? 0 : sec);
  long size = atol(field(js, "size", sec == std::string::npos ? 0 : sec).c_str());
  Serial.printf("[update] GitHub has %s (%ld bytes), this is %s\n", ver.c_str(), size, TIDYOS_VERSION);
  if (sec == std::string::npos || ver.empty() || app.empty() || sha.size() != 64 || size < 100000)
    finish(status, "No update information on GitHub yet", 0, 6000);
  if (versionCmp(ver, TIDYOS_VERSION) <= 0) {
    snprintf(b, sizeof(b), "You have the newest TidyOS (%s)", TIDYOS_VERSION);
    finish(status, b, 100, 5000);
  }

  snprintf(b, sizeof(b), "New version %s - getting the SD card ready...", ver.c_str());
  status(b, 15);
  if (!sdMountForUpdate()) finish(status, "Updating needs the SD card - please put it in", 0, 7000);
  mkdir(SD_MOUNT "/tidyos", 0777);
  FILE * f = fopen(SD_MOUNT "/tidyos/update.bin", "wb");
  if (!f) finish(status, "Can't write to the SD card", 0, 6000);

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);
  size_t done = 0;
  int lastPct = -1;
  bool writeOk = true;
  code = httpsGet(UPD_HOST, UPD_BASE + app, [&](const uint8_t * d, size_t n, size_t total) {
    if (fwrite(d, 1, n, f) != n) { writeOk = false; return false; }
    mbedtls_sha256_update_ret(&ctx, d, n);
    done += n;
    int pct = 15 + (int)((uint64_t)done * 80 / (total ? total : size));
    if (pct != lastPct) { lastPct = pct; char m[80]; snprintf(m, sizeof(m), "Downloading TidyOS %s ... %u KB", ver.c_str(), (unsigned)(done / 1024)); status(m, pct); }
    return true;
  });
  fclose(f);
  uint8_t h[32];
  mbedtls_sha256_finish_ret(&ctx, h);
  if (code != 200 || !writeOk || (long)done != size || hexOf(h) != sha) {
    remove(SD_MOUNT "/tidyos/update.bin");
    if (code == -1 && s_lastErr[0]) snprintf(b, sizeof(b), "Download failed: %.70s", s_lastErr);
    else snprintf(b, sizeof(b), code != 200 ? "Download failed (%d) - nothing changed" : !writeOk ? "SD card write failed - nothing changed" : "Download was damaged - nothing changed", code);
    finish(status, b, 0, 7000);
  }
  f = fopen(SD_MOUNT "/tidyos/update.sha", "w");
  if (f) { fputs(sha.c_str(), f); fclose(f); }

  // hand over to the loader
  const esp_partition_t * loader = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  esp_ota_set_boot_partition(loader);
  snprintf(b, sizeof(b), "Installing %s - TidyOS restarts in about 30 s", ver.c_str());
  finish(status, b, 100, 2500);
}
