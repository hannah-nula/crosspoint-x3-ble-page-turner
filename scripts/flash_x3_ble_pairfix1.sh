#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN_PATH="${X3_BLE_FIRMWARE_BIN:-$HOME/Downloads/crosspoint-x3-ble-pairfix1.bin}"

exec "$SCRIPT_DIR/flash_x3_ble_guardedauto.sh" "$BIN_PATH"
