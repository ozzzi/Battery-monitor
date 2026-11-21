#include "net/WifiFsm.h"

namespace wififsm {

namespace {
bool reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }
}  // namespace

const char* modeName(Mode m) {
  switch (m) {
    case Mode::ApSetup: return "AP_SETUP";
    case Mode::StaConnecting: return "STA_CONNECTING";
    case Mode::StaOnline: return "STA_ONLINE";
    case Mode::ApRescue: return "AP_RESCUE";
  }
  return "?";
}

uint32_t backoffMs(uint8_t attempt) {
  static const uint32_t steps[] = {5000, 10000, 30000, 60000};
  return steps[attempt < 3 ? attempt : 3];
}

Actions Fsm::start(bool has_creds, uint32_t now) {
  Actions a;
  creds_ = has_creds;
  attempt_ = 0;
  lost_since_ = now;
  ap_off_pending_ = false;
  if (!has_creds) {
    mode_ = Mode::ApSetup;
    ap_ = true;
    a.ap_on = true;
    return a;
  }
  mode_ = Mode::StaConnecting;
  a.sta_begin = true;
  next_try_ = now + backoffMs(attempt_++);
  return a;
}

Actions Fsm::step(bool sta_up, bool trial, uint32_t now) {
  Actions a;
  if (sta_up) {
    if (mode_ != Mode::StaOnline) {
      mode_ = Mode::StaOnline;
      attempt_ = 0;
      if (ap_ && !ap_off_pending_) {
        ap_off_pending_ = true;
        ap_off_at_ = now + cfg_.ap_off_after_ms;
      }
    }
    if (ap_ && ap_off_pending_ && reached(now, ap_off_at_)) {
      ap_ = false;
      ap_off_pending_ = false;
      a.ap_off = true;
    }
    return a;
  }

  if (mode_ == Mode::StaOnline) {
    lost_since_ = now;
    next_try_ = now;
    attempt_ = 0;
    ap_off_pending_ = false;
    mode_ = ap_ ? Mode::ApRescue : Mode::StaConnecting;
  }

  if (!creds_) {
    mode_ = Mode::ApSetup;
    if (!ap_) {
      ap_ = true;
      a.ap_on = true;
    }
    return a;
  }

  if (!trial && reached(now, next_try_)) {
    a.sta_begin = true;
    next_try_ = now + backoffMs(attempt_);
    if (attempt_ < 255) attempt_++;
  }
  if (!ap_ && (int32_t)(now - lost_since_) >= (int32_t)cfg_.rescue_after_ms) {
    ap_ = true;
    a.ap_on = true;
  }
  mode_ = ap_ ? Mode::ApRescue : Mode::StaConnecting;
  return a;
}

void Fsm::credsSaved(uint32_t now, uint32_t ap_off_ms) {
  creds_ = true;
  if (ap_) {
    ap_off_pending_ = true;
    ap_off_at_ = now + ap_off_ms;
  }
}

const char* trialName(Trial t) {
  switch (t) {
    case Trial::None: return "none";
    case Trial::Testing: return "testing";
    case Trial::Ok: return "ok";
    case Trial::BadPass: return "bad_pass";
    case Trial::NoSsid: return "no_ssid";
    case Trial::Failed: return "failed";
  }
  return "?";
}

Trial classifyDisconnect(uint8_t reason) {
  switch (reason) {
    case 8:
      return Trial::None;
    case 201:   // NO_AP_FOUND
    case 210:   // NO_AP_FOUND_W_COMPATIBLE_SECURITY
    case 211:   // NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
    case 212:   // NO_AP_FOUND_IN_RSSI_THRESHOLD
      return Trial::NoSsid;
    case 14:    // MIC_FAILURE
    case 15:
    case 202:   // AUTH_FAIL (WPA3/SAE)
    case 204:   // HANDSHAKE_TIMEOUT
      return Trial::BadPass;
    default:
      return Trial::Failed;
  }
}

bool validHostname(const char* s) {
  if (!s || !*s) return false;
  int n = 0;
  for (const char* p = s; *p; ++p, ++n) {
    char c = *p;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
    if (!ok || n >= 31) return false;
  }
  return s[0] != '-' && s[n - 1] != '-';
}

}  // namespace wififsm
