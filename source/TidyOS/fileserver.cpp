// TidyOS file sharing web server (port 80, http://tidyos.local)
#include <WiFi.h>
#include <ESPmDNS.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fileserver.h"
#include "core.h"
#include "screen.h"

static volatile bool s_on = false;
static volatile bool s_taskStarted = false;
#ifdef TIDYOS_HTML5
#define FILES_PORT 8081          // port 80 shows the desktop in the HTML5 edition
#else
#define FILES_PORT 80
#endif
static WiFiServer s_srv(FILES_PORT);
static volatile bool s_media = false;
static volatile uint32_t s_lastUse = 0;   // media serving switches itself off after 10 idle minutes     // on for the music player / full-colour pictures
static bool s_listening = false;

void netMdnsBegin()
{
  static bool up = false;
  if (!up && WiFi.status() == WL_CONNECTED) {
    up = MDNS.begin(boardHost().c_str());    // http://tidyos-a1b2c3.local
    if (up) MDNS.addService("http", "tcp", 80);
  }
}

// ---------------------------------------------------------------- the page
static const char PAGE[] = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>TidyOS Files</title><style>
body{margin:0;font-family:system-ui,sans-serif;background:#00aaaa;color:#111}
header{background:#0055aa;color:#fff;padding:14px 16px;display:flex;align-items:center;gap:12px;flex-wrap:wrap}
header h1{margin:0;font-size:1.3rem}#free{opacity:.85;font-size:.9rem}
main{max-width:860px;margin:16px auto;padding:0 12px}
.card{background:#fff;border:2px solid #000;box-shadow:5px 5px 0 #000;padding:14px;margin-bottom:16px}
#crumbs a{color:#0055aa;font-weight:600;text-decoration:none;margin-right:4px}
table{width:100%;border-collapse:collapse}td{padding:7px 6px;border-bottom:1px solid #ddd}
td.s{text-align:right;color:#555;white-space:nowrap;font-size:.9rem}a.f{color:#111;text-decoration:none}
button,.btn{background:#ddd;border:2px solid #000;padding:6px 12px;font:inherit;cursor:pointer}
button.p{background:#0055aa;color:#fff}.x{background:none;border:none;color:#a00;font-size:1rem;padding:2px 6px}
#drop{border:3px dashed #0055aa;padding:20px;text-align:center;color:#0055aa;margin-top:10px}
#drop.on{background:#e8f0ff}.bar{height:8px;background:#ddd;margin-top:4px}.bar i{display:block;height:100%;background:#0055aa;width:0}
#msg{min-height:1.2em;color:#0055aa;margin-top:8px}</style></head><body>
<header><h1>TidyOS Files</h1><span id="free"></span></header><main>
<div class="card"><div id="crumbs"></div>
<div id="dlbar" style="margin:8px 0"><button class="p" onclick="dlTicked()">&#11015; Download ticked</button> <button onclick="dlFolder()">&#11015; Download this folder (ZIP)</button>
 <span style="color:#555;font-size:.9rem">One file downloads as itself, several as one ZIP. Click a file's name to download just that one.</span></div>
<table id="list"></table><div id="dlmsg" style="color:#0055aa;margin-top:6px"></div></div>
<div class="card"><button class="p" onclick="pick.click()">Upload files</button> <button onclick="mk()">New folder</button>
<input type="file" id="pick" multiple hidden><div id="drop">...or drop files here</div><div id="prog"></div><div id="msg"></div></div>
</main><script>
let cwd='/SD',rows=[],free=-1;const $=s=>document.querySelector(s);
const esc=s=>s.replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const sz=b=>b<1024?b+' B':b<1048576?(b/1024).toFixed(1)+' KB':(b/1048576).toFixed(1)+' MB';
async function load(){let j;try{j=await (await fetch('/api/list?path='+encodeURIComponent(cwd))).json()}catch(e){$('#msg').textContent='Could not reach TidyOS';return}
 if(j.error){if(cwd!='/'){cwd='/';return load()}$('#msg').textContent=j.error;return}
 $('#free').textContent=j.total?sz(j.free)+' free of '+sz(j.total):'';free=j.total?j.free:-1;
 let parts=cwd.split('/').filter(x=>x),acc='';const cr=$('#crumbs');cr.innerHTML='';
 const link=(t,p)=>{const a=document.createElement('a');a.href='#';a.textContent=t;a.onclick=()=>go(p);cr.append(a)};link('Home','/');
 for(const p of parts){acc+='/'+p;cr.append(' / ');link(acc==='/SD'?'SD card':acc==='/flash'?'Board storage':p,acc)}
 $('#dlbar').style.display=cwd=='/'?'none':'';rows=j.items;
 j.items.sort((a,b)=>b.d-a.d||a.n.localeCompare(b.n));let h='';
 if(!j.items.length)h='<tr><td>(empty)</td></tr>';
 for(const it of j.items){const p=(cwd=='/'?'':cwd)+'/'+it.n;
  const nm=cwd=='/'?(it.n=='SD'?'SD card':'Board storage'):it.n;
  h+='<tr data-p="'+esc(p)+'" data-d="'+it.d+'"><td style="width:22px">'+(cwd=='/'?'':'<input type="checkbox" class="tk" data-n="'+esc(it.n)+'">')+'</td><td>'+(it.d?'&#128193; <a class="f" href="#" data-a="open">'+esc(nm)+'</a>':'&#128196; <a class="f" href="/api/file?dl=1&path='+encodeURIComponent(p)+'">'+esc(it.n)+'</a>')+
  '</td><td class="s">'+(it.d?'':sz(it.s))+'</td><td class="s">'+(cwd=='/'?'':'<button class="x" title="Rename" data-a="ren">&#9998;</button><button class="x" title="Delete" data-a="del">&#10006;</button>')+'</td></tr>'}
 $('#list').innerHTML=h}
function go(p){cwd=p;load();return false}
$('#list').onclick=e=>{const a=e.target.closest('[data-a]');if(!a)return;e.preventDefault();const p=a.closest('tr').dataset.p;
 if(a.dataset.a=='open')go(p);if(a.dataset.a=='ren')ren(p);if(a.dataset.a=='del')del(p)};
async function post(u){const r=await fetch(u,{method:'POST'});const j=await r.json().catch(()=>({}));if(!r.ok)throw new Error(j.error||'Error');return j}
async function del(p){if(!confirm('Delete '+p.split('/').pop()+'?'))return;try{await post('/api/delete?path='+encodeURIComponent(p));load()}catch(e){alert(e.message)}}
async function ren(p){const o=p.split('/').pop(),n=prompt('New name',o);if(!n||n==o)return;try{await post('/api/rename?path='+encodeURIComponent(p)+'&to='+encodeURIComponent(n));load()}catch(e){alert(e.message)}}
async function mk(){if(cwd=='/'){alert('Open the SD card first');return}if(cwd.startsWith('/flash')){alert("The board's own storage can't have folders. Turn the SD card on in TidyOS (Switches > SD card) for folders.");return}const n=prompt('Folder name');if(!n)return;try{await post('/api/mkdir?path='+encodeURIComponent(cwd+'/'+n));load()}catch(e){alert(e.message)}}
function up(files){if(cwd=='/'){alert('Open the SD card first');return}let q=[...files],ok=0,bad=[],room=free;
 const next=()=>{const f=q.shift();if(!f){$('#prog').innerHTML='';$('#msg').textContent=(ok?ok+' file'+(ok==1?'':'s')+' uploaded. ':'')+(bad.length?'Not uploaded: '+bad.join('; '):'');load();return}
  if(room>=0&&f.size+4096>room){bad.push(f.name+' (not enough room: '+sz(Math.max(0,room))+' free'+(cwd.startsWith('/flash')?'; turn the SD card on for more':'')+')');next();return}
  $('#prog').innerHTML=esc(f.name)+(q.length?' <span style="color:#555">('+q.length+' more after this)</span>':'')+'<div class="bar"><i></i></div>';const x=new XMLHttpRequest();
  x.upload.onprogress=e=>{if(e.lengthComputable)$('.bar i').style.width=(100*e.loaded/e.total)+'%'};
  x.onload=()=>{if(x.status==200){ok++;if(room>=0)room-=f.size}else bad.push(f.name+' ('+((x.responseText.match(/"error":"([^"]*)/)||[])[1]||('error '+x.status))+')');setTimeout(next,300)};
  x.onerror=()=>{bad.push(f.name+' (the connection dropped)');setTimeout(next,1500)};
  x.open('PUT','/api/file?path='+encodeURIComponent(cwd+'/'+f.name));x.send(f)};next()}
// several files at once: fetched one after another, put together as a plain (stored) ZIP here in the browser
const T=(()=>{const t=new Uint32Array(256);for(let n=0;n<256;n++){let c=n;for(let k=0;k<8;k++)c=c&1?0xEDB88320^(c>>>1):c>>>1;t[n]=c>>>0}return t})();
const crc=d=>{let c=-1;for(let i=0;i<d.length;i++)c=T[(c^d[i])&255]^(c>>>8);return(c^-1)>>>0};
function zip(fs){const E=new TextEncoder(),P=[],C=[];let o=0;const t=new Date(),tm=t.getHours()<<11|t.getMinutes()<<5|t.getSeconds()>>1,dt=(t.getFullYear()-1980)<<9|(t.getMonth()+1)<<5|t.getDate();
 for(const f of fs){const n=E.encode(f.n),c=crc(f.d),l=f.d.length,h=new DataView(new ArrayBuffer(30)),z=new DataView(new ArrayBuffer(46));
  h.setUint32(0,0x04034b50,1);h.setUint16(4,20,1);h.setUint16(6,0x800,1);h.setUint16(10,tm,1);h.setUint16(12,dt,1);h.setUint32(14,c,1);h.setUint32(18,l,1);h.setUint32(22,l,1);h.setUint16(26,n.length,1);
  z.setUint32(0,0x02014b50,1);z.setUint16(4,20,1);z.setUint16(6,20,1);z.setUint16(8,0x800,1);z.setUint16(12,tm,1);z.setUint16(14,dt,1);z.setUint32(16,c,1);z.setUint32(20,l,1);z.setUint32(24,l,1);z.setUint16(28,n.length,1);z.setUint32(42,o,1);
  P.push(new Uint8Array(h.buffer),n,f.d);C.push(new Uint8Array(z.buffer),n);o+=30+n.length+l}
 const cs=C.reduce((a,b)=>a+b.length,0),e=new DataView(new ArrayBuffer(22));e.setUint32(0,0x06054b50,1);e.setUint16(8,fs.length,1);e.setUint16(10,fs.length,1);e.setUint32(12,cs,1);e.setUint32(16,o,1);
 return new Blob([...P,...C,new Uint8Array(e.buffer)],{type:'application/zip'})}
async function walk(dir,rel,out){const j=await (await fetch('/api/list?path='+encodeURIComponent(dir))).json();if(j.error)throw new Error(j.error);
 for(const it of j.items){if(it.d)await walk(dir+'/'+it.n,rel+it.n+'/',out);else out.push({p:dir+'/'+it.n,n:rel+it.n,s:it.s})}return out}
function save(name,blob){const a=document.createElement('a');a.href=URL.createObjectURL(blob);a.download=name;document.body.append(a);a.click();a.remove();setTimeout(()=>URL.revokeObjectURL(a.href),60000)}
let busy=false;
async function dl(list){if(busy)return;if(!list.length){$('#dlmsg').textContent='Tick some files or folders first.';return}busy=true;
 const tot=list.reduce((a,f)=>a+f.s,0)||1,t0=Date.now();let done=0;const got=[];
 try{for(let i=0;i<list.length;i++){const f=list[i];$('#dlmsg').textContent='Downloading '+(i+1)+' of '+list.length+': '+f.n+'  ('+sz(done)+' of '+sz(tot)+')';
   const r=await fetch('/api/file?path='+encodeURIComponent(f.p));if(!r.ok)throw new Error(f.n+': '+r.status);
   const d=new Uint8Array(await r.arrayBuffer());done+=d.length;got.push({n:f.n,d})}
  const nm=(cwd.split('/').filter(x=>x).join('-')||'board')+'.zip',s=((Date.now()-t0)/1000).toFixed(0);
  if(got.length==1&&!got[0].n.includes('/'))save(got[0].n,new Blob([got[0].d]));else save(nm,zip(got));
  $('#dlmsg').textContent='Downloaded '+(got.length==1?got[0].n:got.length+' files as '+nm)+' ('+sz(done)+' in '+s+' s). Look in your Downloads.'}
 catch(e){$('#dlmsg').textContent='Stopped: '+e.message}finally{busy=false}}
async function dlTicked(){const n=[...document.querySelectorAll('.tk:checked')].map(c=>c.dataset.n),l=[];
 try{for(const x of n){const it=rows.find(r=>r.n==x);if(!it)continue;if(it.d)await walk(cwd+'/'+x,x+'/',l);else l.push({p:cwd+'/'+x,n:x,s:it.s})}}catch(e){$('#dlmsg').textContent=e.message;return}dl(l)}
async function dlFolder(){$('#dlmsg').textContent='Listing the folder...';let l;try{l=await walk(cwd,'',[])}catch(e){$('#dlmsg').textContent=e.message;return}
 const t=l.reduce((a,f)=>a+f.s,0);if(t>50*1048576&&!confirm(l.length+' files, '+sz(t)+'. Download them all?'))return;dl(l)}
pick.onchange=e=>up(e.target.files);const d=$('#drop');
d.ondragover=e=>{e.preventDefault();d.classList.add('on')};d.ondragleave=()=>d.classList.remove('on');
d.ondrop=e=>{e.preventDefault();d.classList.remove('on');up(e.dataTransfer.files)};load();
</script></body></html>)HTML";

// ---------------------------------------------------------------- helpers
static std::string urlDecode(const std::string & s)
{
  std::string o;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') o += ' ';
    else if (s[i] == '%' && i + 2 < s.size()) { o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
    else o += s[i];
  }
  return o;
}

static std::string param(const std::string & query, const char * name)
{
  std::string key = std::string(name) + "=";
  size_t p = 0;
  while (p < query.size()) {
    size_t e = query.find('&', p);
    if (e == std::string::npos) e = query.size();
    if (query.compare(p, key.size(), key) == 0) return urlDecode(query.substr(p + key.size(), e - p - key.size()));
    p = e + 1;
  }
  return "";
}

static bool safePath(const std::string & p)
{
  if (p.find("..") != std::string::npos) return false;
  bool sd = p.compare(0, 3, "/SD") == 0 && (p.size() == 3 || p[3] == '/');
  bool fl = p.compare(0, 6, "/flash") == 0 && (p.size() == 6 || p[6] == '/');
  return (sd && gSDMounted) || (fl && gFlashMounted);
}

static std::string jsonEsc(const std::string & s)
{
  std::string o;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c < 32) o += ' ';
    else o += c;
  }
  return o;
}

static void send(WiFiClient & c, int code, const char * type, const std::string & body, const char * extra = "")
{
  char h[256];
  snprintf(h, sizeof(h), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n%sConnection: close\r\n\r\n",
           code, code == 200 ? "OK" : "Error", type, (unsigned)body.size(), extra);
  c.write((const uint8_t *)h, strlen(h));
  for (size_t i = 0; i < body.size(); i += 1400) c.write((const uint8_t *)body.data() + i, std::min((size_t)1400, body.size() - i));
  gNetTx += body.size();
}

static void sendErr(WiFiClient & c, int code, const std::string & msg) { send(c, code, "application/json", "{\"error\":\"" + jsonEsc(msg) + "\"}"); }

static const char * mimeOf(const std::string & p)
{
  const char * e = fileExt(p.c_str());
  if (!strcmp(e, "txt") || !strcmp(e, "md") || !strcmp(e, "log") || !strcmp(e, "csv")) return "text/plain; charset=utf-8";
  if (!strcmp(e, "html") || !strcmp(e, "htm")) return "text/html";
  if (!strcmp(e, "jpg") || !strcmp(e, "jpeg")) return "image/jpeg";
  if (!strcmp(e, "png")) return "image/png";
  if (!strcmp(e, "pdf")) return "application/pdf";
  if (!strcmp(e, "mp3")) return "audio/mpeg";
  if (!strcmp(e, "mid") || !strcmp(e, "midi")) return "audio/midi";
  if (!strcmp(e, "m4a")) return "audio/mp4";
  if (!strcmp(e, "wav")) return "audio/wav";
  if (!strcmp(e, "ogg") || !strcmp(e, "opus")) return "audio/ogg";
  if (!strcmp(e, "bmp")) return "image/bmp";
  if (!strcmp(e, "gif")) return "image/gif";
  if (!strcmp(e, "js") || !strcmp(e, "mjs")) return "text/javascript";
  if (!strcmp(e, "css")) return "text/css";
  if (!strcmp(e, "json")) return "application/json";
  if (!strcmp(e, "svg")) return "image/svg+xml";
  if (!strcmp(e, "webp")) return "image/webp";
  if (!strcmp(e, "ico")) return "image/x-icon";
  if (!strcmp(e, "wasm")) return "application/wasm";
  if (!strcmp(e, "woff2")) return "font/woff2";
  if (!strcmp(e, "mp4")) return "video/mp4";
  if (!strcmp(e, "webm")) return "video/webm";
  return "application/octet-stream";
}

static void listDir(WiFiClient & c, const std::string & path)
{
  std::string out = "{\"path\":\"" + jsonEsc(path) + "\",\"items\":[";
  bool first = true;
  int64_t total = 0, used = 0;
  if (path == "/") {
    if (gSDMounted)    { out += "{\"n\":\"SD\",\"d\":1,\"s\":0}"; first = false; }
    if (gFlashMounted) { out += std::string(first ? "" : ",") + "{\"n\":\"flash\",\"d\":1,\"s\":0}"; }
  } else {
    if (!safePath(path)) { sendErr(c, 400, "Not allowed"); return; }
    DIR * d = opendir(path.c_str());
    if (!d) { sendErr(c, 404, "Folder not found"); return; }
    struct dirent * e;
    int n = 0;
    while ((e = readdir(d)) != nullptr && n < 500) {
      std::string name = e->d_name;
      if (name == "." || name == "..") continue;
      std::string full = path + "/" + name;
      struct stat st;
      bool isDir = e->d_type == DT_DIR;
      long size = 0;
      if (!isDir && stat(full.c_str(), &st) == 0) size = st.st_size;
      out += std::string(first ? "" : ",") + "{\"n\":\"" + jsonEsc(name) + "\",\"d\":" + (isDir ? "1" : "0") + ",\"s\":" + std::to_string(size) + "}";
      first = false;
      ++n;
    }
    closedir(d);
    FileBrowser::getFSInfo(path.compare(0, 3, "/SD") == 0 ? fabgl::DriveType::SDCard : fabgl::DriveType::SPIFFS, 0, &total, &used);
  }
  out += "],\"total\":" + std::to_string((long long)total) + ",\"free\":" + std::to_string((long long)(total - used)) + "}";
  send(c, 200, "application/json", out);
}

static long s_rangeStart = -1;     // from a "Range: bytes=N-M" request (music streaming / seeking)
static long s_rangeEnd = -1;
static const long RANGE_PIECE = 192 * 1024;   // a range answer is at most this much: the browser asks for the next
                                              // piece, so it can start playing (and probe the song) without waiting

static void sendFile(WiFiClient & c, const std::string & path, bool download)
{
  if (!safePath(path)) { sendErr(c, 400, "Not allowed"); return; }
  FILE * f = fopen(path.c_str(), "rb");
  if (!f) { sendErr(c, 404, "File not found"); return; }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  long from = (s_rangeStart > 0 && s_rangeStart < size) ? s_rangeStart : 0;
  if (s_rangeStart == 0 && s_rangeEnd <= -2) { from = std::max(0L, size + 2 + s_rangeEnd); s_rangeEnd = size - 1; }   // suffix range
  long to = size - 1;
  if (s_rangeStart >= 0) {
    if (s_rangeEnd >= from && s_rangeEnd < size) to = s_rangeEnd;
    if (to - from + 1 > RANGE_PIECE) to = from + RANGE_PIECE - 1;
  }
  fseek(f, from, SEEK_SET);
  std::string name = path.substr(path.rfind('/') + 1);
  char h[400], cr[80] = "";
  if (s_rangeStart >= 0) snprintf(cr, sizeof(cr), "Content-Range: bytes %ld-%ld/%ld\r\n", from, to, size);
  // Access-Control-Allow-Origin: the TidyOS page (port 80) reads MIDI files from here (port 8081) with fetch()
  snprintf(h, sizeof(h), "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %ld\r\nAccept-Ranges: bytes\r\nAccess-Control-Allow-Origin: *\r\n%s%s%s%sConnection: close\r\n\r\n",
           s_rangeStart >= 0 ? "206 Partial Content" : "200 OK", mimeOf(path), to - from + 1, cr,
           download ? "Content-Disposition: attachment; filename=\"" : "", download ? name.c_str() : "", download ? "\"\r\n" : "");
  c.write((const uint8_t *)h, strlen(h));
  uint8_t buf[1400];
  long left = to - from + 1;
  size_t n;
  uint32_t rest = millis();
  while (left > 0 && (n = fread(buf, 1, (size_t)std::min(left, (long)sizeof(buf)), f)) > 0) {
    if (millis() - rest >= 50) { vTaskDelay(1); rest = millis(); }
    if (c.write(buf, n) != n) break;
    gNetTx += n;
    left -= n;
  }
  fclose(f);
}

static void receiveFile(WiFiClient & c, const std::string & path, long len)
{
  if (!safePath(path) || path.back() == '/') { sendErr(c, 400, "Not allowed"); return; }
  std::string tmp = path + ".part";
  FILE * f = fopen(tmp.c_str(), "wb");
  if (!f) { sendErr(c, 500, "Can't write there (is the card full or read-only?)"); return; }
  uint8_t buf[1400];
  long got = 0;
  uint32_t last = millis(), rest = millis();
  bool ok = true, full = false;
  while (got < len) {
    // a big upload arriving fast never waits: give core 0 (and the watchdog's idle task) a moment every 50 ms
    if (millis() - rest >= 50) { vTaskDelay(1); rest = millis(); }
    int av = c.available();
    if (av <= 0) {
      if (!c.connected() || millis() - last > 15000) { ok = false; break; }
      delay(2);
      continue;
    }
    int r = c.read(buf, std::min((long)sizeof(buf), std::min((long)av, len - got)));
    if (r <= 0) continue;
    if (fwrite(buf, 1, r, f) != (size_t)r) { ok = false; full = true; break; }
    got += r;
    gNetRx += r;
    last = millis();
  }
  fclose(f);
  if (full && c.connected()) {        // the card is full (or a write failed): take in the rest, so the
    uint32_t t0 = millis();                       // browser gets the answer instead of a dropped connection
    while (got < len && c.connected() && millis() - t0 < 60000) {
      int r = c.read(buf, std::min((long)sizeof(buf), len - got));
      if (r > 0) { got += r; t0 = millis(); } else delay(2);
    }
    remove(tmp.c_str());
    sendErr(c, 507, "Not enough room for it there");
    return;
  }
  if (!ok) { remove(tmp.c_str()); sendErr(c, 500, "Upload stopped part way"); return; }
  remove(path.c_str());
  rename(tmp.c_str(), path.c_str());
  send(c, 200, "application/json", "{\"ok\":true}");
}

static void handle(WiFiClient & c)
{
  s_rangeStart = s_rangeEnd = -1;
  std::string line, method, target;
  long len = 0;
  bool first = true;
  uint32_t t0 = millis();
  while (millis() - t0 < 4000) {
    if (!c.available()) { if (!c.connected()) return; delay(2); continue; }
    char ch = c.read();
    if (ch == '\r') continue;
    if (ch != '\n') { if (line.size() < 1024) line += ch; continue; }
    if (first) {
      size_t a = line.find(' '), b = line.find(' ', a + 1);
      if (a == std::string::npos) return;
      method = line.substr(0, a);
      target = line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
      first = false;
    } else if (line.empty()) break;
    else if (strncasecmp(line.c_str(), "content-length:", 15) == 0) len = atol(line.c_str() + 15);
    else if (strncasecmp(line.c_str(), "range:", 6) == 0) {
      const char * b = strstr(line.c_str(), "bytes=");
      if (b && b[6] == '-') { s_rangeStart = 0; s_rangeEnd = -2 - atol(b + 7); }   // "bytes=-N": the last N bytes
      else if (b) { char * e; s_rangeStart = strtol(b + 6, &e, 10); if (*e == '-' && isdigit((unsigned char)e[1])) s_rangeEnd = atol(e + 1); }
    }
    line.clear();
  }
  size_t q = target.find('?');
  std::string path = q == std::string::npos ? target : target.substr(0, q);
  std::string query = q == std::string::npos ? "" : target.substr(q + 1);
  std::string p = param(query, "path");

  if (method == "OPTIONS") {        // the TidyOS page (another port) asks before saving a file here (Git-Store downloads)
    static const char * R = "HTTP/1.1 204 No Content\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, PUT, POST, OPTIONS\r\n"
                            "Access-Control-Allow-Headers: Content-Type\r\nAccess-Control-Max-Age: 600\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    c.write((const uint8_t *)R, strlen(R));
  }
  else if (path == "/" || path == "/index.html") send(c, 200, "text/html; charset=utf-8", PAGE);
  else if (path == "/api/list") listDir(c, p.empty() ? "/" : p);
  else if (path.compare(0, 5, "/run/") == 0 && method == "GET") {     // a web page and its files, by path (HTML games / programs)
    std::string f = urlDecode(path.substr(4));
    if (!f.empty() && f.back() == '/') f += "index.html";
    sendFile(c, f, false);
  }
  else if (path == "/api/file" && method == "GET") sendFile(c, p, param(query, "dl") == "1");
  else if (path == "/api/file" && (method == "PUT" || method == "POST")) receiveFile(c, p, len);
  else if (path == "/api/delete" && method == "POST") {
    if (!safePath(p) || p == "/SD" || p == "/flash") sendErr(c, 400, "Not allowed");
    else if (remove(p.c_str()) == 0 || rmdir(p.c_str()) == 0) send(c, 200, "application/json", "{\"ok\":true}");
    else sendErr(c, 500, "Couldn't delete it (folders must be empty)");
  } else if (path == "/api/mkdir" && method == "POST") {
    if (!safePath(p)) sendErr(c, 400, "Not allowed");
    else if (p.compare(0, 6, "/flash") == 0) sendErr(c, 400, "The board's own storage can't have folders: turn the SD card on (Switches) for folders");
    else if (mkdir(p.c_str(), 0777) == 0) send(c, 200, "application/json", "{\"ok\":true}");
    else sendErr(c, 500, "Couldn't make that folder");
  } else if (path == "/api/rename" && method == "POST") {
    std::string to = param(query, "to");
    std::string dest = p.substr(0, p.rfind('/') + 1) + to;
    if (!safePath(p) || to.empty() || to.find('/') != std::string::npos || !safePath(dest)) sendErr(c, 400, "Not allowed");
    else if (rename(p.c_str(), dest.c_str()) == 0) send(c, 200, "application/json", "{\"ok\":true}");
    else sendErr(c, 500, "Couldn't rename it");
  } else {
    if (len > 0) { uint8_t junk[256]; while (len > 0 && c.connected()) { int r = c.read(junk, std::min((long)sizeof(junk), len)); if (r > 0) len -= r; else delay(2); } }
    sendErr(c, 404, "Not found");
  }
  c.flush();
  c.stop();
}

static void serverTask(void *)
{
  uint32_t idleSince = 0;
  for (;;) {
    bool wifi = WiFi.status() == WL_CONNECTED || screenApOn();     // home Wi-Fi, or TidyOS's own hotspot
    bool want = s_on || (s_media && millis() - s_lastUse < 600000);
    // not wanted for 20 s: close up and end the task, giving back its 6 KB stack and the socket
    if (want) idleSince = 0;
    else if (!idleSince) idleSince = millis();
    else if (millis() - idleSince > 20000) {
      if (s_listening) { s_srv.end(); s_listening = false; }
      s_taskStarted = false;
      vTaskDelete(nullptr);          // (nothing with a destructor is alive at this point)
    }
    if (want && wifi && !s_listening) { s_srv.begin(); s_listening = true; netMdnsBegin(); }
    if ((!want || !wifi) && s_listening) { s_srv.end(); s_listening = false; }
    if (s_listening) {
      WiFiClient c = s_srv.available();
      if (c) { s_lastUse = millis(); handle(c); vTaskDelay(1); continue; }   // (a breather between files of a batch)
    }
    vTaskDelay((s_listening ? 20 : 500) / portTICK_PERIOD_MS);
  }
}

void fileServerSet(bool on)
{
  s_on = on;
  if (gSet.fileShare != on) { gSet.fileShare = on; settingsSave(); }
  if (on && !s_taskStarted) {
    s_taskStarted = true;
    xTaskCreatePinnedToCore(serverTask, "fileweb", 8192, nullptr, 1, nullptr, 0);   // (SD card writes need the room)
  }
}

bool fileServerOn() { return s_on; }

// starts it (if needed) and waits up to 1.5 s until it's listening, so the browser's first request finds it
void fileServerMediaReady()
{
  fileServerMedia(true);
  for (int i = 0; i < 75 && !s_listening && (WiFi.status() == WL_CONNECTED || screenApOn()); ++i) delay(20);
}

void fileServerMedia(bool on)
{
  s_media = on;
  if (on) s_lastUse = millis();
  if (on && !s_taskStarted) {
    s_taskStarted = true;
    xTaskCreatePinnedToCore(serverTask, "fileweb", 8192, nullptr, 1, nullptr, 0);   // (SD card writes need the room)
  }
}

int fileServerPort() { return FILES_PORT; }

std::string fileServerMediaPath(const std::string & file)
{
  std::string u = "/api/file?path=";
  static const char * hex = "0123456789ABCDEF";
  for (unsigned char ch : file) {
    if (isalnum(ch) || ch == '/' || ch == '.' || ch == '-' || ch == '_') u += (char)ch;
    else { u += '%'; u += hex[ch >> 4]; u += hex[ch & 15]; }
  }
  return u;
}

std::string fileServerRunPath(const std::string & file)     // "/run/SD/HTML%20Games/x.html": relative links keep working
{
  std::string u = "/run";
  static const char * hex = "0123456789ABCDEF";
  for (unsigned char ch : file) {
    if (isalnum(ch) || ch == '/' || ch == '.' || ch == '-' || ch == '_') u += (char)ch;
    else { u += '%'; u += hex[ch >> 4]; u += hex[ch & 15]; }
  }
  return u;
}

std::string fileServerUrl()
{
  if (!s_on || (WiFi.status() != WL_CONNECTED && !screenApOn())) return "";
  std::string u = std::string("http://") + (WiFi.status() == WL_CONNECTED ? WiFi.localIP() : WiFi.softAPIP()).toString().c_str();
  if (FILES_PORT != 80) u += ":" + std::to_string(FILES_PORT);
  return u;
}
