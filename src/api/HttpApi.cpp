#include "api/HttpApi.h"
#include <ArduinoJson.h>
#include <HTTPUpdateServer.h>
#if PYLONMON_PROBE
#include <LittleFS.h>
#include "util/Captures.h"
#endif
#include <WebServer.h>
#include <WiFi.h>
#include <esp_system.h>
#include <algorithm>
#include <vector>
#include "config.h"
#include "console/ConsoleSession.h"
#include "console/Parser.h"
#include "model/SnapshotStore.h"
#include "net/Network.h"
#include "net/WifiFsm.h"
#include "util/Log.h"
#include "util/Settings.h"
#include "util/Stats.h"
#include "secrets.h"

namespace {

WebServer server(HTTP_PORT);
HTTPUpdateServer updater;
#if PYLONMON_PROBE
HTTPUpdateServer updaterDbg;
#endif
volatile uint32_t rebootAt = 0;
Snapshot snap;

const char* resetReasonName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_USB: return "usb";
    default: return "other";
  }
}

void commonHeaders() {
  stats.http_requests++;
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Access-Control-Allow-Origin", "*");
}

void sendJson(int code, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json; charset=utf-8", out);
}

void sendErr(int code, const char* err) {
  String s = String("{\"err\":\"") + err + "\"}";
  server.send(code, "application/json; charset=utf-8", s);
}

bool requirePost() {
  if (server.method() == HTTP_POST) return true;
  sendErr(405, "use_post");
  return false;
}

