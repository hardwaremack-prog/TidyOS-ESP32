// TidyOS networking: Wi-Fi, clock, and page fetching for the browser
#pragma once
#include <string>
#include <vector>

void netBegin();                                  // radio on + connect to saved network
void netConnect(const char * ssid, const char * pass);
bool netConnected();
bool netClockSet();                 // true once the clock has a real time
void netFixDns();                   // repair DNS servers after the hotspot changes Wi-Fi mode
void netTick();                     // once a second: re-sync the clock after (re)connecting
void netSetClockFrom(uint32_t epoch);   // HTML5: set the clock from the browser if NTP hasn't yet
std::string netStatus();                          // "Home-WiFi 192.168.1.40" / "Wi-Fi off"
void netScanStart();
int  netScanResults(std::vector<std::string> & out);   // -1 still scanning, else count
void netApplyClock();                             // (re)apply time zone + NTP

// ---- page fetching (runs in the background) ----
// Result format (same from the device parser and from the Shelf proxy):
//   TITLE: <title>\nURL: <final url>\n---\n<text with [n] link markers>\n#LINKS#\n1 <url>\n2 <url>...
enum FetchState { FETCH_IDLE, FETCH_BUSY, FETCH_DONE, FETCH_ERROR };
void        fetchStart(const std::string & url);
FetchState  fetchState();
std::string fetchTake();                          // result (or error message), sets state back to idle
std::string urlFromInput(const std::string & typed);   // adds http:// or turns words into a search
