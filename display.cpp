#include "display.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

static Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
static bool display_ok = false;

static int16_t round_to_int(float value)
{
    return (int16_t)lroundf(value);
}

static void draw_fan_icon(int16_t x, int16_t y)
{
    const int16_t cx = x + 8;
    const int16_t cy = y + 8;

    display.drawCircle(cx, cy, 7, SSD1306_WHITE);
    display.drawCircle(cx, cy, 2, SSD1306_WHITE);

    display.fillTriangle(cx, cy, cx, cy - 7, cx + 4, cy - 3, SSD1306_WHITE);
    display.fillTriangle(cx, cy, cx + 7, cy, cx + 3, cy + 4, SSD1306_WHITE);
    display.fillTriangle(cx, cy, cx, cy + 7, cx - 4, cy + 3, SSD1306_WHITE);
    display.fillTriangle(cx, cy, cx - 7, cy, cx - 3, cy - 4, SSD1306_WHITE);
}

static void draw_wind_trails(int16_t x, int16_t y)
{
    // Three identical compact "~" like trails to the right of the fan
    for (uint8_t i = 0; i < 3; ++i)
    {
        const int16_t yy = y + i * 5;
        display.drawLine(x, yy, x + 3, yy, SSD1306_WHITE);
        display.drawLine(x + 3, yy, x + 6, yy - 1, SSD1306_WHITE);
        display.drawLine(x + 6, yy - 1, x + 8, yy - 1, SSD1306_WHITE);
    }
}

void display_setup()
{
    Wire.begin(OLED_SDA, OLED_SCL);
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS))
    {
        display_ok = true;
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 0);
        display.println("Air Hood");
        display.println("Starting...");
        display.display();
    }
    else
    {
        Serial.println("OLED init failed");
    }
}

void display_show_sensor_error()
{
    if (!display_ok)
    {
        return;
    }
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(0, 16);
    display.println("Sensor");
    display.setCursor(0, 40);
    display.println("fail");
    display.display();
}

void display_update(float temperature, float humidity, float baseline, bool fan_on, bool override_active)
{
    if (!display_ok)
    {
        return;
    }
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    draw_fan_icon(0, 0);
    if (fan_on)
    {
        draw_wind_trails(18, 4);
    }

    display.setTextSize(2);
    display.setCursor(40, 1);
    display.print(fan_on ? " ON" : "OFF");

    if (override_active)
    {
        display.setTextSize(1);
        display.setCursor(96, 1);
        display.print("MAN");
    }

    const bool temp_valid = !isnan(temperature);
    const bool hum_valid = !isnan(humidity);
    const bool base_valid = !isnan(baseline);

    display.setTextSize(1);
    display.setCursor(0, 22);
    display.print("Temperature: ");
    display.setCursor(90, 22);
    if (temp_valid)
    {
        display.print(round_to_int(temperature));
    }
    else
    {
        display.print("--");
    }
    // Small degree marker drawn manually to keep it compact
    display.drawCircle(105, 23, 1, SSD1306_WHITE);
    display.setCursor(108, 22);
    display.print("C");

    display.setCursor(0, 34);
    display.print("Humidity: ");
    display.setCursor(90, 34);
    if (hum_valid)
    {
        display.print(round_to_int(humidity));
    }
    else
    {
        display.print("--");
    }
    display.print(" %");

    display.setCursor(0, 46);
    display.print("Base Humidity: ");
    display.setCursor(90, 46);
    if (base_valid)
    {
        display.print(round_to_int(baseline));
    }
    else
    {
        display.print("--");
    }
    display.setTextSize(1);
    display.print(" %");

    display.display();
}
