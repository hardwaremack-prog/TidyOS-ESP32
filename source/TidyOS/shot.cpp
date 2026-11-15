// TidyOS: save what's on the screen as a 16-colour .bmp (read straight from screen memory, row by row)
#include "fabgl.h"
#include "core.h"
#include "config.h"

RGB888 themePal(int i);

static inline int pixAt(const uint8_t * r, int x)
{
  switch (gColors) {
    case 2:  return (r[x >> 3] >> (7 - (x & 7))) & 1;
    case 4:  return (r[x >> 2] >> (6 - 2 * (x & 3))) & 3;
    case 8:  { int k = (x >> 3) * 3; uint32_t v = r[k] | (r[k + 1] << 8) | (r[k + 2] << 16); return (v >> (21 - 3 * (x & 7))) & 7; }
    default: return (r[x >> 1] >> ((x & 1) ? 0 : 4)) & 15;
  }
}

bool screenSaveBMP(const char * path)
{
  FILE * f = fopen(path, "wb");
  if (!f) return false;
  const int W = SCREEN_W, H = SCREEN_H, RB = W / 2;
  uint8_t h[54 + 64];
  memset(h, 0, sizeof(h));
  auto le32 = [&](int o, uint32_t v) { h[o] = v; h[o + 1] = v >> 8; h[o + 2] = v >> 16; h[o + 3] = v >> 24; };
  h[0] = 'B'; h[1] = 'M';
  le32(2, sizeof(h) + RB * H); le32(10, sizeof(h)); le32(14, 40); le32(18, W); le32(22, H);
  h[26] = 1; h[28] = 4;                                  // 1 plane, 4 bits a pixel
  le32(34, RB * H); le32(38, 2835); le32(42, 2835); le32(46, 16);
  int n = gColors >= 2 && gColors <= 16 ? gColors : 16;
  for (int i = 0; i < 16; ++i) {
    RGB888 c = i < n ? themePal(i) : RGB888(0, 0, 0);
    h[54 + i * 4] = c.B; h[55 + i * 4] = c.G; h[56 + i * 4] = c.R;
  }
  bool ok = fwrite(h, 1, sizeof(h), f) == sizeof(h);
  uint8_t out[SCREEN_W / 2];
  for (int y = H - 1; y >= 0 && ok; --y) {               // .bmp rows go bottom to top
    const uint8_t * r = (const uint8_t *)fabgl::VGAPalettedController::sgetScanline(y);
    for (int x = 0; x < W; x += 2) out[x >> 1] = (pixAt(r, x) << 4) | pixAt(r, x + 1);
    ok = fwrite(out, 1, RB, f) == (size_t)RB;
  }
  if (fclose(f) != 0) ok = false;
  if (!ok) remove(path);
  return ok;
}
