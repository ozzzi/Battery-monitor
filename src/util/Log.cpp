#include "util/Log.h"
#include <stdarg.h>
#include <time.h>
#include "config.h"

namespace {
char* ring = nullptr;
size_t head = 0;
bool wrapped = false;
SemaphoreHandle_t mtx = nullptr;

void put(const char* s, size_t n) {
  for (size_t i = 0; i < n; i++) {
    ring[head++] = s[i];
    if (head >= LOG_RING_SIZE) { head = 0; wrapped = true; }
  }
}
}  // namespace

namespace Log {

void begin() {
  if (ring) return;
  ring = (char*)ps_malloc(LOG_RING_SIZE);
  if (!ring) ring = (char*)malloc(LOG_RING_SIZE);
  mtx = xSemaphoreCreateMutex();
}

void printf(const char* fmt, ...) {
  char line[256];
  int p = 0;
  uint32_t ms = millis();
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm tm;
    localtime_r(&now, &tm);
    p = strftime(line, sizeof(line), "%m-%d %H:%M:%S ", &tm);
  } else {
    p = snprintf(line, sizeof(line), "+%lu.%03lu ", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
  }
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line + p, sizeof(line) - p - 2, fmt, ap);
  va_end(ap);
  if (n < 0) return;
  p += n;
  if (p > (int)sizeof(line) - 2) p = sizeof(line) - 2;
  line[p++] = '\n';
  line[p] = 0;
  if (Serial) Serial.write((const uint8_t*)line, p);
  if (!ring) return;
  xSemaphoreTake(mtx, portMAX_DELAY);
  put(line, p);
  xSemaphoreGive(mtx);
}

String dump() {
  String out;
  if (!ring) return out;
  xSemaphoreTake(mtx, portMAX_DELAY);
  size_t len = wrapped ? LOG_RING_SIZE : head;
  out.reserve(len + 1);
  if (wrapped) {
    size_t start = head;
    while (start < LOG_RING_SIZE && ring[start] != '\n') start++;
    if (start + 1 < LOG_RING_SIZE) out.concat(ring + start + 1, LOG_RING_SIZE - start - 1);
  }
  out.concat(ring, head);
  xSemaphoreGive(mtx);
  return out;
}

}  // namespace Log
