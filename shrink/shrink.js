// Music shrinker (Tidy Pro Plus): MP3 / WAV / M4A / FLAC / OGG -> Opus 48 kbps in an Ogg file (.opus),
// done in the browser before the file goes to the board. WebCodecs encodes; this writes the Ogg pages.
var SHRINK_KBPS=48,OPUS_WORKER='https://cdn.jsdelivr.net/npm/opus-recorder@8.0.5/dist/encoderWorker.min.js',SHRINK_FORCE_WASM=false;
function oggCrc(){var t=oggCrc.t;if(!t){t=oggCrc.t=new Uint32Array(256);for(var i=0;i<256;i++){var r=i<<24;for(var k=0;k<8;k++)r=r&0x80000000?(r<<1)^0x04c11db7:r<<1;t[i]=r>>>0;}}return t;}
function oggPage(pkts,granule,serial,seq,flags){            // pkts: Uint8Array[]; returns one page
 var segs=[],body=0;pkts.forEach(function(p){var n=p.length;while(n>=255){segs.push(255);n-=255;}segs.push(n);body+=p.length;});
 var h=27+segs.length,b=new Uint8Array(h+body),v=new DataView(b.buffer);
 b.set([79,103,103,83,0,flags]);v.setUint32(6,granule%4294967296,true);v.setUint32(10,Math.floor(granule/4294967296),true);
 v.setUint32(14,serial,true);v.setUint32(18,seq,true);b[26]=segs.length;b.set(segs,27);
 var o=h;pkts.forEach(function(p){b.set(p,o);o+=p.length;});
 var t=oggCrc(),c=0;for(var i=0;i<b.length;i++)c=((c<<8)^t[((c>>>24)^b[i])&255])>>>0;v.setUint32(22,c,true);return b;}
function oggPagePart(part,granule,serial,seq,cont,more){
 var segs=[],n=part.length;while(n>=255){segs.push(255);n-=255;}if(!more)segs.push(n);else if(n)segs.push(n);
 var h=27+segs.length,b=new Uint8Array(h+part.length),v=new DataView(b.buffer);
 b.set([79,103,103,83,0,cont]);v.setUint32(6,more?4294967295:granule,true);v.setUint32(10,more?4294967295:0,true);
 v.setUint32(14,serial,true);v.setUint32(18,seq,true);b[26]=segs.length;b.set(segs,27);b.set(part,h);
 var t=oggCrc(),c=0;for(var i=0;i<b.length;i++)c=((c<<8)^t[((c>>>24)^b[i])&255])>>>0;v.setUint32(22,c,true);return b;}
function opusHead(ch,preskip){var b=new Uint8Array(19),v=new DataView(b.buffer);b.set([79,112,117,115,72,101,97,100,1,ch]);v.setUint16(10,preskip,true);v.setUint32(12,48000,true);return b;}
function opusTags(list){var te=new TextEncoder(),vend=te.encode('Tidy Pro Plus'),cs=(list||[]).map(function(x){return te.encode(x);}),n=8+4+vend.length+4;
 cs.forEach(function(c){n+=4+c.length;});var b=new Uint8Array(n),v=new DataView(b.buffer),o=0;
 b.set([79,112,117,115,84,97,103,115]);o=8;v.setUint32(o,vend.length,true);o+=4;b.set(vend,o);o+=vend.length;v.setUint32(o,cs.length,true);o+=4;
 cs.forEach(function(c){v.setUint32(o,c.length,true);o+=4;b.set(c,o);o+=c.length;});return b;}
