#!/usr/bin/env bash
set -euo pipefail

FIRMWARE_DIR="${FIRMWARE_DIR:-dist/firmware}"
PORT="${ESPPORT:-/dev/ttyACM0}"
BAUD="${ESPBAUD:-460800}"

# Find esptool executable
ESPTOOL_BIN=""
if command -v esptool.py >/dev/null 2>&1; then
    ESPTOOL_BIN="esptool.py"
elif command -v esptool >/dev/null 2>&1; then
    ESPTOOL_BIN="esptool"
elif python -m esptool version >/dev/null 2>&1; then
    ESPTOOL_BIN="python -m esptool"
else
    echo "Error: esptool not found." >&2
    echo "Install it via mise: mise use -g pipx:esptool" >&2
    exit 1
fi

# Locate the 3 required binaries
BOOTLOADER=$(find "$FIRMWARE_DIR" -name "bootloader.bin" 2>/dev/null | head -n 1)
PARTITION=$(find "$FIRMWARE_DIR" -name "partition-table.bin" 2>/dev/null | head -n 1)
APP=$(find "$FIRMWARE_DIR" -name "nano-esp-bot-hw.bin" 2>/dev/null | head -n 1)

if [ -z "$BOOTLOADER" ] || [ -z "$PARTITION" ] || [ -z "$APP" ]; then
    echo "Error: Firmware binaries not found in '$FIRMWARE_DIR'." >&2
    echo "Please download the latest firmware from CI first via: mise run hw:download" >&2
    exit 1
fi

echo "==> Flashing ESP32-S3 firmware via $ESPTOOL_BIN..."
echo "  Port:       $PORT"
echo "  Baud:       $BAUD"
echo "  Bootloader: $BOOTLOADER (0x0)"
echo "  Partition:  $PARTITION (0x8000)"
echo "  App:        $APP (0x10000)"

# shellcheck disable=SC2086
$ESPTOOL_BIN --chip esp32s3 -p "$PORT" -b "$BAUD" \
    --before default_reset --after hard_reset \
    write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
    0x0 "$BOOTLOADER" \
    0x8000 "$PARTITION" \
    0x10000 "$APP"

echo "==> Flash succeeded! You can monitor the device using: mise run hw:monitor"
