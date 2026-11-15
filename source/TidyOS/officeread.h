#pragma once
// Reading Office files: .xlsx (first worksheet, as values), .docx (text) and old .doc (best-effort text).
// .xlsx/.docx are zip files of XML; entries are inflated in a stream with the ROM's tinfl
// (32 KB window), so a file never has to fit in RAM.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <functional>
#ifdef OFFICE_HOST_TEST
  #include <zlib.h>
#else
  #include "rom/miniz.h"
#endif

namespace office {

typedef std::function<bool(const uint8_t *, size_t)> Sink;     // return false to stop

static uint32_t rd16(const uint8_t * p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t * p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

struct ZipEntry { std::string name; uint32_t method, csize, usize, local; };

static bool zipList(FILE * f, std::vector<ZipEntry> & out, std::string & err)
{
  if (fseek(f, 0, SEEK_END) != 0) { err = "can't read the file"; return false; }
  long size = ftell(f);
  long back = size < 66000 ? size : 66000;
  std::vector<uint8_t> tail(back);
  fseek(f, size - back, SEEK_SET);
  if (fread(tail.data(), 1, back, f) != (size_t)back) { err = "can't read the file"; return false; }
  long eocd = -1;
  for (long i = back - 22; i >= 0; --i) if (rd32(&tail[i]) == 0x06054b50) { eocd = i; break; }
  if (eocd < 0) { err = "not a zip-based Office file"; return false; }
  uint32_t count = rd16(&tail[eocd + 10]), cdSize = rd32(&tail[eocd + 12]), cdOff = rd32(&tail[eocd + 16]);
  tail.clear(); tail.shrink_to_fit();
  if (cdSize > 200000) { err = "too many parts in the file"; return false; }
  std::vector<uint8_t> cd(cdSize);
  fseek(f, cdOff, SEEK_SET);
  if (fread(cd.data(), 1, cdSize, f) != cdSize) { err = "damaged file"; return false; }
  size_t p = 0;
  for (uint32_t i = 0; i < count && p + 46 <= cdSize; ++i) {
    if (rd32(&cd[p]) != 0x02014b50) break;
    ZipEntry e;
    e.method = rd16(&cd[p + 10]); e.csize = rd32(&cd[p + 20]); e.usize = rd32(&cd[p + 24]);
    uint32_t n = rd16(&cd[p + 28]), x = rd16(&cd[p + 30]), c = rd16(&cd[p + 32]);
    e.local = rd32(&cd[p + 42]);
    e.name.assign((const char *)&cd[p + 46], n);
    out.push_back(e);
    p += 46 + n + x + c;
  }
  return true;
}

// stream one entry's contents to sink
static bool zipRead(FILE * f, const ZipEntry & e, const Sink & sink, std::string & err)
{
  uint8_t lh[30];
  fseek(f, e.local, SEEK_SET);
  if (fread(lh, 1, 30, f) != 30 || rd32(lh) != 0x04034b50) { err = "damaged file"; return false; }
  fseek(f, e.local + 30 + rd16(lh + 26) + rd16(lh + 28), SEEK_SET);
  uint8_t in[1024];
  uint32_t left = e.csize;
  if (e.method == 0) {
    while (left) {
      size_t n = fread(in, 1, left < sizeof(in) ? left : sizeof(in), f);
      if (!n) { err = "damaged file"; return false; }
      left -= n;
      if (!sink(in, n)) return true;
    }
    return true;
  }
  if (e.method != 8) { err = "unusual compression in the file"; return false; }
#ifdef OFFICE_HOST_TEST
  z_stream z; memset(&z, 0, sizeof(z));
  inflateInit2(&z, -15);
  std::vector<uint8_t> out(32768);
  int r = Z_OK;
  while (r != Z_STREAM_END) {
    if (z.avail_in == 0 && left) { size_t n = fread(in, 1, left < sizeof(in) ? left : sizeof(in), f); left -= n; z.next_in = in; z.avail_in = n; }
    z.next_out = out.data(); z.avail_out = out.size();
    r = inflate(&z, Z_NO_FLUSH);
    if (r != Z_OK && r != Z_STREAM_END) { inflateEnd(&z); err = "damaged file"; return false; }
    size_t got = out.size() - z.avail_out;
    if (got && !sink(out.data(), got)) break;
  }
  inflateEnd(&z);
  return true;
#else
  tinfl_decompressor * d = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor));
  uint8_t * dict = (uint8_t *)malloc(TINFL_LZ_DICT_SIZE);
  if (!d || !dict) { free(d); free(dict); err = "not enough memory (close an app, or use black & white)"; return false; }
  tinfl_init(d);
  size_t inAvail = 0, inPos = 0, outPos = 0;
  bool ok = true;
  for (;;) {
    if (inAvail == 0 && left) {
      inAvail = fread(in, 1, left < sizeof(in) ? left : sizeof(in), f);
      if (!inAvail) { err = "damaged file"; ok = false; break; }
      left -= inAvail; inPos = 0;
    }
    size_t inN = inAvail, outN = TINFL_LZ_DICT_SIZE - outPos;
    tinfl_status st = tinfl_decompress(d, in + inPos, &inN, dict, dict + outPos, &outN, left ? TINFL_FLAG_HAS_MORE_INPUT : 0);
    inPos += inN; inAvail -= inN;
    if (outN && !sink(dict + outPos, outN)) break;
    outPos = (outPos + outN) & (TINFL_LZ_DICT_SIZE - 1);
    if (st == TINFL_STATUS_DONE) break;
    if (st < 0) { err = "damaged file"; ok = false; break; }
    if (st == TINFL_STATUS_NEEDS_MORE_INPUT && inAvail == 0 && !left) { err = "damaged file"; ok = false; break; }
  }
  free(d); free(dict);
  return ok;
#endif
}