bool requireAdmin() {
  if (settings.admin_pass.isEmpty()) return true;
  if (server.authenticate("admin", settings.admin_pass.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "PylonMon");
  return false;
}

String hexDump(const std::string& d) {
  String out;
  out.reserve(d.size() * 4 + 64);
  char line[100];
  for (size_t off = 0; off < d.size(); off += 16) {
    int p = snprintf(line, sizeof(line), "%04x  ", (unsigned)off);
    for (size_t i = 0; i < 16; i++) {
      if (off + i < d.size()) p += snprintf(line + p, sizeof(line) - p, "%02x ", (uint8_t)d[off + i]);
      else p += snprintf(line + p, sizeof(line) - p, "   ");
    }
    p += snprintf(line + p, sizeof(line) - p, " |");
    for (size_t i = 0; i < 16 && off + i < d.size(); i++) {
      char c = d[off + i];
      line[p++] = (c >= 32 && c < 127) ? c : '.';
    }
    line[p++] = '|';
    line[p++] = '\n';
    line[p] = 0;
    out += line;
  }
  return out;
}

template <typename V, typename T>
void setOrNull(V&& v, bool has, T val) {
  if (has) v = val;
  else v = nullptr;
}
template <typename V>
void setTemp(V&& v, int32_t mc) { setOrNull(v, mc != NA, mc == NA ? 0 : (int32_t)(mc / 100)); }
template <typename V>
void setI32(V&& v, int32_t x) { setOrNull(v, x != NA, x); }

uint32_t ageS(uint32_t ts) { return ts ? (millis() - ts) / 1000 : UINT32_MAX; }

// ---------------- /api/v1 ----------------

void handleSnapshot() {
  commonHeaders();
  SnapshotStore::copy(snap);
  if (!snap.has_data) return sendErr(503, "no_data");
  String etag = "\"" + String(snap.seq) + "\"";
  server.sendHeader("ETag", etag);
  if (server.header("If-None-Match") == etag) { server.send(304); return; }
  const SessionInfo si = ConsoleSession::info();
  const bool online = ConsoleSession::online();
  JsonDocument doc;
  doc["v"] = 1;
  doc["id"] = Net::deviceId();
  doc["seq"] = snap.seq;
  doc["age_s"] = ageS(si.last_ok_ms);
  doc["online"] = online;
  JsonObject sys = doc["sys"].to<JsonObject>();
  setI32(sys["mv"], snap.sys_mv);
  setI32(sys["ma"], snap.sys_ma);
  setOrNull(sys["soc"], snap.sys_soc != 0xFF, snap.sys_soc);
  const char* st = "Unknown";
  if (snap.sys_ma != NA) st = snap.sys_ma > 200 ? "Charge" : snap.sys_ma < -200 ? "Dischg" : "Idle";
  sys["st"] = st;
  sys["dmv"] = snap.sys_delta_mv;
  sys["bal"] = snap.sys_bal_count;
  setTemp(sys["tmin"], snap.sys_tmin_mc);
  setTemp(sys["tmax"], snap.sys_tmax_mc);
  const uint32_t nowMs = millis();
  JsonArray mods = doc["mods"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_MODULES; i++) {
    const ModuleData& m = snap.m[i];
    if (!m.present) continue;
    JsonObject o = mods.add<JsonObject>();
    o["n"] = i + 1;
    setI32(o["mv"], m.mv);
    setI32(o["ma"], m.ma);
    setOrNull(o["soc"], m.soc != 0xFF, m.soc);
    o["st"] = m.base_raw;
    setTemp(o["t"], m.temp_mc);
    setTemp(o["tmos"], m.mos_mc);
    setOrNull(o["rem"], m.remain_mah != 0xFFFFFFFF, m.remain_mah);
    setOrNull(o["cyc"], m.cycles != 0xFFFF, m.cycles);
    setOrNull(o["soh"], m.soh != 0xFF, m.soh);
    o["cmin"] = m.cell_min_mv;
    o["cmini"] = m.cell_min_idx;
    o["cmax"] = m.cell_max_mv;
    o["cmaxi"] = m.cell_max_idx;
    o["dmv"] = m.cell_delta_mv;
    JsonArray cv = o["cv"].to<JsonArray>();
    JsonArray ct = o["ct"].to<JsonArray>();
    for (uint8_t c = 0; c < CELLS; c++) {
      if (m.cells[c].mv) cv.add(m.cells[c].mv); else cv.add(nullptr);
      if (m.cells[c].temp_mc != NA) ct.add(m.cells[c].temp_mc / 100); else ct.add(nullptr);
    }
    o["bm"] = m.bal_mask;
    o["am"] = m.alarm_cell_mask;
    setOrNull(o["cage_s"], m.cells_ts != 0, ageS(m.cells_ts));
    o["alarm"] = moduleAlarm(snap, m, nowMs, online);
  }
  sys["alarm"] = systemAlarm(snap, nowMs, online);
  sendJson(200, doc);
}

void handleBattery() {
  commonHeaders();
  SnapshotStore::copy(snap);
  if (!snap.has_data) return sendErr(503, "no_data");
  const SessionInfo si = ConsoleSession::info();
  JsonDocument doc;
  doc["v"] = 1;
  doc["id"] = Net::deviceId();
  doc["seq"] = snap.seq;
  doc["online"] = ConsoleSession::online();
  doc["age_s"] = ageS(si.last_ok_ms);
  doc["module_count"] = snap.module_count;
  JsonObject sys = doc["sys"].to<JsonObject>();
  setI32(sys["mv"], snap.sys_mv);
  setI32(sys["ma"], snap.sys_ma);
  setOrNull(sys["soc"], snap.sys_soc != 0xFF, snap.sys_soc);
  sys["dmv"] = snap.sys_delta_mv;
  sys["bal"] = snap.sys_bal_count;
  setTemp(sys["tmin"], snap.sys_tmin_mc);
  setTemp(sys["tmax"], snap.sys_tmax_mc);
  JsonArray mods = doc["modules"].to<JsonArray>();
  for (uint8_t i = 0; i < MAX_MODULES; i++) {
    const ModuleData& m = snap.m[i];
    if (!m.present && !m.pwr_ts) continue;
    JsonObject o = mods.add<JsonObject>();
    o["n"] = i + 1;
    o["present"] = m.present;
    setI32(o["mv"], m.mv);
    setI32(o["ma"], m.ma);
    setOrNull(o["soc"], m.soc != 0xFF, m.soc);
    setTemp(o["t"], m.temp_mc);
    setTemp(o["tlow"], m.tlow_mc);
    setTemp(o["thigh"], m.thigh_mc);
    setTemp(o["tmos"], m.mos_mc);
    setOrNull(o["vlow"], m.vlow_mv != 0, m.vlow_mv);
    setOrNull(o["vhigh"], m.vhigh_mv != 0, m.vhigh_mv);
    o["base"] = m.base_raw;
    o["base_enum"] = baseStateName(m.base);
    o["volt"] = levelName(m.v_st);
    o["curr"] = levelName(m.c_st);
    o["temp"] = levelName(m.t_st);
    setOrNull(o["rem"], m.remain_mah != 0xFFFFFFFF, m.remain_mah);
    setOrNull(o["cyc"], m.cycles != 0xFFFF, m.cycles);
    setOrNull(o["soh"], m.soh != 0xFF, m.soh);
    if (m.serial[0]) o["serial"] = m.serial; else o["serial"] = nullptr;
    if (m.fw[0]) o["fw"] = m.fw; else o["fw"] = nullptr;
    o["cmin"] = m.cell_min_mv;
    o["cmini"] = m.cell_min_idx;
    o["cmax"] = m.cell_max_mv;
    o["cmaxi"] = m.cell_max_idx;
    o["dmv"] = m.cell_delta_mv;
    o["bal_count"] = m.bal_count;
    setOrNull(o["stat_age_s"], m.stat_ts != 0, ageS(m.stat_ts));
    setOrNull(o["pwr_age_s"], m.pwr_ts != 0, ageS(m.pwr_ts));
    setOrNull(o["cells_age_s"], m.cells_ts != 0, ageS(m.cells_ts));
    JsonArray cells = o["cells"].to<JsonArray>();
    if (m.cells_ts) {
      for (uint8_t c = 0; c < CELLS; c++) {
        const CellData& d = m.cells[c];
        JsonObject co = cells.add<JsonObject>();
        co["i"] = c;
        co["mv"] = d.mv;
        setTemp(co["t"], d.temp_mc);
        co["v_state"] = levelName(d.v_st);
        co["t_state"] = levelName(d.t_st);
        if (d.bal_known) co["bal"] = d.bal; else co["bal"] = nullptr;
        setOrNull(co["soc"], d.soc != 0xFF, d.soc);
        setOrNull(co["mah"], d.mah != 0xFFFFFFFF, d.mah);
      }
    }
  }
  sendJson(200, doc);
}

void fillStatus(JsonDocument& doc) {
  const SessionInfo si = ConsoleSession::info();
  doc["fw"] = PYLONMON_FW_VERSION;
  doc["build"] = __DATE__ " " __TIME__;
  doc["probe"] = (bool)PYLONMON_PROBE;
  doc["id"] = Net::deviceId();
  doc["uptime_s"] = millis() / 1000;
  doc["seq"] = SnapshotStore::seq();
  doc["reset_reason"] = resetReasonName(esp_reset_reason());
  JsonObject mem = doc["mem"].to<JsonObject>();
  mem["heap_free"] = ESP.getFreeHeap();
  mem["heap_min"] = ESP.getMinFreeHeap();
  mem["heap_max_block"] = ESP.getMaxAllocHeap();
  mem["psram_free"] = ESP.getFreePsram();
  JsonObject w = doc["wifi"].to<JsonObject>();
  w["connected"] = Net::connected();
  w["ssid"] = Net::ssid();
  w["ip"] = WiFi.localIP().toString();
  w["rssi"] = WiFi.RSSI();
  w["hostname"] = settings.hostname;
  w["ap"] = Net::apActive();
  w["ap_name"] = Net::apName();
  w["mode"] = Net::modeName();
  time_t now = time(nullptr);
  if (now > 1700000000) {
    char ts[32];
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S%z", &tm);
    doc["time"] = ts;
  } else {
    doc["time"] = nullptr;
  }
  JsonObject c = doc["console"].to<JsonObject>();
  c["state"] = sessStateName(si.state);
  c["paused"] = si.paused;
  c["online"] = ConsoleSession::online();
  setOrNull(c["last_ok_age_s"], si.last_ok_ms != 0, ageS(si.last_ok_ms));
  c["fails_in_row"] = si.fails_in_row;
  c["offline_rounds"] = si.offline_rounds;
  c["module_count"] = si.module_count;
  c["last_cmd"] = String(si.last_cmd);
  c["last_result"] = String(si.last_result);
  c["last_parse_err"] = String(si.last_parse_err);
  JsonObject u = c["uart"].to<JsonObject>();
  u["tx"] = si.tx;
  u["rx"] = si.rx;
  u["baud"] = si.baud;
  JsonObject cfg = doc["config"].to<JsonObject>();
  cfg["wake_en"] = settings.wake_en;
  cfg["login_en"] = settings.login_en;
  cfg["autopoll"] = settings.autopoll;
  cfg["tick_s"] = settings.tick_s;
  cfg["delta_warn_mv"] = settings.delta_warn_mv;
  cfg["delta_crit_mv"] = settings.delta_crit_mv;
  cfg["admin_pass_set"] = !settings.admin_pass.isEmpty();
#if PYLONMON_PROBE
  cfg["log_s"] = settings.log_s;
#endif
  JsonObject s = doc["counters"].to<JsonObject>();
  s["cmd_ok"] = stats.cmd_ok.load();
  s["cmd_timeout"] = stats.cmd_timeout.load();
  s["cmd_incomplete"] = stats.cmd_incomplete.load();
  s["cmd_rejected"] = stats.cmd_rejected.load();
  s["parse_errors"] = stats.parse_errors.load();
  s["rx_noise"] = stats.rx_noise.load();
  s["wake_count"] = stats.wake_count.load();
  s["wifi_reconnects"] = stats.wifi_reconnects.load();
  s["http_requests"] = stats.http_requests.load();
#if PYLONMON_PROBE
  s["captures_saved"] = stats.captures_saved.load();
  s["capture_errors"] = stats.capture_errors.load();
  JsonObject fs = doc["fs"].to<JsonObject>();
  fs["used"] = Captures::used();
  fs["total"] = Captures::total();
#endif
}

void handleStatus() {
  commonHeaders();
  JsonDocument doc;
  fillStatus(doc);
  sendJson(200, doc);
}

// ---------------- /dbg ----------------

void handleCmd() {
  commonHeaders();
  if (!requireAdmin()) return;
  std::string cmd = (server.hasArg("cmd") ? server.arg("cmd") : server.arg("c")).c_str();
  while (!cmd.empty() && cmd.back() == ' ') cmd.pop_back();
  if (!ConsoleClient::allowed(cmd)) return sendErr(403, "not_whitelisted");
  Job j;
  j.type = JobType::Cmd;
  j.cmd = cmd;
  j.ms = server.hasArg("t") ? constrain(server.arg("t").toInt(), 500, CONSOLE_MANUAL_MAX_MS) : CONSOLE_CMD_TIMEOUT_MS;
  j.save = server.arg("save") == "1";
  if (!ConsoleSession::run(j)) return sendErr(503, "console_busy");
  server.sendHeader("X-Result", cmdResultName(j.reply.result));
  server.sendHeader("X-Ms", String(j.reply.ms));
  server.sendHeader("X-Bytes", String((unsigned)j.reply.text.size()));
  server.sendHeader("X-Prompt", j.reply.prompt ? "1" : "0");
  server.sendHeader("X-Truncated", j.reply.truncated ? "1" : "0");
  if (!j.out.empty()) server.sendHeader("X-Parse-Error", j.out.c_str());
  if (server.arg("hex") == "1") server.send(200, "text/plain; charset=utf-8", hexDump(j.reply.text));
  else server.send(200, "text/plain; charset=utf-8", String(j.reply.text.c_str(), j.reply.text.size()));
}

#if PYLONMON_PROBE
void handleSniff() {
  commonHeaders();
  Job j;
  j.type = JobType::Sniff;
  j.ms = server.hasArg("ms") ? constrain(server.arg("ms").toInt(), 100, 10000) : 3000;
  if (!ConsoleSession::run(j)) return sendErr(503, "console_busy");
  server.sendHeader("X-Bytes", String((unsigned)j.out.size()));
  if (server.arg("hex") == "0") server.send(200, "text/plain; charset=utf-8", String(j.out.c_str(), j.out.size()));
  else server.send(200, "text/plain; charset=utf-8", hexDump(j.out));
}

bool pinAllowed(long p) {
  if (p < 1 || p > 44) return false;
  if (p == PIN_LED || p == 19 || p == 20 || (p >= 22 && p <= 32)) return false;
  return true;
}

void handleUart() {
  commonHeaders();
  if (!requirePost()) return;
  long tx = server.hasArg("tx") ? server.arg("tx").toInt() : settings.uart_tx;
  long rx = server.hasArg("rx") ? server.arg("rx").toInt() : settings.uart_rx;
  long baud = server.hasArg("baud") ? server.arg("baud").toInt() : (long)settings.uart_baud;
  if (server.arg("swap") == "1") std::swap(tx, rx);
  if (!pinAllowed(tx) || !pinAllowed(rx) || tx == rx) return sendErr(400, "bad_pins");
  if (baud < 1200 || baud > 921600) return sendErr(400, "bad_baud");
  settings.uart_tx = tx;
  settings.uart_rx = rx;
  settings.uart_baud = baud;
  settings.save();
  Job j;
  j.type = JobType::Uart;
  j.tx = tx; j.rx = rx; j.baud = baud;
  if (!ConsoleSession::run(j)) LOGI("http: uart job queued, console busy");
  LOGI("http: uart tx=%ld rx=%ld baud=%ld", tx, rx, baud);
  handleStatus();
}
#endif  // PYLONMON_PROBE

void handleConfig() {
  commonHeaders();
  if (server.method() == HTTP_POST) {
    if (!requireAdmin()) return;
    auto b = [](const String& v) { return v == "1" || v == "true"; };
    if (server.hasArg("wake_en")) settings.wake_en = b(server.arg("wake_en"));
    if (server.hasArg("login_en")) settings.login_en = b(server.arg("login_en"));
    if (server.hasArg("autopoll")) settings.autopoll = b(server.arg("autopoll"));
    if (server.hasArg("tick_s")) settings.tick_s = constrain(server.arg("tick_s").toInt(), 2, 600);
    if (server.hasArg("log_s")) settings.log_s = constrain(server.arg("log_s").toInt(), 0, 65535);
    if (server.hasArg("delta_warn_mv")) settings.delta_warn_mv = constrain(server.arg("delta_warn_mv").toInt(), 1, 1000);
    if (server.hasArg("delta_crit_mv")) settings.delta_crit_mv = constrain(server.arg("delta_crit_mv").toInt(), 1, 1000);
    if (server.hasArg("admin_pass") && server.arg("admin_pass").length() <= 32) settings.admin_pass = server.arg("admin_pass");
    bool hostChanged = false;
    if (server.hasArg("hostname")) {
      String h = server.arg("hostname");
      h.toLowerCase();
      if (!wififsm::validHostname(h.c_str())) return sendErr(400, "bad_hostname");
      hostChanged = h != settings.hostname;
      settings.hostname = h;
    }
    bool wifiChanged = false;
    if (server.hasArg("wifi_ssid") && server.arg("wifi_ssid").length()) {
      String ssid = server.arg("wifi_ssid"), pass = server.arg("wifi_pass");
      if (ssid.length() > 32 || pass.length() > 64 || (pass.length() && pass.length() < 8)) return sendErr(400, "bad_wifi");
      if (!(pass.isEmpty() && ssid == settings.wifi_ssid)) settings.wifi_pass = pass;
      settings.wifi_ssid = ssid;
      wifiChanged = true;
    }
    settings.save();
    Job j;
    j.type = JobType::Config;
    if (!ConsoleSession::run(j)) LOGI("http: config job queued, console busy");
    const bool reboot = wifiChanged || hostChanged;
    LOGI("http: config saved%s%s", wifiChanged ? ", wifi changed" : "", hostChanged ? ", hostname changed" : "");
    if (reboot) HttpApi::requestReboot(1500);
    JsonDocument doc;
    fillStatus(doc);
    doc["reboot"] = reboot;
    return sendJson(200, doc);
  }
  JsonDocument doc;
  fillStatus(doc);
  sendJson(200, doc);
}

#if PYLONMON_PROBE
void handleSimpleJob(JobType t) {
  commonHeaders();
  if (!requirePost()) return;
  Job j;
  j.type = t;
  if (!ConsoleSession::run(j)) return sendErr(503, "console_busy");
  JsonDocument doc;
  doc["ok"] = j.ok;
  if (t == JobType::Login) {
    doc["result"] = cmdResultName(j.reply.result);
    doc["text"] = j.reply.text.c_str();
  }
  sendJson(200, doc);
}

void handleFiles() {
  commonHeaders();
  server.send(200, "application/json; charset=utf-8", Captures::listJson());
}

void handleFile() {
  commonHeaders();
  String n = server.arg("n");
  if (server.method() == HTTP_DELETE || (server.method() == HTTP_POST && server.arg("delete") == "1")) {
    bool ok = Captures::remove(n);
    return ok ? sendErr(200, "deleted") : sendErr(404, "not_found");
  }
  if (!Captures::exists(n)) return sendErr(404, "not_found");
  File f = LittleFS.open(Captures::path(n), "r");
  if (!f) return sendErr(500, "open_failed");
  server.streamFile(f, "text/plain; charset=utf-8");
  f.close();
}

void handleParse() {
  commonHeaders();
  String n = server.arg("n");
  if (!Captures::exists(n)) return sendErr(404, "not_found");
  File f = LittleFS.open(Captures::path(n), "r");
  if (!f) return sendErr(500, "open_failed");
  std::string text;
  text.reserve(f.size());
  String meta = f.readStringUntil('\n');
  uint8_t buf[512];
  while (f.available()) {
    int r = f.read(buf, sizeof(buf));
    if (r <= 0) break;
    text.append((const char*)buf, r);
  }
  f.close();
  JsonDocument doc;
  doc["file"] = n;
  doc["meta"] = meta;
  doc["completed"] = parser::completedOk(text);
  std::string err;
  if (n.indexOf("_pwr") > 0) {
    parser::PwrResult r;
    bool ok = parser::parsePwr(text, r, err);
    doc["ok"] = ok;
    doc["present"] = r.present;
    JsonArray rows = doc["rows"].to<JsonArray>();
    for (auto& p : r.rows) {
      JsonObject o = rows.add<JsonObject>();
      o["n"] = p.n;
      o["absent"] = p.absent;
      if (p.absent) continue;
      setI32(o["mv"], p.mv); setI32(o["ma"], p.ma); setI32(o["t_mc"], p.t);
      setI32(o["tlow_mc"], p.tlow); setI32(o["thigh_mc"], p.thigh);
      setI32(o["vlow"], p.vlow); setI32(o["vhigh"], p.vhigh);
      setI32(o["soc"], p.soc); setI32(o["mos_mc"], p.mos);
      o["base"] = p.base; o["vst"] = p.vst; o["cst"] = p.cst; o["tst"] = p.tst;
    }
  } else if (n.indexOf("_bat") > 0) {
    parser::BatResult r;
    bool ok = parser::parseBat(text, r, err);
    doc["ok"] = ok;
    JsonArray rows = doc["cells"].to<JsonArray>();
    for (auto& c : r.cells) {
      JsonObject o = rows.add<JsonObject>();
      o["i"] = c.idx;
      setI32(o["mv"], c.mv); setI32(o["ma"], c.ma); setI32(o["t_mc"], c.t);
      setI32(o["soc"], c.soc); setI32(o["coulomb"], c.coulomb);
      o["base"] = c.base; o["vst"] = c.vst; o["cst"] = c.cst; o["tst"] = c.tst;
      if (c.bal >= 0) o["bal"] = c.bal == 1; else o["bal"] = nullptr;
    }
  } else {
    doc["ok"] = nullptr;
    err = "no parser for this command yet";
  }
  doc["err"] = err;
  parser::Table t;
  std::string terr;
  const char* hw = n.indexOf("_pwr") > 0 ? "Power" : "Battery";
  if (parser::findTable(text, hw, t, terr)) {
    JsonArray h = doc["header"].to<JsonArray>();
    for (auto& s : t.header) h.add(s);
    doc["rows_tokens"] = t.rows.empty() ? 0 : (int)t.rows[0].size();
  }
  sendJson(200, doc);
}

#endif  // PYLONMON_PROBE

void handleLog() {
  commonHeaders();
  if (!requireAdmin()) return;
  server.send(200, "text/plain; charset=utf-8", Log::dump());
}

void handleReboot() {
  commonHeaders();
  if (!requirePost() || !requireAdmin()) return;
  sendErr(200, "rebooting");
  HttpApi::requestReboot();
}


#define PAGE_HEAD "<!doctype html><html lang=ru><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
#define PAGE_CSS "body{font:16px sans-serif;max-width:440px;margin:0 auto;padding:8px 16px;color:#222}" \
  "input,button{width:100%;padding:10px;margin:4px 0 10px;font-size:16px;box-sizing:border-box}" \
  "button{background:#1565c0;color:#fff;border:0;border-radius:4px}button:disabled{opacity:.5}" \
  ".r{display:flex;gap:6px}.r input{flex:1}.r button{width:auto}.e{color:#c62828}.ok{color:#2e7d32}#m{font-weight:bold}"
#define PAGE_JS "var $=function(i){return document.getElementById(i)};" \
  "function E(t){return String(t).replace(/[&<>\"]/g,function(c){return'&#'+c.charCodeAt(0)+';'})}" \
  "function M(t,c){var m=$('m');m.textContent=t;m.className=c||''}" \
  "function H(){var a=$('a').value;return a?{Authorization:'Basic '+btoa(unescape(encodeURIComponent('admin:'+a)))}:{}}" \
  "function J(x){if(x.status==401){$('ad').hidden=false;throw'Нужен пароль администратора'}return x.json()}" \
  "function X(t){M(typeof t=='string'?t:'Нет связи с устройством','e')}"

const char SETUP_HTML[] PROGMEM = PAGE_HEAD "<title>PylonMon: настройка</title><style>" PAGE_CSS
  ".n{display:flex;justify-content:space-between;padding:10px 4px;border-bottom:1px solid #ddd;cursor:pointer}"
  "#l{margin-bottom:8px}.s{background:#757575}</style></head><body>"
  "<h2>PylonMon: настройка Wi-Fi</h2><p>Выберите домашнюю сеть:</p><div id=l>Поиск сетей…</div>"
  "<button type=button class=s onclick=scan(1)>Обновить список</button>"
  "<form id=f><label>Сеть (SSID)</label><input id=s maxlength=32 required>"
  "<label>Пароль</label><div class=r><input id=p type=password maxlength=64>"
  "<button type=button onclick=\"var p=$('p');p.type=p.type=='password'?'text':'password'\">показать</button></div>"
  "<label>Имя устройства</label><input id=h value=battery maxlength=31 required pattern=\"[A-Za-z0-9]([A-Za-z0-9\\-]*[A-Za-z0-9])?\">"
  "<div id=ad hidden><label>Пароль администратора</label><input id=a type=password></div>"
  "<button id=b>Сохранить</button></form><p id=m></p><script>" PAGE_JS
  "var L='<svg width=12 height=14 viewBox=\"0 0 12 14\"><rect y=6 width=12 height=8 rx=1 /><path d=\"M3 6V4a3 3 0 0 1 6 0v2\" fill=none stroke=currentColor stroke-width=2 /></svg>',lost=0;"
  "function scan(r){fetch('/api/v1/wifi/scan'+(r?'?refresh=1':'')).then(function(x){return x.json()}).then(function(j){"
  "if(j.scanning){setTimeout(scan,1500);return}var h='';(j.nets||[]).forEach(function(n){"
  "h+='<div class=n data-s=\"'+E(n.ssid)+'\" data-l='+(n.lock?1:0)+'><span>'+(n.lock?L:'')+' '+E(n.ssid)+'</span><span>'+n.rssi+' дБм</span></div>'});"
  "$('l').innerHTML=h||'Сети не найдены'}).catch(function(){setTimeout(scan,3000)})}"
  "$('l').onclick=function(e){var d=e.target.closest('.n');if(!d)return;$('s').value=d.dataset.s;$('p').value='';if(d.dataset.l==1)$('p').focus()};"
  "function poll(){fetch('/api/v1/wifi/state').then(function(x){return x.json()}).then(function(j){lost=0;var t=j.trial;"
  "if(t=='testing'){setTimeout(poll,1000);return}$('b').disabled=false;"
  "if(t=='ok'){M('Подключено, адрес '+j.host+'.local ('+j.ip+'). Через 10 с точка '+j.ap+' выключится — верните телефон в домашнюю сеть.','ok');return}"
  "X({bad_pass:'Неверный пароль',no_ssid:'Сеть не найдена',failed:'Не удалось подключиться'}[t]||t)})"
  ".catch(function(){if(++lost<8)setTimeout(poll,1500);else{$('b').disabled=false;"
  "X('Связь с устройством потеряна. Если подключение удалось — подключите телефон к домашней сети и откройте http://'+$('h').value+'.local')}})}"
  "$('f').onsubmit=function(e){e.preventDefault();var b=new URLSearchParams();b.append('ssid',$('s').value);b.append('pass',$('p').value);"
  "b.append('hostname',$('h').value);$('b').disabled=true;M('Проверяю подключение…');"
  "fetch('/api/v1/wifi/connect',{method:'POST',headers:H(),body:b}).then(J).then(function(j){if(j.err)throw({bad_ssid:'Укажите сеть',"
  "short_pass:'Пароль Wi-Fi — не меньше 8 символов',bad_hostname:'Имя: латиница, цифры, дефис',busy:'Проверка уже идёт',"
  "already_connected:'Уже подключено'}[j.err]||j.err);poll()}).catch(function(t){$('b').disabled=false;X(t)})};"
  "fetch('/api/v1/wifi/state').then(function(x){return x.json()}).then(function(j){$('h').value=j.host;if(j.auth)$('ad').hidden=false});"
  "scan()</script></body></html>";

const char SETTINGS_HTML[] PROGMEM = PAGE_HEAD "<title>PylonMon</title><style>" PAGE_CSS
  "table{border-collapse:collapse;width:100%}td{padding:5px 4px;border-bottom:1px solid #eee}td:first-child{color:#666}"
  "h3{margin:22px 0 6px}.g{background:#757575}.d{background:#c62828}.c{width:auto;margin-right:8px}</style></head><body>"
  "<h2>PylonMon</h2><table id=t></table>"
  "<div id=ad hidden><h3>Доступ</h3><label>Пароль администратора</label><input id=a type=password></div>"
  "<h3>Wi-Fi</h3><form data-ok='Сохранено, перезагрузка…'><label>Сеть (SSID)</label><input name=wifi_ssid maxlength=32 required>"
  "<label>Пароль</label><input name=wifi_pass type=password maxlength=64 placeholder='не менять'>"
  "<button>Сохранить и перезагрузить</button></form>"
  "<h3>Опрос и пороги</h3><form><label>Тик опроса, с (2–600)</label><input name=tick_s type=number min=2 max=600>"
  "<label>Разбаланс ячеек: предупреждение, мВ</label><input name=delta_warn_mv type=number min=1 max=1000>"
  "<label>Разбаланс ячеек: авария, мВ</label><input name=delta_crit_mv type=number min=1 max=1000><button>Сохранить</button></form>"
  "<h3>Имя и пароль</h3><form><label>Имя устройства (.local; при смене — перезагрузка)</label><input name=hostname maxlength=31>"
  "<label>Новый пароль администратора</label><input name=admin_pass type=password maxlength=32 placeholder='не менять'>"
  "<label><input type=checkbox name=nopass class=c>снять пароль</label><button>Сохранить</button></form>"
  "<h3>Действия</h3><button class=g onclick=\"A('/api/v1/reboot','Перезагрузить устройство?','Перезагрузка…')\">Перезагрузить</button>"
  "<button class=d onclick=\"A('/api/v1/wifi/reset','Сбросить Wi-Fi? Устройство уйдёт из сети и включит точку '+ap+' для новой настройки.','Wi-Fi сброшен, перезагрузка…')\">Сбросить Wi-Fi</button>"
  "<p id=m></p><p><a href=/api/v1/battery>/api/v1/battery</a> · <a href=/api/v1/status>/api/v1/status</a></p><script>" PAGE_JS
  "var ap='PylonMon',F=0;"
  "function U(s){var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return(d?d+' д ':'')+h+' ч '+m+' мин'}"
  "function R(){fetch('/api/v1/status').then(function(x){return x.json()}).then(function(j){var w=j.wifi,c=j.console,g=j.config,f=document.forms;ap=w.ap_name;"
  "var r=[['Wi-Fi',(w.ssid||'—')+', '+w.rssi+' дБм, '+w.ip],['Режим Wi-Fi',w.mode],['Батарея',c.online?'на связи':'нет связи'],"
  "['Модулей',c.module_count],['seq',j.seq],['Время работы',U(j.uptime_s)],['Прошивка',j.fw+(j.probe?' (probe)':'')]];"
  "$('t').innerHTML=r.map(function(x){return'<tr><td>'+x[0]+'<td>'+E(x[1])}).join('');if(g.admin_pass_set)$('ad').hidden=false;"
  "if(!F){F=1;f[0].wifi_ssid.value=w.ssid||'';f[1].tick_s.value=g.tick_s;f[1].delta_warn_mv.value=g.delta_warn_mv;"
  "f[1].delta_crit_mv.value=g.delta_crit_mv;f[2].hostname.value=w.hostname}}).catch(function(){})}"
  "function P(u,b,ok){return fetch(u,{method:'POST',headers:H(),body:b}).then(J).then(function(j){if(j.err&&!/reboot/.test(j.err))throw({bad_hostname:'Имя: латиница, цифры, дефис',bad_wifi:'Пароль Wi-Fi — от 8 до 64 символов'}[j.err]||j.err);M(j.reboot?'Сохранено, перезагрузка…':ok,'ok');return j}).catch(X)}"
  "function A(u,q,ok){if(confirm(q))P(u,'',ok)}"
  "[].forEach.call(document.forms,function(f){f.onsubmit=function(e){e.preventDefault();var b=new URLSearchParams(),np='';"
  "[].forEach.call(f.elements,function(i){if(!i.name)return;if(i.type=='checkbox'){if(i.checked)b.set('admin_pass','');return}"
  "if(i.value!=='')b.set(i.name,i.value)});if(f.admin_pass&&!f.nopass.checked)np=f.admin_pass.value;"
  "P('/api/v1/settings',b,f.dataset.ok||'Сохранено').then(function(j){if(j&&np)$('a').value=np;if(j&&f.admin_pass)f.admin_pass.value=''})}});"
  "R();setInterval(R,5000)</script></body></html>";

void sendPage(const char* html) {
  commonHeaders();
  server.send_P(200, "text/html; charset=utf-8", html);
}

bool viaAp() { return Net::apActive() && server.client().localIP() == WiFi.softAPIP(); }

void redirectPortal() {
  commonHeaders();
  server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/");
  server.send(302, "text/plain", "");
}

void handleRoot() { sendPage(viaAp() ? SETUP_HTML : SETTINGS_HTML); }

String scanCache;
void handleWifiScan() {
  commonHeaders();
  if (!Net::apActive()) return sendErr(409, "ap_off");
  const int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return server.send(200, "application/json; charset=utf-8", "{\"scanning\":true}");
  if (n >= 0) {
    struct Net_ { String ssid; int rssi; bool lock; };
    std::vector<Net_> v;
    for (int i = 0; i < n; i++) {
      String s = WiFi.SSID(i);
      if (s.isEmpty()) continue;
      int r = WiFi.RSSI(i);
      auto it = std::find_if(v.begin(), v.end(), [&](const Net_& x) { return x.ssid == s; });
      if (it == v.end()) v.push_back({s, r, WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
      else if (r > it->rssi) it->rssi = r;
    }
    WiFi.scanDelete();
    std::sort(v.begin(), v.end(), [](const Net_& a, const Net_& b) { return a.rssi > b.rssi; });
    if (v.size() > 20) v.resize(20);
    JsonDocument doc;
    JsonArray a = doc["nets"].to<JsonArray>();
    for (auto& x : v) {
      JsonObject o = a.add<JsonObject>();
      o["ssid"] = x.ssid;
      o["rssi"] = x.rssi;
      o["lock"] = x.lock;
    }
    scanCache = "";
    serializeJson(doc, scanCache);
  } else if ((scanCache.isEmpty() || server.arg("refresh") == "1") && Net::trial() != wififsm::Trial::Testing) {
    if (WiFi.scanNetworks(true) == WIFI_SCAN_RUNNING)
      return server.send(200, "application/json; charset=utf-8", "{\"scanning\":true}");
  }
  server.send(200, "application/json; charset=utf-8", scanCache.isEmpty() ? String("{\"nets\":[]}") : scanCache);
}

void handleWifiConnect() {
  commonHeaders();
  if (!requirePost()) return;
  if (!Net::apActive()) return sendErr(409, "ap_off");
  if (!viaAp()) return sendErr(403, "not_via_ap");
  if (Net::mode() == wififsm::Mode::ApRescue && !requireAdmin()) return;
  if (Net::connected()) return sendErr(409, "already_connected");
  String ssid = server.arg("ssid"), pass = server.arg("pass"), host = server.arg("hostname");
  host.trim();
  host.toLowerCase();
  if (host.isEmpty()) host = settings.hostname;
  if (ssid.isEmpty() || ssid.length() > 32 || pass.length() > 64) return sendErr(400, "bad_ssid");
  if (pass.length() && pass.length() < 8) return sendErr(400, "short_pass");
  if (!wififsm::validHostname(host.c_str())) return sendErr(400, "bad_hostname");
  if (!Net::startTrial(ssid, pass, host)) return sendErr(409, "busy");
  LOGI("http: portal test requested");
  server.send(200, "application/json; charset=utf-8", "{\"ok\":true}");
}

void handleWifiState() {
  commonHeaders();
  JsonDocument doc;
  doc["mode"] = Net::modeName();
  doc["trial"] = wififsm::trialName(Net::trial());
  doc["ssid"] = Net::trialSsid();
  if (Net::connected()) doc["ip"] = WiFi.localIP().toString(); else doc["ip"] = nullptr;
  doc["host"] = settings.hostname;
  doc["ap"] = Net::apName();
  doc["auth"] = Net::mode() == wififsm::Mode::ApRescue && !settings.admin_pass.isEmpty();
  sendJson(200, doc);
}

void handleWifiReset() {
  commonHeaders();
  if (!requirePost() || !requireAdmin()) return;
  settings.clearWifi();
  LOGI("http: wifi reset");
  sendErr(200, "wifi_reset_rebooting");
  HttpApi::requestReboot(1000);
}

void handleWifi() {
  commonHeaders();
  if (server.method() != HTTP_POST) {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
    return;
  }
  if (!requireAdmin()) return;
  if (server.arg("reset") == "1") settings.clearWifi();
  else {
    String ssid = server.arg("ssid");
    if (ssid.isEmpty() || ssid.length() > 32 || server.arg("pass").length() > 64) return sendErr(400, "bad_ssid");
    settings.wifi_ssid = ssid;
    settings.wifi_pass = server.arg("pass");
    settings.save();
  }
  LOGI("http: wifi settings saved, rebooting");
  server.send(200, "text/plain; charset=utf-8", "Сохранено. Перезагрузка…");
  HttpApi::requestReboot(1000);
}

void handleIndex() {
  commonHeaders();
  String s = "PylonMon " PYLONMON_FW_VERSION "\n\n"
             "GET  /                                страница настроек (из точки доступа — портал Wi-Fi)\n"
             "GET  /api/v1/snapshot | /api/v1/battery | /api/v1/status\n"
             "GET  /api/v1/console?cmd=pwr[&t=ms]   (admin_pass; t <= 5000; 503 console_busy)\n"
             "GET|POST /api/v1/settings?tick_s=&delta_warn_mv=&delta_crit_mv=&admin_pass=&hostname=&wifi_ssid=&wifi_pass=  (POST: admin_pass)\n"
             "POST /api/v1/reboot | /api/v1/wifi/reset (admin_pass)\n"
             "GET  /api/v1/wifi/scan[?refresh=1] | /api/v1/wifi/state ; POST /api/v1/wifi/connect?ssid=&pass=&hostname= (только из AP)\n"
             "GET  /api/v1/log                      (admin_pass)\n"
             "POST /api/v1/update                   (admin / OTA_PASS, header Origin)\n"
             "POST /wifi?ssid=&pass=                (admin_pass, старый путь)\n"
#if PYLONMON_PROBE
             "\nprobe:\n"
             "GET  /dbg/status\n"
             "GET  /dbg/cmd?c=pwr[&hex=1][&save=1][&t=ms]   (whitelist: pwr, bat N, stat N, soh N, info N, help, login debug)\n"
             "GET  /dbg/sniff?ms=3000[&hex=0]\n"
             "POST /dbg/uart?tx=17&rx=18&baud=115200  | ?swap=1\n"
             "GET|POST /dbg/config?wake_en=&login_en=&autopoll=&tick_s=&log_s=&...\n"
             "POST /dbg/wake | /dbg/login | /dbg/reprobe | /dbg/reboot\n"
             "GET  /dbg/files | /dbg/file?n=NAME | /dbg/parse?n=NAME ; DELETE /dbg/file?n=NAME|all\n"
             "GET  /dbg/log\n"
             "GET|POST /dbg/update  (= /api/v1/update)\n"
#endif
             ;
  server.send(200, "text/plain; charset=utf-8", s);
}

void httpTask(void*) {
  for (;;) {
    server.handleClient();
    delay(2);
  }
}

}  // namespace

namespace HttpApi {

void begin() {
  const char* keep[] = {"If-None-Match"};
  server.collectHeaders(keep, 1);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/settings", HTTP_GET, [] { sendPage(SETTINGS_HTML); });
  server.on("/help", HTTP_GET, handleIndex);
  server.on("/api/v1/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/v1/wifi/connect", handleWifiConnect);
  server.on("/api/v1/wifi/state", HTTP_GET, handleWifiState);
  for (const char* u : {"/generate_204", "/gen_204", "/hotspot-detect.html", "/connecttest.txt", "/ncsi.txt", "/fwlink"})
    server.on(u, redirectPortal);
  server.on("/api/v1/snapshot", HTTP_GET, handleSnapshot);
  server.on("/api/v1/battery", HTTP_GET, handleBattery);
  server.on("/api/v1/status", HTTP_GET, handleStatus);
  server.on("/api/v1/console", HTTP_GET, handleCmd);
  server.on("/api/v1/settings", handleConfig);
  server.on("/api/v1/reboot", handleReboot);
  server.on("/api/v1/wifi/reset", handleWifiReset);
  server.on("/api/v1/log", HTTP_GET, handleLog);
  server.on("/wifi", handleWifi);
#if PYLONMON_PROBE
  server.on("/dbg/status", HTTP_GET, handleStatus);
  server.on("/dbg/cmd", HTTP_GET, handleCmd);
  server.on("/dbg/sniff", HTTP_GET, handleSniff);
  server.on("/dbg/uart", handleUart);
  server.on("/dbg/config", handleConfig);
  server.on("/dbg/wake", [] { handleSimpleJob(JobType::Wake); });
  server.on("/dbg/login", [] { handleSimpleJob(JobType::Login); });
  server.on("/dbg/reprobe", [] { handleSimpleJob(JobType::Reprobe); });
  server.on("/dbg/files", HTTP_GET, handleFiles);
  server.on("/dbg/file", handleFile);
  server.on("/dbg/parse", HTTP_GET, handleParse);
  server.on("/dbg/log", HTTP_GET, handleLog);
  server.on("/dbg/reboot", handleReboot);
#endif
  server.onNotFound([] {
    if (viaAp()) return redirectPortal();
    commonHeaders();
    sendErr(404, "not_found");
  });
  updater.setup(&server, "/api/v1/update", "admin", OTA_PASS);
#if PYLONMON_PROBE
  updaterDbg.setup(&server, "/dbg/update", "admin", OTA_PASS);
#endif
  server.begin();
  LOGI("http: listening on %d", HTTP_PORT);
  xTaskCreate(httpTask, "httpTask", 8192, nullptr, 2, nullptr);
}

void requestReboot(uint32_t delay_ms) { rebootAt = (millis() + delay_ms) | 1; }

void loopCheck() {
  if (rebootAt && (int32_t)(millis() - rebootAt) >= 0) {
    LOGI("reboot requested");
    delay(100);
    ESP.restart();
  }
}

}  // namespace HttpApi
