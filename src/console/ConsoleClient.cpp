#include "console/ConsoleClient.h"
#include "config.h"
#include "util/Log.h"
#include "util/Stats.h"

static HardwareSerial& port = Serial1;
static const char* PRESS_ENTER = "Press [Enter] to be continued";

const char* cmdResultName(CmdResult r) {
  switch (r) {
    case CmdResult::Ok: return "ok";
    case CmdResult::Incomplete: return "incomplete";
    case CmdResult::Timeout: return "timeout";
    case CmdResult::Rejected: return "rejected";
  }
  return "?";
}

bool endsWithPrompt(const std::string& s) {
  size_t e = s.find_last_not_of(" \r\n\t");
  if (e == std::string::npos) return false;
  std::string_view v(s.data(), e + 1);
  auto ends = [&](const char* p) {
    size_t n = strlen(p);
    return v.size() >= n && v.compare(v.size() - n, n, p) == 0;
  };
  return ends("pylon>") || ends("pylon_debug>");
}

void ConsoleClient::begin(uint8_t tx, uint8_t rx, uint32_t baud) {
  tx_ = tx; rx_ = rx; baud_ = baud;
  port.setRxBufferSize(CONSOLE_UART_RX_BUF);
  port.begin(baud, SERIAL_8N1, rx, tx);
  LOGI("console: UART1 tx=%u rx=%u baud=%lu", tx, rx, (unsigned long)baud);
}

void ConsoleClient::reconfigure(uint8_t tx, uint8_t rx, uint32_t baud) {
  port.end();
  delay(20);
  begin(tx, rx, baud);
}

bool ConsoleClient::allowed(const std::string& cmd) {
  if (cmd.empty() || cmd == "pwr" || cmd == "help" || cmd == "login debug") return true;
  static const char* withN[] = {"pwr", "bat", "stat", "soh", "info"};
  for (const char* w : withN) {
    size_t n = strlen(w);
    if (cmd.size() == n + 2 && cmd.compare(0, n, w) == 0 && cmd[n] == ' ' && cmd[n + 1] >= '1' && cmd[n + 1] <= '8')
      return true;
  }
  return false;
}

void ConsoleClient::flushRx() {
  while (port.available()) port.read();
}

CmdReply ConsoleClient::exec(const std::string& cmd, uint32_t timeout_ms) {
  CmdReply r;
  if (!allowed(cmd)) {
    r.result = CmdResult::Rejected;
    stats.cmd_rejected++;
    LOGI("console: rejected '%s'", cmd.c_str());
    return r;
  }
  r.text.reserve(cmd.empty() ? 256 : 4096);
  flushRx();
  port.write((const uint8_t*)cmd.data(), cmd.size());
  port.write("\r\n");
  port.flush();

  const uint32_t t0 = millis();
  uint32_t lastRx = 0;
  size_t scanFrom = 0;
  bool dollars = false;
  uint8_t buf[256];
  for (;;) {
    int avail = port.available();
    if (avail > 0) {
      int n = port.read(buf, avail > (int)sizeof(buf) ? sizeof(buf) : avail);
      if (r.text.size() + n > CONSOLE_MAX_RESPONSE) {
        n = CONSOLE_MAX_RESPONSE - r.text.size();
        r.truncated = true;
      }
      r.text.append((const char*)buf, n);
      lastRx = millis();
      size_t p = r.text.find(PRESS_ENTER, scanFrom > 40 ? scanFrom - 40 : 0);
      if (p != std::string::npos) {
        r.text.erase(p, strlen(PRESS_ENTER));
        port.write("\r");
      }
      scanFrom = r.text.size();
      if (!dollars && r.text.find("$$") != std::string::npos) dollars = true;
      if (endsWithPrompt(r.text)) { r.prompt = true; break; }
      if (r.truncated) break;
      continue;
    }
    uint32_t now = millis();
    if (lastRx) {
      if (dollars && now - lastRx >= CONSOLE_TAIL_MS) break;
      if (now - lastRx >= CONSOLE_SILENCE_MS) break;
    }
    if (now - t0 >= timeout_ms) break;
    delay(5);
  }
  r.ms = millis() - t0;
  if (r.text.empty()) {
    r.result = CmdResult::Timeout;
    stats.cmd_timeout++;
  } else if (cmd.empty() ? r.prompt : (r.text.find("Command completed successfully") != std::string::npos)) {
    r.result = CmdResult::Ok;
    stats.cmd_ok++;
  } else {
    r.result = CmdResult::Incomplete;
    stats.cmd_incomplete++;
  }
  return r;
}

std::string ConsoleClient::sniff(uint32_t ms) {
  std::string out;
  uint32_t t0 = millis();
  uint8_t buf[128];
  while (millis() - t0 < ms && out.size() < CONSOLE_MAX_RESPONSE) {
    int a = port.available();
    if (a > 0) {
      int n = port.read(buf, a > (int)sizeof(buf) ? sizeof(buf) : a);
      out.append((const char*)buf, n);
    } else {
      delay(5);
    }
  }
  return out;
}

bool ConsoleClient::wake() {
  stats.wake_count++;
  LOGI("console: wake sequence @%d baud", CONSOLE_WAKE_BAUD);
  port.flush();
  port.updateBaudRate(CONSOLE_WAKE_BAUD);
  port.write(CONSOLE_WAKE_SEQ);
  port.flush();
  delay(1000);
  port.updateBaudRate(baud_);
  delay(50);
  CmdReply r = exec("", 1500);
  LOGI("console: wake -> %s", r.prompt ? "prompt" : "no prompt");
  return r.prompt;
}
