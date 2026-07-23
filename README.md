# Vibe Keyboard

Custom ESP32-S3 firmware and a native macOS controller for the VibeBoard.

## Repository layout

```text
.
├── mac/        SwiftUI controller, USB protocol library, diagnostics, and tests
├── firmware/   ESP-IDF firmware, native tests, validation, and flash tools
└── docs/       Shared hardware and protocol contracts
```

Generated builds, downloaded ESP-IDF components, packaged applications, and
local configuration files are excluded from Git.

## macOS app

Requirements: macOS 13 or newer and a Swift 6.2 toolchain.

```bash
cd mac
swift test -Xswiftc -strict-concurrency=complete
./scripts/package_app.sh
open "dist/Vibe Keyboard.app"
```

See [mac/README.md](mac/README.md) for the client architecture and USB ownership
rules.

## Firmware

Requirements: ESP-IDF 5.5.2 with the ESP32-S3 toolchain.

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
python3 tests/run_native_tests.py
python3 -m unittest tests.test_contract
python3 tools/validate_build.py build
```

The device currently relies on its original bootloader. To preserve the
bootloader, partition table, NVS, and asset storage, enter ROM download mode and
flash only the application partition:

```bash
python3 tools/auto_flash.py build /dev/cu.usbmodemXXXX
```

Do not use `idf.py flash` for this hardware profile unless a replacement
bootloader has been validated separately.

## Documentation

Start with [docs/INDEX.md](docs/INDEX.md). Hardware facts, binary protocol
details, display geometry, asset format, input, audio, and LED behavior are kept
as versioned contracts under `docs/`.
