#pragma once
// A tiny .xlsx (Excel) writer: a zip with five small XML files, no compression.
// The sheet is generated twice (once to work out its size and checksum, once to write it),
// so nothing big is held in RAM: rows come from a callback.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <functional>

namespace xlsx {

struct Sink { virtual void put(const char * p, size_t n) = 0; void put(const std::string & s) { put(s.data(), s.size()); } virtual ~Sink() {} };

struct CrcSink : Sink {
  using Sink::put;
  uint32_t crc = 0xFFFFFFFF; uint32_t len = 0;
  void put(const char * p, size_t n) override {
    len += n;
    for (size_t i = 0; i < n; ++i) {
      crc ^= (uint8_t)p[i];
      for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
    }
  }
  uint32_t value() const { return crc ^ 0xFFFFFFFF; }
};

struct FileSink : Sink {
  using Sink::put;
  FILE * f; bool ok = true; uint32_t pos = 0;
  explicit FileSink(FILE * file) : f(file) {}
  void put(const char * p, size_t n) override { if (n && fwrite(p, 1, n, f) != n) ok = false; pos += n; }
};

static void le16(std::string & s, uint16_t v) { s += (char)(v & 255); s += (char)(v >> 8); }
static void le32(std::string & s, uint32_t v) { le16(s, v & 0xFFFF); le16(s, v >> 16); }

static std::string esc(const std::string & t)
{
  std::string o;
  for (unsigned char c : t) {
    if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;";
    else if (c < 32 && c != '\t') o += ' ';
    else if (c >= 128) { char b[12]; snprintf(b, sizeof(b), "&#%d;", c); o += b; }   // Latin-1 -> character reference
    else o += (char)c;
  }
  return o;
}

static std::string colName(int c) { std::string s; c++; while (c) { s.insert(s.begin(), (char)('A' + (c - 1) % 26)); c = (c - 1) / 26; } return s; }

// cell(r, c, isNum) returns the text of row r (0 = header), column c; set isNum for a number
typedef std::function<std::string(int r, int c, bool & isNum)> CellFn;

static void sheetXml(Sink & o, int rows, int cols, const int * widths, const CellFn & cell)
{
  o.put("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">");
  if (widths) {
    o.put("<cols>");
    char b[96];
    for (int c = 0; c < cols; ++c) { snprintf(b, sizeof(b), "<col min=\"%d\" max=\"%d\" width=\"%d\" customWidth=\"1\"/>", c + 1, c + 1, widths[c]); o.put(b, strlen(b)); }
    o.put("</cols>");
  }
  o.put("<sheetData>");
  for (int r = 0; r < rows; ++r) {
    char b[48];
    snprintf(b, sizeof(b), "<row r=\"%d\">", r + 1); o.put(b, strlen(b));
    for (int c = 0; c < cols; ++c) {
      bool num = false;
      std::string v = cell(r, c, num);
      if (v.empty()) continue;
      std::string ref = colName(c) + std::to_string(r + 1);
      if (num) o.put("<c r=\"" + ref + "\"><v>" + esc(v) + "</v></c>");
      else o.put("<c r=\"" + ref + "\" t=\"inlineStr\"><is><t>" + esc(v) + "</t></is></c>");
    }
    o.put("</row>");
  }
  o.put("</sheetData></worksheet>");
}

// writes `path`; returns false if the card couldn't be written
static bool write(const char * path, const char * sheetName, int rows, int cols, const int * widths, const CellFn & cell)
{
  const std::string ct = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
    "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
    "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
    "</Types>";
  const std::string rels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
    "</Relationships>";
  const std::string wb = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
    "<sheets><sheet name=\"" + esc(sheetName) + "\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>";
  const std::string wbrels = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
    "</Relationships>";
  struct Entry { const char * name; const std::string * fixed; uint32_t crc, size, offset; };
  Entry e[5] = { { "[Content_Types].xml", &ct, 0, 0, 0 }, { "_rels/.rels", &rels, 0, 0, 0 }, { "xl/workbook.xml", &wb, 0, 0, 0 },
                 { "xl/_rels/workbook.xml.rels", &wbrels, 0, 0, 0 }, { "xl/worksheets/sheet1.xml", nullptr, 0, 0, 0 } };
  for (auto & x : e) {                                // pass 1: sizes and checksums
    CrcSink c;
    if (x.fixed) c.put(*x.fixed); else sheetXml(c, rows, cols, widths, cell);
    x.crc = c.value(); x.size = c.len;
  }
  FILE * f = fopen(path, "wb");
  if (!f) return false;
  FileSink o(f);
  const uint16_t T = 0, D = (46 << 9) | (1 << 5) | 1;   // 2026-01-01 00:00 (any valid date)
  for (auto & x : e) {                                // pass 2: write
    x.offset = o.pos;
    std::string h;
    le32(h, 0x04034b50); le16(h, 20); le16(h, 0); le16(h, 0); le16(h, T); le16(h, D);
    le32(h, x.crc); le32(h, x.size); le32(h, x.size); le16(h, strlen(x.name)); le16(h, 0);
    h += x.name;
    o.put(h);
    if (x.fixed) o.put(*x.fixed); else sheetXml(o, rows, cols, widths, cell);
  }
  uint32_t cdStart = o.pos;
  for (auto & x : e) {
    std::string h;
    le32(h, 0x02014b50); le16(h, 20); le16(h, 20); le16(h, 0); le16(h, 0); le16(h, T); le16(h, D);
    le32(h, x.crc); le32(h, x.size); le32(h, x.size); le16(h, strlen(x.name)); le16(h, 0); le16(h, 0);
    le16(h, 0); le16(h, 0); le32(h, 0); le32(h, x.offset);
    h += x.name;
    o.put(h);
  }
  std::string end;
  le32(end, 0x06054b50); le16(end, 0); le16(end, 0); le16(end, 5); le16(end, 5);
  le32(end, o.pos - cdStart); le32(end, cdStart); le16(end, 0);
  o.put(end);
  bool ok = o.ok;
  if (fclose(f) != 0) ok = false;
  return ok;
}

} // namespace xlsx
