# Vibe Keyboard

Custom ESP32-S3 firmware and a native macOS controller for the 428×142
VibeBoard.

The project communicates over USB and lets the Mac control the screen, four
physical keys, microphone recording, and device content.

## Features

- Upload static images, GIF/APNG animations, and pets.
- Show a live two-tile dashboard with four rotating module slots.
- Display Codex usage, Claude usage, CPU/memory, network throughput, and up to
  12 stock symbols.
- Map K1–K4 single, double, and long presses to shortcuts, applications,
  URLs, text, media controls, or executable commands.
- Record the device microphone as Ogg Opus.
- Preview screen content before committing it to the device.

## Repository Layout

```text
.
├── mac/        SwiftUI app, USB library, diagnostics, and tests
├── firmware/   ESP-IDF firmware, flash helper, and native tests
└── docs/       Hardware, protocol, display, and UI documentation
```

Generated builds, packaged applications, and local device data are ignored by
Git.

## Build the macOS App

Requirements:

- macOS 13 or newer
- Swift 6.2 toolchain

```bash
cd mac
swift test -Xswiftc -strict-concurrency=complete
./scripts/package_app.sh
open "dist/Vibe Keyboard.app"
```

The packaged application is written to `mac/dist/Vibe Keyboard.app`, and the
shareable archive is `mac/dist/VibeKeyboard-macOS-arm64.zip`.

Only one Vibe Keyboard or diagnostic process can own the USB serial device at a
time.

## First Connection

1. Install the firmware using the instructions below.
2. Connect the board directly over USB and launch Vibe Keyboard.
3. Wait for `Device > Status` to show `Ready`.
4. Grant Accessibility permission when macOS asks. It is required for keyboard
   shortcuts, text insertion, and application control.

## Use the Dashboard

1. Open `Screen` and choose `Dashboard`.
2. Select the modules for Page A left/right and Page B left/right.
3. If a slot uses `Pet`, choose or import an animation under `Dashboard Pet`,
   then upload it. The pet stays on that side across both pages.
4. Choose a page interval from 4 to 12 seconds.
5. Add stock symbols such as `sh000001`, `hk00700`, or `usAAPL`.
6. Click `Save`, then `Install & start`.

The display shows two tiles at once:

```text
Page A
+----------------------+----------------------+
| A1 CODEX             | A2 SYSTEM            |
| LIMIT 47%            | CPU 15%              |
| TODAY 109.3M         | MEM 64%              |
+----------------------+----------------------+

Page B
+----------------------+----------------------+
| B1 NETWORK           | B2 STOCKS 1-2/4      |
| DOWN 4K/s            | 000001 3876.8 +0.25% |
| UP 25K/s             | AAPL 220.12 +0.57%   |
+----------------------+----------------------+
```

Page A and Page B rotate automatically. Stock tiles show two quotes and advance
through the configured symbols. Keep the Mac app running for live updates.

## Upload Images and Dashboard Pets

- `Screen > Import & Upload…` converts an image to the device format and
  uploads it. Select `Image`, then click `Commit uploaded image`.
- `Screen > Dashboard > Dashboard Pet` loads local pets and the public Petdex
  catalog. Select or import a pet, upload it, assign `Pet` to a dashboard side,
  then click `Install & start`.

## Configure Keys

1. Open `Keys` and select K1–K4.
2. Choose the gesture and action.
3. Fill in the action-specific value when required.
4. Use `Test selected action`, then save the mapping.

Custom commands execute an absolute executable directly without a shell. Each
argument line is passed as one literal argument.

## Use the Microphone

1. Open `Audio`.
2. Select one physical voice key.
3. Enable recording storage if required.
4. Hold the selected key to record and release it to finish.

Saved files are written under
`~/Library/Application Support/VibeKeyboard/Recordings/`.

## Build and Install Firmware

Requirements:

- ESP-IDF 5.5.2
- ESP32-S3 toolchain

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
python3 tests/run_native_tests.py
python3 -m unittest tests.test_contract
python3 tools/validate_build.py build
```

This hardware profile keeps the existing bootloader and writes only the
application partition:

```bash
python3 tools/auto_flash.py build /dev/cu.usbmodemXXXX
```

The helper validates the build, attempts automatic download-mode entry, writes
the application partition, verifies the written bytes, and leaves the
bootloader, partition table, NVS, OTA metadata, and asset storage unchanged.
If automatic entry is unavailable, hold K1 while reconnecting USB and run the
command again.

Do not use this firmware or flash command with a different board.

## Troubleshooting

- `Disconnected`: close other Vibe Keyboard/diagnostic processes and reconnect
  USB.
- Shortcuts do nothing: enable Vibe Keyboard in macOS
  `Privacy & Security > Accessibility`.
- Storage remains busy after an interrupted upload: power-cycle the device
  once, reconnect, and retry.
- Live data does not change: keep the app open and confirm the selected source
  is available on the Mac.
- In-app firmware update is unavailable: install the application image with
  `firmware/tools/auto_flash.py`.

More details are available in [mac/README.md](mac/README.md),
[firmware/README.md](firmware/README.md), and [docs/INDEX.md](docs/INDEX.md).

## License

Project code and documentation are licensed under the [MIT License](LICENSE).
Third-party components retain the licenses listed in
[`firmware/third_party/notices`](firmware/third_party/notices/).
