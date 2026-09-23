# Range Hood — Smart Kitchen Fan Controller

A DIY smart range hood / kitchen fan controller built on an ESP8266 NodeMCU V3.
Integrates with **Apple HomeKit** natively (no hub, no cloud), measures temperature and humidity with an **SHT31-D** sensor, displays live status on an **OLED screen**, controls a **relay**, and supports a **capacitive touch sensor** for hands-free manual control.

---

## Features

- **Apple HomeKit** — control and automate from the iOS Home app, Siri, and Shortcuts
- **Auto fan logic** — turns the fan on/off based on humidity rise/fall and temperature spikes relative to a learned ambient baseline
- **Two-phase baseline tracking** — faster ambient re-acquisition after cooking, slower tracking during steady state
- **Manual override** — a touch, a HomeKit toggle that changes the fan state, or an HTTP `/on`/`/off` sets a 30-minute manual override window; double-touch within 2 seconds cancels override and returns to auto mode
- **Safety timeout** — fan is forced OFF after 3 hours of continuous operation to protect against stuck sensor readings
- **Sensor failure fallback** — if the sensor stays down for 35 minutes while the fan is ON, the fan is turned OFF; if it is OFF, it stays OFF
- **I2C bus recovery** — automatic clock-pulse recovery if the I2C bus hangs (e.g. from relay switching noise)
- **Baseline persistence** — ambient baselines are saved to flash (LittleFS) every 15 minutes and restored on boot if they look sane
- **OLED status display** — live temperature, humidity, baselines, fan state, and manual override indicator with burn-in mitigation (auto-dim after 60s, display off after 5 min when fan is OFF)
- **WiFiManager** — first-boot captive-portal setup; no hardcoded credentials
- **OTA updates** — flash over WiFi with `./flash-release.sh` (USB only for the very first flash)
- **Telnet log console** — live logs and a heartbeat on port 23 (`./log-rangehood.sh`)
- **HTTP API** — status and on/off on port 8080 for Home Assistant, alongside HomeKit

---

## Hardware

| Component                | Description                                                                |
| ------------------------ | -------------------------------------------------------------------------- |
| **MCU**                  | NodeMCU V3 — Wireless Module CH340/CP2102, ESP8266 ESP-12E                 |
| **Humidity/Temp sensor** | Adafruit SHT31-D (I2C, address 0x44)                                       |
| **Display**              | TENSTAR 0.96" I2C OLED, SSD1315 driver, 128x64, 4-pin IIC                  |
| **Relay**                | 1-Channel 5V Relay Module with Optocoupler, High/Low Level Trigger         |
| **Touch sensor**         | TTP223B Digital Capacitive Touch Module (active HIGH, with filter circuit) |

---

## Wiring

### I2C Bus (shared by OLED + SHT31-D)

| Signal | NodeMCU Pin | GPIO  |
| ------ | ----------- | ----- |
| SDA    | D2          | GPIO4 |
| SCL    | D1          | GPIO5 |

> Both the OLED (address `0x3C`) and the SHT31-D (address `0x44`) share the same I2C bus.

### Relay

| Relay pin | NodeMCU pin | Note                                              |
| --------- | ----------- | ------------------------------------------------- |
| IN        | D6 (GPIO12) | LOW = fan ON, HIGH = fan OFF (active-low trigger) |
| VCC       | 5V (VIN)    | The relay module needs 5V coil voltage            |
| GND       | GND         | Common ground                                     |

> The relay IN signal is driven LOW to energize the coil (active-low). The load (fan) is wired to the relay's **NO** (Normally Open) contacts.

### TTP223B Capacitive Touch Sensor

| Touch module pin | NodeMCU pin | Note                                            |
| ---------------- | ----------- | ----------------------------------------------- |
| VCC              | 3.3V or 5V  | Module works with both                          |
| GND              | GND         |                                                 |
| I/O              | D5 (GPIO14) | Active HIGH — goes HIGH when finger is detected |

> The TTP223B defaults to **momentary, active-HIGH** output. No soldering pads need to be bridged for basic operation. If you want latching (toggle) mode, bridge the **A** pad on the module.

### Full Pin Summary

```text
NodeMCU D1  →  OLED SCL / SHT31 SCL
NodeMCU D2  →  OLED SDA / SHT31 SDA
NodeMCU D5  →  TTP223B I/O
NodeMCU D6  →  Relay IN
NodeMCU 3V3 →  OLED VCC, SHT31 VIN, TTP223B VCC
NodeMCU VIN →  Relay VCC (5V from USB)
NodeMCU GND →  All GND
```

---

## Software Dependencies (Arduino Libraries)

Install via the Arduino Library Manager or Board Manager:

