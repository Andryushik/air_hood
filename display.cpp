#include "display.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

static Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
static bool display_ok = false;
static bool display_on = true;
static bool display_dimmed = false;
static uint32_t display_last_activity = 0;

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

// WiFi signal icon: 3 arcs + dot, or crossed-out WiFi (blinking) when disconnected.
static void draw_wifi_icon(int16_t x, int16_t y, int16_t rssi)
{
    if (rssi == 0)
    {
        // Disconnected: draw WiFi shape with a cross-out line, blinks every other refresh
        if (((millis() / 500UL) % 2UL) != 0)
        {
            return; // hidden phase of blink
        }
        // Draw the WiFi arcs (greyed out / static shape)
        display.fillRect(x + 4, y + 8, 2, 2, SSD1306_WHITE);
        display.drawCircleHelper(x + 5, y + 9, 4, 0x1, SSD1306_WHITE);
        display.drawCircleHelper(x + 5, y + 9, 7, 0x1, SSD1306_WHITE);
        // Diagonal cross-out line
        display.drawLine(x, y + 9, x + 10, y, SSD1306_WHITE);
        return;
    }
    // Base dot (always shown when connected)
    display.fillRect(x + 4, y + 8, 2, 2, SSD1306_WHITE);
    // 1 bar: RSSI > -80
    if (rssi > -80)
    {
        display.drawCircleHelper(x + 5, y + 9, 4, 0x1, SSD1306_WHITE);
    }
    // 2 bars: RSSI > -65
    if (rssi > -65)
    {
        display.drawCircleHelper(x + 5, y + 9, 7, 0x1, SSD1306_WHITE);
    }
    // 3 bars: RSSI > -50
    if (rssi > -50)
    {
        display.drawCircleHelper(x + 5, y + 9, 10, 0x1, SSD1306_WHITE);
    }
}

void display_setup()
{
    Wire.begin(OLED_SDA, OLED_SCL);
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS))
    {
        display_ok = true;
        display_last_activity = millis();
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

void display_update(float temperature,
                    float humidity,
                    float humidity_baseline,
                    float temperature_baseline,
                    bool fan_on,
                    bool override_active,
                    int16_t wifi_rssi)
{
    if (!display_ok || !display_on)
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

    // Top-right corner: MAN override or WiFi status
    if (override_active)
    {
        display.setTextSize(1);
        display.setCursor(96, 1);
        display.print("MAN");
    }
    else
    {
        draw_wifi_icon(116, 1, wifi_rssi);
    }

    const bool temp_valid = !isnan(temperature);
    const bool hum_valid = !isnan(humidity);
    const bool base_h_valid = !isnan(humidity_baseline);
    const bool base_t_valid = !isnan(temperature_baseline);

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

    display.setCursor(0, 52);
    display.print("Base T:");
    display.setCursor(44, 52);
    if (base_t_valid)
    {
        display.print(round_to_int(temperature_baseline));
    }
    else
    {
        display.print("--");
    }
    {
        const int16_t x = display.getCursorX();
        const int16_t y = display.getCursorY();
        display.drawCircle(x + 3, y + 1, 1, SSD1306_WHITE);
        display.setCursor(x + 6, y);
        display.print("C");
    }

    display.setCursor(76, 52);
    display.print("H:");
    display.setCursor(90, 52);
    if (base_h_valid)
    {
        display.print(round_to_int(humidity_baseline));
    }
    else
    {
        display.print("--");
    }
    display.print(" %");

    display.display();
}

void display_wake()
{
    if (!display_ok)
    {
        return;
    }
    display_last_activity = millis();
    if (!display_on)
    {
        display.ssd1306_command(SSD1306_DISPLAYON);
        display_on = true;
    }
    if (display_dimmed)
    {
        display.ssd1306_command(SSD1306_SETCONTRAST);
        display.ssd1306_command(0xCF); // default contrast
        display_dimmed = false;
    }
}

void display_check_timeout(uint32_t now, bool fan_on)
{
    if (!display_ok || fan_on)
    {
        // Keep display active while fan is running
        if (fan_on)
        {
            display_last_activity = now;
        }
        return;
    }

    const int32_t idle = (int32_t)(now - display_last_activity);
    if (idle < 0)
    {
        return;
    }

    if (display_on && !display_dimmed && idle >= (int32_t)DISPLAY_DIM_MS)
    {
        display.ssd1306_command(SSD1306_SETCONTRAST);
        display.ssd1306_command(DISPLAY_DIM_CONTRAST);
        display_dimmed = true;
    }

    if (display_on && idle >= (int32_t)DISPLAY_OFF_MS)
    {
        display.ssd1306_command(SSD1306_DISPLAYOFF);
        display_on = false;
    }
}
