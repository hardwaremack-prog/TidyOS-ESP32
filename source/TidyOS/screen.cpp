// TidyOS HTML5 edition: web screen server
//
// The desktop is still drawn into FabGL's normal 640x480 frame buffer (so a VGA monitor
// works too if one is plugged in). This task watches that buffer, sends the rows that
// changed to the browser over a WebSocket, and feeds the browser's keyboard and mouse
// back into TidyOS. One browser at a time; opening it somewhere else takes over.
//
// With no Wi-Fi set up (or if it can't connect) TidyOS starts a "TidyOS-Setup" hotspot,
// so a phone or PC can join it, open http://192.168.4.1 and use Settings to join home Wi-Fi.

#include "config.h"
#ifdef TIDYOS_HTML5

#include <WiFi.h>
#include <DNSServer.h>
#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"
#include "fabgl.h"
#include "core.h"
#include "screen.h"
#include "fileserver.h"
#include "net.h"

void inputWebKey(uint8_t usage, uint8_t mods, bool down);
void inputWebMouse(int x, int y, int wheel, uint8_t buttons);
void inputWebReleaseAll();
RGB888 themePal(int i);

static WiFiServer    s_srv(80);
static bool          s_listening = false;
static DNSServer     s_dns;
static volatile bool s_ap = false;
static WiFiClient    s_ws;
static volatile bool s_view = false;
static uint32_t      s_hash[SCREEN_H];
static uint32_t      s_apSince = 0, s_connSince = 0;

