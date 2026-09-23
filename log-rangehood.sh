#!/usr/bin/env bash
# Telnet debug-console capturer with auto-reconnect (mirrors log-shades.sh).
# Streams the Range Hood's LOG_D output (port 23) to rangehood.log next to this file.
#
# Usage:
#   ./log-rangehood.sh                  # default: 192.168.2.151
#   ./log-rangehood.sh <host-or-ip>
#
# Detached:
#   nohup ./log-rangehood.sh > /dev/null 2>&1 &
#   disown
#   tail -f rangehood.log

PORT=23
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$SCRIPT_DIR/rangehood.log"
HOST="${1:-192.168.2.151}"
TAG="rangehood"

while true; do
  printf '[%s][%s] === connecting to %s:%s ===\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$TAG" "$HOST" "$PORT" >> "$LOG"
  # -w 15: drop if no data for 15 s. The device sends a heartbeat every 5 s
  # while a client is attached, so 15 s of silence = dead peer -> reconnect.
  nc -w 15 "$HOST" "$PORT" 2>&1 | while IFS= read -r line; do
    printf '[%s][%s] %s\n' "$(date '+%H:%M:%S')" "$TAG" "$line" >> "$LOG"
  done
  printf '[%s][%s] === disconnected, reconnecting in 3s ===\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$TAG" >> "$LOG"
  sleep 3
done
