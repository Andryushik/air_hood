# Air Hood — Smart Kitchen Fan Controller

A DIY smart air hood / kitchen fan controller built on an ESP8266 NodeMCU V3.
Integrates with **Apple HomeKit** natively (no hub, no cloud), reads air quality via a **SHT31-D** temperature/humidity sensor, displays live status on an **OLED screen**, controls a **relay**, and supports a **capacitive touch sensor** for hands-free manual control.

---

## Features

- **Apple HomeKit** — control and automate from the iOS Home app, Siri, and Shortcuts
- **Auto fan logic** — turns the fan on/off based on humidity rise/fall and temperature spikes relative to a learned ambient baseline
- **Two-phase baseline tracking** — faster ambient re-acquisition after cooking, slower tracking during steady state
- **Manual override** — touch sensor or HomeKit toggle sets a 30-minute manual override window; double-touch within 2 seconds cancels override and returns to auto mode
- **Safety timeout** — fan is forced OFF after 3 hours of continuous operation to protect against stuck sensor readings
- **Sensor failure fallback** — if the sensor fails for more than 5 minutes, the fan (if ON) is given a 30-minute grace period then turned OFF; if OFF, it stays OFF
- **I2C bus recovery** — automatic clock-pulse recovery if the I2C bus hangs (e.g. from relay switching noise)
- **Baseline persistence** — ambient baselines are saved to flash (LittleFS) every 15 minutes and restored on boot (up to 4 hours old), preventing corrupted baselines after a reboot during cooking
- **OLED status display** — live temperature, humidity, baselines, fan state, and manual override indicator with burn-in mitigation (auto-dim after 60s, display off after 5 min when fan is OFF, pixel shifting)
- **WiFiManager** — first-boot captive-portal setup; no hardcoded credentials
- **OTA-free simplicity** — short flash cycle over USB

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

```
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

> **Board**: `NodeMCU 1.0 (ESP-12E Module)` — 160 MHz CPU, 4MB Flash, `Generic ESP8266` build.

---

## First-Time Setup

1. Flash the firmware via Arduino IDE over USB (CH340 or CP2102 driver required on macOS/Windows).
2. On first boot the device creates a WiFi access point called **`AirHood-Setup`**.
3. Connect to it from your phone and enter your home WiFi credentials through the captive portal (180 seconds timeout).
4. The device reboots and connects to your WiFi automatically from then on.
5. Open the **iOS Home app → Add Accessory → More options** and scan for "Air Hood".
   Enter the pairing code: **`281-42-814`**

> To reset HomeKit pairing without reflashing, uncomment `homekit_storage_reset()` in `setup()`, flash once, then comment it out again and reflash.

---

## How the Auto Fan Logic Works

The firmware adapts to your environment using a **rolling ambient baseline** learned while the fan is OFF.

### Humidity trigger

| Event             | Condition                                                                   |
| ----------------- | --------------------------------------------------------------------------- |
| Fan turns **ON**  | Humidity >= max(55%, baseline + 8%) AND fan has been OFF for at least 2 min |
| Fan turns **OFF** | Humidity <= baseline + 3% AND fan has been ON for at least 5 min            |

### Temperature trigger (stove/cooking detection)

| Event             | Condition                                                                             |
| ----------------- | ------------------------------------------------------------------------------------- |
| Fan turns **ON**  | Temperature >= max(27C, baseline + 3C), **or** a sudden +1C rise in one 30s sample    |
| Fan turns **OFF** | Temperature <= baseline + 1.5C AND fan has been ON for at least 5 min                 |

### Baseline learning

The device tracks ambient humidity and temperature using exponential smoothing **only while the fan is OFF** (so cooking air doesn't corrupt the baseline). Rise is slower than fall to ignore short spikes.

**Two-phase tracking:** For the first 5 minutes after the fan turns OFF, faster smoothing alphas are used to quickly re-acquire the true ambient after a long cooking session. After 5 minutes, the system switches to the normal slow alphas for stability.

**Persistence:** Baselines are saved to flash (LittleFS) every 15 minutes while the fan is OFF. On boot, saved baselines are loaded if they are less than 4 hours old, preventing corrupted baselines when the device reboots during cooking.

### Manual override

Any manual action (touch tap or HomeKit toggle) sets a **30-minute manual override**. During this window, auto-logic is paused. The OLED shows `MAN` in the top-right corner. After 30 minutes, auto-control resumes.

**Double-touch:** Tapping the touch sensor twice within 2 seconds cancels the override immediately and returns to auto mode.

### Safety timeout

If the fan has been running continuously for **3 hours**, it is forced OFF regardless of sensor readings or manual override. This protects against stuck sensor readings or moisture ingress into the sensor housing.

### Sensor failure fallback

If the SHT31-D sensor fails to respond for more than **5 minutes**:

- If the fan is **ON**: it stays ON for a 30-minute grace period, then turns OFF with reason "sensor timeout"
- If the fan is **OFF**: it stays OFF (conservative)

Before each sensor retry, an **I2C bus recovery** sequence (9 clock pulses) is performed to unstick a potentially hung I2C bus — a common failure mode near electrically noisy relay coils.

---

## OLED Display Layout

```
 [FAN]  ON         MAN   ← fan icon + state + override indicator
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

