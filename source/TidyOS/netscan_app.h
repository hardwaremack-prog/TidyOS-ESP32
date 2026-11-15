#pragma once
// ====================================================================== Who's On (network sweep)
// (included from apps.cpp, after the Ping app)
//
// Sweeps this TidyOS's /24 subnet with ARP ("who has 10.0.0.x?"). Every device on the
// network has to answer ARP to talk at all, so this finds computers whose firewall ignores
// ping. Then each one found gets a ping, to show which answer ping and how fast.
// lwIP's ARP table only holds 10 entries, so addresses are asked about 6 at a time.

#include "lwip/etharp.h"
#include "lwip/tcpip.h"
#include "esp_netif.h"

struct ScanHost { uint32_t ip; uint8_t mac[6]; int ms; };   // ms: -1 = no ping reply, -2 = not pinged yet
static std::vector<ScanHost> s_scan;
static std::mutex s_scanMx;
static volatile bool s_scanRun = false, s_scanAlive = false;
static volatile int  s_scanDone = 0, s_scanTotal = 0, s_scanVer = 0;

struct ArpCall { int idx; uint32_t ip; bool query; int found; uint8_t mac[6]; SemaphoreHandle_t done; };
static void arpInLwip(void * a)
{
  ArpCall * c = (ArpCall *)a;
  ip4_addr_t ip; ip.addr = c->ip;
  struct netif * nif = netif_get_by_index((u8_t)c->idx);       // looked up inside lwIP's own task
  c->found = 0;
  if (!nif) { xSemaphoreGive(c->done); return; }
  if (c->query) {
    struct eth_addr * e = nullptr; const ip4_addr_t * ipr = nullptr;
    c->found = etharp_find_addr(nif, &ip, &e, &ipr) >= 0 && e;
    if (c->found) memcpy(c->mac, e->addr, 6);
  } else etharp_request(nif, &ip);
  xSemaphoreGive(c->done);
}
static bool arpRun(ArpCall & c)
{
  if (tcpip_callback(arpInLwip, &c) != ERR_OK) return false;
  return xSemaphoreTake(c.done, pdMS_TO_TICKS(1000)) == pdTRUE;
}

static const char * macMaker(const uint8_t * m)
{
  if (m[0] & 0x02) return "private address (phone/tablet/PC)";
  static const struct { uint8_t a, b, c; const char * n; } K[] = {
    { 0xB8, 0x27, 0xEB, "Raspberry Pi" }, { 0xDC, 0xA6, 0x32, "Raspberry Pi" }, { 0xD8, 0x3A, 0xDD, "Raspberry Pi" },
    { 0xE4, 0x5F, 0x01, "Raspberry Pi" }, { 0x2C, 0xCF, 0x67, "Raspberry Pi" }, { 0x28, 0xCD, 0xC1, "Raspberry Pi" },
    { 0x24, 0x0A, 0xC4, "Espressif (ESP32)" }, { 0x30, 0xAE, 0xA4, "Espressif (ESP32)" }, { 0x84, 0xCC, 0xA8, "Espressif (ESP32)" },
    { 0xAC, 0x67, 0xB2, "Espressif (ESP32)" }, { 0xA4, 0xCF, 0x12, "Espressif (ESP32)" }, { 0x7C, 0x9E, 0xBD, "Espressif (ESP32)" },
    { 0x08, 0x3A, 0xF2, "Espressif (ESP32)" }, { 0xC8, 0xF0, 0x9E, "Espressif (ESP32)" }, { 0x48, 0xE7, 0x29, "Espressif (ESP32)" },
    { 0xEC, 0x62, 0x60, "Espressif (ESP32)" }, { 0x34, 0x85, 0x18, "Espressif (ESP32)" }, { 0xF4, 0x12, 0xFA, "Espressif (ESP32)" },
    { 0x3C, 0x22, 0xFB, "Apple" }, { 0xF0, 0x18, 0x98, "Apple" }, { 0xA4, 0x83, 0xE7, "Apple" }, { 0x00, 0x1C, 0xB3, "Apple" },
    { 0xFC, 0xA6, 0x67, "Amazon" }, { 0x44, 0x65, 0x0D, "Amazon" }, { 0xF0, 0x27, 0x2D, "Amazon" },
    { 0x3C, 0x5A, 0xB4, "Google" }, { 0xF4, 0xF5, 0xD8, "Google" }, { 0xD8, 0x31, 0x34, "Roku" }, { 0xB0, 0xA7, 0x37, "Roku" },
    { 0x00, 0x15, 0x5D, "Microsoft (virtual)" }, { 0x50, 0xC7, 0xBF, "TP-Link" }, { 0x98, 0xDA, 0xC4, "TP-Link" },
    { 0x00, 0x11, 0x32, "Synology" }, { 0x00, 0x24, 0xE4, "Withings" }, { 0x18, 0xB4, 0x30, "Nest" },
  };
  for (auto & k : K) if (m[0] == k.a && m[1] == k.b && m[2] == k.c) return k.n;
  return "";
}

