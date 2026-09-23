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

HOST="${1:-192.168.2.151}"
OTA_PASSWORD="28142814"
BUILD_DIR="./build/release" # its own build dir, so the fw_version.h stamp never leaks into other builds

cd "$(dirname "$0")"

# The HomeKit library must carry docs/patches/homekit-storage-compact.patch, or the
# accessory eventually wipes its own pairing (see README "Required library patch").
HOMEKIT_STORAGE="$(arduino-cli config get directories.user)/libraries/Arduino-HomeKit-ESP8266/src/storage.c"
if ! grep -q 'homekit_storage_reset() <= 0' "$HOMEKIT_STORAGE"; then
  echo "ERROR: HomeKit library is not patched: $HOMEKIT_STORAGE" >&2
  echo "Apply: git -C \"$(dirname "$HOMEKIT_STORAGE")/..\" apply \"$PWD/docs/patches/homekit-storage-compact.patch\"" >&2
  exit 1
fi

# Stamp the build: date.time-commit, plus "-dirty" when tracked files have uncommitted
# changes. fw_version.h exists only for this compile, so IDE builds report "dev".
FW_VERSION="$(date +%Y-%m-%d.%H%M)-$(git rev-parse --short HEAD)"
git diff --quiet HEAD || FW_VERSION="$FW_VERSION-dirty"
trap 'rm -f fw_version.h' EXIT
printf '#define FW_VERSION_STAMP "%s"\n' "$FW_VERSION" > fw_version.h

echo "==> compile $FW_VERSION (board from sketch.yaml)"
arduino-cli compile --build-path "$BUILD_DIR" .

echo "==> OTA upload to $HOST via espota.py (sketch region only — pairing/FS untouched)"
# espota talks straight to the device IP:8266 — no mDNS needed (arduino-cli's
# --protocol network fails with 'port not found' since we run ArduinoOTA.begin(false)).
ESPOTA=$(ls "$HOME"/Library/Arduino15/packages/esp8266/hardware/esp8266/*/tools/espota.py 2>/dev/null | sort -V | tail -1)
if [ -z "$ESPOTA" ]; then
  echo "ERROR: espota.py not found under ~/Library/Arduino15 (is the esp8266 core installed?)" >&2
  exit 1
fi
python3 "$ESPOTA" -i "$HOST" -p 8266 -a "$OTA_PASSWORD" -f "$BUILD_DIR/air_hood.ino.bin" -r
