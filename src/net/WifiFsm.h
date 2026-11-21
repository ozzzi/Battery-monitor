#pragma once
#include <cstdint>

namespace wififsm {

enum class Mode : uint8_t { ApSetup, StaConnecting, StaOnline, ApRescue };
const char* modeName(Mode m);   // "AP_SETUP" ...

uint32_t backoffMs(uint8_t attempt);

struct Config {
  uint32_t rescue_after_ms = 10UL * 60 * 1000;
  uint32_t ap_off_after_ms = 2UL * 60 * 1000;
};

struct Actions {
  bool sta_begin = false;
  bool ap_on = false;
  bool ap_off = false;
};

class Fsm {
 public:
  explicit Fsm(const Config& c = Config()) : cfg_(c) {}

  Actions start(bool has_creds, uint32_t now);
  Actions step(bool sta_up, bool trial, uint32_t now);
  void credsSaved(uint32_t now, uint32_t ap_off_ms);

  Mode mode() const { return mode_; }
  bool apOn() const { return ap_; }
  bool hasCreds() const { return creds_; }
  uint8_t attempt() const { return attempt_; }

 private:
  Config cfg_;
  Mode mode_ = Mode::StaConnecting;
  bool creds_ = false;
  bool ap_ = false;
  bool ap_off_pending_ = false;
  uint32_t ap_off_at_ = 0;
  uint32_t lost_since_ = 0;
  uint32_t next_try_ = 0;
  uint8_t attempt_ = 0;
};

enum class Trial : uint8_t { None, Testing, Ok, BadPass, NoSsid, Failed };
const char* trialName(Trial t);   // "none", "testing", "ok", "bad_pass", "no_ssid", "failed"
Trial classifyDisconnect(uint8_t reason);

bool validHostname(const char* s);

}  // namespace wififsm
