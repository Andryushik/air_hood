#ifndef DISPLAY_H_
#define DISPLAY_H_

#include <Arduino.h>

#ifndef OLED_SDA
#define OLED_SDA D2
#endif
#ifndef OLED_SCL
#define OLED_SCL D1
#endif
#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH 128
#endif
#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT 64
#endif
#ifndef OLED_RESET
#define OLED_RESET -1
#endif
#ifndef OLED_ADDRESS
#define OLED_ADDRESS 0x3C
#endif

#define DISPLAY_DIM_MS 60000     // dim after 60s of inactivity (fan OFF)
#define DISPLAY_OFF_MS 300000    // turn off after 5 min of inactivity (fan OFF)
#define DISPLAY_DIM_CONTRAST 0x01

void display_setup();
void display_show_sensor_error();
void display_update(float temperature,
										float humidity,
										float humidity_baseline,
										float temperature_baseline,
										bool fan_on,
										bool override_active,
										int16_t wifi_rssi);
void display_wake();
void display_check_timeout(uint32_t now, bool fan_on);

#endif /* DISPLAY_H_ */
