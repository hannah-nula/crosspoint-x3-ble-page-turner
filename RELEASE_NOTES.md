# Release Notes: X3 BLE Pairfix1

Release: `1.2.0-x3-ble-pairfix1`

This is a community CrossPoint Reader fork build for the XTEink X3 with
Bluetooth HID page-turner support. It was validated with a Free3/Free3-ER-style
remote on the idlefix15 reconnect baseline, then updated with a first-pairing
fix for new Free2/Free3-style remotes.

## Upstream Base

Based on upstream
[`crosspoint-reader/crosspoint-reader`](https://github.com/crosspoint-reader/crosspoint-reader)
commit
[`b8a6b58b5ee21a2b5b9a53f7ed5366fc858d137c`](https://github.com/crosspoint-reader/crosspoint-reader/commit/b8a6b58b5ee21a2b5b9a53f7ed5366fc858d137c)
from 2026-05-04. `git describe` reports that base as
`1.2.0-94-gb8a6b58`, meaning 94 commits after upstream CrossPoint `1.2.0`.

This release is versioned as `1.2.0-x3-ble-pairfix1`: upstream CrossPoint's
`1.2.0` line plus this fork's X3 BLE page-turner changes.

## Download

Use the release asset:

```text
crosspoint-x3-ble-pairfix1.bin
```

Expected SHA-256:

```text
1211423cfc4d96273a6528893effc584f9a33ac213a8ab56daf261d434f8eacd
```

Expected binary size: `0x5b7630`.

## What Changed From Upstream CrossPoint

- Added NimBLE BLE HID page-turner support.
- Added a Bluetooth settings screen and reader-menu entry.
- Added `Pair New Remote` for first-time pairing.
- Added bonded remote persistence and manual `Reconnect Remote`.
- Added Free2/Free3-style HID profile support.
- Added virtual button injection into the normal CrossPoint reader input path.
- Added automatic reconnect after reader sleep/wake.
- Added automatic reconnect after the remote sleeps, turns off, or restarts.
- Added long-idle recovery that does not keep the X3 awake indefinitely.
- Added reconnect crash-loop guard behavior.
- Added persisted BLE reconnect diagnostics.

## What Changed Since Idlefix15

- Replaced the disabled `Scan Disabled` menu entry with `Pair New Remote`.
- Pairing now runs in the same bounded BLE worker task used by stable reconnect.
- Pairing uses finite active scanning with scan-response support for
  Free2/Free3-style remotes.
- Pairing skips the currently saved remote when searching for a new device.
- A new bond is saved only after HID service discovery and report subscription
  succeed.
- Unknown-name fallback is conservative: it only pairs exactly one connectable
  HID candidate and fails safe if multiple HID candidates are nearby.
- `Forget Bonded Remote` now also disconnects current remotes and clears the
  old per-device learned mapping.
- The known-good idlefix15 reconnect scan duration, connect timeout, wake
  reconnect, long-idle recovery, and crash-loop guard behavior are preserved.

## Validation Summary

Idlefix15 passed on actual XTEink X3 hardware:

- flash to app0 and app1 with explicit `verify-flash`;
- boot and UI responsiveness;
- manual `Reconnect Remote`;
- forward and back page turns in an EPUB;
- reader sleep/wake reconnect without opening Bluetooth settings;
- Free3 sleep/off/on reconnect without opening Bluetooth settings;
- long idle recovery after the previous failure window;
- 3 reader sleep/wake cycles and 3 Free3 sleep/off/on cycles with no crash;
- reconnect guard source audit and post-validation manual reconnect availability.

Pairfix1 software validation:

- release build compiles;
- pairing flow audit passes;
- reconnect invariant audits still pass;
- first-time pairing is reachable from the settings UI.

## Known Limits

- Tested with one XTEink X3 and one Free3/Free3-ER-style page turner.
- The old open-ended `Scan for devices` list is still not the supported daily
  setup path. Use `Pair New Remote` for first pairing and `Reconnect Remote`
  afterwards.
- Other BLE remotes may work through common profiles or the learn wizard, but
  they are not validated to the same level.

More detail:

- `docs/x3-ble-page-turner.md`
- `docs/x3-ble-pairfix1-validation.md`
- `docs/x3-ble-idlefix15-hardware-validation.md`
- `docs/x3-ble-idlefix15-completion-audit.md`
