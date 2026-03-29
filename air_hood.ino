#include <Arduino.h>
#include <arduino_homekit_server.h>
#include <Adafruit_SHT31.h>
#include <LittleFS.h>
#include "wifi_info.h"
#include "display.h"

#define LOG_D(fmt, ...) printf_P(PSTR(fmt "\n"), ##__VA_ARGS__);

#define PIN_SWITCH D6
#define PIN_TOUCH D5 // TTP223B capacitive touch sensor (active HIGH)
#define BUTTON_DEBOUNCE_MS 50
#define SENSOR_READ_INTERVAL_MS 30000
#define HEAP_LOG_INTERVAL_MS 60000
#define AUTO_MIN_ON_MS (5UL * 60UL * 1000UL)
#define AUTO_MIN_OFF_MS (2UL * 60UL * 1000UL)
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
#define TEMP_DELTA_OFF 1.5f
// Optional fast-rise trigger (per sensor sample, ~30s): catches sudden heating quickly.
#define TEMP_RISE_ON_DELTA 1.0f
// Baseline smoothing while fan is OFF (ambient temperature).
#define TEMP_BASELINE_ALPHA_UP 0.02f
#define TEMP_BASELINE_ALPHA_DOWN 0.05f

#define SAFETY_MAX_ON_MS (3UL * 60UL * 60UL * 1000UL)
#define BASELINE_SAVE_INTERVAL_MS (15UL * 60UL * 1000UL)
#define BASELINE_MAX_AGE_MS (4UL * 60UL * 60UL * 1000UL)
#define SENSOR_FAIL_TIMEOUT_MS (5UL * 60UL * 1000UL)
#define SENSOR_FAIL_FAN_ON_GRACE_MS (30UL * 60UL * 1000UL)
// Faster baseline alphas for the first 5 minutes after fan turns OFF.
#define BASELINE_FAST_PHASE_MS (5UL * 60UL * 1000UL)
#define HUMIDITY_BASELINE_ALPHA_UP_FAST 0.10f
#define TEMP_BASELINE_ALPHA_UP_FAST 0.15f

static const char *BASELINES_PATH = "/baselines.dat";

struct BaselineData
{
	float humidity_baseline;
	float temperature_baseline;
	uint32_t saved_millis;
	uint32_t magic; // simple validity marker
};

static const uint32_t BASELINE_MAGIC = 0xA1B2C3D4;

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

	const uint32_t age = millis() - data.saved_millis;
	if (age > BASELINE_MAX_AGE_MS)
	{
		LOG_D("Baselines too old (%lu ms), discarding", (unsigned long)age);
		return;
	}
	if (!baseline_values_valid(data.humidity_baseline, data.temperature_baseline))
	{
		LOG_D("Baselines contain invalid values, discarding");
		return;
	}
	hum_base = data.humidity_baseline;
	temp_base = data.temperature_baseline;
	LOG_D("Baselines loaded: H=%.1f%% T=%.1fC (age %lu s)",
				hum_base, temp_base, (unsigned long)(age / 1000));
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
	data.saved_millis = millis();
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

static Adafruit_SHT31 sht31 = Adafruit_SHT31();
static bool sht31_ok = false;
static int touch_last_level = LOW;
static uint32_t touch_last_change_millis = 0;

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
}

static void sensor_setup()
{
	// SHT31-D default addresses: 0x44 (ADDR low) or 0x45 (ADDR high)
	if (sht31.begin(0x44))
	{
		sht31_ok = true;
	}
	else if (sht31.begin(0x45))
	{
		sht31_ok = true;
	}
	else
	{
		sht31_ok = false;
		LOG_D("SHT31-D not found on I2C (0x44/0x45)");
		display_show_sensor_error();
		return;
	}

	// Heater affects humidity readings; keep it off for normal operation.
	sht31.heater(false);
}

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
	wifi_connect(); // in wifi_info.h
	// homekit_storage_reset(); // to remove the previous HomeKit pairing storage
	my_homekit_setup();
}

void loop()
{
	my_homekit_loop();
	delay(10);
}

//==============================
// HomeKit setup and loop
//==============================

// access your HomeKit characteristics defined in my_accessory.c
extern "C" homekit_server_config_t config;
extern "C" homekit_characteristic_t cha_switch_on;
extern "C" homekit_characteristic_t cha_current_temperature;
extern "C" homekit_characteristic_t cha_current_humidity;

static uint32_t next_heap_millis = 0;
static uint32_t next_sensor_millis = 0;
static uint32_t next_baseline_save_millis = 0;
static float last_temperature = NAN;
static float last_humidity = NAN;
static bool switch_state = false;
static uint32_t last_fan_change_millis = 0;
static uint32_t last_fan_off_millis = 0;
static float humidity_baseline = NAN;
static float temperature_baseline = NAN;
static uint32_t manual_override_until_millis = 0;
static uint32_t sensor_fail_since_millis = 0;
static uint32_t last_touch_millis = 0;

static bool manual_override_active(uint32_t now)
{
	return manual_override_until_millis != 0 && (int32_t)(manual_override_until_millis - now) > 0;
}

