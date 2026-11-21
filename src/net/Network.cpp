#include "net/Network.h"
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_attr.h>
#include <esp_mac.h>
#include <esp_task_wdt.h>
#include <atomic>
#include <mutex>
#include "config.h"
#include "console/ConsoleSession.h"
#include "model/SnapshotStore.h"
#include "util/Log.h"
#include "util/Settings.h"
#include "util/Stats.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Нет include/secrets.h: скопируйте include/secrets.example.h в include/secrets.h и заполните"
#endif

using wififsm::Mode;
using wififsm::Trial;

static RTC_NOINIT_ATTR uint32_t setupOnBoot;
static constexpr uint32_t SETUP_MAGIC = 0x5E7A9B01;

namespace {
String devId;
std::atomic<bool> gotIp{false}, ap{false}, ota{false};
std::atomic<uint8_t> modeA{(uint8_t)Mode::StaConnecting};
bool forceSetup = false;
bool servicesUp = false;
bool mdnsUp = false;
wififsm::Fsm fsm(wififsm::Config{WIFI_RESCUE_AP_AFTER_MS, WIFI_RESCUE_AP_OFF_MS});
DNSServer dns;
WiFiUDP beaconUdp;
uint32_t lastBeaconMs = 0;
bool beaconSent = false;
uint8_t txtMods = 0xFF;

std::mutex trialMx;              // trialReq*, curSsid: httpTask <-> netTask
bool trialReq = false;
String reqSsid, reqPass, reqHost;
String curSsid, curPass, curHost;
std::atomic<uint8_t> trialSt{(uint8_t)Trial::None};
std::atomic<uint8_t> trialDisc{0};
std::atomic<uint8_t> lastReason{0};
uint32_t trialStartMs = 0;

bool hasCreds() {
  if (forceSetup) return false;
  if (settings.wifi_ssid.length()) return true;
#if PYLONMON_PROBE
  return strlen(WIFI_SSID) > 0;
#else
  return false;
#endif
}

String staSsid() {
#if PYLONMON_PROBE
  if (settings.wifi_ssid.isEmpty()) return String(WIFI_SSID);
#endif
  return settings.wifi_ssid;
}
String staPass() {
#if PYLONMON_PROBE
  if (settings.wifi_ssid.isEmpty()) return String(WIFI_PASS);
#endif
  return settings.wifi_pass;
}

void onEvent(arduino_event_id_t ev, arduino_event_info_t info) {
  switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      gotIp = true;
      LOGI("wifi: connected, ip=%s rssi=%d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      uint8_t r = info.wifi_sta_disconnected.reason;
      lastReason = r;
      if (trialSt == (uint8_t)Trial::Testing && wififsm::classifyDisconnect(r) != Trial::None) trialDisc++;
      if (gotIp) {
        stats.wifi_reconnects++;
        LOGI("wifi: disconnected, reason %u", r);
      }
      gotIp = false;
      break;
    }
    default:
      break;
  }
}

void startMdns() {
  if (mdnsUp) return;
  if (!MDNS.begin(settings.hostname.c_str())) {
    LOGI("mdns: begin failed");
    return;
  }
  mdnsUp = true;
  txtMods = 0xFF;
  MDNS.addService("pylonmon", "tcp", HTTP_PORT);
  MDNS.addServiceTxt("pylonmon", "tcp", "id", devId.c_str());
  MDNS.addServiceTxt("pylonmon", "tcp", "api", String(PYLONMON_API_VERSION).c_str());
  MDNS.addServiceTxt("pylonmon", "tcp", "path", "/api/v1");
  MDNS.addServiceTxt("pylonmon", "tcp", "fw", PYLONMON_FW_VERSION);
  MDNS.addService("http", "tcp", HTTP_PORT);
  LOGI("mdns: %s.local, _pylonmon._tcp", settings.hostname.c_str());
}

