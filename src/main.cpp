#include <Arduino.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "api/HttpApi.h"
#include "console/ConsoleSession.h"
#include "model/SnapshotStore.h"
#include "net/Network.h"
#if PYLONMON_PROBE
#include "util/Captures.h"
#endif
#include "util/Log.h"
#include "util/Settings.h"
#include "util/Stats.h"

Stats stats;

static void setupWatchdog() {
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = TWDT_TIMEOUT_S * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
}

void setup() {
  Serial.begin(115200);
  Log::begin();
  LOGI("PylonMon %s%s, reset reason %d", PYLONMON_FW_VERSION, PYLONMON_PROBE ? " (probe)" : "", (int)esp_reset_reason());
  setupWatchdog();
  settings.load();
  SnapshotStore::begin();
#if PYLONMON_PROBE
  Captures::begin();
#endif
  Net::begin();
  HttpApi::begin();
  ConsoleSession::begin();
}

void loop() {
  static uint32_t lastHeapLog = 0;
  uint32_t freeHeap = ESP.getFreeHeap();
  if (millis() - lastHeapLog > 60000) {
    lastHeapLog = millis();
    LOGI("heap free=%u min=%u psram=%u", (unsigned)freeHeap, (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getFreePsram());
  }
  if (freeHeap < HEAP_MIN_REBOOT && !Net::otaActive() && !Update.isRunning()) {
    LOGI("heap low (%u), rebooting", (unsigned)freeHeap);
    delay(200);
    ESP.restart();
  }
  HttpApi::loopCheck();
  delay(1000);
}
