#pragma once

#ifndef PYLONMON_FW_VERSION
#define PYLONMON_FW_VERSION "0.0.0"
#endif
#ifndef PYLONMON_PROBE
#define PYLONMON_PROBE 0
#endif
#ifndef PYLONMON_API_VERSION
#define PYLONMON_API_VERSION 1
#endif

#define PIN_CONSOLE_TX_DEFAULT 17   // UART1 TX -> MAX3232 T1IN
#define PIN_CONSOLE_RX_DEFAULT 18   // UART1 RX <- MAX3232 R1OUT
#define PIN_LED                15
#define PIN_BOOT_BTN           0

#define CONSOLE_BAUD_DEFAULT   115200
#define CONSOLE_UART_RX_BUF    4096
#define CONSOLE_MAX_RESPONSE   (16 * 1024)
#define CONSOLE_CMD_TIMEOUT_MS 3000
#define CONSOLE_SILENCE_MS     1500
#define CONSOLE_TAIL_MS        300
#define CONSOLE_INTER_CMD_MS   200
#define CONSOLE_WAKE_BAUD      1200
#define CONSOLE_WAKE_SEQ       "~20014682C0048520FCC3\r"
#define CONSOLE_ONLINE_WINDOW_MS 60000       // battery.online
#define CONSOLE_MANUAL_MAX_MS  5000
#define CONSOLE_RUN_WAIT_MS    15000
#define PWR_FAILS_RELOGIN      3

#define TICK_S_DEFAULT         5
#define LOG_PERIOD_S_DEFAULT   600
#define STAT_PERIOD_S          600    // stat N, soh N
#define INFO_PERIOD_S          (6 * 3600)
#define DELTA_WARN_MV_DEFAULT  30
#define DELTA_CRIT_MV_DEFAULT  50
#define BEACON_PORT            47801
#define BEACON_PERIOD_MS       15000
#define CAPTURE_DIR            "/cap"
#define CAPTURE_FS_LIMIT       (1100 * 1024)

#define HOSTNAME_DEFAULT       "battery"
#if PYLONMON_PROBE
#define WIFI_RESCUE_AP_AFTER_MS (2UL * 60 * 1000)
#else
#define WIFI_RESCUE_AP_AFTER_MS (10UL * 60 * 1000)
#endif
#define WIFI_RESCUE_AP_OFF_MS  (2UL * 60 * 1000)
#define WIFI_PORTAL_AP_OFF_MS  10000
#define WIFI_TRIAL_TIMEOUT_MS  20000
#define NTP_TZ                 "EET-2EEST,M3.5.0/3,M10.5.0/4"
#define NTP_SERVER1            "pool.ntp.org"
#define NTP_SERVER2            "time.google.com"
#define HTTP_PORT              80
#define OTA_PORT               3232

#define TWDT_TIMEOUT_S         30
#define HEAP_MIN_REBOOT        (16 * 1024)
#define LOG_RING_SIZE          (16 * 1024)