void startServices() {
  startMdns();
  if (servicesUp) return;
  servicesUp = true;
  ArduinoOTA.setHostname(settings.hostname.c_str());
  ArduinoOTA.setPort(OTA_PORT);
  ArduinoOTA.setPassword(OTA_PASS);
  ArduinoOTA.setMdnsEnabled(false);
  ArduinoOTA.onStart([] {
    ota = true;
    esp_task_wdt_delete(nullptr);
    ConsoleSession::setPaused(true);
    LOGI("ota: start");
  });
  ArduinoOTA.onEnd([] { LOGI("ota: done, rebooting"); });
  ArduinoOTA.onError([](ota_error_t e) {
    LOGI("ota: error %d", (int)e);
    ota = false;
    ConsoleSession::setPaused(false);
    esp_task_wdt_add(nullptr);
  });
  ArduinoOTA.begin();
  LOGI("ota: listening on %d", OTA_PORT);

  configTzTime(NTP_TZ, NTP_SERVER1, NTP_SERVER2);
}

void beacon(uint32_t now) {
  if (beaconSent && (int32_t)(now - lastBeaconMs) < (int32_t)BEACON_PERIOD_MS) return;
  beaconSent = true;
  lastBeaconMs = now;
  char buf[200];
  int n = snprintf(buf, sizeof(buf),
                   "{\"svc\":\"pylonmon\",\"id\":\"%s\",\"api\":%d,\"port\":%d,\"path\":\"/api/v1\",\"fw\":\"%s\",\"seq\":%lu}",
                   devId.c_str(), PYLONMON_API_VERSION, HTTP_PORT, PYLONMON_FW_VERSION, (unsigned long)SnapshotStore::seq());
  if (n <= 0 || n >= (int)sizeof(buf)) return;
  if (beaconUdp.beginPacket(IPAddress(255, 255, 255, 255), BEACON_PORT)) {
    beaconUdp.write((const uint8_t*)buf, n);
    beaconUdp.endPacket();
  }
}

void updateTxtMods() {
  uint8_t m = SnapshotStore::moduleCount();
  if (m == txtMods || !mdnsUp) return;
  txtMods = m;
  MDNS.addServiceTxt("pylonmon", "tcp", "mods", String(m).c_str());
}

void startAp() {
  if (ap) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(Net::apName().c_str());
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", WiFi.softAPIP());
  ap = true;
  LOGI("wifi: AP %s at %s (%s)", Net::apName().c_str(), WiFi.softAPIP().toString().c_str(), wififsm::modeName(fsm.mode()));
}

void stopAp() {
  if (!ap) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  ap = false;
  LOGI("wifi: AP off");
}

void staBegin() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.disconnect(false);
  WiFi.setHostname(settings.hostname.c_str());   // DHCP hostname
  WiFi.begin(staSsid().c_str(), staPass().c_str());
  LOGI("wifi: connecting to '%s' as %s (attempt %u)", staSsid().c_str(), settings.hostname.c_str(), fsm.attempt());
}

void apply(const wififsm::Actions& a) {
  if (a.ap_on) startAp();
  if (a.ap_off) stopAp();
  if (a.sta_begin) staBegin();
}

void trialStep(uint32_t now) {
  bool fresh = false;
  {
    std::lock_guard<std::mutex> lk(trialMx);
    if (trialReq) {
      trialReq = false;
      curSsid = reqSsid;
      curPass = reqPass;
      curHost = reqHost;
      reqPass = "";
      trialDisc = 0;
      trialStartMs = now;
      trialSt = (uint8_t)Trial::Testing;
      fresh = true;
    } else if (trialSt != (uint8_t)Trial::Testing) {
      return;
    }
  }
  if (fresh) {
    WiFi.disconnect(false);
    WiFi.setHostname(curHost.c_str());
    WiFi.begin(curSsid.c_str(), curPass.c_str());
    LOGI("wifi: portal test '%s' as %s", curSsid.c_str(), curHost.c_str());
    return;
  }
  if (gotIp) {
    const bool hostChanged = settings.hostname != curHost;
    settings.wifi_ssid = curSsid;
    settings.wifi_pass = curPass;
    settings.hostname = curHost;
    settings.save();
    curPass = "";
    forceSetup = false;
    if (hostChanged && mdnsUp) {
      MDNS.end();
      mdnsUp = false;
    }
    fsm.credsSaved(now, WIFI_PORTAL_AP_OFF_MS);
    trialSt = (uint8_t)Trial::Ok;
    LOGI("wifi: portal test ok, saved; AP off in %u s", (unsigned)(WIFI_PORTAL_AP_OFF_MS / 1000));
    return;
  }
  const uint8_t d = trialDisc;
  if (d < 2 && (int32_t)(now - trialStartMs) < (int32_t)WIFI_TRIAL_TIMEOUT_MS) return;
  Trial r = d ? wififsm::classifyDisconnect(lastReason) : Trial::Failed;
  if (r == Trial::None) r = Trial::Failed;
  trialSt = (uint8_t)r;
  curPass = "";
  WiFi.disconnect(false);
  WiFi.setHostname(settings.hostname.c_str());
  LOGI("wifi: portal test '%s' failed: %s (reason %u)", curSsid.c_str(), wififsm::trialName(r), (unsigned)lastReason.load());
}

