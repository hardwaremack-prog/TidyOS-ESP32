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
// ESP-IDF's certificate bundle (the common root certificates browsers trust), built into
// the Arduino core's mbedTLS library.
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");

static char s_lastErr[100] = "";

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
static int httpsGet(std::string host, std::string path, const std::function<bool(const uint8_t *, size_t, size_t)> & sink, int redirects = 3)
{
  WiFiClientSecure c;
  c.setCACertBundle(x509_crt_bundle_start);
  c.setTimeout(20);
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
  std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nUser-Agent: TidyOS/" TIDYOS_VERSION "\r\nConnection: close\r\n\r\n";
  c.write((const uint8_t *)req.data(), req.size());
  int code = -1;
  long len = -1;
  std::string location;
  bool first = true;
  uint32_t t0 = millis();
  for (;;) {
    if (!c.connected() && !c.available()) return -1;
    if (millis() - t0 > 20000) return -1;
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
  static uint8_t buf[2048];
  size_t got = 0;
  t0 = millis();
  while (len < 0 || (long)got < len) {
    int a = c.available();
    if (a <= 0) {
      if (!c.connected()) break;
      if (millis() - t0 > 20000) return -1;
      delay(2);
      continue;
    }
    int r = c.read(buf, std::min((size_t)a, sizeof(buf)));
    if (r <= 0) continue;
    t0 = millis();
    got += r;
    if (!sink(buf, r, len < 0 ? 0 : (size_t)len)) return -2;
  }
  if (len >= 0 && (long)got != len) return -1;
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
    if (code == -1 && s_lastErr[0]) snprintf(b, sizeof(b), "Couldn't reach GitHub: %.50s", s_lastErr);
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
    snprintf(b, sizeof(b), code != 200 ? "Download failed (%d) - nothing changed" : !writeOk ? "SD card write failed - nothing changed" : "Download was damaged - nothing changed", code);
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
