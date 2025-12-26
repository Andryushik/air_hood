#ifndef WIFI_INFO_H_
#define WIFI_INFO_H_

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#elif defined(ESP32)
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#endif

#include <WiFiManager.h>

#ifndef WIFI_CONFIG_PORTAL_TIMEOUT
#define WIFI_CONFIG_PORTAL_TIMEOUT 180 // seconds
#endif

void wifi_connect()
{
	WiFi.persistent(false);
	WiFi.mode(WIFI_STA);

	WiFiManager wm;
	wm.setWiFiAutoReconnect(true);
	wm.setConfigPortalTimeout(WIFI_CONFIG_PORTAL_TIMEOUT);

	if (!wm.autoConnect("AirHood-Setup"))
	{
		Serial.println("WiFi config failed, restarting...");
		delay(1000);
#if defined(ESP8266) || defined(ESP32)
		ESP.restart();
#endif
	}

	Serial.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
}

#endif
