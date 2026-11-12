// TidyOS <-> The Shelf messaging
//
//  1. Shelf network Messages (Wi-Fi): the same protocol as The Shelf (server.js) and the
//     Shelf boards: "shelf-presence" UDP broadcasts on port 41234, HTTP on port 8080
//     (/api/messaging/hello, /identity, /receive). TidyOS shows up in every Shelf's
//     Messages list and can chat one-to-one with each of them.
//  2. Coms mesh (2.4 GHz): the Shelf boards' board-to-board mesh over ESP-NOW. TidyOS
//     joins as a member and relay: room-style chat, "received" receipts, 12-hour catch-up.
#pragma once
#include <string>
#include <vector>
#include <stdint.h>

struct MsgPeer {
  std::string id, name, ip;
  int  port = 8080;
  bool online = false;
  int  unread = 0;
};

struct ChatLine {
  bool        in = true;        // received (true) or sent by me
  uint64_t    t = 0;            // ms since 1970 (0 = unknown)
  std::string who;              // sender name
  std::string text;
  int         hops = 0;         // Coms only
  int         receipts = 0;     // Coms only: how many boards said "received"
  bool        failed = false;   // not delivered
};

void        msgBegin();                       // call once after Wi-Fi is started
uint32_t    msgVersion();                     // changes whenever anything new arrives
std::string msgMyName();
void        msgSetName(const std::string & n);

// Shelf network
void        msgPeers(std::vector<MsgPeer> & out);
void        msgConversation(const std::string & peerId, std::vector<ChatLine> & out);   // also marks it read
void        msgSend(const std::string & peerId, const std::string & text);             // queued; result shows in the conversation
void        msgAddAddress(const std::string & address);                                // "10.0.0.5" or "host:8080"
std::string msgStatus();
int         msgUnreadTotal();
std::string msgLastNotice();                  // "New message from Kitchen Pi" (empty when none)

// Coms mesh (2.4 GHz)
void        comsLog(std::vector<ChatLine> & out);
void        comsSend(const std::string & text);
void        comsMembers(std::vector<std::string> & out);
bool        comsUp();
int         comsChannel();
int         comsUnread();
void        comsMarkRead();
