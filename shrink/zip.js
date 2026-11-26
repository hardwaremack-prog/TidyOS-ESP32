// A tiny zip writer (stored, no compression: music and photos are compressed already)
function zipCrc(u){var t=zipCrc.t;if(!t){t=zipCrc.t=new Int32Array(256);for(var i=0;i<256;i++){var c=i;for(var k=0;k<8;k++)c=c&1?0xEDB88320^(c>>>1):c>>>1;t[i]=c;}}
 var c=-1;for(var i=0;i<u.length;i++)c=t[(c^u[i])&255]^(c>>>8);return (c^-1)>>>0;}
function makeZip(files){                       // files: [{name, data: Uint8Array}] -> Blob
 var te=new TextEncoder(),parts=[],central=[],off=0;
 files.forEach(function(f){var nm=te.encode(f.name),crc=zipCrc(f.data),h=new Uint8Array(30+nm.length),v=new DataView(h.buffer);
  v.setUint32(0,0x04034b50,true);v.setUint16(4,20,true);v.setUint16(6,0x0800,true);v.setUint32(14,crc,true);v.setUint32(18,f.data.length,true);v.setUint32(22,f.data.length,true);
  v.setUint16(26,nm.length,true);h.set(nm,30);parts.push(h,f.data);
  var c=new Uint8Array(46+nm.length),w=new DataView(c.buffer);w.setUint32(0,0x02014b50,true);w.setUint16(4,20,true);w.setUint16(6,20,true);w.setUint16(8,0x0800,true);
  w.setUint32(16,crc,true);w.setUint32(20,f.data.length,true);w.setUint32(24,f.data.length,true);w.setUint16(28,nm.length,true);w.setUint32(42,off,true);c.set(nm,46);central.push(c);
  off+=h.length+f.data.length;});
 var cs=central.reduce(function(a,c){return a+c.length;},0),e=new Uint8Array(22),ev=new DataView(e.buffer);
 ev.setUint32(0,0x06054b50,true);ev.setUint16(8,files.length,true);ev.setUint16(10,files.length,true);ev.setUint32(12,cs,true);ev.setUint32(16,off,true);
 return new Blob(parts.concat(central,[e]),{type:'application/zip'});}
