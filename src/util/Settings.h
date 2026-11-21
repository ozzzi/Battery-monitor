#pragma once
#include <Arduino.h>

struct Settings {
  String   wifi_ssid, wifi_pass;
  String   hostname;
  uint8_t  uart_tx, uart_rx;
  uint32_t uart_baud;
  bool     wake_en;
  bool     login_en;
  bool     autopoll;
  uint16_t tick_s;
  uint16_t delta_warn_mv, delta_crit_mv;
  String   admin_pass;
  uint16_t log_s;

  void load();
  void save();
  void clearWifi();
};

extern Settings settings;
