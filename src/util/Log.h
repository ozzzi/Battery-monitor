#pragma once
#include <Arduino.h>

namespace Log {
void begin();
void printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
String dump();
}

#define LOGI(...) Log::printf(__VA_ARGS__)
