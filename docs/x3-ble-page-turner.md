# X3 BLE Page-Turner Fork

This fork adds Bluetooth HID page-turner support to CrossPoint Reader for the
XTEink X3. It was built and hardware-validated with a Free3/Free3-ER-style BLE
page turner after a sequence of reconnect and long-idle fixes.

The fork is based on upstream `crosspoint-reader/crosspoint-reader` at commit
[`b8a6b58b5ee21a2b5b9a53f7ed5366fc858d137c`](https://github.com/crosspoint-reader/crosspoint-reader/commit/b8a6b58b5ee21a2b5b9a53f7ed5366fc858d137c)
(`docs: expand first use of OPDS acronym and provide a wikipedia link
(#1824)`, dated 2026-05-04). In Git terms, that upstream base describes as
`1.2.0-94-gb8a6b58`, meaning 94 commits after the upstream `1.2.0` tag.

This fork is versioned as `1.2.0-x3-ble-idlefix15`. It starts from that
upstream CrossPoint `1.2.0` line, then adds the X3 BLE page-turner work
described here. It is not based on the separate unofficial CrossPoint-BLE fork,
and it is not an official upstream CrossPoint release.

## What Changed

The normal CrossPoint firmware this was based on does not include this X3 BLE
page-turner stack. This fork adds:

- a NimBLE-based BLE HID manager, pinned to `h2zero/NimBLE-Arduino @ 2.5.0`;
- a Bluetooth settings screen under Settings and from the reader menu;
- bonded remote persistence in `/.crosspoint/settings.json`;
- virtual button injection so BLE reports flow through the normal CrossPoint
  page-turn path;
- Free2/Free3-style HID profile handling, plus a setup/learn path for other
  remotes;
- reconnect diagnostics in `/.crosspoint/ble_diag.log`;
- serial `CMD:BLE_DIAG` output for persisted reconnect diagnostics;
- guarded automatic reconnect to avoid repeated crash loops;
- reader sleep/wake reconnect by arming a wake marker before deep sleep;
- Free3 sleep/off/on recovery with active reconnect scans;
- long-idle recovery that no longer prevents the X3 from auto-sleeping;
- X3 USB-power wake handling that avoids immediately going back to sleep while
  connected to USB.

## Why Idlefix15 Exists

Earlier builds proved that the Free3 could work, but the failure mode kept
moving:

- the known-good short reconnect behavior used a broad HID report discovery
  pass, a 45 second active manual scan, a 12 second connect timeout, and a
  bond-reset connect attempt;
- long idle testing then showed that background reconnect activity could keep
  the X3 awake or leave reconnect recovery unavailable after the device sat
  unused;
- a later attempt restored most of the reconnect behavior but still crashed
  during manual `Reconnect Remote`;
- idlefix15 keeps the known-good reconnect shape, keeps the long-idle
  auto-sleep bookkeeping fix, and holds normal CPU speed while the BLE
  scan/connect/subscribe worker is running.

That last point matters on the X3 because the main loop can otherwise lower the
CPU clock during idle. Reconnect work now owns a `HalPowerManager::Lock`, and
the main loop keeps power saving disabled while bonded reconnect work is active.

## Reconnect Behavior

Manual reconnect:

- active scan for up to 45 seconds;
- one bounded connect attempt with a 12 second timeout from the settings UI;
- bond reset before reconnect;
- broad HID report characteristic enumeration;
- no post-connect connection-parameter update, data-length negotiation, RSSI
  read, protocol-mode write, or report-map read.

Automatic page-turner recovery:

- Free2/Free3-style remotes that look connected but have stopped sending HID
  reports are treated as stale after about 195 seconds;
- the first 20 minutes of lost-page-turner recovery use dense active scans;
- recovery then continues with a sparse active-scan window for up to 4 hours;
- reader wake and local button input refresh the fast recovery window;
- reconnect workers do not count as recent BLE activity for the auto-sleep
  inactivity timer;
- auto-sleep defers only while an active reconnect worker is running.

Crash-loop guard:

- automatic reconnect writes `/.crosspoint/ble_auto_reconnect_guard` while it
  is running;
- panic, watchdog, CPU-lockup, or an existing guard file disables automatic
  reconnect for that boot;
- manual `Reconnect Remote` remains available and can clear guarded mode after
  a successful reconnect.

## Hardware Validation

Validated on 2026-05-09 with:

- XTEink X3;
- Free3/Free3-ER-style BLE page turner;
- firmware `1.2.0-x3-ble-idlefix15`;
- SHA-256
  `5b23aee2453df26f35fc837ea580eedc3b7c8fa7deeb0f01092e9b7ff7b2949f`;
- binary size `0x5b62e0`, below the `0x640000` OTA app partition limit.

Validation gates:

| Gate | Result |
| --- | --- |
| Flash to app0 and app1 with `verify-flash` | Passed |
| Boot and UI responsiveness | Passed |
| Manual `Reconnect Remote` | Passed |
| Forward and back page turns | Passed |
| Reader sleep/wake reconnect without settings menu | Passed |
| Free3 sleep/off/on reconnect without settings menu | Passed |
| Long idle recovery after the previous failure window | Passed |
| 3 reader sleep/wake cycles and 3 Free3 sleep/off/on cycles | Passed |
| Guard source audit and no guarded reconnect crash during validation | Passed |

See `docs/x3-ble-idlefix15-hardware-validation.md` for the full checklist.

## Known Limits

- This has been hardware-tested on one X3 plus one Free3/Free3-ER-style remote.
- The `Scan for devices` entry is intentionally disabled on the X3 candidate
  because earlier testing made manual scanning a crash path. The intended flow
  is to use one successful bonded setup/reconnect, then automatic reconnect.
- Other BLE remotes may work through common HID profiles or the learn wizard,
  but they are not validated to the same level.
- This is a community fork, not an official CrossPoint Reader release and not
  affiliated with XTEink.

## Useful Files

- `lib/hal/BluetoothHIDManager.*`: BLE lifecycle, scan/connect, HID report
  handling, reconnect state, guard behavior.
- `lib/hal/DeviceProfiles.*`: known page-turner HID profiles and learned
  mapping persistence.
- `lib/hal/BluetoothDiagnostics.*`: persisted boot/reconnect diagnostic ring.
- `src/activities/settings/BluetoothSettingsActivity.*`: Bluetooth settings,
  reconnect action, setup wizard, debug monitor.
- `src/main.cpp`: BLE restore, sleep/wake arming, reconnect polling, diagnostic
  serial command.
- `scripts/verify_x3_ble_idlefix15.py`: local source/artifact consistency
  checks.
- `scripts/audit_x3_ble_goal_completion.py`: strict final validation audit.
- `scripts/read_x3_ble_diag.py`: mounted-storage BLE diagnostic summarizer.

## Building

Install PlatformIO, clone recursively, then build the release environment:

```sh
git clone --recursive https://github.com/<owner>/<repo>.git
cd <repo>
pio run -e gh_release
```

The firmware image is written to:

```sh
.pio/build/gh_release/firmware.bin
```

To run the local validation checks against a packaged binary:

```sh
cp .pio/build/gh_release/firmware.bin ~/Downloads/crosspoint-x3-ble-idlefix15.bin
python3 scripts/verify_x3_ble_idlefix15.py
python3 scripts/preflight_x3_ble_idlefix15.py --local-only
```

## Flashing

The final flash helper writes and verifies both OTA app slots:

```sh
X3_BLE_FIRMWARE_BIN=~/Downloads/crosspoint-x3-ble-idlefix15.bin \
  scripts/flash_record_x3_ble_idlefix15.sh
```

If the X3 flash port is not visible on macOS:

```sh
python3 scripts/diagnose_x3_usb_visibility.py
```

The helper skips the known Jieli USB VID/PID used by some remotes and requires
`esptool --chip esp32c3 chip-id` to pass before writing flash.
