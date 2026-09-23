# DHH review — Range Hood firmware (2026-09-23)

Status: **A, B, C DONE** (fw 2026-09-23.3 = A+B, .4 = C; both verified on the device: pairing survives the reboot, hub reconnects, HTTP OK). **D not started** — needs a decision. Uncommitted.

Originally: review only. Three read-only agents: core sketch; modules (display, RemoteLog, wifi, accessory); tooling/docs/structure. Every behaviour-changing claim was checked against the core/library sources before landing here. Annotate inline; delete this file once acted on.

## Verdict

Working, field-hardened firmware wearing two layers of ceremony: dead guards and constants, the same thing written 2–4 times, and a README that describes an older device. Two findings change behaviour; everything else is deletion and simplification.

## A. Bugs — change behaviour (verified in sources)

1. **The I2C clock-stretch cap is never in force** (`display.cpp:89`, `air_hood.ino:156`). Every `Wire.begin()` → `Twi::init()` resets the limit to 150 ms (`core_esp8266_si2c.cpp:249`). `display.begin()` calls `wire->begin()` by default (`Adafruit_SSD1306.cpp:525`, `periphBegin=true`), and `sht31.begin()` does too via BusIO (`Adafruit_I2CDevice.cpp:31`). So the 2 ms cap added in July is undone immediately. It only matters when the bus wedges with SCL held low (then one OLED frame can block for seconds). Fix: set the limit after the last `begin()` — `display.begin(…, true, false)` plus `Wire.setClockStretchLimit(2000)` after `sht31.begin()` in `sensor_setup()`.
2. **Replacing a stuck telnet client can block the loop for up to 300 ms** (`RemoteLog.cpp:19-20`). `WiFiClient::stop()` is `stop(0)` → `flush()` waits up to `WIFICLIENT_MAX_FLUSH_WAIT_MS` = 300 ms for acks. Fix: `_client.stop(1)`.
3. Latent: `LOG_D` ends with `;` (`air_hood.ino:12`), so `if (x) LOG_D(…); else …` won't compile. Drop the `;`.
4. Minor: a failed sensor tick pushes the "Sensor fail" frame up to 3 times, ~25 ms of blocking I2C each (`air_hood.ino:442-458`). Draw it once.
5. Minor: touch redraws pass `last_temperature`, sensor redraws the fresh reading, so the shown degree can flip at a rounding edge (`air_hood.ino:292/312` vs `497`). A single status source fixes it (D2).

## B. Docs that lie or are missing — high value

