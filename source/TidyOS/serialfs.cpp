// =====================================================================
//  USB file manager: lets the web flasher page list, copy in, copy out
//  and delete files on the SD card and the internal flash over the USB
//  cable (Web Serial), with no Wi-Fi needed.
//
//  Commands arrive as serial lines starting with "!fs " (see input.cpp).
//  Every answer line starts with "@", so the page can pick them out of
//  the normal log lines.
//    !fs drives                  -> @drive SD|flash <total> <free>   ... @ok
//    !fs ls <path>               -> @d <name> / @f <size> <name>     ... @ok
//    !fs put <size> <crc32> <path>  -> @go, then raw bytes in 1024-byte
//                                   blocks, each answered with @ack <bytes so far>,
//                                   then @ok (or @err ...)
//    !fs get <path>              -> @size <n>, @b <base64> lines, @ok <crc32>
//    !fs rm <path>  !fs mkdir <path>  !fs fast  !fs slow
// =====================================================================

#include <Arduino.h>
#include <string>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include "fabgl.h"
#include "config.h"
#include "core.h"
#include "esp_rom_crc.h"

static uint32_t s_fastUntil = 0;     // the fast (921600 baud) link drops back after a quiet spell

static bool okPath(const std::string & p)
{
  if (p.find("..") != std::string::npos) return false;
  bool sd = p.compare(0, 3, "/SD") == 0 && (p.size() == 3 || p[3] == '/');
  bool fl = p.compare(0, 6, "/flash") == 0 && (p.size() == 6 || p[6] == '/');
  return (sd && gSDMounted) || (fl && gFlashMounted);
}

static void say(const char * fmt, const char * s = "") { Serial.printf(fmt, s); }

static void drives()
{
  int64_t total = 0, used = 0;
  if (gSDMounted && fabgl::FileBrowser::getFSInfo(fabgl::DriveType::SDCard, 0, &total, &used))
    Serial.printf("@drive SD %lld %lld\n", (long long)total, (long long)(total - used));
  total = used = 0;
  if (gFlashMounted && fabgl::FileBrowser::getFSInfo(fabgl::DriveType::SPIFFS, 0, &total, &used))
    Serial.printf("@drive flash %lld %lld\n", (long long)total, (long long)(total - used));
  say("@ok\n");
}

static void list(const std::string & path)
{
  if (!okPath(path)) { say("@err Not allowed\n"); return; }
  DIR * d = opendir(path.c_str());
  if (!d) { say("@err Folder not found\n"); return; }
  struct dirent * e;
  int n = 0;
  while ((e = readdir(d)) != nullptr && n < 1000) {
    std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    if (e->d_type == DT_DIR) { Serial.printf("@d %s\n", name.c_str()); }
    else {
      struct stat st;
      long size = stat((path + "/" + name).c_str(), &st) == 0 ? (long)st.st_size : 0;
      Serial.printf("@f %ld %s\n", size, name.c_str());
    }
    ++n;
  }
  closedir(d);
  say("@ok\n");
}

static uint8_t * s_buf = nullptr;        // 1 KB, only while a file is being copied

static void put(const std::string & args)
{
  long size = 0; unsigned long crcWant = 0; int used = 0;
  if (sscanf(args.c_str(), "%ld %lx %n", &size, &crcWant, &used) < 2 || used <= 0 || size < 0) { say("@err Bad command\n"); return; }
  std::string path = args.substr(used);
  if (!okPath(path) || path.back() == '/') { say("@err Not allowed\n"); return; }
  int64_t total = 0, usedB = 0;
  bool sd = path.compare(0, 3, "/SD") == 0;
  if (fabgl::FileBrowser::getFSInfo(sd ? fabgl::DriveType::SDCard : fabgl::DriveType::SPIFFS, 0, &total, &usedB) &&
      size > total - usedB) { say("@err Not enough room\n"); return; }
  std::string tmp = path + ".part";
  FILE * f = fopen(tmp.c_str(), "wb");
  if (!f) { say("@err Can't write there\n"); return; }
  say("@go\n");
  long got = 0;
  uint32_t crc = 0;
  bool ok = true;
  const char * why = "Timed out";
  while (got < size) {
    size_t want = (size_t)std::min(1024L, size - got), have = 0;
    uint32_t last = millis();
    while (have < want) {
      int av = Serial.available();
      if (av > 0) { have += Serial.read(s_buf + have, std::min((size_t)av, want - have)); last = millis(); }
      else if (millis() - last > 5000) { ok = false; break; }
      else delay(1);
    }
    if (!ok) break;
    if (fwrite(s_buf, 1, want, f) != want) { ok = false; why = "Disk full or write error"; break; }
    crc = esp_rom_crc32_le(crc, s_buf, want);
    got += want;
    Serial.printf("@ack %ld\n", got);
  }
  fclose(f);
  if (!ok) {                                    // throw away the rest of the file still coming in
    uint32_t q = millis();
    while (millis() - q < 1000) { if (Serial.available()) { Serial.read(); q = millis(); } else delay(2); }
  }
  if (ok && crc != (uint32_t)crcWant) { ok = false; why = "Check failed, try again"; }
  if (!ok) { unlink(tmp.c_str()); Serial.printf("@err %s\n", why); return; }
  unlink(path.c_str());
  if (rename(tmp.c_str(), path.c_str()) != 0) { unlink(tmp.c_str()); say("@err Can't save\n"); return; }
  say("@ok\n");
}

