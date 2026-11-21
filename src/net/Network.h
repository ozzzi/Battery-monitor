#pragma once
#include <Arduino.h>
#include "net/WifiFsm.h"

namespace Net {
void begin();
bool connected();
bool apActive();
bool otaActive();
wififsm::Mode mode();
const char* modeName();       // "AP_SETUP" ...
const String& deviceId();
String apName();              // PylonMon-XXXX
String ssid();

bool startTrial(const String& ssid, const String& pass, const String& hostname);
wififsm::Trial trial();
String trialSsid();
}
