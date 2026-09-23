#ifndef WIFI_INFO_H_
#define WIFI_INFO_H_

#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>

#include <WiFiManager.h>

#ifndef WIFI_CONFIG_PORTAL_TIMEOUT
#define WIFI_CONFIG_PORTAL_TIMEOUT 180 // seconds
#endif

static inline void wifi_connect()
{
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleepMode(WIFI_NONE_SLEEP); // keep radio awake — HomeKit needs low latency

  WiFiManager wm;
  wm.setWiFiAutoReconnect(true);
  wm.setConfigPortalTimeout(WIFI_CONFIG_PORTAL_TIMEOUT);

  if (!wm.autoConnect("RangeHood-Setup"))
  {
    Serial.println("WiFi config failed, restarting...");
    delay(1000);
    ESP.restart();
  }

  WiFi.setSleepMode(WIFI_NONE_SLEEP); // re-assert; WiFiManager may have toggled it
  Serial.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
}

#endif
