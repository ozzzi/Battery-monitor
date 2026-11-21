#pragma once
#include <Arduino.h>

namespace HttpApi {
void begin();
void requestReboot(uint32_t delay_ms = 500);
void loopCheck();
}
