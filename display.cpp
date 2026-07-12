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

// Skip the ~25ms full-frame I2C push when nothing visible changed (cuts the
// periodic HomeKit latency blip). Reset by display_wake() so waking repaints.
static bool s_force_redraw = true;
struct RenderState
{
    bool valid;
    bool fan_on, override_active;
    bool t_valid, h_valid, bt_valid, bh_valid;
    int16_t t, h, bt, bh;
    int8_t wifi_bars; // -1=disconnected, 0..3=bars, 99=hidden (MAN override)
};
static RenderState s_last = {false, false, false, false, false, false, false, 0, 0, 0, 0, 0};

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
        // Disconnected: static WiFi shape with a cross-out line.
        // (display_update refreshes only ~every 30s, so the old millis()-based
        //  blink never actually toggled — draw it steadily instead.)
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
    Wire.setClockStretchLimit(2000); // cap a wedged-bus stall at ~2ms
    if (display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS))
    {
        display_ok = true;
        display_last_activity = millis();
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 0);
        display.println("Range Hood");
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
    display_wake(); // ensure the panel is on so the error is actually visible
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

    RenderState cur;
    cur.valid = true;
    cur.fan_on = fan_on;
    cur.override_active = override_active;
    cur.t_valid = !isnan(temperature);
    cur.h_valid = !isnan(humidity);
    cur.bt_valid = !isnan(temperature_baseline);
    cur.bh_valid = !isnan(humidity_baseline);
    cur.t = cur.t_valid ? round_to_int(temperature) : 0;
    cur.h = cur.h_valid ? round_to_int(humidity) : 0;
    cur.bt = cur.bt_valid ? round_to_int(temperature_baseline) : 0;
    cur.bh = cur.bh_valid ? round_to_int(humidity_baseline) : 0;
    if (override_active)
        cur.wifi_bars = 99; // WiFi icon hidden while MAN shown; ignore rssi changes
    else if (wifi_rssi == 0)
        cur.wifi_bars = -1;
    else if (wifi_rssi > -50)
        cur.wifi_bars = 3;
    else if (wifi_rssi > -65)
        cur.wifi_bars = 2;
    else if (wifi_rssi > -80)
        cur.wifi_bars = 1;
    else
        cur.wifi_bars = 0;

    if (!s_force_redraw && s_last.valid &&
        cur.fan_on == s_last.fan_on && cur.override_active == s_last.override_active &&
        cur.t_valid == s_last.t_valid && cur.h_valid == s_last.h_valid &&
        cur.bt_valid == s_last.bt_valid && cur.bh_valid == s_last.bh_valid &&
        cur.t == s_last.t && cur.h == s_last.h &&
        cur.bt == s_last.bt && cur.bh == s_last.bh &&
        cur.wifi_bars == s_last.wifi_bars)
    {
        return; // nothing visible changed — skip the full-frame I2C push
    }
    s_last = cur;
    s_force_redraw = false;

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
    s_force_redraw = true; // panel may have been off/stale — repaint on next update
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