static int16_t get_wifi_rssi()
{
	return (WiFi.status() == WL_CONNECTED) ? (int16_t)WiFi.RSSI() : 0;
}

void apply_switch_state(bool on, bool notify, const char *reason)
{
	const bool changed = (switch_state != on);
	switch_state = on;
	cha_switch_on.value.bool_value = on;
	digitalWrite(PIN_SWITCH, on ? LOW : HIGH);
	if (changed)
	{
		const uint32_t now = millis();
		last_fan_change_millis = now;
		if (!on)
		{
			last_fan_off_millis = now;
		}
	}
	if (notify && changed)
	{
		homekit_characteristic_notify(&cha_switch_on, cha_switch_on.value);
	}
	if (changed)
	{
		if (reason)
		{
			LOG_D("Fan: %s (%s)", on ? "ON" : "OFF", reason);
		}
		else
		{
			LOG_D("Fan: %s", on ? "ON" : "OFF");
		}
	}
	display_wake();
	display_update(last_temperature, last_humidity, humidity_baseline, temperature_baseline, switch_state, manual_override_active(millis()), get_wifi_rssi());
}

// TTP223B capacitive touch sensor — output goes HIGH on touch.
// Single touch: toggle fan + 10-min override.
// Double touch (within 2s): cancel override, return to auto.
void poll_touch(uint32_t now)
{
	const int level = digitalRead(PIN_TOUCH);
	if (level != touch_last_level && (int32_t)(now - touch_last_change_millis) >= BUTTON_DEBOUNCE_MS)
	{
		touch_last_change_millis = now;
		touch_last_level = level;
		if (level == HIGH) // active-HIGH: finger detected
		{
			display_wake();
			if ((now - last_touch_millis) < 2000 && manual_override_active(now))
			{
				manual_override_until_millis = 0;
				LOG_D("Override cancelled (double touch)");
				display_update(last_temperature, last_humidity, humidity_baseline, temperature_baseline, switch_state, false, get_wifi_rssi());
			}
			else
			{
				manual_override_until_millis = now + MANUAL_OVERRIDE_MS;
				apply_switch_state(!switch_state, true, "touch toggle");
			}
			last_touch_millis = now;
		}
	}
}

void update_switch_from_environment(float humidity, float temperature, uint32_t now)
{
	if (isnan(humidity))
	{
		return;
	}

	// Track baselines only while fan is OFF (ambient conditions).
	// Use faster alphas for the first 5 minutes after fan turns OFF
	// to quickly re-acquire true ambient after a long cooking session.
	if (!switch_state)
	{
		const bool fast_phase = (now - last_fan_off_millis) < BASELINE_FAST_PHASE_MS;

		if (isnan(humidity_baseline) && !isnan(humidity))
		{
			humidity_baseline = humidity;
		}
		else if (!isnan(humidity))
		{
			const float alpha_up = fast_phase ? HUMIDITY_BASELINE_ALPHA_UP_FAST : HUMIDITY_BASELINE_ALPHA_UP;
			const float alpha = (humidity > humidity_baseline) ? alpha_up : HUMIDITY_BASELINE_ALPHA_DOWN;
			humidity_baseline = humidity_baseline + (humidity - humidity_baseline) * alpha;
		}

		if (isnan(temperature_baseline) && !isnan(temperature))
		{
			temperature_baseline = temperature;
		}
		else if (!isnan(temperature))
		{
			const float alpha_up = fast_phase ? TEMP_BASELINE_ALPHA_UP_FAST : TEMP_BASELINE_ALPHA_UP;
			const float alpha = (temperature > temperature_baseline) ? alpha_up : TEMP_BASELINE_ALPHA_DOWN;
			temperature_baseline = temperature_baseline + (temperature - temperature_baseline) * alpha;
		}
	}

	// Always keep rise tracker current so it is not stale after manual override.
	static float last_temperature_for_rise = NAN;
	const bool temp_valid = !isnan(temperature);
	const float temp_rise = (temp_valid && !isnan(last_temperature_for_rise)) ? (temperature - last_temperature_for_rise) : 0.0f;
	if (temp_valid)
	{
		last_temperature_for_rise = temperature;
	}

	// Safety: force fan OFF after 3 hours continuous operation
	const uint32_t since_change = now - last_fan_change_millis;
	if (switch_state && since_change >= SAFETY_MAX_ON_MS)
	{
		LOG_D("Safety timeout: fan ON for >3h, forcing OFF");
		manual_override_until_millis = 0; // clear any override
		apply_switch_state(false, true, "safety timeout");
		return;
	}

	// If user changed manually in Home app, pause auto-control
	if (manual_override_active(now))
	{
		return;
	}
	const float hum_base = isnan(humidity_baseline) ? humidity : humidity_baseline;
	const float hum_on_threshold = max(HUMIDITY_ABS_ON_MIN, hum_base + HUMIDITY_DELTA_ON);
	const float hum_off_threshold = hum_base + HUMIDITY_DELTA_OFF;

	const float temp_base = isnan(temperature_baseline) ? temperature : temperature_baseline;
	const float temp_on_threshold = temp_valid ? max(TEMP_ABS_ON_MIN, temp_base + TEMP_DELTA_ON) : NAN;
	const float temp_off_threshold = temp_valid ? (temp_base + TEMP_DELTA_OFF) : NAN;

	if (!switch_state)
	{
		if (since_change >= AUTO_MIN_OFF_MS &&
				(humidity >= hum_on_threshold ||
				 (temp_valid && (temperature >= temp_on_threshold || temp_rise >= TEMP_RISE_ON_DELTA))))
		{
			const char *reason = (humidity >= hum_on_threshold) ? "humidity rise"
																													: ((temp_rise >= TEMP_RISE_ON_DELTA) ? "temp rise" : "temp high");
			apply_switch_state(true, true, reason);
		}
	}
	else
	{
		const bool humidity_ok = humidity <= hum_off_threshold;
		const bool temperature_ok = !temp_valid || temperature <= temp_off_threshold;
		if (since_change >= AUTO_MIN_ON_MS && humidity_ok && temperature_ok)
		{
			apply_switch_state(false, true, "environment normal");
		}
	}
}

