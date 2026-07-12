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

// Never gate the write on connected()/availableForWrite(): just attempt it and
// let write() return 0 on a bad socket. Disconnects are handled in loop().
size_t RemoteLog::write(uint8_t c)
{
	Serial.write(c);
	if (_clientActive)
		_client.write(c);
	return 1;
}

size_t RemoteLog::write(const uint8_t *buffer, size_t size)
{
	Serial.write(buffer, size);
	if (_clientActive)
		_client.write(buffer, size);
	return size;
}
