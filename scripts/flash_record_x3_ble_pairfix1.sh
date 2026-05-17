#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LOG_DIR="${X3_FLASH_LOG_DIR:-$HOME/Downloads/x3-ble-pairfix1-flash-logs}"
mkdir -p "$LOG_DIR"

timestamp="$(date +%Y%m%d-%H%M%S)"
log_file="$LOG_DIR/pairfix1-flash-${timestamp}.log"

"$SCRIPT_DIR/flash_x3_ble_pairfix1.sh" 2>&1 | tee "$log_file"

X3_BLE_VALIDATION_DOC="$ROOT_DIR/docs/x3-ble-pairfix1-validation.md" \
  python3 "$SCRIPT_DIR/record_x3_ble_validation_result.py" \
    "Flash" passed "app0 app1 write and verify-flash succeeded; log=$log_file"
