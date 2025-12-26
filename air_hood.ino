#include <Arduino.h>
#include <arduino_homekit_server.h>
#include <DHT.h>
#include "wifi_info.h"
#include "display.h"

#define LOG_D(fmt, ...) printf_P(PSTR(fmt "\n"), ##__VA_ARGS__);

#define PIN_SWITCH D6
#define DHT_PIN D7
#define DHT_TYPE DHT11
#define SENSOR_READ_INTERVAL_MS 30000
#define HEAP_LOG_INTERVAL_MS 60000
#define AUTO_MIN_ON_MS (5UL * 60UL * 1000UL)
#define AUTO_MIN_OFF_MS (2UL * 60UL * 1000UL)
#define MANUAL_OVERRIDE_MS (10UL * 60UL * 1000UL)
#define HUMIDITY_ABS_ON_MIN 55.0f
#define HUMIDITY_DELTA_ON 8.0f
#define HUMIDITY_DELTA_OFF 3.0f
#define HUMIDITY_BASELINE_ALPHA 0.10f

DHT dht(DHT_PIN, DHT_TYPE);

void setup()
{
	Serial.begin(115200);
	display_setup();
	dht.begin();
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
	display_update(last_temperature, last_humidity, humidity_baseline, switch_state, manual_override_active(millis()));
}

void update_switch_from_humidity(float humidity, uint32_t now)
{
	if (isnan(humidity))
	{
		return;
	}

	// Track baseline only while fan is OFF (ambient humidity)
	if (!switch_state)
	{
		if (isnan(humidity_baseline))
		{
			humidity_baseline = humidity;
		}
		else
		{
			humidity_baseline = humidity_baseline * (1.0f - HUMIDITY_BASELINE_ALPHA) + humidity * HUMIDITY_BASELINE_ALPHA;
		}
	}

	// If user changed manually in Home app, pause auto-control
	if (manual_override_active(now))
	{
		return;
	}

	const uint32_t since_change = now - last_fan_change_millis;
	const float baseline = isnan(humidity_baseline) ? humidity : humidity_baseline;
	const float on_threshold = max(HUMIDITY_ABS_ON_MIN, baseline + HUMIDITY_DELTA_ON);
	const float off_threshold = baseline + HUMIDITY_DELTA_OFF;

	if (!switch_state)
	{
		if (since_change >= AUTO_MIN_OFF_MS && humidity >= on_threshold)
		{
			apply_switch_state(true, true, "humidity rise");
		}
	}
	else
	{
		if (since_change >= AUTO_MIN_ON_MS && humidity <= off_threshold)
		{
			apply_switch_state(false, true, "humidity normal");
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

	float humidity = dht.readHumidity();
	float temperature = dht.readTemperature();

	if (isnan(humidity) || isnan(temperature))
	{
		LOG_D("DHT read failed");
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

	display_update(temperature, humidity, humidity_baseline, switch_state, manual_override_active(t));
	update_switch_from_humidity(humidity, t);
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
