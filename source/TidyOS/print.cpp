// TidyOS printing: PDF + PWG raster writers, IPP client, printer discovery, Print window
#include <WiFi.h>
#include <ESPmDNS.h>
#include <stdio.h>
#include <sys/stat.h>
#include "print.h"
#include "core.h"
#include "style.h"
#include "net.h"

using namespace fabgl;

static const int COLS = 80;          // characters per printed line
static const int LPP  = 56;          // lines per page (after the header)

// ====================================================================== layout

static void wrapLines(const PrintDoc & doc, std::vector<std::string> & out)
{
  for (auto l : doc.lines) {
    for (auto & c : l) if ((unsigned char)c < 32 || (unsigned char)c > 126) c = (c == '\t') ? ' ' : '?';
    if (l.empty()) { out.push_back(""); continue; }
    while ((int)l.size() > COLS) {
      int cut = l.rfind(' ', COLS);
      if (cut <= 0) cut = COLS;
      out.push_back(l.substr(0, cut));
      l.erase(0, cut);
      while (!l.empty() && l[0] == ' ') l.erase(0, 1);
    }
    out.push_back(l);
  }
  if (out.empty()) out.push_back("");
}

static std::string headerLine(const PrintDoc & doc, int page, int pages)
{
  char right[32];
  snprintf(right, sizeof(right), "Page %d of %d", page, pages);
  std::string t = doc.title.substr(0, COLS - strlen(right) - 2);
  return t + std::string(COLS - t.size() - strlen(right), ' ') + right;
}

// ====================================================================== PDF

static std::string pdfEsc(const std::string & s)
{
  std::string o;
  for (char c : s) {
    if (c == '(' || c == ')' || c == '\\') o += '\\';
    o += c;
  }
  return o;
}

bool makePdf(const PrintDoc & doc, const char * path, std::string & err)
{
  std::vector<std::string> L;
  wrapLines(doc, L);
  int pages = (L.size() + LPP - 1) / LPP;
  FILE * f = fopen(path, "wb");
  if (!f) { err = "Can't write the file (is the SD card in?)"; return false; }
  std::vector<long> off(4 + pages * 2, 0);
  fputs("%PDF-1.4\n%\xE2\xE3\xCF\xD3\n", f);
  off[1] = ftell(f); fputs("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n", f);
  off[3] = ftell(f); fputs("3 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Courier /Encoding /WinAnsiEncoding >>\nendobj\n", f);
  for (int p = 0; p < pages; ++p) {
    int po = 4 + p * 2, co = po + 1;
    std::string c = "BT /F1 10 Tf 12 TL 54 738 Td (" + pdfEsc(headerLine(doc, p + 1, pages)) + ") Tj T* (" + std::string(COLS, '-') + ") Tj T*\n";
    for (int i = p * LPP; i < (int)L.size() && i < (p + 1) * LPP; ++i)
      c += "(" + pdfEsc(L[i]) + ") Tj T*\n";
    c += "ET\n";
    off[po] = ftell(f);
    fprintf(f, "%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> /Contents %d 0 R >>\nendobj\n", po, co);
    off[co] = ftell(f);
    fprintf(f, "%d 0 obj\n<< /Length %u >>\nstream\n", co, (unsigned)c.size());
    fwrite(c.data(), 1, c.size(), f);
    fputs("\nendstream\nendobj\n", f);
  }
  off[2] = ftell(f);
  fputs("2 0 obj\n<< /Type /Pages /Kids [", f);
  for (int p = 0; p < pages; ++p) fprintf(f, "%d 0 R ", 4 + p * 2);
  fprintf(f, "] /Count %d >>\nendobj\n", pages);
  long xref = ftell(f);
  int n = 4 + pages * 2;
  fprintf(f, "xref\n0 %d\n0000000000 65535 f \n", n);
  for (int i = 1; i < n; ++i) fprintf(f, "%010ld 00000 n \n", off[i]);
  fprintf(f, "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", n, xref);
  bool ok = !ferror(f);
  fclose(f);
  if (!ok) err = "Writing the PDF failed";
  return ok;
}

// ====================================================================== PWG raster (300 dpi, 8-bit gray, US Letter)

