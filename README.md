# CrossPoint Reader X3 BLE Page-Turner Fork

Community fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)
for the **XTEink X3**, adding Bluetooth HID page-turner support and validated
automatic reconnect for a Free3/Free3-ER-style remote.

This fork is unaffiliated with XTEink and with the upstream CrossPoint Reader
project. It keeps the normal CrossPoint EPUB reader experience, then adds an X3
BLE remote path for people who want physical page-turn buttons.

CrossPoint Reader is a purpose-built firmware designed to be a drop-in, fully open-source replacement for the official 
Xteink firmware. It aims to match or improve upon the standard EPUB reading experience.

![](./docs/images/cover.jpg)

## Upstream Base

This fork is based on the upstream
[`crosspoint-reader/crosspoint-reader`](https://github.com/crosspoint-reader/crosspoint-reader)
repository at this exact commit:

- Commit: [`b8a6b58b5ee21a2b5b9a53f7ed5366fc858d137c`](https://github.com/crosspoint-reader/crosspoint-reader/commit/b8a6b58b5ee21a2b5b9a53f7ed5366fc858d137c)
- Upstream position: `1.2.0-94-gb8a6b58`, meaning 94 commits after the
  upstream `1.2.0` tag
- Commit date: 2026-05-04
- Commit title: `docs: expand first use of OPDS acronym and provide a wikipedia link (#1824)`

The firmware in this fork is versioned as `1.2.0-x3-ble-idlefix15`: it starts
from that upstream CrossPoint `1.2.0` line, then adds the X3 BLE page-turner
work documented below. It is not based on the separate unofficial
CrossPoint-BLE fork, and it is not an official upstream CrossPoint release.

## What Is Different In This Fork?

Starting from the upstream base above, this fork adds the validated
`1.2.0-x3-ble-idlefix15` X3 BLE page-turner work.

Highlights:

- Bluetooth settings screen under Settings and from the reader menu.
- NimBLE-based HID page-turner support with Free2/Free3-style profiles.
- Bonded `Reconnect Remote` flow for the X3.
- Automatic reconnect after reader sleep/wake.
- Automatic reconnect after the Free3 sleeps, turns off, or is restarted.
- Long-idle recovery that lets the X3 auto-sleep instead of being kept awake by
  reconnect attempts.
- Crash-loop guard for automatic reconnect, while keeping manual reconnect
  available.
- BLE diagnostics in `/.crosspoint/ble_diag.log` and serial `CMD:BLE_DIAG`.

The final candidate was hardware validated on an XTEink X3 with a
Free3/Free3-ER-style page turner. See
[docs/x3-ble-page-turner.md](./docs/x3-ble-page-turner.md) for the full design,
validation, and limitations.

## Current X3 BLE Release

- Version: `1.2.0-x3-ble-idlefix15`
- Firmware SHA-256:
  `5b23aee2453df26f35fc837ea580eedc3b7c8fa7deeb0f01092e9b7ff7b2949f`
- Binary size: `0x5b62e0`
- Validated hardware: XTEink X3 plus Free3/Free3-ER-style BLE page turner
- Validation status: flash, boot, manual reconnect, page input, reader
  sleep/wake, remote sleep/off/on, long idle recovery, repeated cycles, and
  reconnect guard behavior all passed

The firmware binary is intended to be published as a GitHub release asset named
`crosspoint-x3-ble-idlefix15.bin`.

## Motivation

E-paper devices are fantastic for reading, but most commercially available readers are closed systems with limited 
customisation. The **Xteink X4** is an affordable, e-paper device, however the official firmware remains closed.
CrossPoint exists partly as a fun side-project and partly to open up the ecosystem and truly unlock the device's
potential.

CrossPoint Reader aims to:
* Provide a **fully open-source alternative** to the official firmware.
* Offer a **document reader** capable of handling EPUB content on constrained hardware.
* Support **customisable font, layout, and display** options.
* Run purely on the **Xteink X4 hardware**.

This project is **not affiliated with Xteink**; it's built as a community project.

## Features & Usage

- [x] EPUB parsing and rendering (EPUB 2 and EPUB 3)
- [x] Image support within EPUB
- [x] Saved reading position
- [x] File explorer with file picker
  - [x] Basic EPUB picker from root directory
  - [x] Support nested folders
  - [ ] EPUB picker with cover art
- [x] Custom sleep screen
  - [x] Cover sleep screen
- [x] Wifi book upload
- [x] Wifi OTA updates
- [x] KOReader Sync integration for cross-device reading progress
- [x] Configurable font, layout, and display options
  - [ ] User provided fonts
  - [ ] Full UTF support
- [x] Screen rotation
- [x] X3 Bluetooth HID page-turner support in this fork
- [x] Free3-style remote reconnect after sleep/wake, remote off/on, and long idle

Multi-language support: Read EPUBs in various languages, including English, Spanish, French, German, Italian, Portuguese, Russian, Ukrainian, Polish, Swedish, Norwegian, [and more](./USER_GUIDE.md#supported-languages).

See [the user guide](./USER_GUIDE.md) for instructions on operating CrossPoint, including the
[KOReader Sync quick setup](./USER_GUIDE.md#365-koreader-sync-quick-setup).

For more details about the scope of the project, see the [SCOPE.md](SCOPE.md) document.

## Installing

### X3 BLE fork release

1. Download `crosspoint-x3-ble-idlefix15.bin` from this repository's
   [releases page](https://github.com/hannah-nula/crosspoint-x3-ble-page-turner/releases).
2. Connect the XTEink X3 over USB and put it into the ESP32-C3 flash/download
   mode if needed.
3. Flash both OTA app slots using the helper:

```sh
X3_BLE_FIRMWARE_BIN=~/Downloads/crosspoint-x3-ble-idlefix15.bin \
  scripts/flash_record_x3_ble_idlefix15.sh
```

The helper verifies both app slots after writing. If macOS cannot see the X3
flash port, run:

```sh
python3 scripts/diagnose_x3_usb_visibility.py
```

After flashing, enable Bluetooth from Settings, connect/reconnect the remote,
and use the reader normally. The intended daily-driver path is bonded reconnect
and automatic reconnect; `Scan for devices` is intentionally disabled in this
X3 candidate because it was unstable during testing.

### Web (latest firmware)

1. Connect your Xteink X4 to your computer via USB-C and wake/unlock the device
2. Go to https://xteink.dve.al/ and click "Flash CrossPoint firmware"

To revert back to the official firmware, you can flash the latest official firmware from https://xteink.dve.al/, or swap
back to the other partition using the "Swap boot partition" button here https://xteink.dve.al/debug.

### Web (specific firmware version)

1. Connect your Xteink X4 to your computer via USB-C
2. Download the `firmware.bin` file from the release of your choice via the [releases page](https://github.com/crosspoint-reader/crosspoint-reader/releases)
3. Go to https://xteink.dve.al/ and flash the firmware file using the "OTA fast flash controls" section

To revert back to the official firmware, you can flash the latest official firmware from https://xteink.dve.al/, or swap
back to the other partition using the "Swap boot partition" button here https://xteink.dve.al/debug.

### Command line (specific firmware version)

1. Install [`esptool`](https://github.com/espressif/esptool) :
```bash
pip install esptool
```
2. Download the `firmware.bin` file from the release of your choice via the [releases page](https://github.com/crosspoint-reader/crosspoint-reader/releases)
3. Connect your Xteink X4 to your computer via USB-C.
4. Note the device location. On Linux, run `dmesg` after connecting. On MacOS, run :
```bash
log stream --predicate 'subsystem == "com.apple.iokit"' --info
```
5. Flash the firmware :
```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```
Change `/dev/ttyACM0` to the device for your system.

### Manual

See [Development](#development) below.

## Development

### Prerequisites

* **PlatformIO Core** (`pio`) or **VS Code + PlatformIO IDE**
* Python 3.8+
* USB-C cable for flashing the ESP32-C3
* Xteink X4

### Checking out the code

CrossPoint uses PlatformIO for building and flashing the firmware. To get started, clone the repository:

```
git clone --recursive https://github.com/hannah-nula/crosspoint-x3-ble-page-turner

# Or, if you've already cloned without --recursive:
git submodule update --init --recursive
```

### Flashing your device

Connect your Xteink X4 to your computer via USB-C and run the following command.

```sh
pio run --target upload
```
### Debugging

After flashing the new features, it’s recommended to capture detailed logs from the serial port.

First, make sure all required Python packages are installed:

```python
python3 -m pip install pyserial colorama matplotlib
```
after that run the script:
```sh
# For Linux
# This was tested on Debian and should work on most Linux systems.
python3 scripts/debugging_monitor.py

# For macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```
Minor adjustments may be required for Windows.

## Internals

CrossPoint Reader is pretty aggressive about caching data down to the SD card to minimise RAM usage. The ESP32-C3 only
has ~380KB of usable RAM, so we have to be careful. A lot of the decisions made in the design of the firmware were based
on this constraint.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the 
cache. This cache directory exists at `.crosspoint` on the SD card. The structure is as follows:


```
.crosspoint/
├── epub_12471232/       # Each EPUB is cached to a subdirectory named `epub_<hash>`
│   ├── progress.bin     # Stores reading progress (chapter, page, etc.)
│   ├── cover.bmp        # Book cover image (once generated)
│   ├── book.bin         # Book metadata (title, author, spine, table of contents, etc.)
│   └── sections/        # All chapter data is stored in the sections subdirectory
│       ├── 0.bin        # Chapter data (screen count, all text layout info, etc.)
│       ├── 1.bin        #     files are named by their index in the spine
│       └── ...
│
└── epub_189013891/
```

Deleting the `.crosspoint` directory will clear the entire cache. 

Due the way it's currently implemented, the cache is not automatically cleared when a book is deleted and moving a book
file will use a new cache directory, resetting the reading progress.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).

## Contributing

Contributions are very welcome!

If you are new to the codebase, start with the [contributing docs](./docs/contributing/README.md).

If you're looking for a way to help out, take a look at the [ideas discussion board](https://github.com/crosspoint-reader/crosspoint-reader/discussions/categories/ideas).
If there's something there you'd like to work on, leave a comment so that we can avoid duplicated effort.

Everyone here is a volunteer, so please be respectful and patient. For more details on our governance and community 
principles, please see [GOVERNANCE.md](GOVERNANCE.md).

### To submit a contribution:

1. Fork the repo
2. Create a branch (`feature/dithering-improvement`)
3. Make changes
4. Submit a PR

---

CrossPoint Reader is **not affiliated with Xteink or any manufacturer of the X4 hardware**.

Huge shoutout to [**diy-esp32-epub-reader** by atomic14](https://github.com/atomic14/diy-esp32-epub-reader), which was a project I took a lot of inspiration from as I
was making CrossPoint.

This fork also owes its foundation to the upstream
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)
project. The X3 BLE page-turner changes are community fork work layered on top
of that codebase.
