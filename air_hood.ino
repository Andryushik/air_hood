#include <Arduino.h>
#include <arduino_homekit_server.h>
#include <Adafruit_SHT31.h>
#include <LittleFS.h>
#include <ArduinoOTA.h>
#include <ESP8266WebServer.h>
#include "wifi.h"
#include "display.h"
#include "RemoteLog.h"

// LOG_D goes to Serial AND the telnet console (port 23) via RemoteLog.
#define LOG_D(fmt, ...) rlog.printf_P(PSTR(fmt "\n"), ##__VA_ARGS__)

#define OTA_HOSTNAME "RangeHood"
#define OTA_PASSWORD "28142814"

// flash-release.sh stamps each build into fw_version.h (date.time-commit) and deletes
// it afterwards, so any other build reports "dev". The telnet banner shows it.
#if __has_include("fw_version.h")
#include "fw_version.h"
#else
#define FW_VERSION_STAMP "dev"
#endif
static const char *FW_VERSION = FW_VERSION_STAMP;

//==============================
// Pins, timing, thresholds
//==============================

#define PIN_RELAY D6
#define RELAY_ON LOW // active-low relay module
#define RELAY_OFF HIGH
#define PIN_TOUCH D5 // TTP223B capacitive touch sensor (active HIGH)
#define BUTTON_DEBOUNCE_MS 50
#define DOUBLE_TAP_MS 2000 // a second touch this soon cancels the manual override
#define SENSOR_READ_INTERVAL_MS 30000
#define AUTO_MIN_ON_MS (5UL * 60UL * 1000UL)
#define AUTO_MIN_OFF_MS (2UL * 60UL * 1000UL)
// After the air returns to normal, keep running until it has stayed calm for
// this long CONTINUOUSLY. Continued cooking (steam/heat pulses) re-arms the
// countdown, so the fan won't switch off mid-cook just because it extracted
// the current burst. Any spike above the OFF thresholds resets the timer.
#define AUTO_OFF_OVERRUN_MS (10UL * 60UL * 1000UL)
#define MANUAL_OVERRIDE_MS (30UL * 60UL * 1000UL)
#define HUMIDITY_ABS_ON_MIN 55.0f
#define HUMIDITY_DELTA_ON 8.0f
#define HUMIDITY_DELTA_OFF 3.0f
// Baseline smoothing while fan is OFF (ambient humidity).
// Use slower rise (ignore short humidity spikes) and slightly faster fall.
#define HUMIDITY_BASELINE_ALPHA_UP 0.01f
#define HUMIDITY_BASELINE_ALPHA_DOWN 0.05f

// Temperature-based trigger (useful for stove/hood): turn ON if temperature rises above ambient.
#define TEMP_ABS_ON_MIN 27.0f
#define TEMP_DELTA_ON 3.0f
#define TEMP_DELTA_OFF 2.0f
// Optional fast-rise trigger (per sensor sample, ~30s): catches sudden heating quickly.
#define TEMP_RISE_ON_DELTA 1.0f
// Baseline smoothing while fan is OFF (ambient temperature).
#define TEMP_BASELINE_ALPHA_UP 0.02f
#define TEMP_BASELINE_ALPHA_DOWN 0.05f

#define SAFETY_MAX_ON_MS (3UL * 60UL * 60UL * 1000UL)
#define BASELINE_SAVE_INTERVAL_MS (15UL * 60UL * 1000UL)
// Sensor down this long while the fan is ON -> switch the fan OFF (a dead sensor never reports "calm").
#define SENSOR_FAIL_FAN_OFF_MS (35UL * 60UL * 1000UL)
// Faster baseline alphas for the first 5 minutes after fan turns OFF.
#define BASELINE_FAST_PHASE_MS (5UL * 60UL * 1000UL)
#define HUMIDITY_BASELINE_ALPHA_UP_FAST 0.10f
#define TEMP_BASELINE_ALPHA_UP_FAST 0.15f

static const char *BASELINES_PATH = "/baselines.dat";

struct BaselineData
{
  float humidity_baseline;
  float temperature_baseline;
  uint32_t reserved; // was saved_millis; kept so existing baseline files still load
  uint32_t magic;    // simple validity marker
};