static const int DPI = 300, PW = 2550, PH = 3300;
static const int GLYPH_SX = 3, GLYPH_SY = 3;          // 8x14 font drawn 3x: 24 x 42 dots, 80 columns = 6.4 inches
static const int LINE_PITCH = 50, LEFT = (PW - COLS * 8 * GLYPH_SX) / 2, TOP = 150;

static void be32(uint8_t * p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void pwgHeader(uint8_t * h, int totalPages)
{
  memset(h, 0, 1796);
  strcpy((char *)h, "PwgRaster");
  be32(h + 276, DPI); be32(h + 280, DPI);              // HWResolution
  be32(h + 340, 1);                                    // NumCopies
  be32(h + 352, 612); be32(h + 356, 792);              // PageSize (points)
  be32(h + 372, PW);  be32(h + 376, PH);               // Width, Height
  be32(h + 384, 8);   be32(h + 388, 8);                // BitsPerColor, BitsPerPixel
  be32(h + 392, PW);                                   // BytesPerLine
  be32(h + 396, 0);                                    // ColorOrder chunky
  be32(h + 400, 18);                                   // ColorSpace sGray
  be32(h + 420, 1);                                    // NumColors
  be32(h + 452, totalPages);                           // TotalPageCount
  be32(h + 456, 1); be32(h + 460, 1);                  // Cross/FeedTransform
  be32(h + 484, 4);                                    // PrintQuality normal
  strcpy((char *)h + 1732, "na_letter_8.5x11in");      // PageSizeName
}

// one compressed line (PWG "PackBits-like": runs of a repeated pixel or literal pixels)
static void pwgLine(FILE * f, const uint8_t * row, int repeat)
{
  uint8_t out[PW + PW / 64 + 8];
  int n = 0;
  out[n++] = repeat - 1;
  int i = 0;
  while (i < PW) {
    int run = 1;
    while (i + run < PW && run < 128 && row[i + run] == row[i]) ++run;
    if (run >= 2 || i + 1 >= PW) {
      out[n++] = run - 1;
      out[n++] = row[i];
      i += run;
    } else {
      int lit = 1;
      while (i + lit < PW && lit < 128 && !(i + lit + 1 < PW && row[i + lit] == row[i + lit + 1])) ++lit;
      if (lit == 1) { out[n++] = 0; out[n++] = row[i]; i += 1; continue; }
      out[n++] = 257 - lit;
      memcpy(out + n, row + i, lit);
      n += lit;
      i += lit;
    }
  }
  fwrite(out, 1, n, f);
}

static void renderRow(uint8_t * row, const std::string * text, int glyphRow)
{
  memset(row, 255, PW);
  if (!text) return;
  const uint8_t * data = FONT_8x14.data;
  int fh = FONT_8x14.height;
  for (int c = 0; c < (int)text->size() && c < COLS; ++c) {
    uint8_t bits = data[(uint8_t)(*text)[c] * fh + glyphRow];
    if (!bits) continue;
    int x0 = LEFT + c * 8 * GLYPH_SX;
    for (int b = 0; b < 8; ++b)
      if (bits & (0x80 >> b))
        memset(row + x0 + b * GLYPH_SX, 0, GLYPH_SX);
  }
}

bool makePwg(const PrintDoc & doc, const char * path, std::string & err)
{
  std::vector<std::string> L;
  wrapLines(doc, L);
  int pages = (L.size() + LPP - 1) / LPP;
  FILE * f = fopen(path, "wb");
  if (!f) { err = "Can't write the print file (is the SD card in?)"; return false; }
  uint8_t * row  = (uint8_t *) malloc(PW);
  uint8_t * prev = (uint8_t *) malloc(PW);
  uint8_t * hdr  = (uint8_t *) malloc(1796);
  if (!row || !prev || !hdr) { free(row); free(prev); free(hdr); fclose(f); err = "Out of memory"; return false; }
  fwrite("RaS2", 1, 4, f);
  std::string dashes(COLS, '-');
  for (int p = 0; p < pages; ++p) {
    pwgHeader(hdr, pages);
    fwrite(hdr, 1, 1796, f);
    std::string head = headerLine(doc, p + 1, pages);
    int repeat = 0;
    for (int y = 0; y < PH; ++y) {
      // which text line / glyph row is at this y?
      const std::string * t = nullptr;
      int gr = 0;
      int ly = y - TOP;
      if (ly >= 0) {
        int li = ly / LINE_PITCH, within = ly % LINE_PITCH;
        if (within < FONT_8x14.height * GLYPH_SY) {
          gr = within / GLYPH_SY;
          if (li == 0) t = &head;
          else if (li == 1) t = &dashes;
          else if (li - 2 < LPP && p * LPP + li - 2 < (int)L.size()) t = &L[p * LPP + li - 2];
        }
      }
      renderRow(row, t, gr);
      if (repeat > 0 && repeat < 256 && !memcmp(row, prev, PW)) { ++repeat; continue; }
      if (repeat > 0) pwgLine(f, prev, repeat);
      memcpy(prev, row, PW);
      repeat = 1;
    }
    if (repeat > 0) pwgLine(f, prev, repeat);
  }
  free(row); free(prev); free(hdr);
  bool ok = !ferror(f);
  fclose(f);
  if (!ok) err = "Writing the print file failed";
  return ok;
}

// ====================================================================== IPP

struct PrinterUri { std::string host, path; int port = 631; };

static bool parseUri(std::string u, PrinterUri & out)
{
  while (!u.empty() && u[0] == ' ') u.erase(0, 1);
  while (!u.empty() && u.back() == ' ') u.pop_back();
  if (u.empty()) return false;
  size_t s = u.find("://");
  if (s != std::string::npos) u = u.substr(s + 3);
  size_t sl = u.find('/');
  std::string hp = sl == std::string::npos ? u : u.substr(0, sl);
  out.path = sl == std::string::npos ? "/ipp/print" : u.substr(sl);
  size_t c = hp.find(':');
  out.host = c == std::string::npos ? hp : hp.substr(0, c);
  out.port = c == std::string::npos ? 631 : atoi(hp.c_str() + c + 1);
  return !out.host.empty();
}

static void ippAttr(std::string & b, uint8_t tag, const char * name, const std::string & value)
{
  size_t nl = strlen(name);
  b += (char)tag;
  b += (char)(nl >> 8); b += (char)(nl & 255); b += name;
  b += (char)(value.size() >> 8); b += (char)(value.size() & 255); b += value;
}

// returns IPP status (0 = OK), or -1 when the printer couldn't be reached
static int ippPrintFile(const PrinterUri & pu, const char * path, const char * mime, const std::string & title, std::string & err)
{
  FILE * f = fopen(path, "rb");
  if (!f) { err = "Print file missing"; return -1; }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);

  std::string ipp;
  ipp += (char)1; ipp += (char)1;                // IPP 1.1
  ipp += (char)0; ipp += (char)2;                // Print-Job
  ipp += (char)0; ipp += (char)0; ipp += (char)0; ipp += (char)1;   // request id
  ipp += (char)0x01;                             // operation attributes
  ippAttr(ipp, 0x47, "attributes-charset", "utf-8");
  ippAttr(ipp, 0x48, "attributes-natural-language", "en");
  ippAttr(ipp, 0x45, "printer-uri", "ipp://" + pu.host + ":" + std::to_string(pu.port) + pu.path);
  ippAttr(ipp, 0x42, "requesting-user-name", "tidyos");
  ippAttr(ipp, 0x42, "job-name", title.substr(0, 60));
  ippAttr(ipp, 0x49, "document-format", mime);
  ipp += (char)0x03;                             // end of attributes

  WiFiClient c;
  if (!c.connect(pu.host.c_str(), pu.port, 6000)) { fclose(f); err = "Could not reach the printer at " + pu.host; return -1; }
  char head[300];
  snprintf(head, sizeof(head), "POST %s HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/ipp\r\nContent-Length: %ld\r\nConnection: close\r\n\r\n",
           pu.path.c_str(), pu.host.c_str(), pu.port, (long)(ipp.size() + size));
  c.write((const uint8_t *)head, strlen(head));
  c.write((const uint8_t *)ipp.data(), ipp.size());
  uint8_t buf[1024];
  size_t n;
  bool sendOk = true;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (c.write(buf, n) != n) { sendOk = false; break; }
  }
  fclose(f);
  if (!sendOk) { c.stop(); err = "The printer stopped taking data"; return -1; }

  // read the reply: HTTP status, headers, then the IPP answer
  uint32_t t0 = millis();
  std::string resp;
  while (millis() - t0 < 30000) {
    while (c.available() && resp.size() < 4096) resp += (char)c.read();
    size_t he = resp.find("\r\n\r\n");
    if (he != std::string::npos && resp.size() >= he + 8) break;
    if (!c.connected() && !c.available()) break;
    delay(10);
  }
  c.stop();
  int httpCode = 0;
  size_t sp = resp.find(' ');
  if (sp != std::string::npos) httpCode = atoi(resp.c_str() + sp + 1);
  size_t he = resp.find("\r\n\r\n");
  if (httpCode != 200 || he == std::string::npos || resp.size() < he + 8) {
    err = httpCode ? "The printer answered HTTP " + std::to_string(httpCode) : "No answer from the printer";
    return -1;
  }
  int status = ((uint8_t)resp[he + 6] << 8) | (uint8_t)resp[he + 7];
  if (status > 0x00FF) {
    char b[64];
    snprintf(b, sizeof(b), "The printer said no (IPP error 0x%04X)", status);
    err = b;
  }
  return status;
}

