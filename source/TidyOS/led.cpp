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
#include "core.h"
#define LED_PIN 2
#define LED_CH  7                 // LEDC (PWM) channel 7: nothing else in TidyOS uses LEDC

static bool s_pwm = false;
static inline void ledLevel(uint32_t v)      // 0 = off .. 255 = full
{
#ifdef TIDYOS_HTML5
  if (s_pwm) ledcWrite(LED_CH, v);
  else
#endif
  digitalWrite(LED_PIN, v >= 128 ? HIGH : LOW);   // no PWM: plain on / off
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

static volatile bool s_wanted = false, s_running = false;

static void ledTaskBody(void * arg)
{
  for (;;) {
    if (!s_wanted) { ledLevel(0); s_running = false; return; }
    uint32_t now = millis();
    uint32_t duty;
    uint32_t sd = gSdActivityMs;
    if (!s_ready) duty = (now / 500) % 2 ? 255 : 0;                                   // starting: slow blink
    else if (sd && now - sd < 90) duty = (now / 35) % 2 ? 255 : 0;                    // card busy: flicker
    else if (ramFree() < 10000) duty = (now / 70) % 2 ? 255 : 0;              // low memory: fast flash
    else {                                                                            // ready: breathing
      float t = (now % 4000) / 4000.0f;
      float v = 0.5f - 0.5f * cosf(t * 2 * PI);
      duty = (uint32_t)(4 + v * v * 251);                                             // squared: looks even to the eye
    }
    ledLevel(duty);
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

// runs the task, then ends it - after the body's locals are destroyed (vTaskDelete
// inside the body would skip their destructors and leak their memory)
static void ledTask(void * arg) { ledTaskBody(arg); vTaskDelete(nullptr); }

void ledBegin()
{
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
#ifdef TIDYOS_HTML5
  s_pwm = ledcSetup(LED_CH, 5000, 8) != 0;        // fading
  if (s_pwm) ledcAttachPin(LED_PIN, LED_CH);
#endif                                            // the monitor edition has no IRAM to spare for it: on / off
  ledLevel(0);
  Serial.printf("[TidyOS] blue light on GPIO %d (%s), switch %s\n", LED_PIN, s_pwm ? "PWM" : "on/off", gSet.ledOn ? "on" : "off");
  ledSetOn(gSet.ledOn);
}

// the light's little task only exists while it's on (~2 KB of RAM)
void ledSetOn(bool on)
{
  s_wanted = on;
  if (on && !s_running) {
    s_running = true;
    if (xTaskCreatePinnedToCore(ledTask, "led", 2048, nullptr, 1, nullptr, 0) != pdPASS) s_running = false;
  }
  if (!on && !s_running) ledLevel(0);
}

void ledReady() { s_ready = true; }