| Library                   | Purpose                          |
| ------------------------- | -------------------------------- |
| `esp8266` board package   | Core ESP8266 support             |
| `arduino-homekit-esp8266` | Native HomeKit server stack      |
| `Adafruit SHT31 Library`  | SHT31-D sensor driver            |
| `Adafruit GFX Library`    | OLED graphics primitives         |
| `Adafruit SSD1306`        | OLED driver                      |
| `Adafruit BusIO`          | I2C/SPI abstraction (dependency) |
| `WiFiManager`             | First-boot WiFi captive portal   |

> **Board**: `NodeMCU 1.0 (ESP-12E Module)` (`esp8266:esp8266:nodemcuv2`) — 160 MHz CPU, 4 MB flash with a 2 MB filesystem (`eesz=4M2M`).

### Required library patch

`arduino-homekit-esp8266` v1.2.0 — the latest release; the project is unmaintained — eventually wipes its own HomeKit pairing storage. After enough pairing operations the accessory comes back from a reboot with a new ID, unpaired, and Apple Home shows "No Response" for good. Apply the 2-line fix once after installing the library (it matches upstream esp-homekit). Run it from the sketch folder:

```sh
git -C ../libraries/Arduino-HomeKit-ESP8266 apply "$PWD/docs/patches/homekit-storage-compact.patch"
```

`flash-release.sh` refuses to build against an unpatched library. The patch file's header explains the bug.

---

## First-Time Setup

1. Apply the library patch (above), then flash the firmware once over USB (CH340 or CP2102 driver required on macOS/Windows). On any later USB flash use **Erase Flash: Only Sketch**, or the HomeKit pairing is wiped.
2. On first boot the device creates a WiFi access point called **`RangeHood-Setup`**.
3. Connect to it from your phone and enter your home WiFi credentials through the captive portal (180 seconds timeout).
4. The device reboots and connects to your WiFi automatically from then on.
5. Open the **iOS Home app → Add Accessory → More options** and scan for "Range Hood".
   Enter the pairing code: **`281-42-814`**

> To reset HomeKit pairing, uncomment `homekit_storage_reset()` in `setup()`, flash once, then comment it out again and flash a second time.

---

## Updates, Logs and HTTP API

- **Flash over WiFi:** `./flash-release.sh [ip]` (default `192.168.2.151`) builds the sketch and pushes it with `espota.py`. ArduinoOTA runs without mDNS, because HomeKit owns the single mDNS responder, so upload is by IP. OTA rewrites only the sketch, so the HomeKit pairing survives. Bump `FW_VERSION` in `air_hood.ino` so the console banner shows the new build.
- **Logs:** `./log-rangehood.sh [ip]` streams the telnet console (port 23) to `rangehood.log`, including a heartbeat with free heap, HomeKit clients and RSSI every 5 s.
- **HTTP API** (port 8080, for Home Assistant):
  - `GET /status` → `{"on":bool,"temp":float|null,"hum":float|null,"manual":bool,"rssi":int}`
  - `POST /on`, `POST /off` → `OK`. A request that changes the state also starts the 30-minute manual override.

---

## How the Auto Fan Logic Works

The firmware adapts to your environment using a **rolling ambient baseline** learned while the fan is OFF.

### Humidity trigger

| Event             | Condition                                                                   |
| ----------------- | --------------------------------------------------------------------------- |
| Fan turns **ON**  | Humidity >= max(55%, baseline + 8%) AND fan has been OFF for at least 2 min |
| Fan turns **OFF** | Humidity <= baseline + 3% — see _Auto-off_ below                            |

### Temperature trigger (stove/cooking detection)

| Event             | Condition                                                                          |
| ----------------- | ---------------------------------------------------------------------------------- |
| Fan turns **ON**  | Temperature >= max(27C, baseline + 3C), **or** a sudden +1C rise in one 30s sample |
| Fan turns **OFF** | Temperature <= baseline + 2C — see _Auto-off_ below                                |

### Auto-off

The fan turns OFF only when **both** humidity and temperature are below their OFF thresholds **continuously for 10 minutes** (`AUTO_OFF_OVERRUN_MS`), and it has run for at least 5 minutes. Any spike back above a threshold restarts the 10-minute countdown. The countdown exists because the fan extracts the steam and heat itself, so the sensor can briefly read "normal" while you are still cooking. Continued cooking keeps re-arming it, and the fan only shuts off once cooking has really stopped.

### Baseline learning