// ====================================================================== printer discovery (mDNS / Bonjour)

struct FoundPrinter { std::string label, uri; };
static std::vector<FoundPrinter> s_found;
static volatile bool s_finding = false;

static void findTask(void *)
{
  static bool mdnsUp = false;
  if (!mdnsUp) mdnsUp = MDNS.begin("tidyos");
  std::vector<FoundPrinter> list;
  if (mdnsUp) {
    int n = MDNS.queryService("ipp", "tcp");
    for (int i = 0; i < n; ++i) {
      String rp = MDNS.txt(i, "rp");
      String ty = MDNS.txt(i, "ty");
      String ip = MDNS.IP(i).toString();
      std::string label = (ty.length() ? ty : MDNS.hostname(i)).c_str();
      std::string uri = std::string("ipp://") + ip.c_str() + ":" + std::to_string(MDNS.port(i)) + "/" + (rp.length() ? rp.c_str() : "ipp/print");
      list.push_back({ label + "  (" + ip.c_str() + ")", uri });
    }
  }
  s_found = list;
  s_finding = false;
  vTaskDelete(nullptr);
}

// ====================================================================== the print job (runs in the background)

enum { JOB_NONE, JOB_PRINTER, JOB_HELPER, JOB_PDF };
static volatile int  s_jobState = 0;    // 0 idle, 1 busy, 2 done, 3 failed
static int           s_jobKind = JOB_NONE;
static std::string   s_jobUri, s_jobMsg, s_jobOut;
static const PrintDoc * s_doc = nullptr;
static SemaphoreHandle_t s_msgLock = nullptr;

