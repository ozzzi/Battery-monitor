# Pylontech battery monitor

ESP32-S2 firmware that reads the Console port of a Pylontech US2000C battery and serves the full battery state, including per-cell voltages, temperatures and balancing flags, over a local HTTP JSON API.

## Features

- Polls the battery console (`pwr`, `bat N`, `stat N`, `soh N`, `info N`) on a fixed schedule
- Header-driven parser: column names are mapped to indexes, so it tolerates different BMS firmware formats
- Derived values computed on the device: cell delta, min/max cell, balancing count, alarm flags
- Compact `GET /api/v1/snapshot` (~0.85 KB for 2 modules, with `ETag`) and a full `GET /api/v1/battery`
- Wi-Fi provisioning through a captive portal (`PylonMon-XXXX` access point)
- Discovery via mDNS (`_pylonmon._tcp`) with a UDP broadcast beacon fallback
- OTA update over HTTP (`POST /api/v1/update`)
- Console access restricted to a command whitelist (no write commands)

## Hardware

| Part | Notes |
| --- | --- |
| ESP32-S2 Mini | 4 MB flash, 2 MB PSRAM |
| MAX3232 board with DB9 | RS232 level shifter, power it from **3.3 V** |
| RJ45 patch cord (T568B), 1–2 m
| USB 5 V / 1 A adapter

### Wiring

Battery Console (RJ45) to MAX3232 (RS232 side):

| RJ45 pin | Battery signal | Connect to |
| --- | --- | --- |
| 3 | 232-TX (battery output) | MAX3232 `R1IN` |
| 6 | 232-RX (battery input) | MAX3232 `T1OUT` |
| 8 | GND | MAX3232 GND (DB9 pin 5) |

MAX3232 (TTL side) to ESP32-S2 Mini:

| MAX3232 | ESP32-S2 Mini |
| --- | --- |
| VCC | 3V3 |
| GND | GND |
| TXD / `T1IN` | GPIO17 (UART1 TX) |
| RXD / `R1OUT` | GPIO18 (UART1 RX) |

Notes:

- DB9 pinout differs between MAX3232 boards. Measure it: with the board powered and the ESP disconnected, the DB9 pin 2/3 at about −5…−6 V (relative to pin 5) is `T1OUT`.
- Pins are set in `include/config.h`.

### LED and button

| State | LED (GPIO15) |
| --- | --- |
| Access point mode | fast blink (5 Hz) |
| Connecting to Wi-Fi | slow blink (1 Hz) |
| Running, battery responds | short flash every 3 s |
| Running, battery silent | double flash |

Holding **BOOT** (GPIO0) for more than 5 s erases the Wi-Fi settings and reboots into setup mode.

Three FreeRTOS tasks on a single core:

```
include/config.h                  pins, timings, limits
include/secrets.example.h         template for secrets.h
src/main.cpp                      init order, heap watchdog
src/console/ConsoleClient.*       UART, command whitelist, response capture
src/console/ConsoleSession.*      session state machine, scheduler, job queue
src/console/Parser.*              console output → snapshot
src/model/Snapshot.*              data model, derived values, alarms
src/model/SnapshotStore.*         working + published snapshot, mutex
src/net/WifiFsm.*                 Wi-Fi mode state machine (no Arduino deps)
src/net/Network.*                 netTask
src/api/HttpApi.*                 httpTask, HTML pages
src/util/                         settings (NVS), log ring, counters, captures
scripts/                          OTA upload and capture helpers
test/                             Unity tests and console fixtures
```

### Polling schedule

One console command per tick (default 5 s):

- even tick: `pwr`
- odd tick: `bat N` for the next module, round-robin
- `stat`/`soh` at startup and every 10 min, `info` at startup and every 6 h


## Build and flash

Requires [PlatformIO](https://platformio.org/).

```sh
cp include/secrets.example.h include/secrets.h   # required by every firmware build; set OTA_PASS (WIFI_* is used by probe only)
pio run -e s2mini -t upload                      # first flash over USB
```

Environments:

| Env | Purpose |
| --- | --- |
| `s2mini` | Production firmware |
| `probe` | Diagnostic build: `/dbg/*` endpoints, console capture logger to flash, Wi-Fi from `secrets.h` |
| `probe_ota` | `probe` uploaded via ArduinoOTA |
| `native` | Host unit tests |

Later updates over Wi-Fi:

```sh
scripts/http_update.sh s2mini
```

The script builds the firmware and uploads it to `POST /api/v1/update` using Basic auth (`admin` / `OTA_PASS`). Set `PYLONMON_HOST` if the device is not at `battery.local`.

## First setup

1. Power the device. The LED blinks fast.
2. Connect a phone to the open network `PylonMon-XXXX`.
3. The setup page opens by itself, otherwise browse to `http://192.168.4.1`.
4. Pick your Wi-Fi network, enter the password, optionally change the device name, and save.
5. The device verifies the connection, then shuts down the access point.

The device is then reachable at `http://battery.local/` (the name is configurable). The settings page lets you change polling interval, cell delta thresholds, hostname and admin password.

## HTTP API

| Endpoint | Purpose |
| --- | --- |
| `GET /api/v1/snapshot` | Compact snapshot for clients, supports `ETag` / `304` |
| `GET /api/v1/battery` | Full snapshot with named fields |
| `GET /api/v1/status` | Device status: Wi-Fi, console, counters, uptime, heap |
| `GET /api/v1/console?cmd=…` | Raw output of a whitelisted console command (admin) |
| `GET`/`POST /api/v1/settings` | Read / change settings (POST needs admin password if set) |
| `POST /api/v1/reboot`, `POST /api/v1/wifi/reset` | Maintenance (admin) |
| `GET /api/v1/log` | Device log ring buffer (admin) |
| `POST /api/v1/update` | Firmware upload (Basic `admin` / `OTA_PASS`, `Origin` header required) |

"Admin" endpoints use HTTP Basic auth (user `admin`) and are open until an admin password is set in the settings.

```sh
curl http://battery.local/api/v1/snapshot
```

### Discovery

- mDNS: hostname `battery.local`, services `_pylonmon._tcp` and `_http._tcp` on port 80, TXT records `id`, `api`, `path`, `fw`, `mods`
- UDP broadcast every 15 s to `255.255.255.255:47801`:

```json
{"svc":"pylonmon","id":"A1B2C3D4E5F6","api":1,"port":80,"path":"/api/v1","fw":"0.3.1","seq":12345}
```


