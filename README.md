# Air Hood — Smart Kitchen Fan Controller

A DIY smart air hood / kitchen fan controller built on an ESP8266 NodeMCU V3.  
Integrates with **Apple HomeKit** natively (no hub, no cloud), reads air quality via a **SHT31-D** temperature/humidity sensor, displays live status on an **OLED screen**, controls a **relay**, and supports a **capacitive touch sensor** for hands-free manual control.

---

## Features

- **Apple HomeKit** — control and automate from the iOS Home app, Siri, and Shortcuts
- **Auto fan logic** — turns the fan on/off based on humidity rise/fall and temperature spikes relative to a learned ambient baseline
- **Manual override** — touch sensor or HomeKit toggle sets a 10-minute manual override window
- **OLED status display** — live temperature, humidity, baselines, fan state, and manual override indicator
- **WiFiManager** — first-boot captive-portal setup; no hardcoded credentials
- **OTA-free simplicity** — short flash cycle over USB

---

## Hardware

| Component                | Description                                                                |
| ------------------------ | -------------------------------------------------------------------------- |
| **MCU**                  | NodeMCU V3 — Wireless Module CH340/CP2102, ESP8266 ESP-12E                 |
| **Humidity/Temp sensor** | Adafruit SHT31-D (I²C, address 0x44)                                       |
| **Display**              | TENSTAR 0.96" I²C OLED, SSD1315 driver, 128×64, 4-pin IIC                  |
| **Relay**                | 1-Channel 5V Relay Module with Optocoupler, High/Low Level Trigger         |
| **Touch sensor**         | TTP223B Digital Capacitive Touch Module (active HIGH, with filter circuit) |

---

## Wiring

### I²C Bus (shared by OLED + SHT31-D)

| Signal | NodeMCU Pin | GPIO  |
| ------ | ----------- | ----- |
| SDA    | D2          | GPIO4 |
| SCL    | D1          | GPIO5 |

> Both the OLED (address `0x3C`) and the SHT31-D (address `0x44`) share the same I²C bus.

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
| `Adafruit BusIO`          | I²C/SPI abstraction (dependency) |
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

| Event             | Condition                                                                  |
| ----------------- | -------------------------------------------------------------------------- |
| Fan turns **ON**  | Humidity ≥ max(55%, baseline + 8%) AND fan has been OFF for at least 2 min |
| Fan turns **OFF** | Humidity ≤ baseline + 3% AND fan has been ON for at least 5 min            |

### Temperature trigger (stove/cooking detection)

| Event             | Condition                                                                            |
| ----------------- | ------------------------------------------------------------------------------------ |
| Fan turns **ON**  | Temperature ≥ max(27°C, baseline + 3°C), **or** a sudden +1°C rise in one 30s sample |
| Fan turns **OFF** | Temperature ≤ baseline + 1.5°C AND fan has been ON for at least 5 min                |

### Baseline learning

The device tracks ambient humidity and temperature using exponential smoothing **only while the fan is OFF** (so cooking air doesn't corrupt the baseline). Rise is slower than fall to ignore short spikes.

### Manual override

Any manual action (touch tap or HomeKit toggle) sets a **10-minute manual override**. During this window, auto-logic is paused. The OLED shows `MAN` in the top-right corner. After 10 minutes, auto-control resumes.

---

## OLED Display Layout

```
 [FAN]  ON         MAN   ← fan icon + state + override indicator
 Temperature:    22 °C
 Humidity:       48 %
 Base T: 21 °C  H: 46 %  ← learned ambient baselines
```

When the sensor is unreachable, the display shows `Sensor fail`.

---

## Configuration Constants (air_hood.ino)

| Constant                  | Default   | Description                                          |
| ------------------------- | --------- | ---------------------------------------------------- |
| `SENSOR_READ_INTERVAL_MS` | 30 000 ms | How often sensor is polled                           |
| `AUTO_MIN_ON_MS`          | 5 min     | Minimum fan-ON time before auto-off                  |
| `AUTO_MIN_OFF_MS`         | 2 min     | Minimum fan-OFF time before auto-on                  |
| `MANUAL_OVERRIDE_MS`      | 10 min    | How long a manual action blocks auto-logic           |
| `HUMIDITY_ABS_ON_MIN`     | 55.0%     | Absolute humidity floor to trigger fan               |
| `HUMIDITY_DELTA_ON`       | 8.0%      | Rise above baseline to trigger fan ON                |
| `HUMIDITY_DELTA_OFF`      | 3.0%      | Rise above baseline below which fan turns OFF        |
| `TEMP_ABS_ON_MIN`         | 27.0°C    | Absolute temperature floor to trigger fan            |
| `TEMP_DELTA_ON`           | 3.0°C     | Rise above baseline to trigger fan ON                |
| `TEMP_DELTA_OFF`          | 1.5°C     | Rise above baseline below which fan turns OFF        |
| `TEMP_RISE_ON_DELTA`      | 1.0°C     | Sudden per-sample rise to trigger fan ON immediately |

---

## Project Structure

```
air_hood/
├── air_hood.ino      — Main sketch: setup, loop, sensor logic, HomeKit glue
├── display.cpp/.h    — OLED rendering (Adafruit SSD1306)
├── my_accessory.c    — HomeKit accessory definition (Fan + Temp + Humidity services)
├── wifi_info.h       — WiFiManager connection helper
└── README.md         — This file
```

---

## License

MIT — free to use, modify, and share.