// MP3 tags (ID3v2.3 / 2.4): title, artist, album, track, year, genre and the cover picture
function readId3(u8){
 var t={};if(u8.length<10||u8[0]!=73||u8[1]!=68||u8[2]!=51)return t;
 var ver=u8[3],ss=function(o){return (u8[o]&127)<<21|(u8[o+1]&127)<<14|(u8[o+2]&127)<<7|(u8[o+3]&127);},end=10+ss(6),o=10;
 if(u8[5]&64)o+=ver==4?ss(10):(u8[10]<<24|u8[11]<<16|u8[12]<<8|u8[13])+4;      // extended header
 function txt(a,b,enc){var d=u8.subarray(a,b);try{
  if(enc==0)return new TextDecoder('latin1').decode(d).replace(/\0.*$/,'');
  if(enc==3)return new TextDecoder('utf-8').decode(d).replace(/\0.*$/,'');
  var le=enc==1&&d[0]==255&&d[1]==254,be=enc==2||(enc==1&&d[0]==254&&d[1]==255);if(enc==1)d=d.subarray(2);
  return new TextDecoder(le||!be?'utf-16le':'utf-16be').decode(d).replace(/\0.*$/,'');}catch(e){return '';}}
 var map={TIT2:'TITLE',TPE1:'ARTIST',TALB:'ALBUM',TRCK:'TRACKNUMBER',TYER:'DATE',TDRC:'DATE',TCON:'GENRE',TPE2:'ALBUMARTIST'};
 while(o+10<=end&&o+10<=u8.length){
  var id=String.fromCharCode(u8[o],u8[o+1],u8[o+2],u8[o+3]);if(!/^[A-Z0-9]{4}$/.test(id))break;
  var sz=ver==4?ss(o+4):(u8[o+4]<<24|u8[o+5]<<16|u8[o+6]<<8|u8[o+7])>>>0,a=o+10,b=Math.min(a+sz,u8.length);
  if(map[id]&&b>a)t[map[id]]=txt(a+1,b,u8[a]).trim();
  else if(id=='APIC'&&b>a+4&&!t.cover){var enc=u8[a],p=a+1,q=p;while(q<b&&u8[q])q++;var mime=txt(p,q,0);p=q+2;      // mime, picture type
   if(enc==1||enc==2){while(p+1<b&&(u8[p]||u8[p+1]))p+=2;p+=2;}else{while(p<b&&u8[p])p++;p++;}                 // description
   if(p<b)t.cover={mime:mime.indexOf('/')>0?mime:'image/jpeg',data:u8.slice(p,b)};}
  o=a+sz;
 }
 return t;
}
// the cover, made small (up to 300 px, JPEG) so it costs ~20-40 KB, as a FLAC picture block in base64
function coverBlock(c){
 return createImageBitmap(new Blob([c.data],{type:c.mime})).then(function(img){
  var s=Math.min(1,300/Math.max(img.width,img.height)),w=Math.max(1,Math.round(img.width*s)),h=Math.max(1,Math.round(img.height*s));
  var cv=document.createElement('canvas');cv.width=w;cv.height=h;cv.getContext('2d').drawImage(img,0,0,w,h);
  function enc(q){return new Promise(function(r){cv.toBlob(r,'image/jpeg',q);}).then(function(bl){return bl.arrayBuffer();}).then(function(ab){
   if(ab.byteLength>45000&&q>0.4)return enc(q-0.15);return new Uint8Array(ab);});}
  return enc(0.82).then(function(jp){
   var mime=new TextEncoder().encode('image/jpeg'),blk=new Uint8Array(32+mime.length+jp.length),v=new DataView(blk.buffer),o=0;
   v.setUint32(o,3);o+=4;v.setUint32(o,mime.length);o+=4;blk.set(mime,o);o+=mime.length;v.setUint32(o,0);o+=4;
   v.setUint32(o,w);o+=4;v.setUint32(o,h);o+=4;v.setUint32(o,24);o+=4;v.setUint32(o,0);o+=4;v.setUint32(o,jp.length);o+=4;blk.set(jp,o);
   var s2='';for(var i=0;i<blk.length;i+=0x8000)s2+=String.fromCharCode.apply(null,blk.subarray(i,i+0x8000));return btoa(s2);});
 }).catch(function(){return null;});
}
function canShrink(f){return typeof OfflineAudioContext!='undefined'&&typeof Worker!='undefined'&&/\.(mp3|wav|m4a|aac|flac|ogg|oga|wma|aif|aiff)$/i.test(f.name);}
// Without WebCodecs (pages on plain http, like the board's own desktop, don't get it): libopus in a worker
// (opus-recorder's encoder, loaded from the CDN). Resolves to {pre: pre-skip, pages: its audio pages}.
function opusWorkerEncode(chans,len,ch,progress){
 return fetch(OPUS_WORKER).then(function(r){if(!r.ok)throw new Error('could not load the Opus encoder');return r.text();}).then(function(code){
  return new Promise(function(res,rej){
   var url=URL.createObjectURL(new Blob([code],{type:'text/javascript'})),w=new Worker(url),pages=[],pre=312,hdr=0;
   w.onerror=function(e){rej(new Error(e.message||'encoder failed'));};
   w.onmessage=function(e){var d=e.data;
    if(d&&d.message=='ready'){w.postMessage({command:'getHeaderPages'});feed();}
    else if(d&&d.message=='done'){w.terminate();URL.revokeObjectURL(url);res({pre:pre,pages:pages});}
    else if(d&&d.page){var pg=new Uint8Array(d.page);
     if(hdr<2&&pg[28]==79&&pg[29]==112&&pg[30]==117&&pg[31]==115){if(pg[32]==72)pre=pg[38]|pg[39]<<8;hdr++;return;}   // its own OpusHead / OpusTags: ours replace them
     pages.push(pg);}};
   w.postMessage({command:'init',encoderSampleRate:48000,originalSampleRate:48000,numberOfChannels:ch,encoderBitRate:SHRINK_KBPS*1000,encoderApplication:2049,encoderFrameSize:20,maxFramesPerPage:40,resampleQuality:3});
   var i=0,STEP=48000;
   function feed(){var end=Math.min(len,i+STEP*20);
    for(;i<end;i+=STEP){var n=Math.min(STEP,len-i),b=[];for(var k=0;k<ch;k++)b.push(chans[k].slice(i,i+n));w.postMessage({command:'encode',buffers:b});}
    if(progress)progress(Math.round(i*100/len));
    if(i<len)setTimeout(feed,0);else w.postMessage({command:'done'});}
  });
 });
}
// a page from the worker, renumbered after our own header pages (and its checksum redone)
function oggRenumber(pg,seq,eos){var b=pg.slice(),v=new DataView(b.buffer);v.setUint32(18,seq,true);if(eos)b[5]|=4;v.setUint32(22,0,true);
 var t=oggCrc(),c=0;for(var i=0;i<b.length;i++)c=((c<<8)^t[((c>>>24)^b[i])&255])>>>0;v.setUint32(22,c,true);return b;}
