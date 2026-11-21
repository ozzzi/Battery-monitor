#pragma once
#include <Arduino.h>
#include <string>

namespace Captures {
bool begin();
bool save(const std::string& cmd, const char* result, const char* src, const std::string& text);
String listJson();                 // {"total":..,"used":..,"files":[{"n":..,"size":..}]}
bool exists(const String& name);
String path(const String& name);
bool remove(const String& name);
size_t used();
size_t total();
}