// ---- a small streaming XML scanner: tags (name + attributes) and the text between them
struct Xml {
  std::function<void(const std::string & name, const std::string & attrs, bool end, bool selfClose)> onTag;
  std::function<void(const std::string & text)> onText;
  bool inTag = false;
  std::string buf;
  size_t textCap = 8000;
  static std::string unescape(const std::string & s) {
    if (s.find('&') == std::string::npos) return s;
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] != '&') { o += s[i]; continue; }
      size_t sc = s.find(';', i);
      if (sc == std::string::npos || sc - i > 10) { o += s[i]; continue; }
      std::string e = s.substr(i + 1, sc - i - 1);
      unsigned cp = 0;
      if (e == "amp") cp = '&'; else if (e == "lt") cp = '<'; else if (e == "gt") cp = '>'; else if (e == "quot") cp = '"'; else if (e == "apos") cp = '\'';
      else if (e.size() > 1 && e[0] == '#') cp = e[1] == 'x' ? strtoul(e.c_str() + 2, nullptr, 16) : strtoul(e.c_str() + 1, nullptr, 10);
      if (!cp) { o += s[i]; continue; }
      if (cp < 0x80) o += (char)cp;
      else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
      else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
      i = sc;
    }
    return o;
  }
  void feed(const uint8_t * p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      char c = p[i];
      if (!inTag) {
        if (c == '<') { if (!buf.empty() && onText) onText(unescape(buf)); buf.clear(); inTag = true; }
        else if (buf.size() < textCap) buf += c;
      } else {
        if (c == '>') {
          inTag = false;
          if (!buf.empty() && buf[0] != '?' && buf[0] != '!') {
            bool end = buf[0] == '/', self = buf.back() == '/';
            std::string t = buf.substr(end ? 1 : 0, buf.size() - (end ? 1 : 0) - (self ? 1 : 0));
            size_t sp = t.find_first_of(" \t\r\n");
            std::string name = sp == std::string::npos ? t : t.substr(0, sp), attrs = sp == std::string::npos ? "" : t.substr(sp);
            if (onTag) onTag(name, attrs, end, self);
          }
          buf.clear();
        } else if (buf.size() < 4000) buf += c;
      }
    }
  }
  static std::string attr(const std::string & attrs, const char * key) {
    std::string k = std::string(" ") + key + "=\"";
    size_t p = attrs.find(k);
    if (p == std::string::npos) { k[k.size() - 1] = '\''; p = attrs.find(k); if (p == std::string::npos) return ""; }
    size_t s = p + k.size(), e = attrs.find(k.back(), s);
    return e == std::string::npos ? "" : attrs.substr(s, e - s);
  }
};