// resolves to a new .opus File, or the original when it can't be made smaller
function shrinkAudio(f,progress){
 var comments=[];
 return f.arrayBuffer().then(function(ab){
  var t=readId3(new Uint8Array(ab));
  Object.keys(t).forEach(function(k){if(k!='cover'&&t[k])comments.push(k+'='+t[k]);});
  var art=t.cover?coverBlock(t.cover).then(function(b64){if(b64)comments.push('METADATA_BLOCK_PICTURE='+b64);}):Promise.resolve();
  var ac=new OfflineAudioContext(2,48000,48000);                 // decoding here resamples to 48 kHz
  return art.then(function(){return ac.decodeAudioData(ab);});
 }).then(function(buf){
  var ch=Math.min(2,buf.numberOfChannels),len=buf.length,packets=[],err=null;
  if(typeof AudioEncoder=='undefined'||SHRINK_FORCE_WASM){
   var cs=[];for(var k=0;k<ch;k++)cs.push(buf.getChannelData(k));
   return opusWorkerEncode(cs,len,ch,progress).then(function(r){
    var serial=(Math.random()*4294967295)>>>0,seq=0,out=[];
    if(r.pages.length){var v0=new DataView(r.pages[0].buffer);serial=v0.getUint32(14,true);}
    out.push(oggPage([opusHead(ch,r.pre)],0,serial,seq++,2));
    var tg=opusTags(comments);
    for(var at=0;at<tg.length;at+=255*250){var part=tg.subarray(at,Math.min(tg.length,at+255*250));
     out.push(oggPagePart(part,0,serial,seq++,at?1:0,at+255*250<tg.length));}
    r.pages.forEach(function(pg,j){out.push(oggRenumber(pg,seq++,j==r.pages.length-1));});
    var blob=new Blob(out,{type:'audio/ogg'});
    if(blob.size>=f.size)return f;
    return new File([blob],f.name.replace(/\.[^.]+$/,'')+'.opus',{type:'audio/ogg'});
   });
  }
  return AudioEncoder.isConfigSupported({codec:'opus',sampleRate:48000,numberOfChannels:ch,bitrate:SHRINK_KBPS*1000}).then(function(s){
   if(!s.supported)throw new Error('no Opus encoder in this browser');
   var enc=new AudioEncoder({output:function(c){var d=new Uint8Array(c.byteLength);c.copyTo(d);packets.push({d:d,n:Math.round((c.duration||20000)*48/1000)});},error:function(e){err=e;}});
   enc.configure({codec:'opus',sampleRate:48000,numberOfChannels:ch,bitrate:SHRINK_KBPS*1000,opus:{frameDuration:20000,application:'audio'}});
   var CH=48000,chans=[];for(var k=0;k<ch;k++)chans.push(buf.getChannelData(k));
   var i=0;
   function feed(){                                                 // in steps, so the page stays responsive
    var end=Math.min(len,i+CH*10);
    for(;i<end;i+=CH){var n=Math.min(CH,len-i),d=new Float32Array(n*ch);for(var k=0;k<ch;k++)d.set(chans[k].subarray(i,i+n),k*n);
     enc.encode(new AudioData({format:'f32-planar',sampleRate:48000,numberOfFrames:n,numberOfChannels:ch,timestamp:Math.round(i*1e6/48000),data:d}));}
    if(progress)progress(Math.round(i*100/len));
    if(i<len)return new Promise(function(r){setTimeout(r,0);}).then(feed);
    return enc.flush().then(function(){enc.close();if(err)throw err;});
   }
   return feed();
  }).then(function(){
   var PRE=312,serial=(Math.random()*4294967295)>>>0,seq=0,out=[];
   out.push(oggPage([opusHead(ch,PRE)],0,serial,seq++,2));
   var tg=opusTags(comments);                                      // (big tags: split over pages, as Ogg allows)
   for(var at=0;at<tg.length;at+=255*250){var part=tg.subarray(at,Math.min(tg.length,at+255*250));
    out.push(oggPagePart(part,0,serial,seq++,at?1:0,at+255*250<tg.length));}
   var gran=0,page=[],segs=0;
   for(var j=0;j<packets.length;j++){
    var p=packets[j],ns=Math.floor(p.d.length/255)+1;
    if(segs+ns>255||page.length>=50){out.push(oggPage(page,PRE+gran,serial,seq++,0));page=[];segs=0;}
    page.push(p.d);segs+=ns;gran+=p.n;
   }
   out.push(oggPage(page,PRE+Math.min(gran,len),serial,seq++,4));    // the last page: end of stream, exact length
   var blob=new Blob(out,{type:'audio/ogg'});
   if(blob.size>=f.size)return f;                                  // already small: keep it as it was
   return new File([blob],f.name.replace(/\.[^.]+$/,'')+'.opus',{type:'audio/ogg'});
  });
 });
}