static const uint32_t BASELINE_MAGIC = 0xA1B2C3D4;

// HomeKit characteristics defined in my_accessory.c
extern "C" homekit_server_config_t homekit_config;
extern "C" homekit_characteristic_t cha_fan_on;
extern "C" homekit_characteristic_t cha_current_temperature;
extern "C" homekit_characteristic_t cha_current_humidity;

//==============================
// State
//==============================

static Adafruit_SHT31 sht31 = Adafruit_SHT31();
static ESP8266WebServer httpd(8080); // HTTP API for Home Assistant (parallel to HomeKit)

static bool sht31_ok = false;
static int touch_last_level = LOW;
static uint32_t touch_last_change_millis = 0;
static uint32_t last_touch_millis = 0;
static uint32_t next_sensor_millis = 0;
static uint32_t next_baseline_save_millis = 0;
static uint32_t next_heartbeat_millis = 0;
static float last_temperature = NAN; // last reading published to HomeKit (NAN = none yet)
static float last_humidity = NAN;
static bool fan_on = false;
static uint32_t last_fan_change_millis = 0;
static float humidity_baseline = NAN;
static float temperature_baseline = NAN;
static uint32_t manual_override_until_millis = 0;
static uint32_t sensor_fail_since_millis = 0;
static float last_temperature_for_rise = NAN; // per-sample temp-rise trigger reference
static uint32_t env_calm_since_millis = 0;    // when air first went calm while fan ON (0 = not calm)

static bool manual_override_active(uint32_t now)
{
  return manual_override_until_millis != 0 && (int32_t)(manual_override_until_millis - now) > 0;
}

static int16_t get_wifi_rssi()
{
  return (WiFi.status() == WL_CONNECTED) ? (int16_t)WiFi.RSSI() : 0;
}

// True once `deadline` has passed; then re-arms it `every` ms from now. Wrap-safe.
static bool due(uint32_t &deadline, uint32_t every, uint32_t now)
{
  if ((int32_t)(now - deadline) < 0)
  {
    return false;
  }
  deadline = now + every;
  return true;
}

//==============================
// Baselines on flash
//==============================

static bool baseline_values_valid(float hum_base, float temp_base)
{
  return !isnan(hum_base) && !isnan(temp_base) && hum_base >= 0.0f && hum_base <= 100.0f && temp_base >= 0.0f && temp_base <= 60.0f;
}

static void baselines_load(float &hum_base, float &temp_base)
{
  File f = LittleFS.open(BASELINES_PATH, "r");
  if (!f)
  {
    LOG_D("No saved baselines found");
    return;
  }
  BaselineData data;
  if (f.read((uint8_t *)&data, sizeof(data)) != sizeof(data) || data.magic != BASELINE_MAGIC)
  {
    LOG_D("Baselines file invalid");
    f.close();
    return;
  }
  f.close();

  // No age check: millis() restarts at every boot, so it can't tell how old the file is.
  if (!baseline_values_valid(data.humidity_baseline, data.temperature_baseline))
  {
    LOG_D("Baselines contain invalid values, discarding");
    return;
  }
  hum_base = data.humidity_baseline;
  temp_base = data.temperature_baseline;
  LOG_D("Baselines loaded: H=%.1f%% T=%.1fC", hum_base, temp_base);
}

static void baselines_save(float hum_base, float temp_base)
{
  if (!baseline_values_valid(hum_base, temp_base))
  {
    return;
  }
  BaselineData data;
  data.humidity_baseline = hum_base;
  data.temperature_baseline = temp_base;
  data.reserved = 0;
  data.magic = BASELINE_MAGIC;

  File f = LittleFS.open(BASELINES_PATH, "w");
  if (!f)
  {
    LOG_D("Failed to open baselines file for writing");
    return;
  }
  if (f.write((uint8_t *)&data, sizeof(data)) != sizeof(data))
  {
    LOG_D("Failed to write complete baselines file");
    f.close();
    return;
  }
  f.close();
  LOG_D("Baselines saved: H=%.1f%% T=%.1fC", hum_base, temp_base);
}

//==============================
// Sensor
//==============================