static void scanTask(void *)
{
  esp_netif_t * en = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  int nifIdx = en ? esp_netif_get_netif_impl_index(en) : 0;
  uint32_t me = (uint32_t)WiFi.localIP(), gw = (uint32_t)WiFi.gatewayIP();
  uint32_t base = ntohl(me) & 0xFFFFFF00;            // this /24
  ArpCall c = {};
  c.idx = nifIdx;
  c.done = xSemaphoreCreateBinary();
  s_scanTotal = 254; s_scanDone = 0;
  {
    std::lock_guard<std::mutex> lk(s_scanMx);
    s_scan.clear();
    ScanHost self = { me, { 0 }, 0 };
    WiFi.macAddress(self.mac);
    s_scan.push_back(self);
  }
  s_scanVer++;
  for (int h = 1; h <= 254 && s_scanRun && nifIdx > 0 && c.done; h += 6) {
    int n = imin(6, 255 - h);
    for (int i = 0; i < n; ++i) {                     // ask
      uint32_t ip = htonl(base | (h + i));
      if (ip == me) continue;
      c.ip = ip; c.query = false; arpRun(c);
    }
    vTaskDelay(pdMS_TO_TICKS(450));
    for (int i = 0; i < n; ++i) {                     // who answered?
      uint32_t ip = htonl(base | (h + i));
      if (ip == me) continue;
      c.ip = ip; c.query = true; c.found = 0;
      if (arpRun(c) && c.found) {
        ScanHost s; s.ip = ip; memcpy(s.mac, c.mac, 6); s.ms = -2;
        std::lock_guard<std::mutex> lk(s_scanMx);
        s_scan.push_back(s);
        s_scanVer++;
      }
    }
    s_scanDone = imin(254, h + n - 1);
  }
  // ping each device found (some ignore pings: that's fine, ARP already found them)
  int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
  uint16_t seq = 1000;
  for (size_t i = 1; s_scanRun && sock >= 0; ++i) {
    uint32_t ip;
    { std::lock_guard<std::mutex> lk(s_scanMx); if (i >= s_scan.size()) break; ip = s_scan[i].ip; }
    int ms = pingOnce(sock, ip, seq++, 700);
    std::lock_guard<std::mutex> lk(s_scanMx);
    if (i < s_scan.size()) s_scan[i].ms = ms;
    s_scanVer++;
  }
  if (sock >= 0) close(sock);
  if (c.done) vSemaphoreDelete(c.done);
  (void)gw;
  s_scanRun = false;
  s_scanAlive = false;
  s_scanVer++;
  vTaskDelete(nullptr);
}

class ScanApp : public AppFrame {
public:
  static AppFrame * inst;
  TextArea * view;
  uiLabel * status;
  uiButton * scanBtn;
  int shownVer = -1;

  ScanApp() : AppFrame("Who's On", nextPos(520, 400), Size(520, 400), &inst) {
    scanBtn = button("Scan", cx, cy, 80, 24);
    scanBtn->onClick = [this]() { toggle(); };
    button("Add all to Ping", cx + 86, cy, 120, 24)->onClick = [this]() { addToPing(); };
    status = label("", cx + 214, cy + 4, 290, 16);
    status->labelStyle().textFont = &FONT_std_12;
    view = new TextArea(this, Point(cx, cy + 30), Size(504, 330), false);
    startTimer(500);
    onTimer = [this](uiTimerHandle) { refresh(); };
    refresh();
  }

