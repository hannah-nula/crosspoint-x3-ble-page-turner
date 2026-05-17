#!/usr/bin/env python3
"""Preflight pairfix1 before attempting to flash the X3."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BIN = str(Path(os.environ.get(
    "X3_BLE_FIRMWARE_BIN",
    str(Path.home() / "Downloads/crosspoint-x3-ble-pairfix1.bin"),
)))


def run_step(label: str, command: list[str], env: dict[str, str] | None = None) -> tuple[int, str]:
    print(f"== {label} ==")
    run_env = os.environ.copy()
    if env:
        run_env.update(env)
    result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, check=False, env=run_env)
    print(result.stdout.rstrip())
    print()
    return result.returncode, result.stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--local-only", action="store_true",
                        help="Run local artifact/source checks but do not require a visible X3 port.")
    args = parser.parse_args()

    failures: list[str] = []
    steps = [
        ("local pairfix1 verifier", [sys.executable, "scripts/verify_x3_ble_pairfix1.py"], None,
         "All local pairfix1 checks passed"),
        ("firmware image-info", [sys.executable, "scripts/inspect_x3_ble_firmware_image.py"],
         {"X3_BLE_IMAGE_LABEL": "pairfix1", "X3_BLE_FIRMWARE_BIN": BIN},
         "pairfix1 image-info check passed"),
        ("pairing flow", [sys.executable, "scripts/audit_x3_ble_pairing_flow.py"], None,
         "Pairing flow audit passed"),
        ("remote-sleep timing", [sys.executable, "scripts/simulate_x3_ble_reconnect_timing.py"], None,
         "idlefix15 intent"),
        ("reconnect invariants", [sys.executable, "scripts/audit_x3_ble_reconnect_invariants.py"], None,
         "All reconnect invariant checks passed"),
        ("guard behavior", [sys.executable, "scripts/audit_x3_ble_guard_behavior.py"], None,
         "All guard behavior checks passed"),
        ("remote sleep auto-reconnect", [sys.executable, "scripts/audit_x3_ble_remote_sleep_autoreconnect.py"], None,
         "All remote-sleep auto-reconnect source checks passed"),
        ("page-input path", [sys.executable, "scripts/audit_x3_ble_page_input_path.py"], None,
         "All page-input path checks passed"),
    ]

    for label, command, env, required_text in steps:
        code, output = run_step(label, command, env)
        if code != 0 or required_text not in output:
            failures.append(label)

    code, status_output = run_step("status", [sys.executable, "scripts/x3_ble_status.py"])
    if code != 0:
        failures.append("status")

    if failures:
        print("Preflight result: FAILED local checks: " + ", ".join(failures))
        return 1

    if args.local_only:
        print("Preflight result: LOCAL READY. Hardware port was not required in --local-only mode.")
        return 0

    if "port: X3 candidate" in status_output:
        print("Preflight result: READY TO FLASH.")
        print("Next: scripts/flash_record_x3_ble_pairfix1.sh")
        return 0

    print("Preflight result: BLOCKED. Local checks passed, but no X3 flash port is visible.")
    print("Connect the X3 or enter download mode, then rerun this preflight.")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
