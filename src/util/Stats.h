#pragma once
#include <atomic>
#include <cstdint>

struct Stats {
  std::atomic<uint32_t> cmd_ok{0}, cmd_timeout{0}, cmd_incomplete{0}, cmd_rejected{0};
  std::atomic<uint32_t> parse_errors{0}, rx_noise{0}, wake_count{0}, wifi_reconnects{0};
  std::atomic<uint32_t> http_requests{0}, captures_saved{0}, capture_errors{0};
};

extern Stats stats;