static void i2c_recover()
{
  pinMode(OLED_SDA, INPUT_PULLUP);
  pinMode(OLED_SCL, OUTPUT);
  for (int i = 0; i < 9; i++)
  {
    digitalWrite(OLED_SCL, LOW);
    delayMicroseconds(5);
    digitalWrite(OLED_SCL, HIGH);
    delayMicroseconds(5);
  }
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClockStretchLimit(I2C_STRETCH_LIMIT_US); // sensor_setup() re-applies it after sht31.begin()
}

static void sensor_setup()
{
  // SHT31-D default addresses: 0x44 (ADDR low) or 0x45 (ADDR high)
  sht31_ok = sht31.begin(0x44) || sht31.begin(0x45);
  // sht31.begin() calls Wire.begin(), which resets the clock-stretch limit to 150 ms.
  Wire.setClockStretchLimit(I2C_STRETCH_LIMIT_US);
  if (!sht31_ok)
  {
    LOG_D("SHT31-D not found on I2C (0x44/0x45)");
    return;
  }

  // Heater affects humidity readings; keep it off for normal operation.
  sht31.heater(false);
}

// Read the SHT31. After a failed read, clear the bus and re-init the sensor first.
static bool read_sensor(float &temperature, float &humidity)
{
  if (!sht31_ok)
  {
    i2c_recover();
    sensor_setup(); // on failure the read below fails and draws the error screen once
  }
  return sht31_ok && sht31.readBoth(&temperature, &humidity) && !isnan(temperature) && !isnan(humidity);
}

//==============================
// Fan
//==============================

// Show the current state. display_update() skips the I2C push when nothing visible changed.
static void redraw_display()
{
  HoodStatus status;
  status.temperature = last_temperature;
  status.humidity = last_humidity;
  status.temperature_baseline = temperature_baseline;
  status.humidity_baseline = humidity_baseline;
  status.fan_on = fan_on;
  status.override_active = manual_override_active(millis());
  status.wifi_rssi = get_wifi_rssi();
  display_update(status);
}

// Drive the relay to `on` and show it. Returns true if the state changed.
// Doesn't tell HomeKit: see change_fan().
static bool set_fan(bool on, const char *reason)
{
  const bool changed = (fan_on != on);
  fan_on = on;
  cha_fan_on.value.bool_value = on;
  digitalWrite(PIN_RELAY, on ? RELAY_ON : RELAY_OFF);
  if (changed)
  {
    last_fan_change_millis = millis();
    env_calm_since_millis = 0; // reset the auto-off overrun countdown on any state change
    LOG_D("Fan: %s (%s)", on ? "ON" : "OFF", reason);
  }
  display_wake();
  redraw_display();
  return changed;
}

// set_fan() and tell the HomeKit controllers. Everything but the HomeKit setter
// uses this: the controller that wrote the value already knows it.
static void change_fan(bool on, const char *reason)
{
  if (set_fan(on, reason))
  {
    homekit_characteristic_notify(&cha_fan_on, cha_fan_on.value);
  }
}

// A manual change pauses auto-control for 30 min. Arm it only on a real state
// change, so redundant writes (automations, hub re-asserts) don't keep auto paused.
static void arm_override_if_changing(bool on)
{
  if (on != fan_on)
  {
    manual_override_until_millis = millis() + MANUAL_OVERRIDE_MS;
  }
}

// TTP223B capacitive touch sensor — output goes HIGH on touch.
// Single touch: toggle fan + 30-min override.
// Double touch (within 2s): cancel override, return to auto.
static void poll_touch(uint32_t now)
{
  const int level = digitalRead(PIN_TOUCH);
  if (level != touch_last_level && (int32_t)(now - touch_last_change_millis) >= BUTTON_DEBOUNCE_MS)
  {
    touch_last_change_millis = now;
    touch_last_level = level;
    if (level == HIGH) // active-HIGH: finger detected
    {
      display_wake();
      if ((now - last_touch_millis) < DOUBLE_TAP_MS && manual_override_active(now))
      {
        manual_override_until_millis = 0;
        LOG_D("Override cancelled (double touch)");
        redraw_display();
      }
      else
      {
        const bool on = !fan_on;
        arm_override_if_changing(on);
        change_fan(on, "touch toggle");
      }
      last_touch_millis = now;
    }
  }
}

//==============================
// Auto-control
//==============================

