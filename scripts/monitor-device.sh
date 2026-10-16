#!/usr/bin/env bash
set -euo pipefail

PORT="${ESPPORT:-/dev/ttyACM0}"
BAUD="${ESPBAUD:-115200}"

if command -v idf.py >/dev/null 2>&1; then
    exec idf.py -p "$PORT" monitor
elif python -m serial.tools.miniterm --help >/dev/null 2>&1; then
    exec python -m serial.tools.miniterm "$PORT" "$BAUD"
elif command -v picocom >/dev/null 2>&1; then
    exec picocom -b "$BAUD" "$PORT"
elif command -v minicom >/dev/null 2>&1; then
    exec minicom -D "$PORT" -b "$BAUD"
else
    echo "Error: No serial monitor tool found (idf.py, pyserial miniterm, picocom, or minicom)." >&2
    echo "Install pyserial via pip or use mise: mise use -g pipx:pyserial" >&2
    exit 1
fi
