# X3 BLE Pairfix1 Validation

- Firmware: `crosspoint-x3-ble-pairfix1.bin`
- Version marker: `1.2.0-x3-ble-pairfix1`
- SHA-256: `1211423cfc4d96273a6528893effc584f9a33ac213a8ab56daf261d434f8eacd`
- Binary size: `0x5b7630`
- Base reconnect build: `1.2.0-x3-ble-idlefix15`
- Purpose: restore safe first-time pairing for new remotes while preserving the
  validated idlefix15 reconnect behavior.

## Issue

The public idlefix15 build intentionally disabled the old manual scan UI on X3
because earlier X3 tests made open-ended settings scans a crash path. That kept
the bonded Free3 reconnect path stable, but it left first-time users unable to
pair a new Free2/Free3-style remote. The settings screen showed `Scan Disabled`
and `Reconnect Remote` could only work after a bond already existed.

## Fix

Pairfix1 replaces `Scan Disabled` with `Pair New Remote`.

The new pairing path:

- starts from the settings menu;
- runs scan/connect/discover/subscribe work inside the BLE worker task;
- holds normal CPU speed during BLE work;
- uses a finite active scan with scan-response support;
- skips the currently saved remote when searching for a new one;
- prefers known page-turner names such as Free2/Free3, GameBrick, mini
  keyboard, IINE, and Kobo remotes;
- falls back to an unknown-name device only when exactly one connectable HID
  candidate is visible;
- refuses ambiguous multiple-HID scans instead of pairing an arbitrary device;
- saves the new bonded address/name/type only after HID service discovery and
  report subscription succeed;
- arms the existing bonded automatic reconnect path after successful pairing.

## Preserved Reconnect Behavior

Pairfix1 deliberately leaves the idlefix15 reconnect baseline intact:

- manual `Reconnect Remote` still uses the reconnect worker;
- manual reconnect still uses the 45 second active scan window;
- manual reconnect still uses the 12 second connect timeout;
- automatic reconnect still uses `startBondedReconnect(..., true)`;
- reader sleep/wake reconnect, remote sleep/off/on reconnect, long-idle
  recovery, and crash-loop guard behavior remain on the idlefix15 path.

## Local Validation

Required checks before flashing:

```sh
python3 scripts/audit_x3_ble_pairing_flow.py
python3 scripts/audit_x3_ble_reconnect_invariants.py
python3 scripts/audit_x3_ble_guard_behavior.py
python3 scripts/audit_x3_ble_remote_sleep_autoreconnect.py
python3 scripts/audit_x3_ble_page_input_path.py
python3 scripts/verify_x3_ble_pairfix1.py
python3 scripts/preflight_x3_ble_pairfix1.py --local-only
```

Build check:

```sh
pio run -e gh_release
```

## Hardware Validation Gates

Because first-time pairing requires a physical unpaired remote, this is the
hardware checklist for anyone validating pairfix1 on device:

| Gate | Steps | Expected Result | Status |
| --- | --- | --- | --- |
| Flash | Run `scripts/flash_record_x3_ble_pairfix1.sh` | Both app slots write and explicit `verify-flash` checks pass | Passed 2026-05-17: app0 and app1 write plus explicit `verify-flash` succeeded |
| Boot | Start the X3 after flashing pairfix1 | CrossPoint UI appears and stays responsive for at least 2 minutes | Pending |
| Existing reconnect | With an already saved Free3, use `Reconnect Remote` if needed | Reconnect succeeds without freeze/reboot | Pending |
| Pair new remote | Settings -> Bluetooth -> `Pair New Remote` while Free2/Free3 is awake/pairing | Remote pairs, menu reports `Paired ...`, and the bond is saved | Pending |
| Page input | Open an EPUB and press both remote directions | Forward and back page turns work | Pending |
| Sleep/wake reconnect | Sleep the reader, wake it, then press the remote | Page turning recovers without opening Bluetooth settings | Pending |
| Remote sleep/off/on reconnect | Let the remote sleep or restart it | Page turning recovers without opening Bluetooth settings | Pending |
| Ambiguous scan safety | If multiple HID remotes are nearby, run `Pair New Remote` | Pairing fails safe with an ambiguity message, not a random bond or crash | Optional |

## Switching Away From An Existing Free3

To intentionally remove the current Free3 bond on pairfix1:

1. Settings -> Bluetooth -> `Forget Bonded Remote`.
2. Wake the new remote or put it in pairing mode.
3. Settings -> Bluetooth -> `Pair New Remote`.

`Forget Bonded Remote` now disconnects the current remote, clears the saved bond,
and clears the old per-device learned mapping.