  void toggle() {
    if (s_scanAlive) { s_scanRun = false; return; }
    if (!netConnected()) { gApp->messageBox("Who's On", "Join a Wi-Fi network first (Settings).", "OK", nullptr, nullptr, uiMessageBoxIcon::Warning); return; }
    s_scanRun = s_scanAlive = true;
    if (xTaskCreatePinnedToCore(scanTask, "netscan", 3072, nullptr, 1, nullptr, 0) != pdPASS) {
      s_scanRun = s_scanAlive = false;
      gApp->messageBox("Who's On", "Not enough memory to scan.", "OK", nullptr, nullptr, uiMessageBoxIcon::Warning);
    }
    refresh();
  }

  void addToPing() {
    int added = 0;
    {
      std::lock_guard<std::mutex> lk(s_scanMx);
      std::lock_guard<std::mutex> lk2(s_pmx);
      if (s_pt.empty()) {                            // Ping not opened yet: start from its saved list
        std::string f;
        if (readFileStr(dataPath("ping.txt").c_str(), f)) {
          size_t st = 0;
          for (size_t i = 0; i <= f.size(); ++i)
            if (i == f.size() || f[i] == '\n') {
              std::string l = f.substr(st, i - st); st = i + 1;
              while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
              if (!l.empty()) { PingTarget t; t.addr = l; s_pt.push_back(t); }
            }
        }
      }
      for (size_t i = 1; i < s_scan.size(); ++i) {
        IPAddress a(s_scan[i].ip);
        std::string ip = a.toString().c_str();
        bool have = false;
        for (auto & t : s_pt) if (t.addr == ip) have = true;
        if (!have) { PingTarget t; t.addr = ip; s_pt.push_back(t); ++added; }
      }
      std::string s;
      for (auto & t : s_pt) s += t.addr + "\n";
      writeFileStr(dataPath("ping.txt").c_str(), s);
    }
    s_pChanged++;
    char b[64]; snprintf(b, sizeof(b), "Added %d address%s to Ping.", added, added == 1 ? "" : "es");
    status->setText(b); status->repaint();
  }

  void refresh() {
    char b[120];
    scanBtn->setText(s_scanAlive ? "Stop" : "Scan"); scanBtn->repaint();
    if (s_scanVer == shownVer) return;
    shownVer = s_scanVer;
    std::vector<std::string> l;
    uint32_t gw = (uint32_t)WiFi.gatewayIP();
    int n = 0;
    {
      std::lock_guard<std::mutex> lk(s_scanMx);
      n = s_scan.size();
      if (n == 0) l.push_back("Press Scan to find everything on this network.");
      for (size_t i = 0; i < s_scan.size(); ++i) {
        auto & h = s_scan[i];
        IPAddress a(h.ip);
        char ping[24];
        if (i == 0) snprintf(ping, sizeof(ping), "this TidyOS");
        else if (h.ms == -2) snprintf(ping, sizeof(ping), "...");
        else if (h.ms < 0) snprintf(ping, sizeof(ping), "no ping reply");
        else snprintf(ping, sizeof(ping), "ping %d ms", h.ms);
        const char * who = i == 0 ? "" : h.ip == gw ? "router" : macMaker(h.mac);
        snprintf(b, sizeof(b), "%-15s %02x:%02x:%02x:%02x:%02x:%02x  %-14s %s", a.toString().c_str(),
                 h.mac[0], h.mac[1], h.mac[2], h.mac[3], h.mac[4], h.mac[5], ping, who);
        l.push_back(b);
      }
    }
    if (n > 0) {
      l.push_back("");
      l.push_back("\"no ping reply\" is normal for Windows PCs and sleeping phones: ARP still found them.");
    }
    int t = view->top;
    view->setLines(std::move(l));
    view->top = imin(t, imax(0, (int)view->lines.size() - 1));
    view->repaint();
    if (s_scanAlive) snprintf(b, sizeof(b), s_scanDone < s_scanTotal ? "Looking... %d of %d addresses, %d found" : "Pinging the %d devices found...", s_scanDone < s_scanTotal ? s_scanDone : n, s_scanTotal, n);
    else if (n) snprintf(b, sizeof(b), "%d devices online (including this TidyOS)", n);
    else b[0] = 0;
    status->setText(b); status->repaint();
  }

  void onClosing() override { s_scanRun = false; }
};
AppFrame * ScanApp::inst = nullptr;

void openScan()
{
  if (!ScanApp::inst) new ScanApp();
  ScanApp::inst->open();
}
