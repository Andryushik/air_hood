#include "RemoteLog.h"

RemoteLog rlog;
Print &debugOut = rlog; // LOG_D routes here (Serial + optional telnet client)

void RemoteLog::begin()
{
	_server.begin();
	_server.setNoDelay(true);
}

void RemoteLog::loop()
{
	// Accept a new client (one at a time). A fresh connection replaces any
	// prior one — one device with one logger is the realistic case.
	if (_server.hasClient())
	{
		WiFiClient incoming = _server.accept();
		if (_clientActive)
			_client.stop();
		_client = incoming;
		_clientActive = true;
		_client.setNoDelay(true);
		_client.printf("=== Air Hood fw=%s  ip=%s  reset=%s  uptime=%lus  (console %s %s) ===\r\n",
									 FW_VERSION,
									 WiFi.localIP().toString().c_str(),
									 ESP.getResetReason().c_str(),
									 millis() / 1000UL,
									 __DATE__, __TIME__);
	}

	// Reap a client that has closed so write() stops targeting a dead socket.
	if (_clientActive && !_client.connected())
	{
		_client.stop();
		_clientActive = false;
	}

	// Drain and discard any input from the client (keeps the RX buffer clear).
	if (_clientActive && _client.available())
	{
		while (_client.available())
			_client.read();
	}
}

bool RemoteLog::hasClient()
{
	return _clientActive && _client.connected();
}

// Non-blocking: only push to the telnet client when the TCP send buffer has room
// (availableForWrite() == tcp_sndbuf, which is 0 on a full or closed socket). A
// stalled/half-open peer therefore DROPS bytes instead of blocking the HomeKit
// loop for up to WiFiClient's 5s write timeout. Serial always gets the bytes.
size_t RemoteLog::write(uint8_t c)
{
	Serial.write(c);
	if (_clientActive && _client.availableForWrite() >= 1)
		_client.write(c);
	return 1;
}

size_t RemoteLog::write(const uint8_t *buffer, size_t size)
{
	Serial.write(buffer, size);
	if (_clientActive && _client.availableForWrite() >= (int)size)
		_client.write(buffer, size);
	return size;
}
