#!/usr/bin/env bash
# Compile and OTA-flash the Range Hood (ESP8266) over WiFi — no USB cable needed.
# Mirrors the shades_homekit_esp32 workflow (arduino-cli network upload / espota).
# espota writes ONLY the sketch region — the HomeKit pairing + LittleFS sectors
# are untouched, so OTA never un-pairs the device.
#
# Usage:
#   ./flash-release.sh                 # default IP 192.168.2.151
#   ./flash-release.sh 192.168.2.151   # explicit IP (use if DHCP changed it)
#
# The device does NOT advertise _arduino._tcp (ArduinoOTA mDNS is off so HomeKit
# keeps sole ownership of mDNS), so upload by IP. Find the IP from your router
# or the telnet console banner (./log-rangehood.sh).

set -e

PORT="${1:-192.168.2.151}"
FQBN="esp8266:esp8266:nodemcuv2:xtal=160,vt=flash,exception=disabled,stacksmash=disabled,ssl=all,mmu=3232,non32xfer=fast,eesz=4M2M,led=2,ip=lm2f,dbg=Disabled,lvl=None____,wipe=none,baud=115200"
OTA_PASSWORD="28142814"
BUILD_DIR="./build/release"

cd "$(dirname "$0")"

# The HomeKit library must carry docs/patches/homekit-storage-compact.patch, or the
# accessory eventually wipes its own pairing (see README "Required library patch").
HOMEKIT_STORAGE="$(arduino-cli config get directories.user)/libraries/Arduino-HomeKit-ESP8266/src/storage.c"
if ! grep -q 'homekit_storage_reset() <= 0' "$HOMEKIT_STORAGE"; then
  echo "ERROR: HomeKit library is not patched: $HOMEKIT_STORAGE" >&2
  echo "Apply: git -C \"$(dirname "$HOMEKIT_STORAGE")/..\" apply \"$PWD/docs/patches/homekit-storage-compact.patch\"" >&2
  exit 1
fi

echo "==> compile (release) @ 160MHz"
arduino-cli compile --fqbn "$FQBN" \
  --output-dir "$BUILD_DIR" \
  .

echo "==> OTA upload to $PORT via espota.py (sketch region only — pairing/FS untouched)"
# espota talks straight to the device IP:8266 — no mDNS needed (arduino-cli's
# --protocol network fails with 'port not found' since we run ArduinoOTA.begin(false)).
ESPOTA=$(ls "$HOME"/Library/Arduino15/packages/esp8266/hardware/esp8266/*/tools/espota.py 2>/dev/null | sort -V | tail -1)
python3 "$ESPOTA" -i "$PORT" -p 8266 -a "$OTA_PASSWORD" -f "$BUILD_DIR/air_hood.ino.bin" -r
