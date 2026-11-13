// TidyOS file sharing web server (port 80, http://tidyos.local)
#include <WiFi.h>
#include <ESPmDNS.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fileserver.h"
#include "core.h"

static volatile bool s_on = false;
static bool s_taskStarted = false;
#ifdef TIDYOS_HTML5
#define FILES_PORT 8081          // port 80 shows the desktop in the HTML5 edition
#else
#define FILES_PORT 80
#endif
static WiFiServer s_srv(FILES_PORT);
static bool s_listening = false;

void netMdnsBegin()
{
  static bool up = false;
  if (!up && WiFi.status() == WL_CONNECTED) {
    up = MDNS.begin("tidyos");
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
<div class="card"><div id="crumbs"></div><table id="list"></table></div>
<div class="card"><button class="p" onclick="pick.click()">Upload files</button> <button onclick="mk()">New folder</button>
<input type="file" id="pick" multiple hidden><div id="drop">...or drop files here</div><div id="prog"></div><div id="msg"></div></div>
</main><script>
let cwd='/SD';const $=s=>document.querySelector(s);
const esc=s=>s.replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const sz=b=>b<1024?b+' B':b<1048576?(b/1024).toFixed(1)+' KB':(b/1048576).toFixed(1)+' MB';
async function load(){let j;try{j=await (await fetch('/api/list?path='+encodeURIComponent(cwd))).json()}catch(e){$('#msg').textContent='Could not reach TidyOS';return}
 if(j.error){if(cwd!='/'){cwd='/';return load()}$('#msg').textContent=j.error;return}
 $('#free').textContent=j.total?sz(j.free)+' free of '+sz(j.total):'';
 let parts=cwd.split('/').filter(x=>x),acc='';const cr=$('#crumbs');cr.innerHTML='';
 const link=(t,p)=>{const a=document.createElement('a');a.href='#';a.textContent=t;a.onclick=()=>go(p);cr.append(a)};link('Home','/');
 for(const p of parts){acc+='/'+p;cr.append(' / ');link(p,acc)}
 j.items.sort((a,b)=>b.d-a.d||a.n.localeCompare(b.n));let h='';
 if(!j.items.length)h='<tr><td>(empty)</td></tr>';
 for(const it of j.items){const p=(cwd=='/'?'':cwd)+'/'+it.n;
  h+='<tr data-p="'+esc(p)+'" data-d="'+it.d+'"><td>'+(it.d?'&#128193; <a class="f" href="#" data-a="open">'+esc(it.n)+'</a>':'&#128196; <a class="f" href="/api/file?dl=1&path='+encodeURIComponent(p)+'">'+esc(it.n)+'</a>')+
  '</td><td class="s">'+(it.d?'':sz(it.s))+'</td><td class="s">'+(cwd=='/'?'':'<button class="x" title="Rename" data-a="ren">&#9998;</button><button class="x" title="Delete" data-a="del">&#10006;</button>')+'</td></tr>'}
 $('#list').innerHTML=h}
function go(p){cwd=p;load();return false}
$('#list').onclick=e=>{const a=e.target.closest('[data-a]');if(!a)return;e.preventDefault();const p=a.closest('tr').dataset.p;
 if(a.dataset.a=='open')go(p);if(a.dataset.a=='ren')ren(p);if(a.dataset.a=='del')del(p)};
async function post(u){const r=await fetch(u,{method:'POST'});const j=await r.json().catch(()=>({}));if(!r.ok)throw new Error(j.error||'Error');return j}
async function del(p){if(!confirm('Delete '+p.split('/').pop()+'?'))return;try{await post('/api/delete?path='+encodeURIComponent(p));load()}catch(e){alert(e.message)}}
async function ren(p){const o=p.split('/').pop(),n=prompt('New name',o);if(!n||n==o)return;try{await post('/api/rename?path='+encodeURIComponent(p)+'&to='+encodeURIComponent(n));load()}catch(e){alert(e.message)}}
async function mk(){if(cwd=='/'){alert('Open SD first');return}const n=prompt('Folder name');if(!n)return;try{await post('/api/mkdir?path='+encodeURIComponent(cwd+'/'+n));load()}catch(e){alert(e.message)}}
function up(files){if(cwd=='/'){alert('Open SD first');return}let q=[...files];const next=()=>{const f=q.shift();if(!f){$('#prog').innerHTML='';$('#msg').textContent='Upload done';load();return}
 $('#prog').innerHTML=esc(f.name)+'<div class="bar"><i></i></div>';const x=new XMLHttpRequest();
 x.upload.onprogress=e=>{if(e.lengthComputable)$('.bar i').style.width=(100*e.loaded/e.total)+'%'};
 x.onload=()=>{if(x.status!=200)$('#msg').textContent=f.name+': '+x.responseText;next()};x.onerror=()=>{$('#msg').textContent=f.name+' failed';next()};
 x.open('PUT','/api/file?path='+encodeURIComponent(cwd+'/'+f.name));x.send(f)};next()}
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
  snprintf(h, sizeof(h), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\n%sConnection: close\r\n\r\n",
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

static void sendFile(WiFiClient & c, const std::string & path, bool download)
{
  if (!safePath(path)) { sendErr(c, 400, "Not allowed"); return; }
  FILE * f = fopen(path.c_str(), "rb");
  if (!f) { sendErr(c, 404, "File not found"); return; }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::string name = path.substr(path.rfind('/') + 1);
  char h[400];
  snprintf(h, sizeof(h), "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n%s%s%sConnection: close\r\n\r\n",
           mimeOf(path), size, download ? "Content-Disposition: attachment; filename=\"" : "", download ? name.c_str() : "", download ? "\"\r\n" : "");
  c.write((const uint8_t *)h, strlen(h));
  uint8_t buf[1400];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (c.write(buf, n) != n) break;
    gNetTx += n;
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
  uint32_t last = millis();
  bool ok = true;
  while (got < len) {
    int av = c.available();
    if (av <= 0) {
      if (!c.connected() || millis() - last > 15000) { ok = false; break; }
      delay(2);
      continue;
    }
    int r = c.read(buf, std::min((long)sizeof(buf), std::min((long)av, len - got)));
    if (r <= 0) continue;
    if (fwrite(buf, 1, r, f) != (size_t)r) { ok = false; break; }
    got += r;
    gNetRx += r;
    last = millis();
  }
  fclose(f);
  if (!ok) { remove(tmp.c_str()); sendErr(c, 500, "Upload stopped part way"); return; }
  remove(path.c_str());
  rename(tmp.c_str(), path.c_str());
  send(c, 200, "application/json", "{\"ok\":true}");
}

static void handle(WiFiClient & c)
{
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
    line.clear();
  }
  size_t q = target.find('?');
  std::string path = q == std::string::npos ? target : target.substr(0, q);
  std::string query = q == std::string::npos ? "" : target.substr(q + 1);
  std::string p = param(query, "path");

  if (path == "/" || path == "/index.html") send(c, 200, "text/html; charset=utf-8", PAGE);
  else if (path == "/api/list") listDir(c, p.empty() ? "/" : p);
  else if (path == "/api/file" && method == "GET") sendFile(c, p, param(query, "dl") == "1");
  else if (path == "/api/file" && (method == "PUT" || method == "POST")) receiveFile(c, p, len);
  else if (path == "/api/delete" && method == "POST") {
    if (!safePath(p) || p == "/SD" || p == "/flash") sendErr(c, 400, "Not allowed");
    else if (remove(p.c_str()) == 0 || rmdir(p.c_str()) == 0) send(c, 200, "application/json", "{\"ok\":true}");
    else sendErr(c, 500, "Couldn't delete it (folders must be empty)");
  } else if (path == "/api/mkdir" && method == "POST") {
    if (!safePath(p)) sendErr(c, 400, "Not allowed");
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
  for (;;) {
    bool wifi = WiFi.status() == WL_CONNECTED;
    if (s_on && wifi && !s_listening) { s_srv.begin(); s_listening = true; netMdnsBegin(); }
    if ((!s_on || !wifi) && s_listening) { s_srv.end(); s_listening = false; }
    if (s_listening) {
      WiFiClient c = s_srv.available();
      if (c) { handle(c); continue; }
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
    xTaskCreatePinnedToCore(serverTask, "fileweb", 6144, nullptr, 1, nullptr, 0);
  }
}

bool fileServerOn() { return s_on; }

std::string fileServerUrl()
{
  if (!s_on || WiFi.status() != WL_CONNECTED) return "";
  std::string u = std::string("http://") + WiFi.localIP().toString().c_str();
  if (FILES_PORT != 80) u += ":" + std::to_string(FILES_PORT);
  return u;
}