// One exponential-smoothing step toward `sample`: alpha_up when rising, alpha_down otherwise.
static float ema(float base, float sample, float alpha_up, float alpha_down)
{
  const float alpha = (sample > base) ? alpha_up : alpha_down;
  return base + (sample - base) * alpha;
}

// Track baselines only while the fan is OFF (ambient conditions).
// Use faster alphas for the first 5 minutes after the fan turns OFF
// to quickly re-acquire true ambient after a long cooking session.
static void track_baselines(float humidity, float temperature, uint32_t now)
{
  if (fan_on)
  {
    return;
  }
  // While OFF, the last state change was the switch-off (or boot).
  const bool fast_phase = (now - last_fan_change_millis) < BASELINE_FAST_PHASE_MS;

  if (isnan(humidity_baseline))
  {
    humidity_baseline = min(humidity, HUMIDITY_ABS_ON_MIN); // clamp seed so a hot start can't lock out auto-ON
  }
  else
  {
    humidity_baseline = ema(humidity_baseline, humidity, fast_phase ? HUMIDITY_BASELINE_ALPHA_UP_FAST : HUMIDITY_BASELINE_ALPHA_UP, HUMIDITY_BASELINE_ALPHA_DOWN);
  }

  if (isnan(temperature_baseline))
  {
    temperature_baseline = min(temperature, TEMP_ABS_ON_MIN); // clamp seed (see humidity)
  }
  else
  {
    temperature_baseline = ema(temperature_baseline, temperature, fast_phase ? TEMP_BASELINE_ALPHA_UP_FAST : TEMP_BASELINE_ALPHA_UP, TEMP_BASELINE_ALPHA_DOWN);
  }
}

// Safety: force the fan OFF after 3 hours of continuous operation, even during a
// manual override. Returns true if it did.
static bool enforce_safety_limit(uint32_t now)
{
  if (!fan_on || now - last_fan_change_millis < SAFETY_MAX_ON_MS)
  {
    return false;
  }
  LOG_D("Safety timeout: fan ON for >3h, forcing OFF");
  manual_override_until_millis = 0; // clear any override
  change_fan(false, "safety timeout");
  return true;
}

static void auto_control(float humidity, float temperature, float temp_rise, uint32_t now)
{
  const uint32_t since_change = now - last_fan_change_millis;
  const float hum_base = isnan(humidity_baseline) ? humidity : humidity_baseline;
  const float hum_on_threshold = max(HUMIDITY_ABS_ON_MIN, hum_base + HUMIDITY_DELTA_ON);
  const float hum_off_threshold = hum_base + HUMIDITY_DELTA_OFF;

  const float temp_base = isnan(temperature_baseline) ? temperature : temperature_baseline;
  const float temp_on_threshold = max(TEMP_ABS_ON_MIN, temp_base + TEMP_DELTA_ON);
  const float temp_off_threshold = temp_base + TEMP_DELTA_OFF;

  if (!fan_on)
  {
    if (since_change >= AUTO_MIN_OFF_MS &&
        (humidity >= hum_on_threshold ||
         temperature >= temp_on_threshold || temp_rise >= TEMP_RISE_ON_DELTA))
    {
      const char *reason = (humidity >= hum_on_threshold) ? "humidity rise"
                                                          : ((temp_rise >= TEMP_RISE_ON_DELTA) ? "temp rise" : "temp high");
      change_fan(true, reason);
    }
  }
  else
  {
    const bool humidity_ok = humidity <= hum_off_threshold;
    const bool temperature_ok = temperature <= temp_off_threshold;
    const bool env_calm = humidity_ok && temperature_ok;
    if (!env_calm)
    {
      env_calm_since_millis = 0; // still cooking (steam/heat) — cancel the shutoff countdown
    }
    else
    {
      if (env_calm_since_millis == 0)
      {
        env_calm_since_millis = now; // air just went calm — start the overrun countdown
      }
      // Off only after min-on AND the air has stayed calm continuously for the overrun.
      if (since_change >= AUTO_MIN_ON_MS &&
          (now - env_calm_since_millis) >= AUTO_OFF_OVERRUN_MS)
      {
        change_fan(false, "environment normal (overrun elapsed)");
      }
    }
  }
}