static void get(const std::string & path)
{
  static const char * B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (!okPath(path)) { say("@err Not allowed\n"); return; }
  FILE * f = fopen(path.c_str(), "rb");
  if (!f) { say("@err File not found\n"); return; }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  Serial.printf("@size %ld\n", size);
  uint32_t crc = 0;
  char line[8 + 256 + 2];
  size_t n;
  while ((n = fread(s_buf, 1, 192, f)) > 0) {     // 192 bytes -> 256 base64 characters
    crc = esp_rom_crc32_le(crc, s_buf, n);
    char * o = line;
    *o++ = '@'; *o++ = 'b'; *o++ = ' ';
    for (size_t i = 0; i < n; i += 3) {
      uint32_t v = s_buf[i] << 16 | (i + 1 < n ? s_buf[i + 1] << 8 : 0) | (i + 2 < n ? s_buf[i + 2] : 0);
      *o++ = B64[v >> 18 & 63]; *o++ = B64[v >> 12 & 63];
      *o++ = i + 1 < n ? B64[v >> 6 & 63] : '=';
      *o++ = i + 2 < n ? B64[v & 63] : '=';
    }
    *o++ = '\n'; *o = 0;
    Serial.write((const uint8_t *)line, o - line);
  }
  fclose(f);
  Serial.printf("@ok %08lx\n", (unsigned long)crc);
}

static void rmPath(const std::string & path)
{
  if (!okPath(path) || path == "/SD" || path == "/flash") { say("@err Not allowed\n"); return; }
  struct stat st;
  if (stat(path.c_str(), &st) != 0) { say("@err Not found\n"); return; }
  int r = S_ISDIR(st.st_mode) ? rmdir(path.c_str()) : unlink(path.c_str());
  say(r == 0 ? "@ok\n" : "@err Can't delete (folders must be empty)\n");
}

void serialFsTick()          // called from the serial task: a quiet fast link goes back to 115200
{
  if (s_fastUntil && (int32_t)(millis() - s_fastUntil) > 0) {
    s_fastUntil = 0;
    Serial.flush();
    Serial.updateBaudRate(115200);
  }
}

void serialFs(const std::string & c)
{
  bool own = !s_buf;
  if (own) s_buf = (uint8_t *)malloc(1024);
  if (!s_buf) { say("@err Not enough memory\n"); return; }
  std::string arg = c.size() > 3 ? c.substr(3) : "";
  auto after = [&](const char * w) { size_t l = strlen(w); return arg.compare(0, l, w) == 0 ? arg.substr(l) : std::string("\x01"); };
  std::string a;
  if (arg == "drives") drives();
  else if ((a = after("ls ")) != "\x01") list(a);
  else if ((a = after("put ")) != "\x01") put(a);
  else if ((a = after("get ")) != "\x01") get(a);
  else if ((a = after("rm ")) != "\x01") rmPath(a);
  else if ((a = after("mkdir ")) != "\x01") { bool ok = okPath(a) && mkdir(a.c_str(), 0777) == 0; say(ok ? "@ok\n" : "@err Can't make that folder\n"); }
  else if (arg == "hello") Serial.printf("@hello TidyOS %s\n", TIDYOS_VERSION);
  else if (arg == "fast") { say("@fast\n"); Serial.flush(); delay(20); Serial.updateBaudRate(921600); s_fastUntil = millis() + 4000; }   // back to normal unless the page answers soon
  else if (arg == "slow") { say("@ok\n"); Serial.flush(); delay(20); Serial.updateBaudRate(115200); s_fastUntil = 0; }
  else say("@err Unknown file command\n");
  if (s_fastUntil && arg != "fast") s_fastUntil = millis() + 60000;   // stays fast while the page keeps talking
  if (own) { free(s_buf); s_buf = nullptr; }
}
