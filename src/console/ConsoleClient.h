#pragma once
#include <Arduino.h>
#include <string>

enum class CmdResult : uint8_t { Ok, Incomplete, Timeout, Rejected };
const char* cmdResultName(CmdResult r);

struct CmdReply {
  CmdResult   result = CmdResult::Timeout;
  std::string text;
  uint32_t    ms = 0;
  bool        prompt = false;
  bool        truncated = false;
};

class ConsoleClient {
 public:
  void begin(uint8_t tx, uint8_t rx, uint32_t baud);
  void reconfigure(uint8_t tx, uint8_t rx, uint32_t baud);

  static bool allowed(const std::string& cmd);

  CmdReply exec(const std::string& cmd, uint32_t timeout_ms = 3000);
  std::string sniff(uint32_t ms);
  bool wake();

  uint8_t tx() const { return tx_; }
  uint8_t rx() const { return rx_; }
  uint32_t baud() const { return baud_; }

 private:
  void flushRx();
  uint8_t tx_ = 0, rx_ = 0;
  uint32_t baud_ = 0;
};

bool endsWithPrompt(const std::string& s);