// Both readings are valid here: report_environment() only calls this after a good read.
// Order matters: the safety limit beats a manual override, which beats auto-control.
static void update_fan_from_environment(float humidity, float temperature, uint32_t now)
{
  track_baselines(humidity, temperature, now);

  // The rise reference follows every good read, before the early returns, so it stays
  // current during a manual override. report_environment() resets it on sensor
  // recovery, so it never spans an outage.
  const float temp_rise = isnan(last_temperature_for_rise) ? 0.0f : temperature - last_temperature_for_rise;
  last_temperature_for_rise = temperature;

  if (enforce_safety_limit(now))
  {
    return;
  }
  // If the user changed the fan manually, pause auto-control
  if (manual_override_active(now))
  {
    return;
  }
  auto_control(humidity, temperature, temp_rise, now);
}

//==============================
// Sensor tick
//==============================

static void handle_sensor_failure(uint32_t now)
{
  if (sht31_ok)
  {
    LOG_D("SHT31-D read failed");
    sht31_ok = false; // force i2c_recover()+sensor_setup() on the next tick
  }
  display_show_sensor_error();
  if (sensor_fail_since_millis == 0)
  {
    sensor_fail_since_millis = now;
  }
  if (fan_on && now - sensor_fail_since_millis >= SENSOR_FAIL_FAN_OFF_MS)
  {
    LOG_D("Sensor failed >35min with fan ON, forcing OFF");
    change_fan(false, "sensor timeout");
  }
}

// Publish `value` to HomeKit if it moved more than `step` since the last publish
// (keeps event traffic down). Returns true if it did.
static bool publish_if_moved(homekit_characteristic_t &cha, float &last, float value, float step)
{
  if (!isnan(last) && fabsf(value - last) <= step)
  {
    return false;
  }
  last = value;
  cha.value.float_value = value;
  homekit_characteristic_notify(&cha, cha.value);
  return true;
}

static void publish_readings(float temperature, float humidity)
{
  if (publish_if_moved(cha_current_temperature, last_temperature, temperature, 0.1f))
  {
    LOG_D("Temperature: %.1f C", temperature);
  }
  if (publish_if_moved(cha_current_humidity, last_humidity, humidity, 0.2f))
  {
    LOG_D("Humidity: %.1f %%", humidity);
  }
}

// Every SENSOR_READ_INTERVAL_MS: read, publish to HomeKit, redraw, run the fan logic.
static void report_environment()
{
  // A fresh millis(), not loop()'s earlier `now`: a fan change made by the HomeKit
  // setter or HTTP earlier in this loop would be "in the future", and
  // now - last_fan_change_millis would wrap and fire the safety timeout.
  const uint32_t now = millis();
  if (!due(next_sensor_millis, SENSOR_READ_INTERVAL_MS, now))
  {
    return;
  }

  float temperature, humidity;
  if (!read_sensor(temperature, humidity))
  {
    handle_sensor_failure(now);
    return;
  }
  if (sensor_fail_since_millis != 0)
  {
    last_temperature_for_rise = NAN; // reset rise ref so recovery can't fake a spike
    sensor_fail_since_millis = 0;
  }
  publish_readings(temperature, humidity);
  redraw_display();
  update_fan_from_environment(humidity, temperature, now);
}

//==============================
// HomeKit, HTTP, OTA
//==============================

// Called when a HomeKit controller (the Home app, an automation) writes the fan state.
static void cha_fan_on_setter(const homekit_value_t value)
{
  const bool on = value.bool_value;
  arm_override_if_changing(on);
  set_fan(on, "HomeKit request");
}

// ---- HTTP API (Home Assistant) — parallel to HomeKit, no mDNS ----
// Like cha_fan_on_setter, but notifies HomeKit so the Home app follows.
static void http_set_fan(bool on)
{
  arm_override_if_changing(on);
  change_fan(on, "HTTP request");
}

