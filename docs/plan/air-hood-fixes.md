# Air Hood — fixes plan

Context: offline was a stale HomeKit pairing (fixed by re-pairing). Remaining = code fixes from the review, delivered wirelessly.

## Sequencing (decided)
1. **FLASH #1 — OTA enablement, over USB.** Adds ArduinoOTA + a telnet console + flash/log scripts. No logic changes. **IMPLEMENTED — compiles (flash 53%, static RAM 46%, IRAM 94%). Pending the USB flash.**
   - USB flash MUST use **Erase Flash: "Only Sketch Data"/wipe=none** (the FQBN in the scripts already sets `wipe=none`) so the pairing you just set up is preserved.
   - Verify after flashing: device reconnects + still paired; `./log-airhood.sh` shows the console banner + heartbeats; run `./flash-release.sh` once as a no-op to confirm the OTA path works *while USB recovery is still possible*.
2. **Tiers 1-3 below — pushed via `./flash-release.sh` (wireless)** once OTA is confirmed.

OTA feasibility: verified by real build — 557 KB / 1024 KB sketch slot (53%), ~490 KB free for the OTA image. **Feasible.**

Files added for FLASH #1: `RemoteLog.h/.cpp`, `flash-release.sh`, `flash-debug.sh`, `log-airhood.sh`. Edits: `air_hood.ino` (includes, `LOG_D`→telnet, `ota_setup()`, `ArduinoOTA.handle()`, `rlog.loop()`+heartbeat). OTA password `28142814`, hostname `AirHood`, `ArduinoOTA.begin(false)` (HomeKit keeps mDNS; upload by IP).

---

## Tier 1 — Reliability core (recommended, low risk)

### 1. Disable WiFi modem-sleep — `wifi_info.h`
Fixes the latency spikes (5→90 ms ping, 1.2 s TCP) that make a *paired* device laggy / blip to No Response.
```c
// after: WiFi.mode(WIFI_STA);
WiFi.setSleepMode(WIFI_NONE_SLEEP);   // keep radio awake for HomeKit
```
Also re-assert once after `autoConnect()` returns success (before the Serial.printf), since WiFiManager may toggle radio state.

### 2. CPU → 160 MHz — `.vscode/arduino.json`
`configuration` string: `xtal=80` → `xtal=160`. (Library forces 160 at runtime; this makes the whole boot run at 160 and matches the README "must".)

### 3. Re-enable I2C recovery — `air_hood.ino` (~line 402)
`sht31_ok` is never cleared after a runtime read failure, so `i2c_recover()`+`sensor_setup()` never run again. Add one line:
```c
if (sht31_ok)
{
    LOG_D("SHT31-D read failed");
    sht31_ok = false;   // ADD: trigger i2c_recover()+sensor_setup() next 30s tick
}
```

### 4. Cap I2C clock-stretch — `display.cpp` (after Wire.begin, ~L83) and `air_hood.ino` i2c_recover (~L141)
Prevents a wedged-SCL bus from stalling the loop ~15 s (→ real No Response):
```c
Wire.begin(OLED_SDA, OLED_SCL);
Wire.setClockStretchLimit(2000);   // ADD (µs)
```

### 5. Baseline persistence — `air_hood.ino` `baselines_load()` (L80-85, L93-94)
`age = millis() - saved_millis` underflows across a reboot → saved baselines always discarded. Remove the age gate; keep magic + `baseline_values_valid()`:
```c
// DELETE the "age" computation and the "Baselines too old" discard block (L80-85)
// and drop `age` from the success log (L93-94).
```

---

## Tier 2 — On/off control correctness (you asked to check this)

### 6. Don't re-arm override on no-op HomeKit writes — `air_hood.ino` `cha_switch_on_setter` (L446-451)
```c
void cha_switch_on_setter(const homekit_value_t value)
{
    bool on = value.bool_value;
    if (on != switch_state)                                   // CHANGED: only on real transition
        manual_override_until_millis = millis() + MANUAL_OVERRIDE_MS;
    apply_switch_state(on, false, "HomeKit request");
}
```

### 7. Clamp cold-start baseline seed — `air_hood.ino` (L301-303, L312-314)
So a reboot/turn-off during cooking can't pin ON thresholds above current conditions:
```c
humidity_baseline    = min(humidity, HUMIDITY_ABS_ON_MIN);   // CHANGED from = humidity
temperature_baseline = min(temperature, TEMP_ABS_ON_MIN);    // CHANGED from = temperature
```

### 8. Reset temp-rise tracker after a sensor outage — `air_hood.ino`
Make `last_temperature_for_rise` file-scope (near other statics); in `report_environment`, on recovery (before `sensor_fail_since_millis = 0`):
```c
if (sensor_fail_since_millis != 0)
    last_temperature_for_rise = NAN;   // avoid spurious "temp rise" ON across an outage
```

### 9. Doc/const consistency
`TEMP_DELTA_OFF` = 2.0 in code vs README "1.5". Decide the intended value; align README (and the override comment says "10-min" but it's 30 — L258).

---

## Tier 3 — Display / cosmetics (low priority)

- Disconnected-WiFi icon never blinks (refresh ~30 s vs millis()/500) → draw it statically when disconnected (`display.cpp:47-60`).
- `display_show_sensor_error()` doesn't wake the panel → call `display_wake()` first so "Sensor fail" is visible if the OLED had powered off (`display.cpp:102`).
- Optional width guard for 3-digit/negative temp values (`display.cpp:172`).
- README: remove the "pixel-shifting" burn-in claim (no longer in code, `README.md:180`).

---

## Tier 4 — ArduinoOTA (optional; one supervised USB flash to enable, wireless after)

- `#include <ArduinoOTA.h>`; in setup after HomeKit is up: set hostname `AirHood`, a password, `ArduinoOTA.begin();`
- In `loop()`: `ArduinoOTA.handle();` (non-blocking).
- **Risk to verify:** HomeKit already runs the `MDNS` responder; `ArduinoOTA.begin()` re-inits MDNS. Must confirm HomeKit pairing + discovery still work after adding OTA (test on the bench with serial attached before trusting it). Heap cost ~1-2 KB — acceptable given ~440 KB flash headroom, but watch the `Free heap` log after adding.

---

## Verification after flashing
- Serial @115200: confirm boot, `Free heap` stays healthy, pairing intact (no `Formatting HomeKit storage`).
- Re-run ping/HAP-connect from the Mac: latency should drop to steady single-digit ms (confirms sleep fix).
- Trigger fan ON/OFF via touch + Home app; confirm auto still engages after override.
