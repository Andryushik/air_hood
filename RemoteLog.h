#ifndef REMOTE_LOG_H
#define REMOTE_LOG_H

#include <Arduino.h>
#include <ESP8266WiFi.h>

// Telnet debug console — mirrors all LOG_D output to Serial AND a single
// connected telnet client (port 23). Non-blocking: attempts the write and
// discards bytes if the socket is not ready; never stalls the HomeKit loop.
// Ported from shades_homekit_esp32/RemoteLog (ESP32) to the ESP8266 stack.
// (ESP32-only TCP-keepalive setsockopt tuning dropped; liveness is handled by
//  a 5 s app heartbeat + `nc -w 15` in log-airhood.sh.)
class RemoteLog : public Print
{
public:
	void begin();
	void loop(); // call from main loop — accepts one client, drains input
	size_t write(uint8_t c) override;
	size_t write(const uint8_t *buffer, size_t size) override;
	bool hasClient();

private:
	WiFiServer _server{23};
	WiFiClient _client;
	bool _clientActive = false;
};

extern RemoteLog rlog;
extern Print &debugOut;        // LOG_D target (Serial + optional telnet)
extern const char *FW_VERSION; // defined in air_hood.ino; shown in the console banner

#endif // REMOTE_LOG_H
