#!/usr/bin/env python3
"""Verify local X3 BLE pairfix1 source and firmware invariants."""

from __future__ import annotations

import hashlib
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BIN = Path(os.environ.get(
    "X3_BLE_FIRMWARE_BIN",
    str(Path.home() / "Downloads/crosspoint-x3-ble-pairfix1.bin"),
))
BUILD_BIN = ROOT / ".pio/build/gh_release/firmware.bin"
APP_PARTITION_SIZE = 0x640000


def read(path: Path, binary: bool = False):
    mode = "rb" if binary else "r"
    with path.open(mode, encoding=None if binary else "utf-8") as handle:
        return handle.read()


def contains(path: str, needle: str) -> bool:
    return needle in read(ROOT / path)


def require(condition: bool, message: str, failures: list[str]) -> None:
    if condition:
        print(f"ok - {message}")
    else:
        print(f"FAIL - {message}")
        failures.append(message)


def command_ok(command: list[str], required_output: str | None = None) -> bool:
    result = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, check=False)
    if result.returncode != 0:
        print(result.stdout.rstrip())
        return False
    if required_output is not None and required_output not in result.stdout:
        print(result.stdout.rstrip())
        return False
    return True


def main() -> int:
    failures: list[str] = []

    require(contains("platformio.ini", "version = 1.2.0-x3-ble-pairfix1"),
            "platformio version is pairfix1", failures)
    require(contains("src/activities/settings/BluetoothSettingsActivity.cpp", "Pair New Remote"),
            "Bluetooth settings expose Pair New Remote", failures)
    require(not contains("src/activities/settings/BluetoothSettingsActivity.cpp", "Scan Disabled"),
            "disabled scan menu text is absent", failures)
    require(contains("lib/hal/BluetoothHIDManager.cpp", "bool BluetoothHIDManager::startPairNewRemote"),
            "manager implements startPairNewRemote", failures)
    require(contains("lib/hal/BluetoothHIDManager.cpp", "scanForPairingCandidate"),
            "manager implements a dedicated pairing scan", failures)
    require(contains("lib/hal/BluetoothHIDManager.cpp", "startBondedReconnect(BLE_MANUAL_RECONNECT_TIMEOUT_MS, true)"),
            "automatic reconnect still uses the reconnect worker", failures)

    require(command_ok([sys.executable, "scripts/audit_x3_ble_pairing_flow.py"], "Pairing flow audit passed"),
            "pairing flow audit passes", failures)
    require(command_ok([sys.executable, "scripts/audit_x3_ble_reconnect_invariants.py"],
                       "All reconnect invariant checks passed"),
            "reconnect invariant audit passes", failures)
    require(command_ok([sys.executable, "scripts/audit_x3_ble_guard_behavior.py"],
                       "All guard behavior checks passed"),
            "guard behavior audit passes", failures)
    require(command_ok([sys.executable, "scripts/audit_x3_ble_remote_sleep_autoreconnect.py"],
                       "All remote-sleep auto-reconnect source checks passed"),
            "remote sleep auto-reconnect audit passes", failures)
    require(command_ok([sys.executable, "scripts/audit_x3_ble_page_input_path.py"],
                       "All page-input path checks passed"),
            "page-input path audit passes", failures)

    require(BUILD_BIN.exists(), f"PlatformIO build output exists: {BUILD_BIN}", failures)
    if BUILD_BIN.exists():
        build_data = read(BUILD_BIN, binary=True)
        require(b"1.2.0-x3-ble-pairfix1" in build_data,
                "build output contains pairfix1 version marker", failures)
        require(len(build_data) < APP_PARTITION_SIZE,
                "build output fits the 0x640000 app partition", failures)

    if BIN.exists():
        data = read(BIN, binary=True)
        actual_sha = hashlib.sha256(data).hexdigest()
        require(b"1.2.0-x3-ble-pairfix1" in data,
                "packaged firmware contains pairfix1 version marker", failures)
        require(len(data) < APP_PARTITION_SIZE,
                "packaged firmware fits the 0x640000 app partition", failures)
        print(f"info - packaged firmware SHA-256: {actual_sha}")
        print(f"info - packaged firmware size: 0x{len(data):x}")
        if BUILD_BIN.exists():
            require(data == read(BUILD_BIN, binary=True),
                    "packaged firmware matches PlatformIO build output byte-for-byte", failures)
    else:
        print(f"note - packaged firmware not found yet: {BIN}")

    require(contains("README.md", "crosspoint-x3-ble-pairfix1.bin"),
            "README references the pairfix1 binary", failures)
    require(contains("docs/x3-ble-page-turner.md", "Pair New Remote"),
            "technical docs describe Pair New Remote", failures)
    require(contains("docs/x3-ble-pairfix1-validation.md", "Pairfix1 Validation"),
            "pairfix1 validation doc exists", failures)
    require(contains("RELEASE_NOTES.md", "1.2.0-x3-ble-pairfix1"),
            "release notes are pairfix1", failures)

    if failures:
        print(f"\nPairfix1 verification failed with {len(failures)} issue(s).")
        return 1

    print("\nAll local pairfix1 checks passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
