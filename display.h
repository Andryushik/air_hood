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

void display_setup();
void display_show_sensor_error();
void display_update(float temperature,
										float humidity,
										float humidity_baseline,
										float temperature_baseline,
										bool fan_on,
										bool override_active);

#endif /* DISPLAY_H_ */