void led(uint32_t now) {
  bool on;
  const Mode m = fsm.mode();
  if (ota) on = (now / 50) & 1;
  else if (m == Mode::ApSetup || m == Mode::ApRescue) on = (now / 100) & 1;
  else if (m == Mode::StaConnecting) on = (now / 500) & 1;
  else {
    uint32_t ph = now % 3000;
    if (ConsoleSession::online()) on = ph < 50;
    else on = ph < 50 || (ph >= 200 && ph < 250);
  }
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

void button(uint32_t now) {
  static bool pressed = false;
  static uint32_t pressedMs = 0;
  if (digitalRead(PIN_BOOT_BTN) == LOW) {
    if (!pressed) {
      pressed = true;
      pressedMs = now;
    } else if ((int32_t)(now - pressedMs) > 5000) {
      LOGI("button: reset Wi-Fi settings -> AP_SETUP");
      settings.clearWifi();
      setupOnBoot = SETUP_MAGIC;
      digitalWrite(PIN_LED, HIGH);
      delay(200);
      ESP.restart();
    }
  } else {
    pressed = false;
  }
}

void netTask(void*) {
  esp_task_wdt_add(nullptr);
  apply(fsm.start(hasCreds(), millis()));
  modeA = (uint8_t)fsm.mode();
  LOGI("wifi: start in %s", wififsm::modeName(fsm.mode()));
  uint32_t lastTxtMs = 0;
  for (;;) {
    const uint32_t now = millis();
    if (!ota) esp_task_wdt_reset();
    trialStep(now);
    const bool up = gotIp;
    const Mode before = fsm.mode();
    apply(fsm.step(up, trialSt == (uint8_t)Trial::Testing, now));
    if (fsm.mode() != before) LOGI("wifi: %s -> %s", wififsm::modeName(before), wififsm::modeName(fsm.mode()));
    modeA = (uint8_t)fsm.mode();
    if (up) {
      startServices();
      beacon(now);
      if ((int32_t)(now - lastTxtMs) > 5000) { lastTxtMs = now; updateTxtMods(); }
    }
    if (servicesUp) ArduinoOTA.handle();
    led(now);
    button(now);
    delay(10);
  }
}
}  // namespace

namespace Net {

void begin() {
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  if (setupOnBoot == SETUP_MAGIC) forceSetup = true;
  setupOnBoot = 0;
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  devId = buf;

  WiFi.persistent(false);
  WiFi.onEvent(onEvent);
  WiFi.setHostname(settings.hostname.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
  xTaskCreate(netTask, "netTask", 6144, nullptr, 2, nullptr);
}

bool connected() { return gotIp; }
bool apActive() { return ap; }
bool otaActive() { return ota; }
wififsm::Mode mode() { return (Mode)modeA.load(); }
const char* modeName() { return wififsm::modeName(mode()); }
const String& deviceId() { return devId; }
String apName() { return "PylonMon-" + devId.substring(8); }
String ssid() { return staSsid(); }

bool startTrial(const String& s, const String& pass, const String& host) {
  std::lock_guard<std::mutex> lk(trialMx);
  if (trialReq || trialSt == (uint8_t)Trial::Testing) return false;
  reqSsid = s;
  reqPass = pass;
  reqHost = host;
  trialReq = true;
  trialSt = (uint8_t)Trial::Testing;
  return true;
}

Trial trial() { return (Trial)trialSt.load(); }

String trialSsid() {
  std::lock_guard<std::mutex> lk(trialMx);
  return trialReq ? reqSsid : curSsid;
}

}  // namespace Net