// UTF-8 to the Windows-1252 that TidyOS's fonts draw
static std::string toLatin(const std::string & s)
{
  std::string o;
  for (size_t i = 0; i < s.size(); ) {
    unsigned char c = s[i];
    unsigned cp; int n;
    if (c < 0x80) { cp = c; n = 1; }
    else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) { cp = ((c & 31) << 6) | (s[i + 1] & 63); n = 2; }
    else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) { cp = ((c & 15) << 12) | ((s[i + 1] & 63) << 6) | (s[i + 2] & 63); n = 3; }
    else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) { cp = 0x10000; n = 4; }
    else { cp = '?'; n = 1; }
    i += n;
    switch (cp) {
      case 0x2018: case 0x2019: o += '\''; break;
      case 0x201C: case 0x201D: o += '"'; break;
      case 0x2013: o += (char)0x96; break;
      case 0x2014: o += (char)0x97; break;
      case 0x2026: o += "..."; break;
      case 0x2022: o += (char)0x95; break;
      case 0x20AC: o += (char)0x80; break;
      case 0xA0: o += ' '; break;
      default: o += cp < 256 ? (char)cp : '?';
    }
  }
  return o;
}

static int colOf(const std::string & ref, int & row)
{
  int c = 0; size_t i = 0;
  while (i < ref.size() && ref[i] >= 'A' && ref[i] <= 'Z') c = c * 26 + (ref[i++] - 'A' + 1);
  row = atoi(ref.c_str() + i) - 1;
  return c - 1;
}

// the first worksheet of an .xlsx: put(row, col, text) for each cell (values, not formulas)
static bool readXlsx(const char * path, const std::function<void(int, int, const std::string &)> & put, std::string & err)
{
  FILE * f = fopen(path, "rb");
  if (!f) { err = "can't open the file"; return false; }
  std::vector<ZipEntry> es;
  if (!zipList(f, es, err)) { fclose(f); return false; }
  const ZipEntry * ss = nullptr, * sheet = nullptr;
  for (auto & e : es) {
    if (e.name == "xl/sharedStrings.xml") ss = &e;
    if (e.name == "xl/worksheets/sheet1.xml") sheet = &e;
  }
  if (!sheet) for (auto & e : es) if (e.name.compare(0, 20, "xl/worksheets/sheet") == 0 && !sheet) sheet = &e;
  if (!sheet) { fclose(f); err = "no worksheet in the file"; return false; }
  std::vector<std::string> strings;
  size_t stringBytes = 0;
  if (ss) {
    Xml x; std::string cur; bool inSi = false, inT = false, inRph = false;
    x.onTag = [&](const std::string & n, const std::string &, bool end, bool self) {
      if (n == "si") { if (!end) { inSi = true; cur.clear(); } else { inSi = false; stringBytes += cur.size(); strings.push_back(stringBytes < 60000 ? toLatin(cur) : std::string()); } }
      else if (n == "t" && !self) inT = !end;
      else if (n == "rPh") inRph = !end && !self;       // phonetic hints: skip
    };
    x.onText = [&](const std::string & t) { if (inSi && inT && !inRph) cur += t; };
    if (!zipRead(f, *ss, [&](const uint8_t * p, size_t n) { x.feed(p, n); return true; }, err)) { fclose(f); return false; }
  }
  {
    Xml x; int r = -1, c = -1; std::string type, val; bool inV = false, inT = false, inIs = false;
    x.onTag = [&](const std::string & n, const std::string & a, bool end, bool self) {
      if (n == "c") {
        if (!end) { r = -1; c = colOf(Xml::attr(a, "r"), r); type = Xml::attr(a, "t"); val.clear(); }
        if (end || self) {
          if (r >= 0 && c >= 0 && !val.empty()) {
            std::string out;
            if (type == "s") { size_t i = atoi(val.c_str()); out = i < strings.size() ? strings[i] : ""; }
            else if (type == "b") out = val == "1" ? "TRUE" : "FALSE";
            else if (type == "str" || type == "inlineStr" || type == "e") out = toLatin(val);
            else { char b[32]; double v = atof(val.c_str()); snprintf(b, sizeof(b), "%.10g", v); out = b; }
            if (!out.empty()) put(r, c, out);
          }
          r = c = -1;
        }
      } else if (n == "v" && !self) { inV = !end; }
      else if (n == "is") inIs = !end && !self;
      else if (n == "t" && !self) inT = !end;
    };
    x.onText = [&](const std::string & t) { if (inV || (inIs && inT)) val += t; };
    if (!zipRead(f, *sheet, [&](const uint8_t * p, size_t n) { x.feed(p, n); return true; }, err)) { fclose(f); return false; }
  }
  fclose(f);
  return true;
}