static void setMsg(const std::string & m)
{
  xSemaphoreTake(s_msgLock, portMAX_DELAY);
  s_jobMsg = m;
  xSemaphoreGive(s_msgLock);
}

static std::string getMsg()
{
  xSemaphoreTake(s_msgLock, portMAX_DELAY);
  std::string m = s_jobMsg;
  xSemaphoreGive(s_msgLock);
  return m;
}

static std::string urlEnc(const std::string & s)
{
  static const char * hx = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '.' || c == '_') o += c;
    else { o += '%'; o += hx[c >> 4]; o += hx[c & 15]; }
  }
  return o;
}

static bool helperPrint(const PrintDoc & doc, std::string & msg)
{
  String hp = gSet.proxy;
  int colon = hp.indexOf(':');
  String host = colon > 0 ? hp.substring(0, colon) : hp;
  int port = colon > 0 ? hp.substring(colon + 1).toInt() : 80;
  std::string body;
  for (auto & l : doc.lines) body += l + "\n";
  WiFiClient c;
  if (!c.connect(host.c_str(), port, 6000)) { msg = "Could not reach the helper at " + std::string(hp.c_str()); return false; }
  std::string req = "POST /print?title=" + urlEnc(doc.title) + " HTTP/1.0\r\nHost: " + hp.c_str() +
                    "\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
  c.write((const uint8_t *)req.data(), req.size());
  for (size_t i = 0; i < body.size(); i += 1024) c.write((const uint8_t *)body.data() + i, std::min((size_t)1024, body.size() - i));
  std::string resp;
  uint32_t t0 = millis();
  while (millis() - t0 < 20000 && (c.connected() || c.available())) {
    while (c.available() && resp.size() < 2000) resp += (char)c.read();
    delay(10);
  }
  c.stop();
  size_t he = resp.find("\r\n\r\n");
  std::string text = he == std::string::npos ? "" : resp.substr(he + 4);
  bool ok = resp.find(" 200 ") != std::string::npos;
  msg = ok ? (text.empty() ? "Sent to the printer." : text) : (text.empty() ? "The helper couldn't print." : text);
  return ok;
}

