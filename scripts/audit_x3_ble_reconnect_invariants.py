#!/usr/bin/env python3
"""Static reconnect-path checks for the experimental X3 BLE build.

These checks do not replace hardware validation. They guard the source-level
ordering that matters for the Free3 page-turner auto-reconnect path.
"""

from __future__ import annotations

import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANAGER = ROOT / "lib/hal/BluetoothHIDManager.cpp"


def require(condition: bool, message: str, failures: list[str]) -> None:
    if condition:
        print(f"ok - {message}")
    else:
        print(f"FAIL - {message}")
        failures.append(message)


def section(text: str, start: str, end: str) -> str:
    start_idx = text.find(start)
    if start_idx < 0:
        return ""
    end_idx = text.find(end, start_idx + len(start))
    if end_idx < 0:
        return text[start_idx:]
    return text[start_idx:end_idx]


def appears_in_order(text: str, needles: list[str]) -> bool:
    cursor = 0
    for needle in needles:
        idx = text.find(needle, cursor)
        if idx < 0:
            return False
        cursor = idx + len(needle)
    return True


def main() -> int:
    source = MANAGER.read_text(encoding="utf-8")
    failures: list[str] = []

    update_activity = section(source, "void BluetoothHIDManager::updateActivity()", "void BluetoothHIDManager::checkAutoReconnect")
    disconnect = section(source, "bool BluetoothHIDManager::disconnectFromDevice", "bool BluetoothHIDManager::isConnected")
    queue_disconnect = section(source, "void BluetoothHIDManager::onClientDisconnect", "void BluetoothHIDManager::processConnectionEvents")
    process_events = section(source, "void BluetoothHIDManager::processConnectionEvents()", "void BluetoothHIDManager::processInputEvents")
    reconnect_task = section(source, "void BluetoothHIDManager::runBondedReconnectTask()", "bool BluetoothHIDManager::scanForBondedReconnectCandidate")
    auto_reconnect = section(source, "void BluetoothHIDManager::checkAutoReconnect", "void BluetoothHIDManager::saveState")

    require(appears_in_order(update_activity, [
        "processConnectionEvents();",
        "const unsigned long timeoutMs = pageTurnerLike ? BLE_PAGE_TURNER_IDLE_RECONNECT_MS : INACTIVITY_TIMEOUT_MS;",
        "disconnectFromDevice(inactiveAddress)",
        "enterPageTurnerReconnectMode(\"page_turner_idle\", true)",
        "armAutoReconnect(inactiveIsPageTurner ? \"page_turner_idle\" : \"idle_timeout\")",
    ]), "idle cleanup processes queued disconnects first, then re-arms page-turner reconnect", failures)

    require(appears_in_order(disconnect, [
        "_autoReconnectArmed = false;",
        "BluetoothDiagnostics::recordf(\"auto_reconnect_disarmed\"",
        "markIntentionalDisconnect(address);",
        "client->disconnect();",
    ]), "explicit disconnect suppresses callback auto-rearm while disconnecting", failures)

    require("event.suppressAutoReconnect = consumeIntentionalDisconnect(address);" in queue_disconnect,
            "disconnect callback records intentional-disconnect suppress flag", failures)

    require(appears_in_order(process_events, [
        "if (matchedBondedDevice && !event.suppressAutoReconnect)",
        "enterPageTurnerReconnectMode(\"disconnect_event\", true)",
        "armAutoReconnect(\"disconnect_event\")",
    ]), "non-suppressed bonded disconnect events re-arm page-turner reconnect", failures)

    require(appears_in_order(reconnect_task, [
        "scanForBondedReconnectCandidate(candidate, scanMs)",
        "success = connectToDevice(candidate.address, timeoutMs, candidate.addressType, candidate.name);",
        "if (success)",
        "_bondedDeviceAddress = candidate.address;",
        "_bondedDeviceAddressType = candidate.addressType;",
        "armAutoReconnect(pairNew ? \"pair_new_success\"",
        "\"manual_reconnect_success\")",
    ]), "successful reconnect updates bonded address/type before re-arming reconnect", failures)

    require(appears_in_order(auto_reconnect, [
        "if (hasConnectedDevices)",
        "return;",
        "const bool pageTurnerLostWindowActive =",
        "const bool pageTurnerFastWindowActive =",
        "const unsigned long pageTurnerInterval = pageTurnerFastWindowActive ? BLE_PAGE_TURNER_RECONNECT_INTERVAL_MS",
        "requiredInterval = pageTurnerInterval;",
        "startBondedReconnect(BLE_MANUAL_RECONNECT_TIMEOUT_MS, true)",
    ]), "auto reconnect waits for disconnected state and uses page-turner cadence", failures)

    if failures:
        print(f"\n{len(failures)} reconnect invariant check(s) failed.")
        return 1

    print("\nAll reconnect invariant checks passed. Hardware validation is still required.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
