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
#include "esp_wifi.h"
#include <DNSServer.h>
#include "lwip/sockets.h"
#include "mbedtls/sha1.h"
#include "mbedtls/base64.h"
#include "fabgl.h"
#include "core.h"
#include "screen.h"
#include "fileserver.h"
#include "net.h"

void inputWebKey(uint8_t usage, uint8_t mods, bool down);
void inputWebMouse(int x, int y, int wheel, uint8_t buttons);
void deskWeatherFromPage(const char * t);
void storeFromPage(const char * t);
void deskNewsFromPage(const char * t);
void deskTempsFromPage(const char * t);
void deskDropStart();
void deskDropped(const char * name);
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
static char          s_sentHome[64];      // last "home Wi-Fi" address sent to this browser
static char          s_mediaQ[4][300];    // media commands waiting for the page (small ring)
static volatile uint8_t s_mqW = 0, s_mqR = 0;
static portMUX_TYPE  s_mqMux = portMUX_INITIALIZER_UNLOCKED;
static volatile int  s_mState = -1, s_mPos = 0, s_mDur = 0;

// ---------------------------------------------------------------- the page
static const char PAGE[] = R"HTML(<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no"><title>TidyOS</title><style>
html,body{margin:0;height:100%;background:#1c1c1c;color:#ddd;font:14px system-ui,sans-serif;overflow:hidden}
#bar{height:36px;display:flex;align-items:center;gap:6px;padding:0 8px;background:#0055aa;color:#fff}
#bar b{font-size:16px;margin-right:6px}#st{margin-left:auto;font-size:12px;opacity:.9;white-space:nowrap}
button{background:#fff;color:#0055aa;border:0;border-radius:5px;padding:5px 10px;font:inherit;cursor:pointer}
#wrap{position:absolute;top:36px;left:0;right:0;bottom:0;display:flex;align-items:center;justify-content:center}
canvas{background:#00aaaa;touch-action:none;outline:none;cursor:default}
#kb{position:absolute;left:0;top:0;width:1px;height:1px;opacity:0;border:0;padding:0}
#home{display:none;position:absolute;top:36px;left:0;right:0;z-index:2;background:#ffd84d;color:#111;padding:8px 12px;font-size:15px;text-align:center}
#home a{color:#0055aa;font-weight:600}
#msg{position:absolute;top:36px;left:0;right:0;bottom:0;display:flex;flex-direction:column;gap:14px;align-items:center;justify-content:center;background:rgba(0,0,0,.65);font-size:18px;text-align:center;white-space:pre-line;padding:0 16px}
@media(max-width:560px){#bt,#bf{display:none}}
</style></head><body>
<div id="bar"><b>TidyOS</b><button id="bk">Keyboard</button><button id="bt">Task Manager</button><button id="br">Restart</button><button id="bs">Sleep</button><button id="bf">Full screen</button><button id="bz">Size: fit</button><span id="st">Connecting...</span></div>
<div id="home"></div><div id="iv" onclick="this.style.display='none'" style="display:none;position:fixed;inset:0;z-index:9;background:#000d;align-items:center;justify-content:center;cursor:zoom-out"><img style="max-width:96vw;max-height:94vh"></div><div id="wrap"><canvas id="c" width="640" height="480" tabindex="0"></canvas></div>
<input id="kb" autocapitalize="off" autocomplete="off" autocorrect="off" spellcheck="false">
<div id="msg"><div id="mt">Connecting to TidyOS...</div><button id="mb" style="display:none">Use it here</button></div>
<script>var MP=8081;
var c=document.getElementById('c'),g=c.getContext('2d'),o=document.createElement('canvas');o.width=640;o.height=480;window.tidyScreen=o;
var og=o.getContext('2d'),img=og.createImageData(640,480),px=new Uint32Array(img.data.buffer),ZK=1;
var home=null,ws=null,bpp=2,pal=new Uint32Array(8),dirty=false,rx=0,kicked=false,down={},mx=-1,my=-1,mb=0,mDirty=false;
var msg=document.getElementById('msg'),mt=document.getElementById('mt'),btn=document.getElementById('mb'),st=document.getElementById('st'),kb=document.getElementById('kb');
function show(t,b){mt.textContent=t;btn.style.display=b?'':'none';msg.style.display=t?'flex':'none';}
// sizing: whole device pixels when that fills most of the window (every pixel the same size = crisp text),
// smooth scaling otherwise; the Size button switches to exact 1:1 pixels (scroll if needed)
var mode=0;try{mode=+localStorage.tidySize||0;}catch(x){}
// "sharp bilinear": blow the screen up by a whole number with hard pixels, then let the browser
// shrink that a little smoothly. Every letter keeps the same shape at any window size.
function fit(){var r=window.devicePixelRatio||1,w=innerWidth,h=innerHeight-36,s=Math.min(w/640,h/480)*r,wrap=document.getElementById('wrap');
 if(mode==1)s=Math.max(1,Math.round(r));
 if(mode==2)s=Math.max(1,Math.floor(s+0.001));          // sharp: whole screen pixels, no smoothing
 ZK=Math.max(1,Math.ceil(s-0.001));if(c.width!=640*ZK){c.width=640*ZK;c.height=480*ZK;}
 c.style.width=(640*s/r)+'px';c.style.height=(480*s/r)+'px';c.style.imageRendering=mode==2?'pixelated':'auto';dirty=true;
 var big=640*s/r>w||480*s/r>h;wrap.style.overflow=mode==1?'auto':'hidden';wrap.style.alignItems=wrap.style.justifyContent=(mode==1&&big)?'flex-start':'center';
 document.getElementById('bz').textContent=mode==1?'Size: 1:1':mode==2?'Size: sharp':'Size: fit';}
addEventListener('resize',fit);
function send(a){if(ws&&ws.readyState==1)ws.send(new Uint8Array(a));}
function connect(){
 kicked=false;show('Connecting to TidyOS...');
 ws=new WebSocket('ws://'+location.host+'/ws');ws.binaryType='arraybuffer';
 ws.onopen=function(){show('');st.textContent='Connected';c.focus();var t=Math.floor(Date.now()/1000);send([6,t&255,t>>8&255,t>>16&255,t>>>24]);wxAt=0;setTimeout(weather,1500);nwAt=0;setTimeout(news,3000);};
 ws.onmessage=function(e){var d=new Uint8Array(e.data);rx+=d.length;
  if(d[0]==1){bpp=d[1];for(var i=0;i<d[2];i++)pal[i]=0xff000000|d[5+i*3]<<16|d[4+i*3]<<8|d[3+i*3];}
  else if(d[0]==2){var y=d[1]|d[2]<<8,n=d[3],o=4,rb=bpp==1?80:bpp==2?160:240;
   for(var r=0;r<n;r++,y++,o+=rb){var p=y*640;
    if(bpp==1){for(var x=0;x<80;x++){var b=d[o+x];for(var i=7;i>=0;i--)px[p++]=pal[b>>i&1];}}
    else if(bpp==2){for(var x=0;x<160;x++){var b=d[o+x];px[p++]=pal[b>>6];px[p++]=pal[b>>4&3];px[p++]=pal[b>>2&3];px[p++]=pal[b&3];}}
    else{for(var k=0;k<80;k++){var q=o+k*3,v=d[q]|d[q+1]<<8|d[q+2]<<16;for(var i=0;i<8;i++)px[p++]=pal[v>>(21-3*i)&7];}}}
   dirty=true;}
  else if(d[0]==8){var t=new TextDecoder().decode(d.subarray(1)).split('\n');ver=t[0];upBase=parseInt(t[1])||0;upAt=Date.now();}
  else if(d[0]==9)media(new TextDecoder().decode(d.subarray(1)));
  else if(d[0]==3){kicked=true;show('TidyOS was opened in another browser.',true);}
  else if(d[0]==7){var t=new TextDecoder().decode(d.subarray(1)).split('\n');home={ssid:t[0],ip:t[1]};
   var hb=document.getElementById('home');
   if(location.hostname!=home.ip&&location.hostname!='tidyos.local'){hb.innerHTML='TidyOS is on <b></b> now. Switch this device to that Wi-Fi, then open <a href="http://'+home.ip+'/">http://'+home.ip+'</a> or <a href="http://tidyos.local/">http://tidyos.local</a>';hb.querySelector('b').textContent=home.ssid;hb.style.display='block';}
   else hb.style.display='none';}};
 ws.onclose=function(){st.textContent='Disconnected';if(!kicked){
  var t='Lost connection - trying again...';
  if(location.hostname=='192.168.4.1')t='Lost the TidyOS-Setup hotspot - trying again...\nIf TidyOS just joined your Wi-Fi, switch this device to '+(home?home.ssid:'that Wi-Fi')+' and open http://'+(home?home.ip:'tidyos.local');
  show(t);setTimeout(connect,2000);}};
}
btn.onclick=function(){connect();};
function frame(){if(dirty){og.putImageData(img,0,0);g.imageSmoothingEnabled=false;g.drawImage(o,0,0,640*ZK,480*ZK);dirty=false;}
 if(mDirty){mDirty=false;send([2,mx&255,mx>>8,my&255,my>>8,mb,0]);}requestAnimationFrame(frame);}
requestAnimationFrame(frame);
var ver='',upBase=-1,upAt=0,au=null,auBlocked=false;
var bT=null,bA=null,ac=null;
function beepWav(){var r=8000,n=r*0.7|0,b=new ArrayBuffer(44+n*2),v=new DataView(b),w=function(o,s){for(var i=0;i<s.length;i++)v.setUint8(o+i,s.charCodeAt(i));};
 w(0,'RIFF');v.setUint32(4,36+n*2,true);w(8,'WAVEfmt ');v.setUint32(16,16,true);v.setUint16(20,1,true);v.setUint16(22,1,true);v.setUint32(24,r,true);v.setUint32(28,r*2,true);v.setUint16(32,2,true);v.setUint16(34,16,true);w(36,'data');v.setUint32(40,n*2,true);
 for(var i=0;i<n;i++){var t=i/r,k=t%0.22,on=t<0.66&&k<0.13;v.setInt16(44+i*2,on?Math.sin(2*Math.PI*880*t)*12000*Math.min(1,(0.13-k)*60,k*400):0,true);}
 return URL.createObjectURL(new Blob([b],{type:'audio/wav'}));}
function beep(){try{if(!bA){bA=new Audio(beepWav());}bA.currentTime=0;bA.play().catch(function(){auBlocked=true;beepWA();});}catch(e){beepWA();}}
function beepWA(){try{if(!ac)ac=new(window.AudioContext||window.webkitAudioContext)();if(ac.state=='suspended')ac.resume();var t=ac.currentTime;
 for(var i=0;i<3;i++){var o=ac.createOscillator(),g=ac.createGain();o.frequency.value=880;o.connect(g);g.connect(ac.destination);g.gain.value=0;g.gain.setValueAtTime(0.3,t+i*0.22);g.gain.setValueAtTime(0,t+i*0.22+0.13);o.start(t+i*0.22);o.stop(t+i*0.22+0.15);}}catch(e){}}
function alarmBanner(on){var e=document.getElementById('alm');if(!e){e=document.createElement('div');e.id='alm';e.style.cssText='position:fixed;top:40px;left:50%;transform:translateX(-50%);z-index:20;background:#c00;color:#fff;font:bold 22px sans-serif;padding:12px 22px;border-radius:8px;cursor:pointer;display:none';e.textContent='⏰ Alarm! Click to silence';e.onclick=function(){clearInterval(bT);bT=null;alarmBanner(false);};document.body.appendChild(e);}
 e.style.display=on?'block':'none';document.title=on?'⏰ Alarm - TidyOS':'TidyOS';}
function unlockSound(){if(!bA){try{bA=new Audio(beepWav());bA.volume=0;bA.play().then(function(){bA.pause();bA.volume=1;}).catch(function(){bA.volume=1;});}catch(e){}}}
function mUrl(p){return location.protocol+'//'+location.hostname+':'+MP+p;}
function auReport(){if(!au)return;var t=au.currentTime|0,u=isFinite(au.duration)?au.duration|0:0;send([8,au.ended?2:au.paused?0:1,t&255,t>>8&255,u&255,u>>8&255]);}
var midiCur=null;
function midiParse(d){var p=0,u16=function(){var v=d[p]<<8|d[p+1];p+=2;return v;},u32=function(){var v=(d[p]<<24|d[p+1]<<16|d[p+2]<<8|d[p+3])>>>0;p+=4;return v;},
 vlq=function(){var v=0,b;do{b=d[p++];v=v*128+(b&127);}while(b&128&&p<d.length);return v;};
 for(var o=0;o<d.length-4&&o<512;o++)if(d[o]==77&&d[o+1]==84&&d[o+2]==104&&d[o+3]==100)break;if(o>0&&o<d.length-4)d=d.subarray(o);if(String.fromCharCode(d[0],d[1],d[2],d[3])!='MThd')throw 'not midi';p=4;var hl=u32();u16();var nt=u16(),div=u16();if(div&0x8000)div=480;p=8+hl;
 var ev=[];for(var k=0;k<nt&&p<d.length;k++){var id=String.fromCharCode(d[p],d[p+1],d[p+2],d[p+3]);p+=4;var len=u32(),end=p+len,tick=0,rs=0;if(id!='MTrk'){p=end;continue;}
  while(p<end){tick+=vlq();var st=d[p];if(st&128)p++;else st=rs;
   if(st==0xFF){var ty=d[p++],l=vlq();if(ty==0x51&&l==3)ev.push({k:tick,y:2,tp:d[p]<<16|d[p+1]<<8|d[p+2]});p+=l;}
   else if(st==0xF0||st==0xF7){p+=vlq();}
   else{rs=st;var c=st&15,h=st>>4,a=d[p++],b=(h==12||h==13)?0:d[p++];
    if(h==9&&b>0)ev.push({k:tick,y:1,c:c,n:a,v:b});else if(h==8||(h==9&&b==0))ev.push({k:tick,y:0,c:c,n:a});else if(h==12)ev.push({k:tick,y:3,c:c,n:a});}}
  p=end;}
 var ord=[2,3,0,1];ev.sort(function(x,y){return x.k-y.k||ord[x.y]-ord[y.y];});var tp=500000,lt=0,ts=0;
 for(var i=0;i<ev.length;i++){ts+=(ev[i].k-lt)*tp/div/1e6;lt=ev[i].k;ev[i].t=ts;if(ev[i].y==2)tp=ev[i].tp;}
 return ev;}
var noiseBuf=null;
function midiNote(m,e,at){var c=m.ctx,g=c.createGain();g.connect(m.out);
 if(e.c==9){if(!noiseBuf){noiseBuf=c.createBuffer(1,c.sampleRate/2,c.sampleRate);var q=noiseBuf.getChannelData(0);for(var i=0;i<q.length;i++)q[i]=Math.random()*2-1;}
  var n=e.n;if(n==35||n==36){var o=c.createOscillator();o.frequency.setValueAtTime(150,at);o.frequency.exponentialRampToValueAtTime(40,at+0.15);o.connect(g);g.gain.setValueAtTime(0.9*e.v/127,at);g.gain.exponentialRampToValueAtTime(0.001,at+0.2);o.start(at);o.stop(at+0.22);return;}
  var s=c.createBufferSource(),f=c.createBiquadFilter();s.buffer=noiseBuf;s.connect(f);f.connect(g);var hat=n==42||n==44||n==46||n>=49;f.type=hat?'highpass':'bandpass';f.frequency.value=hat?7000:1800;
  var len=n==46||n>=49?0.3:hat?0.05:0.15;g.gain.setValueAtTime(0.5*e.v/127,at);g.gain.exponentialRampToValueAtTime(0.001,at+len);s.start(at);s.stop(at+len+0.02);return;}
 var key=e.c*128+e.n;if(m.on[key])midiOff(m,key,at);if(m.count>40)return;
 var o=c.createOscillator();o.type=e.n<48?'sawtooth':(m.prog[e.c]>=40&&m.prog[e.c]<80?'square':'triangle');o.frequency.value=440*Math.pow(2,(e.n-69)/12);o.connect(g);
 var vol=(o.type=='triangle'?0.22:0.07)*e.v/127;g.gain.setValueAtTime(0,at);g.gain.linearRampToValueAtTime(vol,at+0.01);g.gain.setTargetAtTime(vol*0.6,at+0.05,0.3);
 o.start(at);m.on[key]={o:o,g:g};m.count++;}
function midiOff(m,key,at){var x=m.on[key];if(!x)return;x.g.gain.cancelScheduledValues(at);x.g.gain.setTargetAtTime(0,at,0.05);x.o.stop(at+0.3);delete m.on[key];m.count--;}
function midiPump(){var m=midiCur;if(!m||m.paused)return;var now=m.ctx.currentTime;
 while(m.i<m.ev.length&&m.ev[m.i].t+m.t0<now+0.4){var e=m.ev[m.i++],at=Math.max(now,e.t+m.t0);
  if(e.y==1)midiNote(m,e,at);else if(e.y==0)midiOff(m,e.c*128+e.n,at);else if(e.y==3)m.prog[e.c]=e.n;}
 if(m.i>=m.ev.length&&now>m.t0+m.dur+0.5){m.ended=true;midiReport();clearInterval(m.timer);}}
function midiReport(){var m=midiCur;if(!m)return;var t=m.ended?m.dur:Math.max(0,m.ctx.currentTime-m.t0);t=t|0;var u=m.dur|0;send([8,m.ended?2:m.paused?0:1,t&255,t>>8&255,u&255,u>>8&255]);}
function midiStop(){if(!midiCur)return;var m=midiCur;midiCur=null;clearInterval(m.timer);clearInterval(m.rep);try{m.ctx.close();}catch(e){}}
function midiPlay(url,vol){midiStop();if(au){au.pause();}
 fetch(url).then(function(r){return r.arrayBuffer();}).then(function(b){var ev=midiParse(new Uint8Array(b)),C=window.AudioContext||window.webkitAudioContext,ctx=new C();
  var m={ctx:ctx,ev:ev,i:0,on:{},count:0,prog:[],paused:false,ended:false,out:ctx.createGain()};m.out.gain.value=vol;m.out.connect(ctx.destination);
  m.dur=ev.length?ev[ev.length-1].t:0;m.t0=ctx.currentTime+0.15;midiCur=m;if(ctx.state=='suspended'){ctx.resume();auBlocked=true;}
  m.timer=setInterval(midiPump,50);m.rep=setInterval(midiReport,1000);midiPump();}).catch(function(){send([8,2,0,0,0,0]);});}

function openBanner(u,n){var w=null;try{w=window.open(u,'_blank');}catch(e){}if(w)return;
 var e=document.getElementById('ob');if(!e){e=document.createElement('div');e.id='ob';e.style.cssText='position:fixed;top:44px;left:50%;transform:translateX(-50%);z-index:21;background:#0055aa;color:#fff;font:bold 18px sans-serif;padding:10px 14px;border-radius:8px;display:none;gap:10px;align-items:center;box-shadow:0 4px 16px #0008';document.body.appendChild(e);}
 e.innerHTML='';var b=document.createElement('button');b.textContent='▶ Play '+n;b.style.cssText='font:bold 18px sans-serif;padding:8px 14px;cursor:pointer';
 b.onclick=function(){window.open(u,'_blank');e.style.display='none';};var x=document.createElement('button');x.textContent='✕';x.style.cssText='font:16px sans-serif;padding:6px 10px;cursor:pointer';x.onclick=function(){e.style.display='none';};
 e.appendChild(b);e.appendChild(x);e.style.display='flex';}
var wxAt=0;
function wxSend(s){send([12].concat(Array.from(new TextEncoder().encode(s.slice(0,62)))));}
function geo(){var tries=[['https://ipwho.is/',function(j){return j.success!==false&&j.latitude!=null?{lat:j.latitude,lon:j.longitude,city:j.city}:null;}],
  ['https://ipapi.co/json/',function(j){return j.latitude!=null?{lat:j.latitude,lon:j.longitude,city:j.city}:null;}],
  ['http://ip-api.com/json/?fields=status,city,lat,lon',function(j){return j.status=='success'?{lat:j.lat,lon:j.lon,city:j.city}:null;}]],i=0;
 return new Promise(function(ok,bad){(function next(){if(i>=tries.length){bad('location lookup blocked');return;}var t=tries[i++];
  fetch(t[0]).then(function(r){return r.json();}).then(function(j){var g=t[1](j);if(g)ok(g);else next();}).catch(next);})();});}
function weather(){if(!ws||ws.readyState!=1||Date.now()-wxAt<25*60000)return;wxAt=Date.now();
 geo().then(function(g){
  return fetch('https://api.open-meteo.com/v1/forecast?latitude='+g.lat+'&longitude='+g.lon+'&current=temperature_2m,weather_code&daily=temperature_2m_max,temperature_2m_min&hourly=temperature_2m&past_days=4&temperature_unit=fahrenheit&timezone=auto&forecast_days=1')
   .then(function(r){return r.json();}).then(function(w){
    try{var h=w.hourly,now=Date.now(),pts=[];for(var i=0;i<h.time.length;i++){if(new Date(h.time[i]).getTime()>now)break;pts.push(h.temperature_2m[i]);}
     pts=pts.slice(-96);var s=[];for(var i=0;i<pts.length;i+=3)if(pts[i]!=null)s.push(Math.round(pts[i]));
     send([17].concat(Array.from(new TextEncoder().encode(s.join(',')))));}catch(e){}
    wxSend(Math.round(w.current.temperature_2m)+'|'+w.current.weather_code+'|'+Math.round(w.daily.temperature_2m_max[0])+'|'+Math.round(w.daily.temperature_2m_min[0])+'|'+(+g.lat).toFixed(2)+'|'+(+g.lon).toFixed(2)+'|'+(g.city||'').replace(/[^\x20-\x7e]/g,'').slice(0,24));})
   .catch(function(){wxSend('E|weather service unreachable|'+(+g.lat).toFixed(2)+'|'+(+g.lon).toFixed(2));throw 1;});})
 .catch(function(e){if(e!==1)wxSend('E|'+(typeof e=='string'?e:'no internet from this browser'));wxAt=Date.now()-22*60000;});}
setInterval(weather,30000);
function rep13(s){send([13].concat(Array.from(new TextEncoder().encode(s.slice(0,1000)))));}
function store(v){var a=v.split('\t'),op=a[0],R=(a[1]||'').replace(/\/+$/,''),B=a[2];
 var chk=function(r){return r.json().then(function(j){if(!r.ok||j.error)throw j.error||('error '+r.status);return j;},function(){throw 'the relay gave an odd answer ('+r.status+')';});};
 var err=function(e){rep13(op+'\t'+(typeof e=='string'?e:'could not reach the relay'));};
 var F=function(n){return R+'/file?board='+B+'&name='+encodeURIComponent(n);};
 if(op=='L')fetch(R+'/list?board='+B).then(chk).then(function(j){var s='L\tok\t'+j.used+'\t'+j.max;j.files.forEach(function(f){s+='\n'+f.name.replace(/[^\x20-\x7e]/g,'_')+'\t'+f.size;});rep13(s);}).catch(err);
 else if(op=='U')fetch(mUrl(a[3])).then(function(r){if(!r.ok)throw 'could not read the file on the board';return r.blob();}).then(function(b){return fetch(F(a[4]),{method:'PUT',body:b});}).then(chk).then(function(){rep13('U\tok\t'+a[4]);}).catch(err);
 else if(op=='D')fetch(F(a[3])).then(function(r){if(!r.ok)throw 'not in the store';return r.blob();}).then(function(b){return fetch(mUrl(a[4]),{method:'PUT',body:b});}).then(function(r){if(!r.ok)throw 'could not save it on the board';rep13('D\tok\t'+a[3]);}).catch(err);
 else if(op=='X')fetch(F(a[3]),{method:'DELETE'}).then(chk).then(function(){rep13('X\tok\t'+a[3]);}).catch(err);}
var nwAt=0;function plain(t){return t.replace(/[‘’]/g,"'").replace(/[“”]/g,'"').replace(/[–—]/g,'-').replace(/…/g,'...').replace(/&amp;/g,'&').replace(/[^\x20-\x7e]/g,'').trim();}
function news(){if(!ws||ws.readyState!=1||Date.now()-nwAt<30*60000)return;nwAt=Date.now();
 var feeds=['https://feeds.npr.org/1001/rss.xml','https://feeds.bbci.co.uk/news/rss.xml'],i=0;
 (function next(){if(i>=feeds.length){nwAt=Date.now()-25*60000;return;}var f=feeds[i++];
  fetch('https://api.rss2json.com/v1/api.json?rss_url='+encodeURIComponent(f)).then(function(r){return r.json();}).then(function(j){
   if(j.status!='ok'||!j.items||!j.items.length)throw 0;var t=j.items.slice(0,6).map(function(x){return plain(x.title||'').slice(0,120);});
   send([14].concat(Array.from(new TextEncoder().encode(t.join('\n')))));}).catch(next);})();}
setInterval(news,60000);
var dropQ=[];
function note(t,ms){var e=document.getElementById('dn');if(!e){e=document.createElement('div');e.id='dn';e.style.cssText='position:fixed;bottom:18px;right:18px;z-index:22;background:#222;color:#fff;font:15px sans-serif;padding:10px 14px;border-radius:8px;box-shadow:0 4px 16px #0008';document.body.appendChild(e);}
 e.textContent=t;e.style.display='block';clearTimeout(e.t);if(ms)e.t=setTimeout(function(){e.style.display='none';},ms);}
addEventListener('dragover',function(e){e.preventDefault();e.dataTransfer.dropEffect='copy';},false);
addEventListener('drop',function(e){e.preventDefault();var fs=[].slice.call(e.dataTransfer.files||[]);if(!fs.length)return;
 if(!ws||ws.readyState!=1){note('TidyOS is not connected.',4000);return;}dropQ=dropQ.concat(fs);note('Getting TidyOS ready for '+fs.length+' file'+(fs.length>1?'s':'')+'...');send([15]);},false);
function dropNext(){var f=dropQ.shift();if(!f){return;}var nm=f.name.replace(/[\/\\:*?"<>|]/g,'_').replace(/[^\x20-\x7e]/g,'_').slice(0,90),tries=0;
 note('Sending '+nm+' ('+Math.ceil(f.size/1024)+' KB)...');
 (function put(){fetch(mUrl('/api/file?path='+encodeURIComponent('/SD/Incoming/'+nm)),{method:'PUT',body:f}).then(function(r){if(!r.ok)throw r.status;
   send([16].concat(Array.from(new TextEncoder().encode(nm))));note('Sent '+nm+' - pick a folder on TidyOS.',dropQ.length?0:5000);dropNext();})
  .catch(function(e){if(++tries<6&&typeof e!='number'){setTimeout(put,800);return;}   // the board's file server may still be starting
   note('Couldn\'t send '+nm+(typeof e=='number'?' (TidyOS said '+e+')':' (no answer from TidyOS)')+'.',8000);dropNext();});})();}
function media(t){var k=t.charAt(0),v=t.substr(2),iv=document.getElementById('iv');
 if(midiCur&&(k=='p'||k=='r'||k=='s'||k=='v')){if(k=='p'){midiCur.paused=true;midiCur.ctx.suspend();midiReport();}else if(k=='r'){midiCur.paused=false;midiCur.ctx.resume();}else if(k=='s'){midiStop();send([8,255,0,0,0,0]);}else midiCur.out.gain.value=Math.max(0,Math.min(1,+v));return;}
 if(k=='m'){midiPlay(mUrl(v),au?au.volume:0.8);return;}
 if(k=='o'){var t=v.split('\t');openBanner(mUrl(t[0]),t[1]||'');return;}
 if(k=='g'){store(v);return;}
 if(k=='w'){wxAt=0;weather();nwAt=0;news();return;}
 if(k=='u'){if(v=='go')setTimeout(dropNext,300);else{dropQ=[];note('Dropped files need an SD card in TidyOS (turn it on in Switches).',6000);}return;}
 if(k=='a'){midiStop();if(!au){au=new Audio();au.onended=auReport;setInterval(auReport,1000);
   au.onerror=function(){if(au.tries++<4&&au.getAttribute('src'))setTimeout(function(){au.load();au.play().catch(function(){auBlocked=true;});},1500);};}
  au.tries=0;au.src=mUrl(v);au.play().catch(function(e){if(e&&e.name=='NotAllowedError')auBlocked=true;});}
 else if(k=='p'&&au)au.pause();else if(k=='r'&&au)au.play().catch(function(){auBlocked=true;});
 else if(k=='s'&&au){au.pause();au.removeAttribute('src');au.load();send([8,255,0,0,0,0]);}
 else if(k=='v'&&au)au.volume=Math.max(0,Math.min(1,+v));else if(k=='k'&&au)au.currentTime=+v;
 else if(k=='i'){iv.firstChild.src=mUrl(v);iv.style.display='flex';}else if(k=='x')iv.style.display='none';
 else if(k=='b'){clearInterval(bT);bT=null;alarmBanner(false);if(v=='1'){var n=0;alarmBanner(true);beep();bT=setInterval(function(){beep();if(++n>=120){clearInterval(bT);bT=null;}},1000);}else if(v=='2')beep();}}
addEventListener('pointerdown',function(){unlockSound();if(auBlocked&&au){auBlocked=false;au.play();}if(ac&&ac.state=='suspended')ac.resume();if(midiCur&&midiCur.ctx.state=='suspended'&&!midiCur.paused)midiCur.ctx.resume();},true);
addEventListener('keydown',function(){unlockSound();},true);
function upStr(){if(upBase<0)return '';var u=upBase+Math.floor((Date.now()-upAt)/1000),d=Math.floor(u/86400),h=Math.floor(u/3600)%24,m=Math.floor(u/60)%60,s=u%60;
 return 'Up '+(d?d+'d '+h+'h '+m+'m':h?h+'h '+m+'m':m+'m '+s+'s');}
setInterval(function(){if(ws&&ws.readyState==1)st.textContent='Connected  '+(rx/1024).toFixed(0)+' KB/s'+(upBase>=0?'   \u00b7   '+upStr():'')+(ver?'   \u00b7   v'+ver:'');rx=0;},1000);
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
document.getElementById('bs').onclick=function(){if(confirm('Put TidyOS into deep sleep? It uses almost no power. Press the BOOT button on the board (or EN) to wake it, then reload this page.')){send([11]);setTimeout(function(){show('TidyOS is asleep. Press BOOT on the board to wake it, then reload.');},600);}c.focus();};
document.getElementById('br').onclick=function(){if(confirm('Restart TidyOS? Anything not saved will be lost.')){send([10]);}c.focus();};
document.getElementById('bz').onclick=function(){mode=mode==0?2:mode==2?1:0;try{localStorage.tidySize=mode;}catch(x){}fit();};
fit();
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

// ---- outbox: one frame for the browser waits here and drains without ever blocking the server.
// A fixed buffer reserved at start-up: no memory is asked for while running (on a busy hotspot
// RAM is too broken up to grow a buffer, even with plenty free in total).
static uint8_t  s_ob[4 + 4 + 16 * 160];   // ws header + frame header + 16 rows (4 colours) / 10 rows (8 colours)
static size_t   s_obLen = 0, s_obPos = 0;
static uint32_t s_outStuck = 0;          // when the outbox last made no progress

static size_t outPending() { return s_obLen - s_obPos; }

// small message (pong, home address); false if the outbox is busy (caller retries later)
static bool wsQueue(const uint8_t * d, size_t n, uint8_t opcode = 2)
{
  if (outPending() || n + 4 > sizeof(s_ob)) return false;
  size_t hl;
  s_ob[0] = 0x80 | opcode;
  if (n < 126) { s_ob[1] = n; hl = 2; }
  else { s_ob[1] = 126; s_ob[2] = n >> 8; s_ob[3] = n & 255; hl = 4; }
  memcpy(s_ob + hl, d, n);
  s_obLen = hl + n; s_obPos = 0;
  gNetTx += s_obLen;
  return true;
}

// false = the connection is gone (or stuck for 15 s)
static bool flushOut(WiFiClient & c)
{
  int fd = c.fd();
  if (fd < 0) return false;
  while (outPending()) {
    int r = send(fd, s_ob + s_obPos, std::min(outPending(), (size_t)2920), MSG_DONTWAIT);
    if (r > 0) { s_obPos += r; s_outStuck = 0; continue; }
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOMEM)) {
      if (!s_outStuck) s_outStuck = millis();
      else if (millis() - s_outStuck > 15000) return false;
      break;
    }
    return false;
  }
  if (!outPending()) { s_obLen = s_obPos = 0; }
  return true;
}

static void sendPalette(WiFiClient & c)
{
  uint8_t m[3 + 8 * 3];
  int n = gColors;
  m[0] = 1; m[1] = gColors == 8 ? 3 : gColors == 2 ? 1 : 2; m[2] = n;
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

// puts the next run of changed rows (up to 24) into the outbox; returns its size, 0 = nothing changed
static size_t queueChanges()
{
  if (outPending()) return 0;
  const int rb = gColors == 8 ? 240 : gColors == 2 ? 80 : 160;
  const int maxRows = (int)((sizeof(s_ob) - 8) / rb);
  for (int y = 0; y < SCREEN_H; ++y) {
    const uint8_t * row = (const uint8_t *)fabgl::VGAPalettedController::sgetScanline(y);
    if (rowHash(row, rb) == s_hash[y]) continue;
    int y0 = y, n = 0;
    uint8_t * out = s_ob + 8;
    while (y < SCREEN_H && n < maxRows) {
      const uint8_t * r = (const uint8_t *)fabgl::VGAPalettedController::sgetScanline(y);
      uint32_t hh = rowHash(r, rb);
      if (hh == s_hash[y]) break;
      memcpy(out + n * rb, r, rb);
      s_hash[y] = hh;
      ++n; ++y;
    }
    size_t len = 4 + (size_t)n * rb;
    s_ob[4] = 2; s_ob[5] = y0 & 255; s_ob[6] = y0 >> 8; s_ob[7] = n;
    if (len >= 126) {                                // WebSocket: 2-byte length form
      s_ob[0] = 0x82; s_ob[1] = 126; s_ob[2] = len >> 8; s_ob[3] = len & 255;
      s_obPos = 0;
    } else {                                         // short frame (one black-and-white row): the
      s_ob[2] = 0x82; s_ob[3] = len;                 // short length form is required, or browsers drop us
      s_obPos = 2;
    }
    s_obLen = 4 + len;
    gNetTx += s_obLen;
    return s_obLen;
  }
  return 0;
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
    static uint8_t p[1100];                        // replies from the page (Git-Store lists) can be long
    size_t keep = std::min(len, sizeof(p));
    if (!readExact(c, p, keep)) return false;
    for (size_t i = keep; i < len; ++i) { uint8_t junk; if (!readExact(c, &junk, 1)) return false; }
    for (size_t i = 0; i < keep; ++i) p[i] ^= mask[i & 3];
    gNetRx += len + 2;
    if (op == 8) return false;                               // close
    if (op == 9) { wsQueue(p, keep, 10); continue; }         // ping -> pong
    if (op != 2 || keep < 1) continue;
    switch (p[0]) {
      case 1: if (keep >= 4) inputWebKey(p[1], p[2], p[3] != 0); break;
      case 2: if (keep >= 7) inputWebMouse(p[1] | (p[2] << 8), p[3] | (p[4] << 8), (int8_t)p[6], p[5]); break;
      case 4: gOpenTaskMgr = true; break;
      case 10: gRestartReq = true; break;
      case 11: gSleepReq = true; break;
      case 12: { char t[65]; size_t n = std::min(keep - 1, sizeof(t) - 1); memcpy(t, p + 1, n); t[n] = 0; deskWeatherFromPage(t); break; }   // weather, looked up by the page
      case 13: { p[keep < sizeof(p) ? keep : sizeof(p) - 1] = 0; storeFromPage((const char *)p + 1); break; }   // Git-Store reply
      case 15: deskDropStart(); break;                                                                  // files dropped on the page
      case 16: { p[keep < sizeof(p) ? keep : sizeof(p) - 1] = 0; deskDropped((const char *)p + 1); break; }
      case 17: { p[keep < sizeof(p) ? keep : sizeof(p) - 1] = 0; deskTempsFromPage((const char *)p + 1); break; } // temperatures, last 4 days
      case 14: { p[keep < sizeof(p) ? keep : sizeof(p) - 1] = 0; deskNewsFromPage((const char *)p + 1); break; } // news headlines
      case 5: inputWebReleaseAll(); break;
      case 6: if (keep >= 5) netSetClockFrom(p[1] | (p[2] << 8) | (p[3] << 16) | ((uint32_t)p[4] << 24)); break;
      case 8: if (keep >= 6) { s_mState = p[1] == 255 ? -1 : p[1]; s_mPos = p[2] | (p[3] << 8); s_mDur = p[4] | (p[5] << 8); } break;
    }
  }
  return true;
}

// ---------------------------------------------------------------- HTTP
static void dropViewer()
{
  s_obLen = s_obPos = 0; s_outStuck = 0;
  if (s_view) { s_view = false; inputWebReleaseAll(); }
  s_ws.stop();
}

// parses a complete request header (already read) and answers it
static void handleHttp(WiFiClient c, const std::string & req)
{
  std::string key, path;
  bool upgrade = false, first = true;
  size_t st = 0;
  while (st < req.size()) {
    size_t e = req.find('\n', st);
    if (e == std::string::npos) e = req.size();
    std::string line = req.substr(st, e - st);
    st = e + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    if (first) {
      first = false;
      size_t sp1 = line.find(' '), sp2 = line.find(' ', sp1 + 1);
      if (sp1 != std::string::npos) path = line.substr(sp1 + 1, sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);
    } else {
      std::string low = line;
      for (auto & k : low) k = tolower(k);
      if (low.rfind("sec-websocket-key:", 0) == 0) { key = line.substr(18); while (!key.empty() && key[0] == ' ') key.erase(0, 1); }
      if (low.rfind("upgrade:", 0) == 0 && low.find("websocket") != std::string::npos) upgrade = true;
    }
  }
  c.setTimeout(2);
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
    s_obLen = s_obPos = 0; s_outStuck = 0;
    sendPalette(s_ws);
    {   // version + uptime for the page's top bar (the page keeps counting from here)
      char m[48];
      int n = snprintf(m, sizeof(m), "\x08" TIDYOS_VERSION "\n%lu", (unsigned long)(millis() / 1000));
      if (n > 0 && n < (int)sizeof(m)) wsSend(s_ws, (const uint8_t *)m, n);
    }
    s_sentHome[0] = 0;
    s_view = true;
    Serial.println("[TidyOS] browser connected");
    return;
  }
  std::string p = path.substr(0, path.find('?'));
  if (p == "/favicon.ico") {
    c.print("HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  } else if (p != "/" && p != "/index.html") {
    // phones' "is there internet?" checks and anything else: a tiny redirect to the desktop
    // (makes the phone offer to open it, without sending the whole page to every check)
    std::string host = s_ap && WiFi.status() != WL_CONNECTED ? WiFi.softAPIP().toString().c_str() : WiFi.localIP().toString().c_str();
    std::string r = "HTTP/1.0 302 Found\r\nLocation: http://" + host + "/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    c.write((const uint8_t *)r.data(), r.size());
  } else {
    // the desktop page
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
  static bool logged = false;
  if (!logged) {
    logged = true;
    WiFi.onEvent([](arduino_event_id_t e, arduino_event_info_t info) {
      if (e == ARDUINO_EVENT_WIFI_AP_STACONNECTED)
        Serial.printf("[TidyOS] hotspot: device %02x:%02x:%02x:%02x:%02x:%02x joined\n", info.wifi_ap_staconnected.mac[0], info.wifi_ap_staconnected.mac[1],
                      info.wifi_ap_staconnected.mac[2], info.wifi_ap_staconnected.mac[3], info.wifi_ap_staconnected.mac[4], info.wifi_ap_staconnected.mac[5]);
      else if (e == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) Serial.println("[TidyOS] hotspot: a device left");
      else if (e == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED)   Serial.printf("[TidyOS] hotspot: gave out %s\n", IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str());
    });
  }
  WiFi.mode(WIFI_AP_STA);
  // channel 6, up to 4 devices; lower transmit power so the current peaks don't dip the 3.3 V supply
  // (the hotspot only needs to reach across a room)
  WiFi.softAP(SCREEN_AP_NAME, SCREEN_AP_PASS, 6, 0, 4);
  WiFi.setTxPower(WIFI_POWER_11dBm);
  WiFi.setSleep(false);
  delay(100);
  Serial.printf("[TidyOS] hotspot up on channel %d, free RAM %u\n", WiFi.channel(), (unsigned)ramFree());
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
  if (s_ap && sta && s_connSince && now - s_connSince > 300000 && WiFi.softAPgetStationNum() == 0) apStop();
}

// Several connections at once, none allowed to stall the others: browsers and Windows open
// connections early and may say nothing for a while. Each one's request is collected bit by bit;
// silent ones are dropped after 4 s, and the oldest is dropped if all slots are busy.
struct Pending { WiFiClient c; std::string buf; uint32_t t0 = 0; bool used = false; };
static Pending s_pend[4];

static void pollHttp()
{
  for (int n = 0; n < 4; ++n) {              // accept what's waiting
    WiFiClient c = s_srv.available();
    if (!c) break;
    int slot = -1;
    uint32_t oldest = 0xFFFFFFFF;
    for (int i = 0; i < 4; ++i) {
      if (!s_pend[i].used) { slot = i; break; }
      if (s_pend[i].t0 < oldest) { oldest = s_pend[i].t0; slot = i; }
    }
    if (s_pend[slot].used) s_pend[slot].c.stop();
    s_pend[slot].c = c;
    s_pend[slot].buf.clear();
    s_pend[slot].t0 = millis();
    s_pend[slot].used = true;
  }
  for (auto & p : s_pend) {
    if (!p.used) continue;
    int a = p.c.available();
    if (a > 0) {
      uint8_t tmp[256];
      int r = p.c.read(tmp, std::min(a, (int)sizeof(tmp)));
      if (r > 0 && p.buf.size() < 2048) p.buf.append((const char *)tmp, r);
      gNetRx += r > 0 ? r : 0;
    }
    bool done = p.buf.find("\r\n\r\n") != std::string::npos || p.buf.find("\n\n") != std::string::npos;
    if (done) {
      WiFiClient c = p.c;
      std::string req;
      req.swap(p.buf);
      p.used = false;
      p.c = WiFiClient();
      handleHttp(c, req);
    } else if (!p.c.connected() || millis() - p.t0 > 4000 || p.buf.size() >= 2048) {
      p.c.stop();
      p.used = false;
      p.buf.clear();
      p.buf.shrink_to_fit();
    }
  }
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
    if (s_ap) for (int i = 0; i < 4; ++i) s_dns.processNextRequest();   // answer a burst of lookups promptly
    if (s_listening) pollHttp();
    if (s_view) {
      if (!s_ws.connected() || !pollInput(s_ws)) { dropViewer(); Serial.println("[TidyOS] browser left"); }
      else if (now - lastAp == 0 && WiFi.status() == WL_CONNECTED) {
        // tell the page where TidyOS lives on the home Wi-Fi (it shows this if it's on the hotspot)
        // (fixed buffers: nothing is allocated here, it runs every second)
        static char m[64];
        IPAddress ip = WiFi.localIP();
        wifi_ap_record_t ap;
        const char * ssid = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? (const char *)ap.ssid : "";
        int n = snprintf(m, sizeof(m), "\x07%.32s\n%u.%u.%u.%u", ssid, ip[0], ip[1], ip[2], ip[3]);
        if (n > 0 && n < (int)sizeof(m) && strcmp(m + 1, s_sentHome) != 0 && wsQueue((const uint8_t *)m, n))
          strncpy(s_sentHome, m + 1, sizeof(s_sentHome) - 1);
      }
      while (s_view && s_mqR != s_mqW && outPending() == 0) {
        static uint8_t mm[224];
        const char * cmd = s_mediaQ[s_mqR % 4];
        size_t n = strlen(cmd);
        mm[0] = 9; memcpy(mm + 1, cmd, n);
        if (!wsQueue(mm, n + 1)) break;
        s_mqR++;
        if (!flushOut(s_ws)) break;
      }
      if (s_view && !flushOut(s_ws)) { dropViewer(); Serial.println("[TidyOS] browser stopped receiving"); }
      else if (s_view && now - lastFrame >= 50 && outPending() == 0) {
        lastFrame = now;
        size_t budget = 0;
        while (s_view && budget < 8000 && outPending() == 0) {
          size_t n = queueChanges();
          if (!n) break;
          budget += n;
          if (!flushOut(s_ws)) { dropViewer(); break; }
        }
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

void screenMedia(const std::string & cmd)
{
  portENTER_CRITICAL(&s_mqMux);
  if ((uint8_t)(s_mqW - s_mqR) >= 4) s_mqR++;           // full: drop the oldest
  strlcpy(s_mediaQ[s_mqW % 4], cmd.c_str(), sizeof(s_mediaQ[0]));
  s_mqW++;
  portEXIT_CRITICAL(&s_mqMux);
}

void screenMediaState(int & state, int & pos, int & dur) { state = s_mState; pos = s_mPos; dur = s_mDur; }
bool screenApOn()   { return s_ap; }

std::string screenUrl()
{
  if (WiFi.status() == WL_CONNECTED) return std::string("http://") + WiFi.localIP().toString().c_str();
  if (s_ap) return std::string("http://") + WiFi.softAPIP().toString().c_str();
  return "";
}

#endif