static void jobTask(void *)
{
  const PrintDoc & doc = *s_doc;
  std::string err, msg;
  bool ok = false;
  std::string tmp = dataPath("print.tmp");
  if (s_jobKind == JOB_PDF) {
    ok = makePdf(doc, s_jobOut.c_str(), err);
    msg = ok ? "Saved " + s_jobOut : err;
  } else if (s_jobKind == JOB_HELPER) {
    ok = helperPrint(doc, msg);
  } else {
    PrinterUri pu;
    if (!parseUri(s_jobUri, pu)) { msg = "Pick a printer or type its address."; }
    else {
      setMsg("Sending a PDF...");
      int st = -1;
      if (makePdf(doc, tmp.c_str(), err)) st = ippPrintFile(pu, tmp.c_str(), "application/pdf", doc.title, err);
      if (st > 0x00FF) {
        // printer doesn't take PDF: send it page images instead (every IPP Everywhere printer takes these)
        setMsg("Making page images...");
        err.clear();
        if (makePwg(doc, tmp.c_str(), err)) st = ippPrintFile(pu, tmp.c_str(), "image/pwg-raster", doc.title, err);
      }
      ok = st >= 0 && st <= 0x00FF;
      msg = ok ? "Sent to the printer." : err;
      remove(tmp.c_str());
    }
  }
  setMsg(msg);
  s_jobState = ok ? 2 : 3;
  vTaskDelete(nullptr);
}

static void startJob(int kind)
{
  if (s_jobState == 1) return;
  setMsg("");
  s_jobKind  = kind;
  s_jobState = 1;
  if (xTaskCreatePinnedToCore(jobTask, "print", 8192, nullptr, 2, nullptr, 0) != pdPASS) {
    setMsg("Out of memory");
    s_jobState = 3;
  }
}

// ====================================================================== the Print window

static std::string safeName(const std::string & t)
{
  std::string o;
  for (char c : t) {
    if (isalnum((unsigned char)c) || c == '-' || c == '_') o += c;
    else if (c == ' ' || c == '.') o += '-';
  }
  if (o.empty()) o = "printout";
  return o.substr(0, 40);
}

