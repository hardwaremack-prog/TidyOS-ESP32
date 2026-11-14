// TidyOS update loader.
// Lives in its own small "factory" partition. TidyOS downloads a new version to the SD card
// (/tidyos/update.bin + update.sha), then restarts into this loader, which checks the file,
// copies it into the main app slot, checks it again, and starts the new TidyOS.
// With no update file it just starts TidyOS. The blue LED flashes fast while it writes.
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include "mbedtls/sha256.h"

#define LED_PIN 2
static SPIClass sdSpi(VSPI);

static void bootApp(const char * why)
{
  Serial.printf("[loader] %s - starting TidyOS\n", why);
  const esp_partition_t * app = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  if (app) esp_ota_set_boot_partition(app);
  delay(100);
  ESP.restart();
}

static bool mountCard(int miso, int mosi, int clk, int cs)
{
  sdSpi.begin(clk, miso, mosi, cs);
  for (int pin : { miso, mosi, cs }) pinMode(pin, INPUT_PULLUP);
  for (uint32_t hz : { 10000000u, 4000000u, 1000000u }) {
    if (SD.begin(cs, sdSpi, hz, "/sd", 2)) return true;
    SD.end();
  }
  sdSpi.end();
  return false;
}

static std::string hexOf(const uint8_t * h)
{
  char b[65];
  for (int i = 0; i < 32; ++i) sprintf(b + i * 2, "%02x", h[i]);
  return std::string(b, 64);
}

void setup()
{
  Serial.begin(115200);
  delay(100);
  Serial.println("\n[loader] TidyOS update loader");
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  // the in-line wiring (MISO 19, CLK 18, MOSI 5, CS 17) first, then the classic one
  if (!mountCard(19, 5, 18, 17) && !mountCard(16, 17, 14, 13)) bootApp("no SD card");

  File f = SD.open("/tidyos/update.bin", FILE_READ);
  if (!f) bootApp("no update waiting");
  size_t size = f.size();
  std::string want;
  File s = SD.open("/tidyos/update.sha", FILE_READ);
  if (s) { want = s.readString().c_str(); s.close(); }
  while (!want.empty() && (want.back() == '\n' || want.back() == '\r' || want.back() == ' ')) want.pop_back();
  Serial.printf("[loader] update.bin %u bytes\n", (unsigned)size);

  static uint8_t buf[4096];
  // 1. check the whole file before touching the app
  mbedtls_sha256_context sha;
  uint8_t h[32];
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  size_t n;
  while ((n = f.read(buf, sizeof(buf))) > 0) mbedtls_sha256_update_ret(&sha, buf, n);
  mbedtls_sha256_finish_ret(&sha, h);
  f.close();
  if (size < 100000 || want.size() != 64 || hexOf(h) != want) {
    Serial.printf("[loader] file check failed (sha %s, want %s)\n", hexOf(h).c_str(), want.c_str());
    SD.rename("/tidyos/update.bin", "/tidyos/update.bad");
    bootApp("update file was damaged - kept the current version");
  }

  // 2. write it into the app slot (Update writes the next OTA slot = app0 when running from here)
  f = SD.open("/tidyos/update.bin", FILE_READ);
  if (!Update.begin(size, U_FLASH)) bootApp("could not start writing");
  size_t done = 0;
  uint32_t lastPct = 101;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    if (Update.write(buf, n) != n) break;
    done += n;
    digitalWrite(LED_PIN, (millis() / 70) % 2);
    uint32_t pct = done * 100 / size;
    if (pct / 10 != lastPct / 10) { lastPct = pct; Serial.printf("[loader] writing %u%%\n", (unsigned)pct); }
  }
  f.close();
  if (done != size || !Update.end(true)) {
    // the app slot may now be half written; the file is still good, so try again on the next start
    Serial.printf("[loader] write failed: %s\n", Update.errorString());
    delay(2000);
    ESP.restart();
  }
  SD.rename("/tidyos/update.bin", "/tidyos/update.done");
  digitalWrite(LED_PIN, LOW);
  Serial.println("[loader] update installed");
  bootApp("update installed");
}

void loop() {}