- **The README never mentions the required library patch**, so a fresh install brings the storage-wipe bug back. Add a "Required library patch" section (`git -C ../libraries/Arduino-HomeKit-ESP8266 apply …/docs/patches/homekit-storage-compact.patch`) and a guard in `flash-release.sh` that refuses to build against an unpatched library.
- README claims contradicted by the code:
  - "OTA-free";
  - baselines "restored if recent / < 4 h" and the `BASELINE_MAX_AGE_MS` row (the age check was removed);
  - "reset pairing without reflashing" (it takes two flashes);
  - the OFF rows imply one reading turns the fan off after 5 min (really: humidity AND temperature calm, 5 min min-on AND 10 min continuous calm);
  - who arms the override (HomeKit only on a real change; HTTP arms it too);
  - HTTP API / telnet / OTA missing from Features;
  - "rise is slower than fall" (false in the fast phase);
  - "MAN in the top-right corner" (the WiFi icon is; MAN sits left of it);
  - Project Structure omits RemoteLog, the scripts and docs;
  - "Generic ESP8266" (it's `nodemcuv2`, `eesz=4M2M`);
  - "reads air quality" (it reads temperature and relative humidity).
- `docs/plan/air-hood-fixes.md`: fully shipped, stale line refs, and Tier 4 contradicts what shipped → delete (git keeps it). `homekit-storage-wipe.md`: once step 5 passes, move the root cause and base commit `8a8e1a0` into the patch header and delete the plan.

## C. Deletion pass — no behaviour change

- Dead NaN guards on the readings inside `update_switch_from_environment` (the only caller passes valid readings). Keep the NaN-*baseline* fallback — that one is live.
- `last_fan_off_millis` equals `last_fan_change_millis` whenever it is read → `fast_phase = since_change < BASELINE_FAST_PHASE_MS`.
- `BASELINE_MAX_AGE_MS` and `saved_millis` are unused. Removing the field shifts the struct, so the old file fails the magic check once (baselines re-learned in minutes); or rename the field `reserved` to keep the layout.
- The `AUTO_MIN_ON_MS` term can never decide anything while `AUTO_OFF_OVERRUN_MS` (10 min) ≥ it, because calm-since resets on every state change. Delete it, or keep it with a comment tying it to the overrun.
- `SENSOR_FAIL_TIMEOUT_MS` + `SENSOR_FAIL_FAN_ON_GRACE_MS` are only used as a sum → one constant, one `if`.
- `flash-debug.sh` and `RANGEHOOD_DEBUG` (nothing reads the flag).
- ESP32 branches in `wifi_info.h`; accessory model string → `"ESP8266"`.
- Display: the `valid` field, the stale "99=hidden" comment, the dead `idle < 0` return. RemoteLog: the `debugOut` alias.
- The duplicate heap log (the heartbeat already prints it); the commented-out `homekit_storage_reset()`.

## D. Simplify — behaviour-preserving refactors (bigger; need an on-device regression pass)

1. **One way to change the fan:** `change_fan(on, reason)` / `set_fan(on, reason)` (no boolean `notify`, no NULL-reason branch), `arm_override_if_changing(on)` (pasted 3×), `redraw_display()` (the 7-arg call pasted 3×).
2. **`HoodStatus` struct for the display:** `display_update(const HoodStatus&)`; compute the frame once (`frame_for` / `==` / `render`) with a `NO_READING` sentinel; `print_reading()` for the copy-pasted rows; RSSI thresholds in one place.
3. **Split the two god functions:** `track_baselines()` (one `ema()` helper) / `enforce_safety_limit()` / `auto_control()`; `read_sensor()` / `handle_sensor_failure()` / `publish_readings()` (`publish_if_moved()` with `fabsf`).
4. `due(next, every, now)` for the four hand-written deadline checks; move `my_homekit_setup/loop` into `setup/loop`, state at the top.
5. Names: switch → fan, `PIN_RELAY` + `RELAY_ON/OFF`, `DOUBLE_TAP_MS`, `wifi_info.h` → `wifi.h`, `config` → `homekit_config`, `cha_switch_on` → `cha_fan_on`, `rlog.connected()`, `rlog.begin(FW_VERSION)`, `HOST` instead of `PORT` in the scripts.
6. Tooling: FQBN once in `sketch.yaml` (`default_fqbn`); `FW_VERSION` stamped by the flash script; an espota-not-found guard.

## E. Nits

Comment noise and changelog-style comments, an "Optional" that isn't, "3h"/"35min" hard-coded in log strings, mixed tabs/spaces, two file-static naming schemes, `%d` for `uint32_t`, a preamble for the patch file (and keep editors from trimming `.patch` — line 18 lost its leading space; `git apply --check` still passes), a temperature-first signature.

## Leave it alone

Wrap-safe `(int32_t)(deadline - now)` casts; the seed clamp; the NaN-baseline fallback; `switch_state` as a separate copy (needed for "arm only on a real change"); the HomeKit setter not notifying; the order safety → override → auto, and the rise-reference reset; the 9-clock bus clear, `ArduinoOTA.begin(false)`, `delay(10)`; the display dirty check and the one-shot error wake; RemoteLog's `availableForWrite() >= size`; both `WIFI_NONE_SLEEP` calls; the `" ON"` leading space, WiFi icon at x=116, MAN at x=84; the accessory services.

## Done notes

- A1–A4 ✅. The stretch cap is now re-applied after `display.begin(…, periphBegin=false)` and after `sht31.begin()` (not observable on the device; verified by call order). A5 is left for D2.
- B ✅. README fixed (patch section, OTA/logs/HTTP section, auto-off rule, false claims); `flash-release.sh` refuses to build without the patch (negative path tested); root cause moved into the patch header; `air-hood-fixes.md` and `homekit-storage-wipe.md` deleted.
- C ✅, with three deliberate deviations:
  - `AUTO_MIN_ON_MS` **kept**: it guards the min run time if `AUTO_OFF_OVERRUN_MS` is ever lowered below 5 min.
  - `saved_millis` renamed `reserved` instead of deleted, so the existing baselines file still loads (no one-time loss).
  - The commented-out `homekit_storage_reset()` **kept**: the README's pairing-reset procedure uses it.

## Recommended order

1. **A1–A4 + B** (patch section, build guard, README truth) — small, low risk, real value. One OTA push.
2. **C** (deletion pass) — mechanical; compiler plus a short on-device check.
3. **D** — optional. D1–D2 give the biggest clarity win. Do it as its own change with an on-device regression pass (touch, HomeKit, HTTP, auto on/off, sensor unplug).

> Q: which groups do you want? (A+B recommended now.)