All display content is shifted by 1-2 pixels each refresh cycle to distribute pixel wear evenly.

---

## Configuration Constants (air_hood.ino)

| Constant                      | Default   | Description                                           |
| ----------------------------- | --------- | ----------------------------------------------------- |
| `SENSOR_READ_INTERVAL_MS`     | 30 000 ms | How often sensor is polled                            |
| `AUTO_MIN_ON_MS`              | 5 min     | Minimum fan-ON time before auto-off                   |
| `AUTO_MIN_OFF_MS`             | 2 min     | Minimum fan-OFF time before auto-on                   |
| `MANUAL_OVERRIDE_MS`          | 30 min    | How long a manual action blocks auto-logic            |
| `SAFETY_MAX_ON_MS`            | 3 hours   | Maximum continuous fan-ON before forced OFF           |
| `BASELINE_SAVE_INTERVAL_MS`   | 15 min    | How often baselines are saved to flash                |
| `BASELINE_MAX_AGE_MS`         | 4 hours   | Maximum age of saved baselines to restore on boot     |
| `SENSOR_FAIL_TIMEOUT_MS`      | 5 min     | Time before sensor failure fallback activates         |
| `SENSOR_FAIL_FAN_ON_GRACE_MS` | 30 min    | Grace period before turning fan OFF on sensor failure |
| `BASELINE_FAST_PHASE_MS`      | 5 min     | Duration of fast baseline tracking after fan turns OFF|
| `HUMIDITY_ABS_ON_MIN`         | 55.0%     | Absolute humidity floor to trigger fan                |
| `HUMIDITY_DELTA_ON`           | 8.0%      | Rise above baseline to trigger fan ON                 |
| `HUMIDITY_DELTA_OFF`          | 3.0%      | Rise above baseline below which fan turns OFF         |
| `TEMP_ABS_ON_MIN`             | 27.0C     | Absolute temperature floor to trigger fan             |
| `TEMP_DELTA_ON`               | 3.0C      | Rise above baseline to trigger fan ON                 |
| `TEMP_DELTA_OFF`              | 1.5C      | Rise above baseline below which fan turns OFF         |
| `TEMP_RISE_ON_DELTA`          | 1.0C      | Sudden per-sample rise to trigger fan ON immediately  |
| `DISPLAY_DIM_MS`              | 60 s      | Inactivity before display dims (fan OFF only)         |
| `DISPLAY_OFF_MS`              | 5 min     | Inactivity before display turns off (fan OFF only)    |

---

## Project Structure

```
air_hood/
├── air_hood.ino      — Main sketch: setup, loop, sensor logic, HomeKit glue
├── display.cpp/.h    — OLED rendering (Adafruit SSD1306) with burn-in mitigation
├── my_accessory.c    — HomeKit accessory definition (Fan + Temp + Humidity services)
├── wifi_info.h       — WiFiManager connection helper
└── README.md         — This file
```

---

## License

MIT — free to use, modify, and share.