void report_environment()
{
	const uint32_t t = millis();
	if ((int32_t)(t - next_sensor_millis) < 0)
	{
		return;
	}
	next_sensor_millis = t + SENSOR_READ_INTERVAL_MS;

	if (!sht31_ok)
	{
		i2c_recover();
		sensor_setup();
		if (!sht31_ok)
		{
			display_show_sensor_error();
		}
	}

	float temperature, humidity;
	bool read_ok = sht31_ok && sht31.readBoth(&temperature, &humidity) && !isnan(temperature) && !isnan(humidity);

	if (!read_ok)
	{
		if (sht31_ok)
		{
			LOG_D("SHT31-D read failed");
		}
		display_show_sensor_error();
		// Sensor failure fallback
		if (sensor_fail_since_millis == 0)
		{
			sensor_fail_since_millis = t;
		}
		const uint32_t fail_duration = t - sensor_fail_since_millis;
		if (fail_duration >= SENSOR_FAIL_TIMEOUT_MS && switch_state)
		{
			if (fail_duration >= SENSOR_FAIL_TIMEOUT_MS + SENSOR_FAIL_FAN_ON_GRACE_MS)
			{
				LOG_D("Sensor failed >35min with fan ON, forcing OFF");
				apply_switch_state(false, true, "sensor timeout");
			}
		}
		return;
	}
	sensor_fail_since_millis = 0;

	if (isnan(last_temperature) || temperature - last_temperature > 0.1f || last_temperature - temperature > 0.1f)
	{
		last_temperature = temperature;
		cha_current_temperature.value.float_value = temperature;
		homekit_characteristic_notify(&cha_current_temperature, cha_current_temperature.value);
		LOG_D("Temperature: %.1f C", temperature);
	}

	if (isnan(last_humidity) || humidity - last_humidity > 0.2f || last_humidity - humidity > 0.2f)
	{
		last_humidity = humidity;
		cha_current_humidity.value.float_value = humidity;
		homekit_characteristic_notify(&cha_current_humidity, cha_current_humidity.value);
		LOG_D("Humidity: %.1f %%", humidity);
	}

	display_update(temperature, humidity, humidity_baseline, temperature_baseline, switch_state, manual_override_active(t), get_wifi_rssi());
	update_switch_from_environment(humidity, temperature, t);
}

// Called when the switch value is changed by iOS Home APP
void cha_switch_on_setter(const homekit_value_t value)
{
	bool on = value.bool_value;
	manual_override_until_millis = millis() + MANUAL_OVERRIDE_MS;
	apply_switch_state(on, false, "HomeKit request");
}

void my_homekit_setup()
{
	pinMode(PIN_SWITCH, OUTPUT);
	digitalWrite(PIN_SWITCH, HIGH);

	baselines_load(humidity_baseline, temperature_baseline);

	cha_switch_on.setter = cha_switch_on_setter;
	arduino_homekit_setup(&config);
	apply_switch_state(cha_switch_on.value.bool_value, false, NULL);
	const uint32_t now = millis();
	last_fan_change_millis = now;
	last_fan_off_millis = now;
	next_sensor_millis = now;
	next_baseline_save_millis = now + BASELINE_SAVE_INTERVAL_MS;
}

void my_homekit_loop()
{
	const uint32_t t = millis();
	poll_touch(t);
	arduino_homekit_loop();
	report_environment();
	display_check_timeout(t, switch_state);

	// Periodically save baselines (only when fan is OFF and baselines are valid)
	if ((int32_t)(t - next_baseline_save_millis) >= 0)
	{
		next_baseline_save_millis = t + BASELINE_SAVE_INTERVAL_MS;
		if (!switch_state)
		{
			baselines_save(humidity_baseline, temperature_baseline);
		}
	}

	if ((int32_t)(t - next_heap_millis) >= 0)
	{
		next_heap_millis = t + HEAP_LOG_INTERVAL_MS;
		LOG_D("Free heap: %d, HomeKit clients: %d",
					ESP.getFreeHeap(), arduino_homekit_connected_clients_count());
	}
}
