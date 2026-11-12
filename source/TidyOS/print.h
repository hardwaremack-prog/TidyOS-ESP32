// TidyOS printing
//   - network printers (IPP / AirPrint style, found automatically on Wi-Fi):
//     sends a PDF, or a PWG raster page image if the printer doesn't take PDF
//   - through the browser helper on a Pi / The Shelf (prints with CUPS "lp")
//   - "Save as PDF" to the SD card
#pragma once
#include <string>
#include <vector>

struct PrintDoc {
  std::string title;
  std::vector<std::string> lines;     // plain text, any length (wrapped at 80 columns)
};

void printDialog(const PrintDoc & doc);           // shows the Print window (modal)

// building blocks
bool makePdf(const PrintDoc & doc, const char * path, std::string & err);
bool makePwg(const PrintDoc & doc, const char * path, std::string & err);
