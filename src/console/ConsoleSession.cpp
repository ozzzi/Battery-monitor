#include "console/ConsoleSession.h"
#include <esp_task_wdt.h>
#include <atomic>
#include <algorithm>
#include <map>
#include <new>
#include <vector>
#include "config.h"
#include "console/Parser.h"
#include "model/SnapshotStore.h"
#if PYLONMON_PROBE
#include "util/Captures.h"
#endif
#include "util/Log.h"
#include "util/Settings.h"
#include "util/Stats.h"

const char* sessStateName(SessState s) {
  switch (s) {
    case SessState::Boot: return "BOOT";
    case SessState::Probe: return "PROBE";
    case SessState::Wake: return "WAKE";
    case SessState::Login: return "LOGIN";
    case SessState::Ready: return "READY";
    case SessState::Offline: return "OFFLINE";
  }
  return "?";
}

namespace {

struct Pending {
  Job job;
  SemaphoreHandle_t done = nullptr;
  std::atomic<int> refs{2};
  std::atomic<bool> abandoned{false};
};

void release(Pending* p) {
  if (p->refs.fetch_sub(1) == 1) {
    vSemaphoreDelete(p->done);
    delete p;
  }
}

ConsoleClient client;
QueueHandle_t jobs = nullptr;
SemaphoreHandle_t infoMtx = nullptr;
SessionInfo inf{};
std::atomic<bool> paused{false};

SessState state = SessState::Boot;
uint32_t nextActionMs = 0;
uint32_t tick = 0;
uint8_t batRR = 0;
uint32_t nextStatMs = 0, nextInfoMs = 0;
uint8_t pwrFails = 0;
uint32_t lastFreshCheckMs = 0;
uint16_t lastTimeKey = 0;
bool helpDone = false;
std::vector<std::string> slowQueue;
std::map<std::string, uint32_t> lastSaved, lastErrSaved;

void setInfo(const std::function<void(SessionInfo&)>& fn) {
  xSemaphoreTake(infoMtx, portMAX_DELAY);
  fn(inf);
  xSemaphoreGive(infoMtx);
}

void setState(SessState s) {
  if (s != state) LOGI("console: %s -> %s", sessStateName(state), sessStateName(s));
  if (s == SessState::Probe) pwrFails = 0;
  state = s;
  setInfo([&](SessionInfo& i) { i.state = s; });
}

void feed() { esp_task_wdt_reset(); }

std::string applyReply(const std::string& cmd, const CmdReply& r) {
  if (r.result != CmdResult::Ok) return "";
  const uint32_t now = millis() | 1;
  std::string err;
  if (cmd == "pwr") {
    parser::PwrResult pr;
    if (!parser::parsePwr(r.text, pr, err)) return err;
    if (pr.ignored) LOGI("parse: pwr: %d present module(s) > %u ignored", pr.ignored, (unsigned)MAX_MODULES);
    SnapshotStore::update([&](Snapshot& s) { parser::applyPwr(pr, s, now); });
    setInfo([&](SessionInfo& i) { i.module_count = (uint8_t)pr.present; });
    return "";
  }
  if (cmd.size() == 5 && cmd.compare(0, 4, "bat ") == 0) {
    int mod = cmd[4] - '0';
    parser::BatResult br;
    if (!parser::parseBat(r.text, br, err)) return err;
    bool ok = true;
    SnapshotStore::update([&](Snapshot& s) { ok = parser::applyBat(mod, br, s, now, err); });
    return ok ? "" : err;
  }
  if (cmd.size() == 6 && cmd.compare(0, 5, "info ") == 0) {
    parser::InfoResult ir;
    if (!parser::parseInfo(r.text, ir, err)) return err;
    SnapshotStore::update([&](Snapshot& s) { parser::applyInfo(cmd[5] - '0', ir, s); });
    return "";
  }
  if (cmd.size() == 6 && cmd.compare(0, 5, "stat ") == 0) {
    parser::StatResult sr;
    if (!parser::parseStat(r.text, sr, err)) return err;
    if (!sr.warn.empty()) {
      stats.parse_errors++;
      LOGI("parse: '%s': %s", cmd.c_str(), sr.warn.c_str());
    }
    SnapshotStore::update([&](Snapshot& s) { parser::applyStat(cmd[5] - '0', sr, s, now); });
    return "";
  }
  return "";
}

CmdReply doCmd(const std::string& cmd, uint32_t timeout, bool forceSave, const char* src, std::string* perr = nullptr,
               bool retry = true) {
  feed();
  CmdReply r = client.exec(cmd, timeout);
  feed();
  std::string err = applyReply(cmd, r);
  if (retry && !err.empty() && err.find("line noise") != std::string::npos) {
    stats.rx_noise++;
    stats.parse_errors++;
    LOGI("parse: '%s': %s, retry", cmd.c_str(), err.c_str());
    delay(CONSOLE_INTER_CMD_MS);
    r = client.exec(cmd, timeout);
    feed();
    err = applyReply(cmd, r);
  }
  if (!err.empty()) {
    stats.parse_errors++;
    if (err.find("line noise") != std::string::npos) stats.rx_noise++;
    LOGI("parse: '%s': %s", cmd.c_str(), err.c_str());
  }
  if (r.result == CmdResult::Ok) setInfo([&](SessionInfo& i) { i.last_ok_ms = millis() | 1; });
  setInfo([&](SessionInfo& i) {
    strlcpy(i.last_cmd, cmd.empty() ? "<enter>" : cmd.c_str(), sizeof(i.last_cmd));
    strlcpy(i.last_result, cmdResultName(r.result), sizeof(i.last_result));
    if (!err.empty()) strlcpy(i.last_parse_err, err.c_str(), sizeof(i.last_parse_err));
  });

#if PYLONMON_PROBE
  bool save = forceSave;
  if (!save && !cmd.empty() && r.result != CmdResult::Rejected) {
    const uint32_t now = millis();
    const bool periodic = cmd == "pwr" || cmd.compare(0, 4, "bat ") == 0;
    if (!periodic) {
      save = true;
    } else if (settings.log_s) {
      auto it = lastSaved.find(cmd);
      if (it == lastSaved.end() || now - it->second >= settings.log_s * 1000UL) save = true;
    }
    if (!save && (!err.empty() || r.result == CmdResult::Incomplete)) {
      auto it = lastErrSaved.find(cmd);
      if (it == lastErrSaved.end() || now - it->second >= 600000UL) {
        save = true;
        lastErrSaved[cmd] = now;
      }
    }
    if (save && periodic) lastSaved[cmd] = now;
  }
  if (save && !r.text.empty()) Captures::save(cmd.empty() ? "enter" : cmd, err.empty() ? cmdResultName(r.result) : "parse_error", src, r.text);
#else
  (void)forceSave; (void)src;
#endif
  if (perr) *perr = err;
  delay(CONSOLE_INTER_CMD_MS);
  return r;
}

uint8_t moduleCount() {
  uint8_t n;
  xSemaphoreTake(infoMtx, portMAX_DELAY);
  n = inf.module_count;
  xSemaphoreGive(infoMtx);
  return (n >= 1 && n <= MAX_MODULES) ? n : 2;
}

void scheduleSlow(uint32_t now) {
  const uint8_t n = moduleCount();
  char b[12];
  auto queue = [&](const char* c) {
    for (uint8_t m = 1; m <= n; m++) {
      snprintf(b, sizeof(b), "%s %u", c, m);
      if (std::find(slowQueue.begin(), slowQueue.end(), b) == slowQueue.end()) slowQueue.push_back(b);
    }
  };
  if (!nextInfoMs || (int32_t)(now - nextInfoMs) >= 0) { queue("info"); nextInfoMs = (now + INFO_PERIOD_S * 1000UL) | 1; }
  if (!nextStatMs || (int32_t)(now - nextStatMs) >= 0) { queue("stat"); queue("soh"); nextStatMs = (now + STAT_PERIOD_S * 1000UL) | 1; }
#if PYLONMON_PROBE
  if (!helpDone) { slowQueue.push_back("help"); helpDone = true; }
#endif
}

void applyAlarmCfg() {
  SnapshotStore::update([](Snapshot& s) {
    s.alarm_cfg.delta_warn_mv = settings.delta_warn_mv;
    s.alarm_cfg.delta_crit_mv = settings.delta_crit_mv;
  });
}

bool probeOnce() {
  CmdReply r = doCmd("", 1000, false, "auto");
  if (r.prompt) return true;
  CmdReply p = doCmd("pwr", CONSOLE_CMD_TIMEOUT_MS, false, "auto");
  return p.result == CmdResult::Ok;
}

uint8_t probeAttempts = 0;

void goOffline() {
  setState(SessState::Offline);
  uint16_t rounds = 0;
  setInfo([&](SessionInfo& i) { rounds = ++i.offline_rounds; });
  nextActionMs = millis() + (rounds > 10 ? 120000UL : 30000UL);
}

void runAutomaton() {
  const uint32_t now = millis();
  if ((int32_t)(now - nextActionMs) < 0) return;
  switch (state) {
    case SessState::Boot:
    case SessState::Probe:
      if (probeOnce()) { probeAttempts = 0; setState(SessState::Login); break; }
      if (++probeAttempts < 3) { nextActionMs = millis() + 1000; break; }
      probeAttempts = 0;
      if (settings.wake_en) setState(SessState::Wake);
      else goOffline();
      break;
    case SessState::Wake:
      feed();
      if (client.wake() || probeOnce()) setState(SessState::Login);
      else goOffline();
      break;
    case SessState::Offline:
      setState(SessState::Probe);
      break;
    case SessState::Login:
      if (settings.login_en) doCmd("login debug", 1000, false, "auto");
      setInfo([&](SessionInfo& i) { i.fails_in_row = 0; i.offline_rounds = 0; });
      setState(SessState::Ready);
      nextActionMs = millis();
      break;
    case SessState::Ready: {
      nextActionMs = now + settings.tick_s * 1000UL;
      scheduleSlow(now);
      std::string cmd;
      if ((tick & 1) == 0) cmd = "pwr";
      else if (!slowQueue.empty() && (tick & 2)) { cmd = slowQueue.front(); slowQueue.erase(slowQueue.begin()); }
      else {
        uint8_t n = moduleCount();
        batRR = (batRR % n) + 1;
        cmd = "bat " + std::to_string(batRR);
      }
      tick++;
      std::string err;
      CmdReply r = doCmd(cmd, CONSOLE_CMD_TIMEOUT_MS, false, "auto", &err);
      uint8_t fails = 0;
      setInfo([&](SessionInfo& i) {
        if (r.result == CmdResult::Timeout) i.fails_in_row++;
        else i.fails_in_row = 0;
        fails = i.fails_in_row;
      });
      bool pwrBad = false;
      if (cmd == "pwr") {
        pwrBad = r.result != CmdResult::Ok || !err.empty();
        pwrFails = pwrBad ? (uint8_t)(pwrFails + 1) : 0;
      }
      if (fails >= 3 || pwrFails >= 2 * PWR_FAILS_RELOGIN) {
        if (pwrFails >= 2 * PWR_FAILS_RELOGIN) LOGI("console: %u pwr failures in a row, reprobe", pwrFails);
        pwrFails = 0;
        setState(SessState::Probe);
      } else if (pwrBad && (pwrFails == PWR_FAILS_RELOGIN || err.find("no data rows") != std::string::npos)) {
        LOGI("console: pwr failure %u in a row (%s%s%s), relogin", pwrFails, cmdResultName(r.result),
             err.empty() ? "" : ": ", err.c_str());
        setState(SessState::Login);
      }
      break;
    }
  }
}

void handleJob(Job& j) {
  switch (j.type) {
    case JobType::Cmd: {
      std::string err;
      j.reply = doCmd(j.cmd, j.ms, j.save, "manual", &err, false);
      j.out = err;
      j.ok = j.reply.result == CmdResult::Ok;
      break;
    }
    case JobType::Sniff:
      feed();
      j.out = client.sniff(j.ms > 10000 ? 10000 : j.ms);
      j.ok = true;
      break;
    case JobType::Wake:
      feed();
      j.ok = client.wake();
      if (j.ok) setInfo([&](SessionInfo& i) { i.last_ok_ms = millis() | 1; });
      break;
    case JobType::Login:
      j.reply = doCmd("login debug", 1000, j.save, "manual");
      j.ok = j.reply.result != CmdResult::Timeout;
      break;
    case JobType::Uart:
      probeAttempts = 0;
      client.reconfigure(j.tx, j.rx, j.baud);
      setInfo([&](SessionInfo& i) { i.tx = j.tx; i.rx = j.rx; i.baud = j.baud; });
      setState(SessState::Probe);
      nextActionMs = millis();
      j.ok = true;
      break;
    case JobType::Config:
      applyAlarmCfg();
      j.ok = true;
      break;
    case JobType::Reprobe:
      probeAttempts = 0;
      setState(SessState::Probe);
      nextActionMs = millis();
      j.ok = true;
      break;
  }
}

void checkFreshness() {
  const uint32_t now = millis();
  if (now - lastFreshCheckMs < 1000) return;
  lastFreshCheckMs = now;
  const bool on = ConsoleSession::online();
  SnapshotStore::updateIf([&](Snapshot& s) {
    const uint16_t k = timeStateKey(s, now, on);
    const bool changed = k != lastTimeKey;
    lastTimeKey = k;
    if (!changed || !s.has_data) return false;
    s.seq++;
    return true;
  });
}

bool skipIfAbandoned(JobType t) {
  return t == JobType::Cmd || t == JobType::Sniff || t == JobType::Wake || t == JobType::Login;
}

void consoleTask(void*) {
  esp_task_wdt_add(nullptr);
  delay(500);
  for (;;) {
    feed();
    checkFreshness();
    Pending* p = nullptr;
    if (xQueueReceive(jobs, &p, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (p->abandoned && skipIfAbandoned(p->job.type)) LOGI("console: abandoned job skipped");
      else handleJob(p->job);
      xSemaphoreGive(p->done);
      release(p);
      continue;
    }
    if (!paused && settings.autopoll) runAutomaton();
  }
}

}  // namespace

namespace ConsoleSession {

void begin() {
  infoMtx = xSemaphoreCreateMutex();
  jobs = xQueueCreate(2, sizeof(Pending*));
  inf.state = SessState::Boot;
  inf.tx = settings.uart_tx;
  inf.rx = settings.uart_rx;
  inf.baud = settings.uart_baud;
  client.begin(settings.uart_tx, settings.uart_rx, settings.uart_baud);
  applyAlarmCfg();
  xTaskCreate(consoleTask, "consoleTask", 8192, nullptr, 3, nullptr);
}

bool run(Job& job, uint32_t wait_ms) {
  Pending* p = new (std::nothrow) Pending();
  if (!p) return false;
  p->done = xSemaphoreCreateBinary();
  if (!p->done) { delete p; return false; }
  p->job = job;
  const uint32_t t0 = millis();
  if (xQueueSend(jobs, &p, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
    vSemaphoreDelete(p->done);
    delete p;
    LOGI("console: job queue full");
    return false;
  }
  const uint32_t spent = millis() - t0;
  const bool ok = xSemaphoreTake(p->done, pdMS_TO_TICKS(wait_ms > spent ? wait_ms - spent : 0)) == pdTRUE;
  if (ok) job = p->job;
  else {
    p->abandoned = true;
    LOGI("console: job wait timeout (%u ms)", (unsigned)wait_ms);
  }
  release(p);
  return ok;
}

void setPaused(bool p) { paused = p; }

SessionInfo info() {
  SessionInfo c;
  xSemaphoreTake(infoMtx, portMAX_DELAY);
  c = inf;
  xSemaphoreGive(infoMtx);
  c.paused = paused || !settings.autopoll;
  return c;
}

bool online() {
  SessionInfo i = info();
  return i.last_ok_ms && millis() - i.last_ok_ms < CONSOLE_ONLINE_WINDOW_MS;
}

}  // namespace ConsoleSession
