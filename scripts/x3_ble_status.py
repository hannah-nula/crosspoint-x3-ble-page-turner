#!/usr/bin/env python3
"""Show the next actionable X3 BLE firmware status."""

from __future__ import annotations

import hashlib
import glob
import json
import os
import re
import subprocess
from pathlib import Path

try:
    import serial.tools.list_ports
except ModuleNotFoundError:
    serial = None


ROOT = Path(__file__).resolve().parents[1]
LABEL = os.environ.get("X3_BLE_IMAGE_LABEL", "pairfix1")
BIN = Path(os.environ.get(
    "X3_BLE_FIRMWARE_BIN",
    str(Path.home() / f"Downloads/crosspoint-x3-ble-{LABEL}.bin"),
))
PIO_PYTHON = Path(os.environ.get("PIO_PYTHON", str(Path.home() / ".platformio/penv/bin/python")))
EXPECTED_SHA256 = os.environ.get(
    "X3_BLE_EXPECTED_SHA256",
    "1211423cfc4d96273a6528893effc584f9a33ac213a8ab56daf261d434f8eacd",
)
EXPECTED_SIZE = int(os.environ.get("X3_BLE_EXPECTED_SIZE", "0x5b7630"), 0)
APP_PARTITION_SIZE = 0x640000
VALIDATION_DOC = Path(os.environ.get(
    "X3_BLE_VALIDATION_DOC",
    str(ROOT / f"docs/x3-ble-{LABEL}-validation.md"),
))
GATE_ORDER = [
    "Flash",
    "Boot",
    "Manual reconnect",
    "Page input",
    "Reader sleep/wake",
    "Remote sleep/off/on",
    "Long idle recovery",
    "Repeated cycles",
    "Guard behavior",
]


def artifact_ok() -> bool:
    if not BIN.exists():
        print(f"artifact: missing {BIN}")
        return False

    data = BIN.read_bytes()
    sha = hashlib.sha256(data).hexdigest()
    ok = True
    if EXPECTED_SHA256 != "pending" and sha != EXPECTED_SHA256:
        print(f"artifact: SHA mismatch {sha}")
        ok = False
    if EXPECTED_SIZE != 0 and len(data) != EXPECTED_SIZE:
        print(f"artifact: size mismatch 0x{len(data):x}")
        ok = False
    if len(data) >= APP_PARTITION_SIZE:
        print(f"artifact: too large for app partition 0x{len(data):x}")
        ok = False
    version_marker = f"1.2.0-x3-ble-{LABEL}".encode()
    if version_marker not in data:
        print(f"artifact: {LABEL} version marker missing")
        ok = False
    if ok:
        print(f"artifact: ok {BIN} sha={sha[:12]}... size=0x{len(data):x}")
    return ok


def x3_ports() -> list[str]:
    ports: list[str] = []
    skipped: list[str] = []
    port_rows = []
    if serial is not None:
        for port in serial.tools.list_ports.comports():
            port_rows.append({
                "device": port.device,
                "vid": port.vid,
                "pid": port.pid,
                "description": port.description,
            })
    elif PIO_PYTHON.exists():
        code = (
            "import json, serial.tools.list_ports\n"
            "print(json.dumps([{'device': p.device, 'vid': p.vid, 'pid': p.pid, "
            "'description': p.description} for p in serial.tools.list_ports.comports()]))\n"
        )
        try:
            result = subprocess.run([str(PIO_PYTHON), "-c", code], check=True, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            port_rows = json.loads(result.stdout)
            print("port: using PlatformIO Python for VID/PID scan")
        except (subprocess.SubprocessError, json.JSONDecodeError) as exc:
            print(f"port: PlatformIO Python scan unavailable ({exc})")

    if not port_rows:
        candidates = sorted(set(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/tty.usbmodem*")))
        ports.extend(f"{path} usbmodem candidate; VID/PID unavailable" for path in candidates)
    else:
        for port in port_rows:
            vid = port.get("vid")
            pid = port.get("pid")
            device = port.get("device", "")
            description = port.get("description", "")
            label = f"{device} {vid:04x}:{pid:04x} {description}" if vid is not None else f"{device} no-vidpid {description}"
            if vid == 0x4C4A and pid == 0x4155:
                skipped.append(f"{label} (Jieli, not X3 flash port)")
                continue
            if vid == 0x303A or "usbmodem" in device:
                ports.append(label)
    for line in skipped:
        print(f"port: skipped {line}")
    if ports:
        for line in ports:
            print(f"port: X3 candidate {line}")
    else:
        print("port: no X3 ESP32-C3 serial/JTAG candidate visible")
    return ports


def diag_logs() -> list[Path]:
    logs = sorted(Path("/Volumes").glob("*/.crosspoint/ble_diag.log"))
    if logs:
        for path in logs:
            print(f"diag: found {path}")
    else:
        print("diag: no mounted /.crosspoint/ble_diag.log found")
    return logs


def next_pending_gate() -> str | None:
    if not VALIDATION_DOC.exists():
        return None

    pattern = re.compile(r"^\|\s*([^|]+?)\s*\|[^|]*\|[^|]*\|\s*([^|]+?)\s*\|$")
    results: dict[str, str] = {}
    for line in VALIDATION_DOC.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if not match:
            continue
        gate = match.group(1).strip()
        if gate in GATE_ORDER:
            results[gate] = match.group(2).strip()

    for gate in GATE_ORDER:
        result = results.get(gate, "")
        if not result.lower().startswith("pass"):
            return gate
    return None


def main() -> int:
    ok = artifact_ok()
    ports = x3_ports()
    diag_logs()

    print()
    if not ok:
        print(f"next: rebuild/package {LABEL} before flashing")
        return 1
    gate = next_pending_gate()
    if gate == "Flash" and ports:
        print(f"next: flash and record with scripts/flash_record_x3_ble_{LABEL}.sh")
        return 0
    if gate:
        print(f"next: hardware validation gate '{gate}'")
        return 0
    print("next: all hardware gates passed; run scripts/audit_x3_ble_goal_completion.py")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
