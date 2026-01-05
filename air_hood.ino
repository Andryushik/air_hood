#include <Arduino.h>
#include <arduino_homekit_server.h>
#include <Adafruit_SHT31.h>
#include "wifi_info.h"
#include "display.h"

#define LOG_D(fmt, ...) printf_P(PSTR(fmt "\n"), ##__VA_ARGS__);

#define PIN_SWITCH D6
#define SENSOR_READ_INTERVAL_MS 30000
#define HEAP_LOG_INTERVAL_MS 60000
#define AUTO_MIN_ON_MS (5UL * 60UL * 1000UL)
#define AUTO_MIN_OFF_MS (2UL * 60UL * 1000UL)
#define MANUAL_OVERRIDE_MS (10UL * 60UL * 1000UL)
#define HUMIDITY_ABS_ON_MIN 52.0f
#define HUMIDITY_DELTA_ON 6.0f
#define HUMIDITY_DELTA_OFF 3.0f
// Baseline smoothing while fan is OFF (ambient humidity).
// Use slower rise (ignore short humidity spikes) and slightly faster fall.
#define HUMIDITY_BASELINE_ALPHA_UP 0.01f
#define HUMIDITY_BASELINE_ALPHA_DOWN 0.05f

// Temperature-based trigger (useful for stove/hood): turn ON if temperature rises above ambient.
#define TEMP_ABS_ON_MIN 25.0f
#define TEMP_DELTA_ON 2.0f
#define TEMP_DELTA_OFF 1.0f
// Optional fast-rise trigger (per sensor sample, ~30s): catches sudden heating quickly.
#define TEMP_RISE_ON_DELTA 0.7f
// Baseline smoothing while fan is OFF (ambient temperature).
#define TEMP_BASELINE_ALPHA_UP 0.02f
#define TEMP_BASELINE_ALPHA_DOWN 0.05f

static Adafruit_SHT31 sht31 = Adafruit_SHT31();
static bool sht31_ok = false;

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
static float last_temperature = NAN;
static float last_humidity = NAN;
static bool switch_state = false;
static uint32_t last_fan_change_millis = 0;
static float humidity_baseline = NAN;
static float temperature_baseline = NAN;
static uint32_t manual_override_until_millis = 0;

static bool manual_override_active(uint32_t now)
{
	return manual_override_until_millis != 0 && (int32_t)(manual_override_until_millis - now) > 0;
}

void apply_switch_state(bool on, bool notify, const char *reason)
{
	const bool changed = (switch_state != on);
	switch_state = on;
	cha_switch_on.value.bool_value = on;
	digitalWrite(PIN_SWITCH, on ? LOW : HIGH);
	if (changed)
	{
		last_fan_change_millis = millis();
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
	display_update(last_temperature, last_humidity, humidity_baseline, temperature_baseline, switch_state, manual_override_active(millis()));
}

void update_switch_from_environment(float humidity, float temperature, uint32_t now)
{
	if (isnan(humidity))
	{
		return;
	}

	// Track baselines only while fan is OFF (ambient conditions)
	if (!switch_state)
	{
		if (isnan(humidity_baseline) && !isnan(humidity))
		{
			humidity_baseline = humidity;
		}
		else if (!isnan(humidity))
		{
			const float alpha = (humidity > humidity_baseline) ? HUMIDITY_BASELINE_ALPHA_UP : HUMIDITY_BASELINE_ALPHA_DOWN;
			humidity_baseline = humidity_baseline + (humidity - humidity_baseline) * alpha;
		}

		if (isnan(temperature_baseline) && !isnan(temperature))
		{
			temperature_baseline = temperature;
		}
		else if (!isnan(temperature))
		{
			const float alpha = (temperature > temperature_baseline) ? TEMP_BASELINE_ALPHA_UP : TEMP_BASELINE_ALPHA_DOWN;
			temperature_baseline = temperature_baseline + (temperature - temperature_baseline) * alpha;
		}
	}

	// If user changed manually in Home app, pause auto-control
	if (manual_override_active(now))
	{
		return;
	}

	const uint32_t since_change = now - last_fan_change_millis;
	const float hum_base = isnan(humidity_baseline) ? humidity : humidity_baseline;
	const float hum_on_threshold = max(HUMIDITY_ABS_ON_MIN, hum_base + HUMIDITY_DELTA_ON);
	const float hum_off_threshold = hum_base + HUMIDITY_DELTA_OFF;

	static float last_temperature_for_rise = NAN;
	const bool temp_valid = !isnan(temperature);
	const float temp_base = isnan(temperature_baseline) ? temperature : temperature_baseline;
	const float temp_on_threshold = temp_valid ? max(TEMP_ABS_ON_MIN, temp_base + TEMP_DELTA_ON) : NAN;
	const float temp_off_threshold = temp_valid ? (temp_base + TEMP_DELTA_OFF) : NAN;
	const float temp_rise = (temp_valid && !isnan(last_temperature_for_rise)) ? (temperature - last_temperature_for_rise) : 0.0f;
	last_temperature_for_rise = temperature;

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
		sensor_setup();
		if (!sht31_ok)
		{
			display_show_sensor_error();
			return;
		}
	}

	float temperature = sht31.readTemperature();
	float humidity = sht31.readHumidity();

	if (isnan(humidity) || isnan(temperature))
	{
		LOG_D("SHT31-D read failed");
		display_show_sensor_error();
		return;
	}

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

	display_update(temperature, humidity, humidity_baseline, temperature_baseline, switch_state, manual_override_active(t));
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

	cha_switch_on.setter = cha_switch_on_setter;
	arduino_homekit_setup(&config);
	apply_switch_state(cha_switch_on.value.bool_value, false, NULL);
	last_fan_change_millis = millis();
}

void my_homekit_loop()
{
	arduino_homekit_loop();
	report_environment();
	const uint32_t t = millis();
	if ((int32_t)(t - next_heap_millis) >= 0)
	{
		// show heap info periodically
		next_heap_millis = t + HEAP_LOG_INTERVAL_MS;
		LOG_D("Free heap: %d, HomeKit clients: %d",
					ESP.getFreeHeap(), arduino_homekit_connected_clients_count());
	}
}
