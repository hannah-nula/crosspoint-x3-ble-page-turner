#!/usr/bin/env python3
"""Audit the X3 BLE first-pairing flow without hardware.

This checks the bug class that made the public idlefix15 build confusing for new
users: the settings menu exposed a disabled scan item, so first-time pairing was
unreachable. The fix must keep pairing on the same bounded worker pattern used
for stable reconnect instead of returning to the older UI-blocking scan path.
"""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str, failures: list[str]) -> None:
    if condition:
        print(f"OK: {message}")
    else:
        print(f"FAIL: {message}")
        failures.append(message)


def contains(path: str, needle: str) -> bool:
    return needle in read(path)


def main() -> int:
    failures: list[str] = []
    ui = read("src/activities/settings/BluetoothSettingsActivity.cpp")
    mgr = read("lib/hal/BluetoothHIDManager.cpp")
    hdr = read("lib/hal/BluetoothHIDManager.h")
    main_cpp = read("src/main.cpp")

    require("Pair New Remote" in ui, "settings menu exposes Pair New Remote", failures)
    require("Scan Disabled" not in ui and "Scan disabled" not in ui,
            "disabled scan menu text is removed from the UI", failures)
    require("btMgr->startPairNewRemote(12000)" in ui,
            "Pair New Remote starts the bounded manager worker", failures)
    require("pairJobPending" in ui and "pollBleJob" in ui,
            "pairing uses the nonblocking settings status poll", failures)
    require("getBondedDeviceAddress()" in ui and "getBondedDeviceName()" in ui and
            "getBondedDeviceAddressType()" in ui,
            "successful pairing persists the manager-selected bond to settings", failures)
    require("DeviceProfiles::clearCustomProfileForDevice(oldBondedAddr)" in ui,
            "forgetting a remote clears its per-device learned mapping", failures)

    require("bool startPairNewRemote" in hdr and "bool isPairingInProgress()" in hdr,
            "manager exposes explicit pairing job APIs", failures)
    require("_reconnectJobPairNew" in hdr,
            "pairing is tracked separately from automatic reconnect intent", failures)
    require('xTaskCreate(&BluetoothHIDManager::bondedReconnectTaskEntry, "bt_pair"' in mgr,
            "pairing runs in the BLE worker task", failures)
    require("HalPowerManager::Lock reconnectPowerLock;" in mgr,
            "BLE worker holds normal CPU speed during scan/connect", failures)
    require("pairNew ? scanForPairingCandidate(candidate, scanMs) : scanForBondedReconnectCandidate(candidate, scanMs)" in mgr,
            "worker dispatches pairing and reconnect scans explicitly", failures)
    require("connectToDevice(candidate.address, timeoutMs, candidate.addressType, candidate.name)" in mgr,
            "pairing passes the discovered device name into connect heuristics", failures)
    require("_bondedDeviceName = candidate.name.empty() ? \"Unknown\" : candidate.name;" in mgr,
            "pairing cannot accidentally keep the old bonded device name", failures)
    require("armAutoReconnect(pairNew ? \"pair_new_success\"" in mgr,
            "successful pairing arms the stable bonded auto-reconnect path", failures)

    require("pScan->setActiveScan(true)" in mgr and "pScan->setDuplicateFilter(false)" in mgr and
            "pScan->setScanResponseTimeout(BLE_MANUAL_SCAN_RESPONSE_TIMEOUT_MS)" in mgr,
            "first pairing uses finite active scan with scan-response support", failures)
    require("if (!_bondedDeviceAddress.empty() && device.address == _bondedDeviceAddress)" in mgr,
            "pair-new candidate selection skips the currently saved remote", failures)
    require("hidCandidateCount == 1" in mgr,
            "unknown-name fallback only pairs a single HID candidate", failures)
    require("Multiple HID remotes found; retry closer" in mgr,
            "ambiguous HID scans fail safe instead of pairing an arbitrary device", failures)

    require("constexpr uint32_t BLE_MANUAL_RECONNECT_SCAN_MS = 45000;" in mgr,
            "known-good manual reconnect scan duration remains unchanged", failures)
    require("constexpr uint32_t BLE_MANUAL_RECONNECT_TIMEOUT_MS = 12000;" in mgr,
            "known-good manual reconnect connect timeout remains unchanged", failures)
    require("btMgr->startBondedReconnect(12000)" in ui,
            "manual reconnect still enters the original reconnect worker", failures)
    require("startBondedReconnect(BLE_MANUAL_RECONNECT_TIMEOUT_MS, true)" in mgr,
            "automatic reconnect still uses the reconnect worker, not pair-new", failures)
    require("bleActiveWork = btMgr.isBondedReconnectInProgress();" in main_cpp,
            "main loop still keeps BLE worker jobs at normal power", failures)

    if failures:
      print(f"\nPairing flow audit failed with {len(failures)} issue(s).")
      return 1

    print("\nPairing flow audit passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
