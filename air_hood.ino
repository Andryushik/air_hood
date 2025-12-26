#include <Arduino.h>
#include <arduino_homekit_server.h>
#include <DHT.h>
#include "wifi_info.h"

#define LOG_D(fmt, ...) printf_P(PSTR(fmt "\n"), ##__VA_ARGS__);

#define PIN_SWITCH D6
#define DHT_PIN D7
#define DHT_TYPE DHT11
#define HUMIDITY_ON_THRESHOLD 60.0f
#define HUMIDITY_OFF_THRESHOLD 58.0f
#define SENSOR_READ_INTERVAL_MS 5000

DHT dht(DHT_PIN, DHT_TYPE);

void setup()
{
	Serial.begin(115200);
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

void apply_switch_state(bool on, bool notify, const char *reason)
{
	switch_state = on;
	cha_switch_on.value.bool_value = on;
	digitalWrite(PIN_SWITCH, on ? LOW : HIGH);
	if (notify)
	{
		homekit_characteristic_notify(&cha_switch_on, cha_switch_on.value);
	}
	if (reason)
	{
		LOG_D("Switch: %s (%s)", on ? "ON" : "OFF", reason);
	}
	else
	{
		LOG_D("Switch: %s", on ? "ON" : "OFF");
	}
}

void update_switch_from_humidity(float humidity)
{
	if (isnan(humidity))
	{
		return;
	}
	if (!switch_state && humidity >= HUMIDITY_ON_THRESHOLD)
	{
		apply_switch_state(true, true, "humidity high");
	}
	else if (switch_state && humidity <= HUMIDITY_OFF_THRESHOLD)
	{
		apply_switch_state(false, true, "humidity low");
	}
}

void report_environment()
{
	const uint32_t t = millis();
	if (t < next_sensor_millis)
	{
		return;
	}
	next_sensor_millis = t + SENSOR_READ_INTERVAL_MS;

	float humidity = dht.readHumidity();
	float temperature = dht.readTemperature();

	if (isnan(humidity) || isnan(temperature))
	{
		LOG_D("DHT read failed");
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

	update_switch_from_humidity(humidity);
}

// Called when the switch value is changed by iOS Home APP
void cha_switch_on_setter(const homekit_value_t value)
{
	bool on = value.bool_value;
	apply_switch_state(on, false, "HomeKit request");
}

void my_homekit_setup()
{
	pinMode(PIN_SWITCH, OUTPUT);
	digitalWrite(PIN_SWITCH, HIGH);

	// Add the .setter function to get the switch-event sent from iOS Home APP.
	// The .setter should be added before arduino_homekit_setup.
	// HomeKit sever uses the .setter_ex internally, see homekit_accessories_init function.
	// Maybe this is a legacy design issue in the original esp-homekit library,
	// and I have no reason to modify this "feature".
	cha_switch_on.setter = cha_switch_on_setter;
	arduino_homekit_setup(&config);
	apply_switch_state(cha_switch_on.value.bool_value, false, NULL);

	// report the switch value to HomeKit if it is changed (e.g. by a physical button)
	// bool switch_is_on = true/false;
	// cha_switch_on.value.bool_value = switch_is_on;
	// homekit_characteristic_notify(&cha_switch_on, cha_switch_on.value);
}

void my_homekit_loop()
{
	arduino_homekit_loop();
	report_environment();
	const uint32_t t = millis();
	if (t > next_heap_millis)
	{
		// show heap info every 5 seconds
		next_heap_millis = t + 5 * 1000;
		LOG_D("Free heap: %d, HomeKit clients: %d",
					ESP.getFreeHeap(), arduino_homekit_connected_clients_count());
	}
}
