#include "util/Settings.h"
#include <Preferences.h>
#include "config.h"

Settings settings;

static const char* NS = "pylonmon";

void Settings::load() {
  Preferences p;
  p.begin(NS, true);
  wifi_ssid = p.getString("wifi_ssid", "");
  wifi_pass = p.getString("wifi_pass", "");
  hostname  = p.getString("hostname", HOSTNAME_DEFAULT);
  uart_tx   = p.getUChar("uart_tx", PIN_CONSOLE_TX_DEFAULT);
  uart_rx   = p.getUChar("uart_rx", PIN_CONSOLE_RX_DEFAULT);
  uart_baud = p.getULong("uart_baud", CONSOLE_BAUD_DEFAULT);
  wake_en   = p.getBool("wake_en", true);
  login_en  = p.getBool("login_en", true);
  autopoll  = p.getBool("autopoll", true);
  tick_s    = p.getUShort("tick_s", TICK_S_DEFAULT);
  delta_warn_mv = p.getUShort("dwarn_mv", DELTA_WARN_MV_DEFAULT);
  delta_crit_mv = p.getUShort("dcrit_mv", DELTA_CRIT_MV_DEFAULT);
  admin_pass = p.getString("admin_pass", "");
  log_s     = p.getUShort("log_s", LOG_PERIOD_S_DEFAULT);
  p.end();
  if (hostname.isEmpty()) hostname = HOSTNAME_DEFAULT;
  if (tick_s < 2) tick_s = 2;
}

void Settings::save() {
  Preferences p;
  p.begin(NS, false);
  p.putString("wifi_ssid", wifi_ssid);
  p.putString("wifi_pass", wifi_pass);
  p.putString("hostname", hostname);
  p.putUChar("uart_tx", uart_tx);
  p.putUChar("uart_rx", uart_rx);
  p.putULong("uart_baud", uart_baud);
  p.putBool("wake_en", wake_en);
  p.putBool("login_en", login_en);
  p.putBool("autopoll", autopoll);
  p.putUShort("tick_s", tick_s);
  p.putUShort("dwarn_mv", delta_warn_mv);
  p.putUShort("dcrit_mv", delta_crit_mv);
  p.putString("admin_pass", admin_pass);
  p.putUShort("log_s", log_s);
  p.end();
}

void Settings::clearWifi() {
  wifi_ssid = "";
  wifi_pass = "";
  save();
}
