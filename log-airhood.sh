#!/usr/bin/env bash
# Telnet debug-console capturer with auto-reconnect (mirrors log-shades.sh).
# Streams the Air Hood's LOG_D output (port 23) to airhood.log next to this file.
#
# Usage:
#   ./log-airhood.sh                  # default: 192.168.2.151
#   ./log-airhood.sh <host-or-ip>
#
# Detached:
#   nohup ./log-airhood.sh > /dev/null 2>&1 &
#   disown
#   tail -f airhood.log

PORT=23
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$SCRIPT_DIR/airhood.log"
HOST="${1:-192.168.2.151}"
TAG="airhood"

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
