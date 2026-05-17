#!/usr/bin/env python3
"""Static guard-behavior checks for the X3 BLE auto-reconnect build.

These checks prove the source-level safety shape of the reconnect crash guard.
They do not replace the real X3/Free3 hardware validation gates.
"""

from __future__ import annotations

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MANAGER = ROOT / "lib/hal/BluetoothHIDManager.cpp"
SETTINGS = ROOT / "src/activities/settings/BluetoothSettingsActivity.cpp"


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
    settings = SETTINGS.read_text(encoding="utf-8")
    failures: list[str] = []

    reset_reason = section(source, "static bool resetReasonSuggestsCrash", "static bool containsCaseInsensitive")
    initialize_guard = section(source, "void BluetoothHIDManager::initializeAutoReconnectGuard", "void BluetoothHIDManager::setAutoReconnectGuard")
    set_guard = section(source, "void BluetoothHIDManager::setAutoReconnectGuard", "void BluetoothHIDManager::setAutoReconnectWakeRequest")
    arm_reconnect = section(source, "void BluetoothHIDManager::armAutoReconnect", "void BluetoothHIDManager::markIntentionalDisconnect")
    enable = section(source, "bool BluetoothHIDManager::enable", "bool BluetoothHIDManager::disable")
    start_reconnect = section(source, "bool BluetoothHIDManager::startBondedReconnect", "BluetoothReconnectStatus BluetoothHIDManager::getReconnectStatus")
    reconnect_task = section(source, "void BluetoothHIDManager::runBondedReconnectTask", "bool BluetoothHIDManager::scanForBondedReconnectCandidate")
    auto_reconnect = section(source, "void BluetoothHIDManager::checkAutoReconnect", "void BluetoothHIDManager::saveState")
    settings_select = section(settings, "selectedIndex == kReconnectBondedIndex", "selectedIndex == kDisconnectDevicesIndex")

    require(all(marker in reset_reason for marker in [
        "ESP_RST_PANIC",
        "ESP_RST_INT_WDT",
        "ESP_RST_TASK_WDT",
        "ESP_RST_WDT",
        "ESP_RST_CPU_LOCKUP",
    ]), "crash-like reset reasons include panic, watchdog, and CPU lockup", failures)

    require(appears_in_order(initialize_guard, [
        "_autoReconnectGuardPresent = Storage.exists(BLE_AUTO_RECONNECT_GUARD_FILE)",
        "if (_autoReconnectGuardPresent || resetReasonSuggestsCrash(resetReason))",
        "_autoReconnectDisabledThisBoot = true",
        "_autoReconnectArmed = false",
        "auto_reconnect_guard_disable",
    ]), "guard initialization disables automatic reconnect after guard file or crash reset", failures)

    require(appears_in_order(set_guard, [
        "if (active)",
        "Storage.writeFile(BLE_AUTO_RECONNECT_GUARD_FILE, content)",
        "_autoReconnectGuardPresent = true",
        "Storage.remove(BLE_AUTO_RECONNECT_GUARD_FILE)",
        "_autoReconnectGuardPresent = false",
    ]), "guard file is written for active auto reconnect and removed on clean finish", failures)

    require(appears_in_order(arm_reconnect, [
        "if (_autoReconnectDisabledThisBoot && !clearCrashGuard)",
        "auto_reconnect_arm_blocked",
        "return;",
        "_autoReconnectArmed = true",
        "if (clearCrashGuard)",
        "_autoReconnectDisabledThisBoot = false",
        "setAutoReconnectGuard(false)",
    ]), "automatic arming is blocked by the guard until a manual-success clear is requested", failures)

    require(appears_in_order(enable, [
        "initializeAutoReconnectGuard();",
        "const bool wakeRequest = consumeAutoReconnectWakeRequest();",
        "if (wakeRequest && !_autoReconnectDisabledThisBoot)",
        "armAutoReconnect(\"wake_request\")",
        "auto_reconnect_wake_request_blocked_by_guard",
    ]), "reader-wake reconnect request is consumed but blocked when the crash guard is active", failures)

    require("_autoReconnectDisabledThisBoot" not in start_reconnect,
            "manual reconnect jobs are not blocked by the automatic-reconnect crash guard", failures)

    require(appears_in_order(reconnect_task, [
        "if (automatic)",
        "setAutoReconnectGuard(true)",
        "scanForBondedReconnectCandidate(candidate, scanMs)",
        "success = connectToDevice(candidate.address, timeoutMs, candidate.addressType, candidate.name)",
        "if (success)",
        "armAutoReconnect(pairNew ? \"pair_new_success\"",
        "\"manual_reconnect_success\")",
        "if (automatic)",
        "setAutoReconnectGuard(false)",
    ]), "automatic reconnect has a persistent in-flight guard and manual success clears guarded mode", failures)

    require(appears_in_order(auto_reconnect, [
        "initializeAutoReconnectGuard();",
        "if (_autoReconnectDisabledThisBoot)",
        "return;",
        "if (!_autoReconnectArmed)",
        "return;",
        "startBondedReconnect(BLE_MANUAL_RECONNECT_TIMEOUT_MS, true)",
    ]), "automatic reconnect returns before queueing work when guarded or not armed", failures)

    require("btMgr->startBondedReconnect(12000)" in settings_select,
            "Bluetooth settings still expose manual Reconnect Remote", failures)
    require("Pair New Remote" in settings and "btMgr->startPairNewRemote(12000)" in settings,
            "Bluetooth settings expose worker-based Pair New Remote", failures)
    require("Scan Disabled" not in settings and "Scan disabled" not in settings,
            "old disabled scan menu text is gone", failures)

    if failures:
        print(f"\n{len(failures)} guard behavior check(s) failed.")
        return 1

    print("\nAll guard behavior checks passed. Hardware validation is still required.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
