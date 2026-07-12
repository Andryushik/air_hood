#!/usr/bin/env bash
# Same as flash-release.sh but compiles with -DAIRHOOD_DEBUG for extra-verbose
# logging (reserved for future gated diagnostics). The telnet console on port 23
# is always available in both builds. OTA over WiFi — no USB cable needed.
#
# Usage:
#   ./flash-debug.sh                 # default IP 192.168.2.151
#   ./flash-debug.sh 192.168.2.151   # explicit IP

set -e

PORT="${1:-192.168.2.151}"
FQBN="esp8266:esp8266:nodemcuv2:xtal=160,vt=flash,exception=disabled,stacksmash=disabled,ssl=all,mmu=3232,non32xfer=fast,eesz=4M2M,led=2,ip=lm2f,dbg=Disabled,lvl=None____,wipe=none,baud=115200"
OTA_PASSWORD="28142814"
BUILD_DIR="./build/debug"

cd "$(dirname "$0")"

echo "==> compile (debug -DAIRHOOD_DEBUG) @ 160MHz"
arduino-cli compile --fqbn "$FQBN" \
  --build-property "compiler.cpp.extra_flags=-DAIRHOOD_DEBUG" \
  --output-dir "$BUILD_DIR" \
  .

echo "==> OTA upload to $PORT via espota.py (sketch region only — pairing/FS untouched)"
# espota talks straight to the device IP:8266 — no mDNS needed (arduino-cli's
# --protocol network fails with 'port not found' since we run ArduinoOTA.begin(false)).
ESPOTA=$(ls "$HOME"/Library/Arduino15/packages/esp8266/hardware/esp8266/*/tools/espota.py 2>/dev/null | sort -V | tail -1)
python3 "$ESPOTA" -i "$PORT" -p 8266 -a "$OTA_PASSWORD" -f "$BUILD_DIR/air_hood.ino.bin" -r