The device tracks ambient humidity and temperature using exponential smoothing **only while the fan is OFF** (so cooking air doesn't corrupt the baseline). Normally the baseline rises more slowly than it falls, so short spikes are ignored. Two-phase tracking (below) reverses this for the first 5 minutes after the fan turns off.

**Two-phase tracking:** For the first 5 minutes after the fan turns OFF, faster smoothing alphas are used to quickly re-acquire the true ambient after a long cooking session. After 5 minutes, the system switches to the normal slow alphas for stability.

**Persistence:** Baselines are saved to flash (LittleFS) every 15 minutes while the fan is OFF. On boot, saved baselines are loaded if they fall into sane sensor ranges. There is no age check: `millis()` restarts at every boot, so it can't tell how old the file is.

### Manual override

A touch tap, a HomeKit toggle that changes the fan state, or an HTTP `POST /on` / `POST /off` that changes it sets a **30-minute manual override**. During this window auto-logic is paused and the OLED shows `MAN` next to the WiFi icon. After 30 minutes, auto-control resumes.

**Double-touch:** Tapping the touch sensor twice within 2 seconds cancels the override immediately and returns to auto mode.

### Safety timeout

If the fan has been running continuously for **3 hours**, it is forced OFF regardless of sensor readings or manual override. This protects against stuck sensor readings or moisture ingress into the sensor housing.

### Sensor failure fallback

If the SHT31-D sensor stops responding:

- If the fan is **ON**: it keeps running for up to **35 minutes**, then turns OFF with reason "sensor timeout"
- If the fan is **OFF**: it stays OFF (conservative)

Before each sensor retry, an **I2C bus recovery** sequence (9 clock pulses) is performed to unstick a potentially hung I2C bus — a common failure mode near electrically noisy relay coils.

---

## OLED Display Layout

```text
 [FAN]  ON    MAN  WiFi  ← fan icon + state + override indicator + WiFi signal
 Temperature:    22 °C
 Humidity:       48 %
 Base T: 21 °C  H: 46 %  ← learned ambient baselines
```

When the sensor is unreachable, the display shows `Sensor fail`.

### Burn-in mitigation

When the fan is OFF (idle state):

- After **60 seconds** of inactivity the display dims to minimum contrast
- After **5 minutes** of inactivity the display turns off entirely
- Any touch or fan state change wakes the display immediately

When the fan is ON, the display stays active so you can monitor conditions while cooking.

Burn-in is mitigated by the auto-dim and auto-off timers above (there is no pixel-shifting).

---

## Configuration Constants (air*hood.ino; `DISPLAY*\*` in display.h)

| Constant                      | Default   | Description                                               |
| ----------------------------- | --------- | --------------------------------------------------------- |
| `SENSOR_READ_INTERVAL_MS`     | 30 000 ms | How often sensor is polled                                |
| `AUTO_MIN_ON_MS`              | 5 min     | Minimum fan-ON time before auto-off                       |
| `AUTO_MIN_OFF_MS`             | 2 min     | Minimum fan-OFF time before auto-on                       |
| `AUTO_OFF_OVERRUN_MS`         | 10 min    | Air must stay calm this long CONTINUOUSLY before auto-off |
| `MANUAL_OVERRIDE_MS`          | 30 min    | How long a manual action blocks auto-logic                |
| `SAFETY_MAX_ON_MS`            | 3 hours   | Maximum continuous fan-ON before forced OFF               |
| `BASELINE_SAVE_INTERVAL_MS`   | 15 min    | How often baselines are saved to flash                    |
| `SENSOR_FAIL_FAN_OFF_MS`      | 35 min    | Sensor down this long with the fan ON → fan turns OFF  |
| `BASELINE_FAST_PHASE_MS`      | 5 min     | Duration of fast baseline tracking after fan turns OFF    |
| `HUMIDITY_ABS_ON_MIN`         | 55.0%     | Absolute humidity floor to trigger fan                    |
| `HUMIDITY_DELTA_ON`           | 8.0%      | Rise above baseline to trigger fan ON                     |
| `HUMIDITY_DELTA_OFF`          | 3.0%      | Rise above baseline below which fan turns OFF             |
| `TEMP_ABS_ON_MIN`             | 27.0C     | Absolute temperature floor to trigger fan                 |
| `TEMP_DELTA_ON`               | 3.0C      | Rise above baseline to trigger fan ON                     |
| `TEMP_DELTA_OFF`              | 2.0C      | Rise above baseline below which fan turns OFF             |
| `TEMP_RISE_ON_DELTA`          | 1.0C      | Sudden per-sample rise to trigger fan ON immediately      |
| `DISPLAY_DIM_MS`              | 60 s      | Inactivity before display dims (fan OFF only)             |
| `DISPLAY_OFF_MS`              | 5 min     | Inactivity before display turns off (fan OFF only)        |

---

## Project Structure

```text
air_hood/
├── air_hood.ino       — Main sketch: setup, loop, sensor + fan logic, HomeKit glue, HTTP API, OTA
├── display.cpp/.h     — OLED rendering (Adafruit SSD1306) with burn-in mitigation
├── RemoteLog.cpp/.h   — Telnet log console on port 23
├── my_accessory.c     — HomeKit accessory definition (Fan + Temp + Humidity services)
├── wifi_info.h        — WiFiManager connection helper
├── flash-release.sh   — Build and flash over WiFi (espota)
├── log-rangehood.sh   — Capture the telnet log to rangehood.log
├── docs/patches/      — Required patch for the HomeKit library
└── README.md          — This file
```

---

## License

MIT — free to use, modify, and share.