// ---------------------------------------------------------------- the page
static const char PAGE[] = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no"><title>TidyOS</title><style>
html,body{margin:0;height:100%;background:#1c1c1c;color:#ddd;font:14px system-ui,sans-serif;overflow:hidden}
#bar{height:36px;display:flex;align-items:center;gap:6px;padding:0 8px;background:#0055aa;color:#fff}
#bar b{font-size:16px;margin-right:6px}#st{margin-left:auto;font-size:12px;opacity:.9;white-space:nowrap}
button{background:#fff;color:#0055aa;border:0;border-radius:5px;padding:5px 10px;font:inherit;cursor:pointer}
#wrap{position:absolute;top:36px;left:0;right:0;bottom:0;display:flex;align-items:center;justify-content:center}
canvas{image-rendering:pixelated;image-rendering:crisp-edges;background:#00aaaa;touch-action:none;outline:none;cursor:default}
#kb{position:absolute;left:0;top:0;width:1px;height:1px;opacity:0;border:0;padding:0}
#msg{position:absolute;top:36px;left:0;right:0;bottom:0;display:flex;flex-direction:column;gap:14px;align-items:center;justify-content:center;background:rgba(0,0,0,.65);font-size:18px;text-align:center}
@media(max-width:520px){#bt{display:none}}
</style></head><body>
<div id="bar"><b>TidyOS</b><button id="bk">Keyboard</button><button id="bt">Task Manager</button><button id="bf">Full screen</button><span id="st">Connecting...</span></div>
<div id="wrap"><canvas id="c" width="640" height="480" tabindex="0"></canvas></div>
<input id="kb" autocapitalize="off" autocomplete="off" autocorrect="off" spellcheck="false">
<div id="msg"><div id="mt">Connecting to TidyOS...</div><button id="mb" style="display:none">Use it here</button></div>
<script>
var c=document.getElementById('c'),g=c.getContext('2d'),img=g.createImageData(640,480),px=new Uint32Array(img.data.buffer);
var ws=null,bpp=2,pal=new Uint32Array(8),dirty=false,rx=0,kicked=false,down={},mx=-1,my=-1,mb=0,mDirty=false;
var msg=document.getElementById('msg'),mt=document.getElementById('mt'),btn=document.getElementById('mb'),st=document.getElementById('st'),kb=document.getElementById('kb');
function show(t,b){mt.textContent=t;btn.style.display=b?'':'none';msg.style.display=t?'flex':'none';}
function fit(){var w=innerWidth,h=innerHeight-36,s=Math.min(w/640,h/480);if(s>1)s=Math.floor(s*4)/4;c.style.width=Math.floor(640*s)+'px';c.style.height=Math.floor(480*s)+'px';}
addEventListener('resize',fit);fit();
function send(a){if(ws&&ws.readyState==1)ws.send(new Uint8Array(a));}
function connect(){
 kicked=false;show('Connecting to TidyOS...');
 ws=new WebSocket('ws://'+location.host+'/ws');ws.binaryType='arraybuffer';
 ws.onopen=function(){show('');st.textContent='Connected';c.focus();var t=Math.floor(Date.now()/1000);send([6,t&255,t>>8&255,t>>16&255,t>>>24]);};
 ws.onmessage=function(e){var d=new Uint8Array(e.data);rx+=d.length;
  if(d[0]==1){bpp=d[1];for(var i=0;i<d[2];i++)pal[i]=0xff000000|d[5+i*3]<<16|d[4+i*3]<<8|d[3+i*3];}
  else if(d[0]==2){var y=d[1]|d[2]<<8,n=d[3],o=4,rb=bpp==2?160:240;
   for(var r=0;r<n;r++,y++,o+=rb){var p=y*640;
    if(bpp==2){for(var x=0;x<160;x++){var b=d[o+x];px[p++]=pal[b>>6];px[p++]=pal[b>>4&3];px[p++]=pal[b>>2&3];px[p++]=pal[b&3];}}
    else{for(var k=0;k<80;k++){var q=o+k*3,v=d[q]|d[q+1]<<8|d[q+2]<<16;for(var i=0;i<8;i++)px[p++]=pal[v>>(21-3*i)&7];}}}
   dirty=true;}
  else if(d[0]==3){kicked=true;show('TidyOS was opened in another browser.',true);}};
 ws.onclose=function(){st.textContent='Disconnected';if(!kicked){show('Lost connection - trying again...');setTimeout(connect,2000);}};
}
btn.onclick=function(){connect();};
function frame(){if(dirty){g.putImageData(img,0,0);dirty=false;}
 if(mDirty){mDirty=false;send([2,mx&255,mx>>8,my&255,my>>8,mb,0]);}requestAnimationFrame(frame);}
requestAnimationFrame(frame);
setInterval(function(){if(ws&&ws.readyState==1)st.textContent='Connected  '+(rx/1024).toFixed(0)+' KB/s';rx=0;},1000);
// ---- keyboard: browser key codes -> USB HID usages
var K={Enter:40,Escape:41,Backspace:42,Tab:43,Space:44,Minus:45,Equal:46,BracketLeft:47,BracketRight:48,Backslash:49,Semicolon:51,Quote:52,Backquote:53,Comma:54,Period:55,Slash:56,CapsLock:57,
PrintScreen:70,ScrollLock:71,Pause:72,Insert:73,Home:74,PageUp:75,Delete:76,End:77,PageDown:78,ArrowRight:79,ArrowLeft:80,ArrowDown:81,ArrowUp:82,NumLock:83,
NumpadDivide:84,NumpadMultiply:85,NumpadSubtract:86,NumpadAdd:87,NumpadEnter:88,Numpad0:98,NumpadDecimal:99,
ControlLeft:224,ShiftLeft:225,AltLeft:226,MetaLeft:227,ControlRight:228,ShiftRight:229,AltRight:230,MetaRight:231};
for(var i=0;i<26;i++)K['Key'+String.fromCharCode(65+i)]=4+i;
for(var i=1;i<10;i++){K['Digit'+i]=29+i;K['Numpad'+i]=88+i;}K.Digit0=39;
for(var i=1;i<13;i++)K['F'+i]=57+i;
function mods(e){return(e.ctrlKey?1:0)|(e.shiftKey?2:0)|(e.altKey?4:0)|(e.metaKey?8:0);}
addEventListener('keydown',function(e){var u=K[e.code];if(!u||e.keyCode==229)return;e.preventDefault();if(e.repeat)return;down[u]=1;send([1,u,mods(e),1]);});
addEventListener('keyup',function(e){var u=K[e.code];if(!u)return;e.preventDefault();delete down[u];send([1,u,mods(e),0]);});
addEventListener('blur',function(){for(var u in down)send([1,+u,0,0]);down={};send([5]);});
// phone keyboards: typed characters -> key taps
var CH={' ':[44,0],'\n':[40,0],'-':[45,0],'_':[45,2],'=':[46,0],'+':[46,2],'[':[47,0],'{':[47,2],']':[48,0],'}':[48,2],'\\':[49,0],'|':[49,2],';':[51,0],':':[51,2],
"'":[52,0],'"':[52,2],'`':[53,0],'~':[53,2],',':[54,0],'<':[54,2],'.':[55,0],'>':[55,2],'/':[56,0],'?':[56,2],'0':[39,0],')':[39,2]},SH='!@#$%^&*(';
for(var i=0;i<26;i++){CH[String.fromCharCode(97+i)]=[4+i,0];CH[String.fromCharCode(65+i)]=[4+i,2];}
for(var i=1;i<10;i++){CH[''+i]=[29+i,0];CH[SH[i-1]]=[29+i,2];}
function tap(u,m){if(m)send([1,225,2,1]);send([1,u,m,1]);send([1,u,m,0]);if(m)send([1,225,0,0]);}
kb.value=' ';
kb.addEventListener('input',function(e){
 if(e.inputType=='deleteContentBackward')tap(42,0);
 else if(e.inputType=='insertLineBreak')tap(40,0);
 else{var t=e.data||'';for(var i=0;i<t.length;i++){var k=CH[t[i]];if(k)tap(k[0],k[1]);}}
 kb.value=' ';});
kb.addEventListener('keydown',function(e){if(e.key=='Enter'&&e.keyCode==229)tap(40,0);});
document.getElementById('bk').onclick=function(){kb.focus();};
document.getElementById('bt').onclick=function(){send([4]);c.focus();};
document.getElementById('bf').onclick=function(){var d=document.documentElement;(d.requestFullscreen||d.webkitRequestFullscreen||function(){}).call(d);};
// ---- mouse and touch
function pos(e){var r=c.getBoundingClientRect();mx=Math.max(0,Math.min(639,(e.clientX-r.left)*640/r.width|0));my=Math.max(0,Math.min(479,(e.clientY-r.top)*480/r.height|0));}
function btns(e){return e.pointerType=='mouse'?(e.buttons&7):(e.type=='pointerup'||e.type=='pointercancel'?0:1);}
c.addEventListener('pointermove',function(e){pos(e);mb=btns(e);mDirty=true;});
c.addEventListener('pointerdown',function(e){e.preventDefault();c.focus();try{c.setPointerCapture(e.pointerId);}catch(x){}pos(e);mb=btns(e);mDirty=false;send([2,mx&255,mx>>8,my&255,my>>8,mb,0]);});
c.addEventListener('pointerup',function(e){pos(e);mb=btns(e);mDirty=false;send([2,mx&255,mx>>8,my&255,my>>8,mb,0]);});
c.addEventListener('pointercancel',function(e){mb=0;mDirty=true;});
c.addEventListener('wheel',function(e){e.preventDefault();pos(e);send([2,mx&255,mx>>8,my&255,my>>8,mb,e.deltaY>0?1:255]);},{passive:false});
c.addEventListener('contextmenu',function(e){e.preventDefault();});
connect();
</script></body></html>)HTML";

// ---------------------------------------------------------------- WebSocket helpers
static bool readExact(WiFiClient & c, uint8_t * b, size_t n, uint32_t ms = 1000)
{
  uint32_t t0 = millis();
  size_t got = 0;
  while (got < n) {
    int a = c.available();
    if (a > 0) {
      int r = c.read(b + got, std::min((size_t)a, n - got));
      if (r > 0) { got += r; continue; }
    }
    if (!c.connected() || millis() - t0 > ms) return false;
    delay(1);
  }
  return true;
}

static bool wsSend(WiFiClient & c, const uint8_t * d, size_t n, uint8_t opcode = 2)
{
  uint8_t h[4];
  size_t hl;
  h[0] = 0x80 | opcode;
  if (n < 126) { h[1] = n; hl = 2; }
  else { h[1] = 126; h[2] = n >> 8; h[3] = n & 255; hl = 4; }
  if (c.write(h, hl) != hl) return false;
  size_t done = 0;
  while (done < n) {
    size_t w = c.write(d + done, std::min((size_t)1436, n - done));
    if (w == 0) return false;
    done += w;
  }
  gNetTx += n + hl;
  return true;
}

static void sendPalette(WiFiClient & c)
{
  uint8_t m[3 + 8 * 3];
  int n = gColors == 8 ? 8 : 4;
  m[0] = 1; m[1] = gColors == 8 ? 3 : 2; m[2] = n;
  for (int i = 0; i < n; ++i) { RGB888 p = themePal(i); m[3 + i * 3] = p.R; m[4 + i * 3] = p.G; m[5 + i * 3] = p.B; }
  wsSend(c, m, 3 + n * 3);
}

static inline uint32_t rowHash(const uint8_t * row, int bytes)
{
  const uint32_t * w = (const uint32_t *)row;
  uint32_t h = 2166136261u;
  for (int i = 0; i < bytes / 4; ++i) h = (h ^ w[i]) * 16777619u;
  return h | 1;      // never 0, so 0 can mean "not sent yet"
}

// sends changed rows; at most ~maxBytes per call so input stays snappy
static bool sendChanges(WiFiClient & c, size_t maxBytes)
{
  static uint8_t buf[4 + 24 * 240];
  const int rb = gColors == 8 ? 240 : 160;
  const int maxRows = (sizeof(buf) - 4) / rb;
  size_t sent = 0;
  int y = 0;
  while (y < SCREEN_H && sent < maxBytes) {
    const uint8_t * row = (const uint8_t *)fabgl::VGAPalettedController::sgetScanline(y);
    uint32_t h = rowHash(row, rb);
    if (h == s_hash[y]) { ++y; continue; }
    int y0 = y, n = 0;
    while (y < SCREEN_H && n < maxRows) {
      const uint8_t * r = (const uint8_t *)fabgl::VGAPalettedController::sgetScanline(y);
      uint32_t hh = rowHash(r, rb);
      if (hh == s_hash[y]) break;
      memcpy(buf + 4 + n * rb, r, rb);
      s_hash[y] = hh;
      ++n; ++y;
    }
    buf[0] = 2; buf[1] = y0 & 255; buf[2] = y0 >> 8; buf[3] = n;
    if (!wsSend(c, buf, 4 + n * rb)) return false;
    sent += 4 + n * rb;
  }
  return true;
}

// browser -> TidyOS
static bool pollInput(WiFiClient & c)
{
  while (c.available() >= 2) {
    uint8_t h[2];
    if (!readExact(c, h, 2)) return false;
    uint8_t op = h[0] & 15;
    size_t len = h[1] & 127;
    if (len == 126) { uint8_t e[2]; if (!readExact(c, e, 2)) return false; len = (e[0] << 8) | e[1]; }
    else if (len == 127) return false;
    uint8_t mask[4] = { 0, 0, 0, 0 };
    if ((h[1] & 128) && !readExact(c, mask, 4)) return false;
    uint8_t p[64];
    size_t keep = std::min(len, sizeof(p));
    if (!readExact(c, p, keep)) return false;
    for (size_t i = keep; i < len; ++i) { uint8_t junk; if (!readExact(c, &junk, 1)) return false; }
    for (size_t i = 0; i < keep; ++i) p[i] ^= mask[i & 3];
    gNetRx += len + 2;
    if (op == 8) return false;                               // close
    if (op == 9) { wsSend(c, p, keep, 10); continue; }       // ping -> pong
    if (op != 2 || keep < 1) continue;
    switch (p[0]) {
      case 1: if (keep >= 4) inputWebKey(p[1], p[2], p[3] != 0); break;
      case 2: if (keep >= 7) inputWebMouse(p[1] | (p[2] << 8), p[3] | (p[4] << 8), (int8_t)p[6], p[5]); break;
      case 4: gOpenTaskMgr = true; break;
      case 5: inputWebReleaseAll(); break;
      case 6: if (keep >= 5) netSetClockFrom(p[1] | (p[2] << 8) | (p[3] << 16) | ((uint32_t)p[4] << 24)); break;
    }
  }
  return true;
}

// ---------------------------------------------------------------- HTTP
static void dropViewer()
{
  if (s_view) { s_view = false; inputWebReleaseAll(); }
  s_ws.stop();
}

static void handleHttp(WiFiClient c)
{
  c.setTimeout(2);
  std::string line, key, path;
  bool upgrade = false, first = true;
  uint32_t t0 = millis();
  while (c.connected() && millis() - t0 < 3000) {
    if (!c.available()) { delay(2); continue; }
    char ch = c.read();
    if (ch == '\r') continue;
    if (ch != '\n') { if (line.size() < 512) line += ch; continue; }
    if (line.empty()) break;
    if (first) {
      first = false;
      size_t a = line.find(' '), b = line.find(' ', a + 1);
      if (a != std::string::npos) path = line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
    } else {
      std::string low = line;
      for (auto & k : low) k = tolower(k);
      if (low.rfind("sec-websocket-key:", 0) == 0) { key = line.substr(18); while (!key.empty() && key[0] == ' ') key.erase(0, 1); }
      if (low.rfind("upgrade:", 0) == 0 && low.find("websocket") != std::string::npos) upgrade = true;
    }
    line.clear();
  }
  gNetRx += 300;

  if (path == "/ws" && upgrade && !key.empty()) {
    std::string src = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char sha[20], b64[40];
    size_t olen = 0;
    mbedtls_sha1_ret((const unsigned char *)src.data(), src.size(), sha);
    mbedtls_base64_encode(b64, sizeof(b64), &olen, sha, 20);
    std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ";
    resp.append((const char *)b64, olen);
    resp += "\r\n\r\n";
    if (s_view) { uint8_t k = 3; wsSend(s_ws, &k, 1); dropViewer(); }     // tell the old browser it was taken over
    c.write((const uint8_t *)resp.data(), resp.size());
    c.setNoDelay(true);
    s_ws = c;
    memset(s_hash, 0, sizeof(s_hash));
    sendPalette(s_ws);
    s_view = true;
    Serial.println("[TidyOS] browser connected");
    return;
  }
  if (path == "/favicon.ico") {
    c.print("HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  } else {
    // anything else gets the desktop page (this also makes phones pop it up when joining the setup hotspot)
    char h[160];
    snprintf(h, sizeof(h), "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %u\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n", (unsigned)(sizeof(PAGE) - 1));
    c.print(h);
    for (size_t i = 0; i < sizeof(PAGE) - 1; i += 1400) c.write((const uint8_t *)PAGE + i, std::min((size_t)1400, sizeof(PAGE) - 1 - i));
    gNetTx += sizeof(PAGE);
  }
  delay(5);
  c.stop();
}

// ---------------------------------------------------------------- setup hotspot
static void apStart()
{
  if (s_ap) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(SCREEN_AP_NAME, SCREEN_AP_PASS);
  delay(100);
  s_dns.setErrorReplyCode(DNSReplyCode::NoError);
  s_dns.start(53, "*", WiFi.softAPIP());
  s_ap = true;
  s_apSince = millis();
  Serial.printf("[TidyOS] setup hotspot \"%s\" (password %s) - open http://%s\n", SCREEN_AP_NAME, SCREEN_AP_PASS, WiFi.softAPIP().toString().c_str());
}

static void apStop()
{
  if (!s_ap) return;
  s_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  s_ap = false;
  delay(200);
  netFixDns();
  Serial.println("[TidyOS] setup hotspot off - on home Wi-Fi now");
}

static void apManage()
{
  bool sta = WiFi.status() == WL_CONNECTED;
  uint32_t now = millis();
  if (sta) { if (!s_connSince) s_connSince = now; } else s_connSince = 0;
  // no Wi-Fi set up, or it hasn't connected 25 s after start: open the hotspot
  if (!s_ap && !sta && (!gSet.ssid.length() || now > 25000)) apStart();
  // on home Wi-Fi for 2 minutes and nobody uses the hotspot: close it
  if (s_ap && sta && s_connSince && now - s_connSince > 120000 && WiFi.softAPgetStationNum() == 0) apStop();
}

// ---------------------------------------------------------------- task
static void screenTask(void *)
{
  uint32_t lastFrame = 0, lastAp = 0;
  for (;;) {
    uint32_t now = millis();
    if (now - lastAp > 1000) {
      lastAp = now;
      apManage();
      if (WiFi.status() == WL_CONNECTED) netMdnsBegin();
      if (!s_listening && (s_ap || WiFi.status() == WL_CONNECTED)) { s_srv.begin(); s_srv.setNoDelay(true); s_listening = true; }
    }
    if (s_ap) s_dns.processNextRequest();
    if (s_listening) {
      WiFiClient c = s_srv.available();
      if (c) handleHttp(c);
    }
    if (s_view) {
      if (!s_ws.connected() || !pollInput(s_ws)) { dropViewer(); Serial.println("[TidyOS] browser left"); }
      else if (now - lastFrame >= 50) {
        lastFrame = now;
        if (!sendChanges(s_ws, 24000)) dropViewer();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(s_view ? 8 : 20));
  }
}

void screenBegin()
{
  static bool started = false;
  if (started) return;
  started = true;
  xTaskCreatePinnedToCore(screenTask, "screen", 6144, nullptr, 2, nullptr, 0);
}

bool screenViewer() { return s_view; }
bool screenApOn()   { return s_ap; }

std::string screenUrl()
{
  if (WiFi.status() == WL_CONNECTED) return std::string("http://") + WiFi.localIP().toString().c_str();
  if (s_ap) return std::string("http://") + WiFi.softAPIP().toString().c_str();
  return "";
}

#endif
