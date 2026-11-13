// TidyOS: the blue LED (GPIO 2 on NodeMCU-32S / DevKitC boards) as a status light, like The Shelf.
// The red LED next to it is the power light and can't be switched.
//   starting up           slow blink (half a second on, half off)
//   ready                 breathing: a slow, smooth fade in and out (~4 s a breath)
//   microSD card busy     a quick flicker while the card is read or written
//   low on memory         fast flash (close an app)
#include <Arduino.h>
#include <math.h>
#include "led.h"
#include "crashlog.h"
#include "soc/gpio_sd_struct.h"
#include "soc/gpio_sig_map.h"
#include "esp_rom_gpio.h"

#define LED_PIN 2
#define LED_SD  0                 // sigma-delta channel 0: hardware dimming with no driver code (keeps IRAM free)

static inline void ledLevel(uint32_t v)      // 0 = off .. 255 = full
{
  SIGMADELTA.channel[LED_SD].duty = (uint8_t)(int8_t)((int)v - 128);
}

volatile uint32_t gSdActivityMs = 0;
static volatile bool s_ready = false;

// Card reads/writes are stamped here. The build links these in place of FatFs's disk functions
// (-Wl,--wrap=ff_disk_read,--wrap=ff_disk_write); without that flag the light never flickers.
// (Internal flash uses SPIFFS, not FatFs, so only the SD card is counted.)
extern "C" {
  int __real_ff_disk_read(uint8_t pdrv, uint8_t * buff, uint32_t sector, unsigned count);
  int __real_ff_disk_write(uint8_t pdrv, const uint8_t * buff, uint32_t sector, unsigned count);
  int __wrap_ff_disk_read(uint8_t pdrv, uint8_t * buff, uint32_t sector, unsigned count)
  { gSdActivityMs = millis(); return __real_ff_disk_read(pdrv, buff, sector, count); }
  int __wrap_ff_disk_write(uint8_t pdrv, const uint8_t * buff, uint32_t sector, unsigned count)
  { gSdActivityMs = millis(); return __real_ff_disk_write(pdrv, buff, sector, count); }
}

static void ledTask(void *)
{
  uint32_t lastHeap = 0;
  for (;;) {
    uint32_t now = millis();
    if (now - lastHeap > 500) { lastHeap = now; gHeapSnap = ESP.getFreeHeap(); gMinHeapSnap = ESP.getMinFreeHeap(); }
    uint32_t duty;
    uint32_t sd = gSdActivityMs;
    if (!s_ready) duty = (now / 500) % 2 ? 255 : 0;                                   // starting: slow blink
    else if (sd && now - sd < 90) duty = (now / 35) % 2 ? 255 : 0;                    // card busy: flicker
    else if (ESP.getFreeHeap() < 20000) duty = (now / 70) % 2 ? 255 : 0;              // low memory: fast flash
    else {                                                                            // ready: breathing
      float t = (now % 4000) / 4000.0f;
      float v = 0.5f - 0.5f * cosf(t * 2 * PI);
      duty = (uint32_t)(4 + v * v * 251);                                             // squared: looks even to the eye
    }
    ledLevel(duty);
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

void ledBegin()
{
  pinMode(LED_PIN, OUTPUT);
  SIGMADELTA.cg.clk_en = 1;
  SIGMADELTA.channel[LED_SD].prescale = 80;
  ledLevel(0);
  esp_rom_gpio_connect_out_signal(LED_PIN, GPIO_SD0_OUT_IDX + LED_SD, false, false);
  xTaskCreatePinnedToCore(ledTask, "led", 2048, nullptr, 1, nullptr, 0);
}

void ledReady() { s_ready = true; }