void printDialog(const PrintDoc & doc)
{
  static PrintDoc copy;
  if (!s_msgLock) s_msgLock = xSemaphoreCreateMutex();
  if (s_jobState == 1) return;         // a job is still running
  copy = doc;
  s_doc = &copy;
  s_jobState = 0;

  std::vector<std::string> wrapped;
  wrapLines(copy, wrapped);
  int pages = (wrapped.size() + LPP - 1) / LPP;
  wrapped.clear();

  auto frame = new uiFrame(gApp->rootWindow(), "Print", UIWINDOW_PARENTCENTER, Size(440, 330), false);
  applyStyle(frame);
  frame->frameProps().resizeable = false;
  frame->frameProps().hasMaximizeButton = false;
  frame->frameProps().hasMinimizeButton = false;
  Point o = clientOrigin(frame);
  int x = o.X + 8, y = o.Y + 6;

  char b[120];
  snprintf(b, sizeof(b), "%s  -  %d page%s", copy.title.substr(0, 40).c_str(), pages, pages == 1 ? "" : "s");
  S(new uiLabel(frame, b, Point(x, y), Size(420, 18)));
  y += 24;
  S(new uiLabel(frame, "Printer", Point(x, y), Size(100, 18)));
  auto findBtn = S(new uiButton(frame, "Find printers", Point(x + 300, y - 2), Size(120, 22)));
  y += 22;
  auto list = S(new uiListBox(frame, Point(x, y), Size(420, 92)));
  list->items().takeStrings();
  y += 98;
  S(new uiLabel(frame, "Or its address", Point(x, y + 3), Size(96, 18)));
  auto addr = S(new uiTextEdit(frame, gSet.printer.c_str(), Point(x + 100, y), Size(320, 22)));
  y += 30;
  auto status = S(new uiLabel(frame, "", Point(x, y), Size(420, 36)));
  status->labelStyle().textFont = &FONT_std_12;
  y += 42;
  auto printBtn = S(new uiButton(frame, "Print",       Point(x,       y), Size(120, 28)));
  auto pdfBtn   = S(new uiButton(frame, "Save as PDF", Point(x + 150, y), Size(120, 28)));
  auto closeBtn = S(new uiButton(frame, "Close",       Point(x + 300, y), Size(120, 28)));
  printBtn->buttonStyle().backgroundColor = T.accent;
  printBtn->buttonStyle().textColor = T.accentText;

  // printer list: [helper] + found printers
  static std::vector<FoundPrinter> rows;
  auto refill = [=]() {
    rows.clear();
    if (gSet.proxy.length()) rows.push_back({ "Through the Pi helper (" + std::string(gSet.proxy.c_str()) + ", uses CUPS)", "helper" });
    for (auto & p : s_found) rows.push_back(p);
    list->items().clear();
    for (auto & r : rows) list->items().append(r.label.c_str());
    list->repaint();
  };
  auto setStatus = [=](const std::string & s) { status->setText(s.c_str()); status->repaint(); };

  refill();
  setStatus(netConnected() ? "Pick a printer, or click Find printers." : "Wi-Fi isn't connected - you can still Save as PDF.");

  list->onChange = [=]() {
    int i = list->firstSelectedItem();
    if (i >= 0 && i < (int)rows.size() && rows[i].uri != "helper") { addr->setText(rows[i].uri.c_str()); addr->repaint(); }
  };
  findBtn->onClick = [=]() {
    if (s_finding || !netConnected()) return;
    s_finding = true;
    setStatus("Looking for printers on your Wi-Fi...");
    xTaskCreatePinnedToCore(findTask, "findprn", 6144, nullptr, 1, nullptr, 0);
  };
  printBtn->onClick = [=]() {
    int i = list->firstSelectedItem();
    if (i >= 0 && i < (int)rows.size() && rows[i].uri == "helper") {
      setStatus("Sending to the helper...");
      startJob(JOB_HELPER);
      return;
    }
    s_jobUri = addr->text();
    gSet.printer = addr->text();
    settingsSave();
    setStatus("Preparing the pages...");
    startJob(JOB_PRINTER);
  };
  pdfBtn->onClick = [=]() {
    s_jobOut = documentsDir() + "/" + safeName(copy.title) + ".pdf";
    setStatus("Making the PDF...");
    startJob(JOB_PDF);
  };
  closeBtn->onClick = [=]() { if (s_jobState != 1) frame->exitModal(0); };

  static bool wasFinding = false;
  wasFinding = false;
  auto timer = gApp->setTimer(frame, 300);
  frame->onTimer = [=](uiTimerHandle) {
    if (s_finding) wasFinding = true;
    else if (wasFinding) {
      wasFinding = false;
      refill();
      setStatus(s_found.empty() ? "No printers answered. Type the printer's IP address instead." : "Pick your printer, then click Print.");
    }
    if (s_jobState == 1) { std::string m = getMsg(); setStatus(m.empty() ? "Working..." : m); }
    if (s_jobState >= 2) { setStatus(getMsg()); s_jobState = 0; }
  };

  gApp->showModalWindow(frame);
  gApp->killTimer(timer);
  gApp->destroyWindow(frame);
}
