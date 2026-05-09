# Release Notes: X3 BLE Idlefix15

Release: `1.2.0-x3-ble-idlefix15`

This is a community CrossPoint Reader fork build for the XTEink X3 with
Bluetooth HID page-turner support. It was validated with a Free3/Free3-ER-style
remote.

## Upstream Base

Based on upstream
[`crosspoint-reader/crosspoint-reader`](https://github.com/crosspoint-reader/crosspoint-reader)
commit
[`b8a6b58b5ee21a2b5b9a53f7ed5366fc858d137c`](https://github.com/crosspoint-reader/crosspoint-reader/commit/b8a6b58b5ee21a2b5b9a53f7ed5366fc858d137c)
from 2026-05-04. `git describe` reports that base as
`1.2.0-94-gb8a6b58`, meaning 94 commits after upstream CrossPoint `1.2.0`.

This release is versioned as `1.2.0-x3-ble-idlefix15`: upstream CrossPoint's
`1.2.0` line plus this fork's X3 BLE page-turner changes.

## Download

Use the release asset:

```text
crosspoint-x3-ble-idlefix15.bin
```

Expected SHA-256:

```text
5b23aee2453df26f35fc837ea580eedc3b7c8fa7deeb0f01092e9b7ff7b2949f
```

## What Changed From Upstream CrossPoint

- Added NimBLE BLE HID page-turner support.
- Added a Bluetooth settings screen and reader-menu entry.
- Added bonded remote persistence and manual `Reconnect Remote`.
- Added Free2/Free3-style HID profile support.
- Added virtual button injection into the normal CrossPoint reader input path.
- Added automatic reconnect after reader sleep/wake.
- Added automatic reconnect after the remote sleeps, turns off, or restarts.
- Added long-idle recovery that does not keep the X3 awake indefinitely.
- Added reconnect crash-loop guard behavior.
- Added persisted BLE reconnect diagnostics.

## Validation Summary

Passed on actual XTEink X3 hardware:

- flash to app0 and app1 with explicit `verify-flash`;
- boot and UI responsiveness;
- manual `Reconnect Remote`;
- forward and back page turns in an EPUB;
- reader sleep/wake reconnect without opening Bluetooth settings;
- Free3 sleep/off/on reconnect without opening Bluetooth settings;
- long idle recovery after the previous failure window;
- 3 reader sleep/wake cycles and 3 Free3 sleep/off/on cycles with no crash;
- reconnect guard source audit and post-validation manual reconnect availability.

## Known Limits

- Tested with one XTEink X3 and one Free3/Free3-ER-style page turner.
- `Scan for devices` is intentionally disabled on this X3 candidate because
  earlier testing made scan a crash path.
- Other BLE remotes may work through common profiles or the learn wizard, but
  they are not validated to the same level.

More detail:

- `docs/x3-ble-page-turner.md`
- `docs/x3-ble-idlefix15-hardware-validation.md`
- `docs/x3-ble-idlefix15-completion-audit.md`