static void web_setup()
{
  httpd.on("/status", HTTP_GET, []()
           {
    char tbuf[16], hbuf[16];
    if (isnan(last_temperature))
      strcpy(tbuf, "null");
    else
      snprintf(tbuf, sizeof(tbuf), "%.1f", last_temperature);
    if (isnan(last_humidity))
      strcpy(hbuf, "null");
    else
      snprintf(hbuf, sizeof(hbuf), "%.1f", last_humidity);
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"on\":%s,\"temp\":%s,\"hum\":%s,\"manual\":%s,\"rssi\":%d}",
             fan_on ? "true" : "false", tbuf, hbuf,
             manual_override_active(millis()) ? "true" : "false",
             (int)get_wifi_rssi());
    httpd.send(200, "application/json", buf); });
  httpd.on("/on", HTTP_POST, []()
           {
    http_set_fan(true);
    httpd.send(200, "text/plain", "OK"); });
  httpd.on("/off", HTTP_POST, []()
           {
    http_set_fan(false);
    httpd.send(200, "text/plain", "OK"); });
  httpd.onNotFound([]()
                   { httpd.send(404, "text/plain", "Not found"); });
  httpd.begin();
  LOG_D("HTTP API on :8080 (/status, POST /on, /off)");
}

// OTA over WiFi (espota). HomeKit owns the single MDNS responder, so start
// ArduinoOTA with useMDNS=false and upload by IP (see flash-release.sh). espota
// writes only the sketch region — the HomeKit pairing/FS sectors are untouched.
static void ota_setup()
{
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]()
                     { LOG_D("OTA: start"); });
  ArduinoOTA.onEnd([]()
                   { LOG_D("OTA: done, rebooting"); });
  ArduinoOTA.onError([](ota_error_t e)
                     { LOG_D("OTA: error %u", (unsigned)e); });
  ArduinoOTA.begin(false);
  LOG_D("Firmware %s | OTA ready: host=%s ip=%s (upload by IP)", FW_VERSION, OTA_HOSTNAME,
        WiFi.localIP().toString().c_str());
}

//==============================
// setup / loop
//==============================

void setup()
{
  Serial.begin(115200);
  display_setup();
  if (!LittleFS.begin())
  {
    LOG_D("LittleFS init failed");
  }
  pinMode(PIN_TOUCH, INPUT);
  touch_last_level = digitalRead(PIN_TOUCH);
  touch_last_change_millis = millis();
  sensor_setup();
  if (!sht31_ok)
  {
    display_show_sensor_error(); // stays visible during a slow WiFi connect / setup portal
  }
  wifi_connect(); // in wifi.h
  // homekit_storage_reset(); // to remove the previous HomeKit pairing storage

  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, RELAY_OFF);
  baselines_load(humidity_baseline, temperature_baseline);
  cha_fan_on.setter = cha_fan_on_setter;
  arduino_homekit_setup(&homekit_config);
  set_fan(cha_fan_on.value.bool_value, "boot");
  const uint32_t now = millis();
  last_fan_change_millis = now;
  next_sensor_millis = now;
  next_baseline_save_millis = now + BASELINE_SAVE_INTERVAL_MS;

  ota_setup();            // enable wireless firmware updates
  rlog.begin(FW_VERSION); // telnet debug console on port 23 (see log-rangehood.sh)
  web_setup();            // HTTP API on :8080 for Home Assistant
}

void loop()
{
  ArduinoOTA.handle();
  const uint32_t now = millis();
  httpd.handleClient(); // HTTP API for Home Assistant (non-blocking)
  rlog.loop();          // accept/drain telnet console clients
  poll_touch(now);
  arduino_homekit_loop();
  report_environment(); // takes its own millis(), see there
  display_check_timeout(now, fan_on);

  // Heartbeat to the telnet console (only when a client is watching) — keeps
  // log-rangehood.sh's `nc -w 15` alive and surfaces live heap/clients/RSSI.
  if (rlog.connected() && due(next_heartbeat_millis, 5000, now))
  {
    LOG_D("[hb] up=%lus heap=%u clients=%d rssi=%d",
          (unsigned long)(now / 1000), ESP.getFreeHeap(),
          arduino_homekit_connected_clients_count(), (int)get_wifi_rssi());
  }

  // Periodically save baselines (only when fan is OFF). due() goes first so the
  // deadline re-arms whatever the fan state.
  if (due(next_baseline_save_millis, BASELINE_SAVE_INTERVAL_MS, now) && !fan_on)
  {
    baselines_save(humidity_baseline, temperature_baseline);
  }

  delay(10);
}
