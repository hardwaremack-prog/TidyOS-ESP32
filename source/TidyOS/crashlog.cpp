// TidyOS crash recorder.
// When the chip crashes, ESP-IDF's panic handler runs just before the restart. The build wraps it
// (-Wl,--wrap=esp_panic_handler) so we can copy the reason and a short backtrace into RTC memory,
// which survives the restart. Task Manager > Power & info then shows it, and it is added to
// tidyos/crash.txt on the SD card. The addresses can be looked up against the firmware's .elf file.
#include <Arduino.h>
#include "esp_attr.h"
#include "esp_system.h"
#include "esp_private/panic_internal.h"
#include "esp_debug_helpers.h"
#include "xtensa/xtensa_context.h"
#include "crashlog.h"
#include "config.h"

#define CRASH_MAGIC 0x7D1C0A5Eu

struct CrashRec {
  uint32_t magic;
  int32_t  core;
  char     reason[48];
  uint32_t pc[12];
  int32_t  n;
  uint32_t heap, minHeap, upSec;
};
static RTC_NOINIT_ATTR CrashRec s_rec;

volatile uint32_t gHeapSnap = 0, gMinHeapSnap = 0;   // refreshed twice a second (led.cpp)
static std::string s_text;

extern "C" {
void __real_esp_panic_handler(panic_info_t * info);
#ifdef TIDYOS_HTML5
IRAM_ATTR
#endif
void __wrap_esp_panic_handler(panic_info_t * info)
{
  s_rec.magic = CRASH_MAGIC;
  s_rec.core = info->core;
  s_rec.heap = gHeapSnap;
  s_rec.minHeap = gMinHeapSnap;
  s_rec.upSec = (uint32_t)(esp_timer_get_time() / 1000000);
  const char * r = info->reason;
  int i = 0;
  if (r) for (; i < 47 && r[i]; ++i) s_rec.reason[i] = r[i];
  s_rec.reason[i] = 0;
  s_rec.n = 0;
  if (info->frame) {
    const XtExcFrame * f = (const XtExcFrame *)info->frame;
    esp_backtrace_frame_t fr = { (uint32_t)f->pc, (uint32_t)f->a1, (uint32_t)f->a0, f };
    s_rec.pc[s_rec.n++] = fr.pc;
    while (s_rec.n < 12 && fr.next_pc && esp_backtrace_get_next_frame(&fr))
      s_rec.pc[s_rec.n++] = fr.pc;
  }
  __real_esp_panic_handler(info);
}
}

void crashBegin()
{
  esp_reset_reason_t rr = esp_reset_reason();
  bool crashed = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT;
  if (crashed && s_rec.magic == CRASH_MAGIC) {
    char b[160];
    s_rec.reason[47] = 0;
    snprintf(b, sizeof(b), "%s on core %d after %um %02us (free RAM %uK, lowest %uK)", s_rec.reason, (int)s_rec.core,
             (unsigned)(s_rec.upSec / 60), (unsigned)(s_rec.upSec % 60), (unsigned)(s_rec.heap / 1024), (unsigned)(s_rec.minHeap / 1024));
    s_text = b;
    s_text += "\nat";
    for (int i = 0; i < s_rec.n && i < 12; ++i) { snprintf(b, sizeof(b), " 0x%08x", (unsigned)s_rec.pc[i]); s_text += b; }
    Serial.printf("[TidyOS] last run crashed: %s\n", s_text.c_str());
  }
  s_rec.magic = 0;
}

std::string crashText() { return s_text; }

void crashSave(const std::string & path)
{
  if (s_text.empty()) return;
  FILE * f = fopen(path.c_str(), "a");
  if (!f) return;
  time_t now = time(nullptr);
  fprintf(f, "TidyOS %s (%s) crash, logged %ld:\n%s\n\n", TIDYOS_VERSION, TIDYOS_EDITION, (long)now, s_text.c_str());
  fclose(f);
}