// the text of a .docx (paragraphs become lines)
static bool readDocx(const char * path, std::string & text, std::string & err, size_t cap = 60000)
{
  FILE * f = fopen(path, "rb");
  if (!f) { err = "can't open the file"; return false; }
  std::vector<ZipEntry> es;
  if (!zipList(f, es, err)) { fclose(f); return false; }
  const ZipEntry * doc = nullptr;
  for (auto & e : es) if (e.name == "word/document.xml") doc = &e;
  if (!doc) { fclose(f); err = "no document text in the file"; return false; }
  Xml x; bool inT = false; std::string utf;
  x.onTag = [&](const std::string & n, const std::string &, bool end, bool self) {
    if (n == "w:t") inT = !end && !self;
    else if (n == "w:p" && end) utf += '\n';
    else if (n == "w:tab" && !end) utf += '\t';
    else if ((n == "w:br" || n == "w:cr") && !end) utf += '\n';
  };
  x.onText = [&](const std::string & t) { if (inT) utf += t; };
  bool ok = zipRead(f, *doc, [&](const uint8_t * p, size_t n) { x.feed(p, n); return utf.size() < cap; }, err);
  fclose(f);
  if (!ok) return false;
  text = toLatin(utf);
  return true;
}

// old Word (.doc): there's no simple way to read its layout, so pull out the runs of readable text
static bool readDoc(const char * path, std::string & text, std::string & err, size_t cap = 60000)
{
  FILE * f = fopen(path, "rb");
  if (!f) { err = "can't open the file"; return false; }
  uint8_t sig[8];
  if (fread(sig, 1, 8, f) != 8 || rd32(sig) != 0xE011CFD0) { fclose(f); err = "not a Word .doc file"; return false; }
  fseek(f, 0, SEEK_SET);
  // try both encodings (8-bit and UTF-16), keep whichever finds more readable text
  std::string a8, a16, run8, run16;
  auto good = [](unsigned c) { return (c >= 32 && c < 127) || c == '\r' || c == '\t' || (c >= 0xA0 && c <= 0xFF) || c == 0x92 || c == 0x93 || c == 0x94 || c == 0x96 || c == 0x97; };
  auto flush = [&](std::string & run, std::string & out) {
    int spaces = 0; for (char ch : run) if (ch == ' ') ++spaces;
    if (run.size() >= 12 && spaces * 12 >= (int)run.size() && out.size() < cap) {
      for (auto & ch : run) if (ch == '\r') ch = '\n';
      out += run; if (out.back() != '\n') out += '\n';
    }
    run.clear();
  };
  uint8_t buf[2048], prev = 0; bool havePrev = false; long pos = 0;
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    for (size_t i = 0; i < n; ++i, ++pos) {
      uint8_t c = buf[i];
      if (good(c)) run8 += (char)c; else flush(run8, a8);
      if (havePrev && (pos & 1)) {                       // UTF-16LE pairs on even boundaries
        unsigned w = prev | (c << 8);
        if (w == 0x2019 || w == 0x2018) run16 += '\'';
        else if (w == 0x201C || w == 0x201D) run16 += '"';
        else if (w < 256 && good(w)) run16 += (char)w; else flush(run16, a16);
      }
      prev = c; havePrev = true;
    }
  }
  flush(run8, a8); flush(run16, a16);
  fclose(f);
  text = a16.size() > a8.size() ? a16 : a8;
  if (text.empty()) { err = "couldn't find readable text in it"; return false; }
  return true;
}

} // namespace office
