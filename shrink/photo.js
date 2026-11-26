// Photo shrinker (Tidy Pro Plus / The Shelf): resizes in the browser and saves as JPEG (TidyOS Pro: its Pictures
// app decodes JPEG on the board) or AVIF (The Shelf). iPhone HEIC photos are read with heic-to, and AVIF is made with
// the jsquash AVIF encoder; both load from the jsDelivr CDN only when needed (nothing is stored on the board).
var PHOTO_CDN={avif:'https://cdn.jsdelivr.net/npm/@jsquash/avif@2.1.1/codec/enc/',heic:'https://cdn.jsdelivr.net/npm/heic-to@1.6.7/dist/heic-to.js'};
var AVIF_OPTS={quality:55,qualityAlpha:-1,denoiseLevel:0,tileColsLog2:0,tileRowsLog2:0,speed:6,subsample:1,chromaDeltaQ:false,sharpness:0,tune:0,enableSharpYUV:false,bitDepth:8,lossless:false};
var avifMod=null;
function avifEncoder(){if(!avifMod)avifMod=import(PHOTO_CDN.avif+'avif_enc.js').then(function(m){return m.default({locateFile:function(p){return PHOTO_CDN.avif+p;}});});return avifMod;}
function isPhoto(f){return /\.(jpe?g|png|webp|heic|heif|bmp|gif|avif)$/i.test(f.name);}
function photoBitmap(f){
 var heic=/\.(heic|heif)$/i.test(f.name);
 var src=heic?import(PHOTO_CDN.heic).then(function(m){return m.heicTo({blob:f,type:'image/jpeg',quality:0.92});}):Promise.resolve(f);
 return src.then(function(b){return createImageBitmap(b,{imageOrientation:'from-image'});});
}
// fmt 'jpeg' or 'avif'; maxPx: the longer side. Resolves to a new File, or the original when that's already smaller.
function shrinkPhoto(f,fmt,maxPx){
 return photoBitmap(f).then(function(img){
  var s=Math.min(1,maxPx/Math.max(img.width,img.height)),w=Math.max(1,Math.round(img.width*s)),h=Math.max(1,Math.round(img.height*s));
  var cv=document.createElement('canvas');cv.width=w;cv.height=h;var g=cv.getContext('2d');
  if(fmt=='jpeg'){g.fillStyle='#fff';g.fillRect(0,0,w,h);}                    // (JPEG has no transparency)
  g.drawImage(img,0,0,w,h);
  var made=fmt=='avif'
   ?avifEncoder().then(function(m){var out=m.encode(new Uint8Array(g.getImageData(0,0,w,h).data.buffer),w,h,AVIF_OPTS);if(!out)throw new Error('AVIF encoding failed');return new Blob([out],{type:'image/avif'});})
     .catch(function(){return new Promise(function(r){cv.toBlob(r,'image/webp',0.8);});})   // no AVIF encoder (offline?): WebP, nearly as small
   :new Promise(function(r){cv.toBlob(r,'image/jpeg',0.82);});
  return made.then(function(b){
   var ext=b.type=='image/avif'?'.avif':b.type=='image/webp'?'.webp':'.jpg';
   var sameKind=(ext=='.jpg'&&/\.jpe?g$/i.test(f.name))||(ext!='.jpg'&&new RegExp('\\'+ext+'$','i').test(f.name));
   if(b.size>=f.size&&sameKind)return f;                                     // already small
   return new File([b],f.name.replace(/\.[^.]+$/,'')+ext,{type:b.type});
  });
 });
}
