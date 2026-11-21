#include "util/Captures.h"
#include <LittleFS.h>
#include <time.h>
#include <vector>
#include <algorithm>
#include "config.h"
#include "util/Log.h"
#include "util/Stats.h"

namespace {
uint32_t nextSeq = 1;
bool ok = false;
SemaphoreHandle_t mtx = nullptr;

struct Entry { String name; size_t size; };

std::vector<Entry> scan() {
  std::vector<Entry> v;
  File dir = LittleFS.open(CAPTURE_DIR);
  if (!dir || !dir.isDirectory()) return v;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (!f.isDirectory()) v.push_back({String(f.name()), (size_t)f.size()});
    f.close();
  }
  std::sort(v.begin(), v.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
  return v;
}

String safeName(const std::string& cmd) {
  String s;
  for (char c : cmd) {
    if (isalnum((unsigned char)c)) s += c;
  }
  return s.isEmpty() ? String("enter") : s;
}

void makeRoom(size_t need) {
  size_t limit = CAPTURE_FS_LIMIT;
  if (LittleFS.totalBytes() < limit + 64 * 1024) limit = LittleFS.totalBytes() - 64 * 1024;
  if (LittleFS.usedBytes() + need <= limit) return;
  auto v = scan();
  for (auto& e : v) {
    if (LittleFS.usedBytes() + need <= limit) break;
    LittleFS.remove(String(CAPTURE_DIR) + "/" + e.name);
  }
}
}  // namespace

namespace Captures {

bool begin() {
  mtx = xSemaphoreCreateMutex();
  ok = LittleFS.begin(true);
  if (!ok) { LOGI("captures: LittleFS mount failed"); return false; }
  if (!LittleFS.exists(CAPTURE_DIR)) LittleFS.mkdir(CAPTURE_DIR);
  auto v = scan();
  if (!v.empty()) nextSeq = (uint32_t)v.back().name.substring(0, 6).toInt() + 1;
  File sf = LittleFS.open("/seq", "r");
  if (sf) {
    uint32_t stored = (uint32_t)sf.readString().toInt();
    sf.close();
    if (stored >= nextSeq) nextSeq = stored + 1;
  }
  LOGI("captures: %u files, used %u / %u, next seq %lu", (unsigned)v.size(), (unsigned)LittleFS.usedBytes(),
       (unsigned)LittleFS.totalBytes(), (unsigned long)nextSeq);
  return true;
}

bool save(const std::string& cmd, const char* result, const char* src, const std::string& text) {
  if (!ok) return false;
  xSemaphoreTake(mtx, portMAX_DELAY);
  makeRoom(text.size() + 4096);
  char name[48];
  snprintf(name, sizeof(name), "%06lu_%s.txt", (unsigned long)nextSeq, safeName(cmd).c_str());
  char ts[32] = "-";
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", &tm);
  }
  File f = LittleFS.open(String(CAPTURE_DIR) + "/" + name, "w");
  bool good = false;
  if (f) {
    f.printf("#PYLONMON cmd=%s result=%s src=%s time=%s uptime_s=%lu fw=%s bytes=%u\n", safeName(cmd).c_str(), result,
             src, ts, (unsigned long)(millis() / 1000), PYLONMON_FW_VERSION, (unsigned)text.size());
    good = f.write((const uint8_t*)text.data(), text.size()) == text.size();
    f.close();
  }
  if (good) {
    File sf = LittleFS.open("/seq", "w");
    if (sf) { sf.print(nextSeq); sf.close(); }
    nextSeq++;
    stats.captures_saved++;
  } else {
    stats.capture_errors++;
    LOGI("captures: write failed %s", name);
  }
  xSemaphoreGive(mtx);
  return good;
}

String listJson() {
  String out;
  xSemaphoreTake(mtx, portMAX_DELAY);
  auto v = scan();
  out.reserve(64 + v.size() * 40);
  out = "{\"total\":" + String((unsigned)LittleFS.totalBytes()) + ",\"used\":" + String((unsigned)LittleFS.usedBytes()) +
        ",\"count\":" + String((unsigned)v.size()) + ",\"files\":[";
  for (size_t i = 0; i < v.size(); i++) {
    if (i) out += ',';
    out += "{\"n\":\"" + v[i].name + "\",\"size\":" + String((unsigned)v[i].size) + "}";
  }
  out += "]}";
  xSemaphoreGive(mtx);
  return out;
}

static bool validName(const String& n) {
  if (n.isEmpty() || n.length() > 40) return false;
  for (char c : n) if (!(isalnum((unsigned char)c) || c == '_' || c == '.')) return false;
  return n.indexOf("..") < 0;
}

bool exists(const String& name) { return validName(name) && LittleFS.exists(path(name)); }
String path(const String& name) { return String(CAPTURE_DIR) + "/" + name; }

bool remove(const String& name) {
  xSemaphoreTake(mtx, portMAX_DELAY);
  bool r = false;
  if (name == "all") {
    for (auto& e : scan()) LittleFS.remove(path(e.name));
    r = true;
  } else if (validName(name)) {
    r = LittleFS.remove(path(name));
  }
  xSemaphoreGive(mtx);
  return r;
}

size_t used() { return LittleFS.usedBytes(); }
size_t total() { return LittleFS.totalBytes(); }

}  // namespace Captures
