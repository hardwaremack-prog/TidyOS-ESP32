// TidyOS - a tiny general purpose computer for the ESP32 (NodeMCU / ESP32 DevKit)
// Pin map and build-time settings.
#pragma once

#define TIDYOS_VERSION "1.6.8"

// Edition. The VGA edition draws on a monitor and uses Bluetooth keyboard + mouse.
// The HTML5 edition shows the desktop in a web browser (keyboard + mouse come from the browser).
// The web flasher has both; when building yourself, uncomment the next line for HTML5.
// #define TIDYOS_HTML5
#ifdef TIDYOS_HTML5
  #define TIDYOS_EDITION "HTML5"
#else
  #define TIDYOS_EDITION "VGA"
#endif

// ---------------- VGA (8 GPIOs, 2 bits per colour + sync) ----------------
// Same pinout as the LilyGO/TTGO "FabGL VGA32" board, so FabGL demos work too.
#define VGA_RED1    GPIO_NUM_22   // red   MSB  -> 270 ohm -> VGA pin 1
#define VGA_RED0    GPIO_NUM_21   // red   LSB  -> 820 ohm -> VGA pin 1
#define VGA_GREEN1  GPIO_NUM_19   // green MSB  -> 270 ohm -> VGA pin 2
#define VGA_GREEN0  GPIO_NUM_18   // green LSB  -> 820 ohm -> VGA pin 2
#define VGA_BLUE1   GPIO_NUM_5    // blue  MSB  -> 270 ohm -> VGA pin 3
#define VGA_BLUE0   GPIO_NUM_4    // blue  LSB  -> 820 ohm -> VGA pin 3
#define VGA_HSYNC   GPIO_NUM_23   // -> 100 ohm -> VGA pin 13
#define VGA_VSYNC   GPIO_NUM_15   // -> 100 ohm -> VGA pin 14

// ---------------- microSD card (SPI) ----------------
#define SD_MISO 16
#define SD_MOSI 17
#define SD_CLK  14
#define SD_CS   13

// HTML5 edition only: the microSD module plugged straight onto GND, 19, 18, 5, 17 (the same
// "in-line" wiring as The Shelf on an ESP32). The VGA edition can't use it: 19, 18 and 5 are
// VGA colour pins there. The HTML5 edition tries this wiring first, then the one above.
#define SD_INLINE_MISO 19
#define SD_INLINE_CLK  18
#define SD_INLINE_MOSI 5
#define SD_INLINE_CS   17

#define SD_MOUNT     "/SD"
#define FLASH_MOUNT  "/flash"

// Screen
#define SCREEN_W 640
#define SCREEN_H 480
