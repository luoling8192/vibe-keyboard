# VibeBoard Firmware

ESP-IDF 5.5.2 firmware for the ESP32-S3 VibeBoard hardware profile:

- NV3007 428×142 RGB565 display;
- static and animated VKA1 assets, text, widgets, and pet scenes;
- four active-low physical keys;
- USB Serial/JTAG control protocol;
- PDM microphone to Opus audio;
- fail-dark SK6812 LED ownership;
- 16 MiB flash and 8 MiB octal PSRAM.

Bluetooth, Wi-Fi, network services, TinyUSB, and USB Audio Class are excluded.

## Build and test

Activate ESP-IDF 5.5.2, then run:

```bash
idf.py set-target esp32s3
idf.py build
python3 tests/run_native_tests.py
python3 -m unittest tests.test_contract
python3 tools/validate_build.py build
```

ESP-IDF downloads pinned dependencies into `managed_components/`; that directory
and all build output are intentionally ignored.

## Flashing

The current device recovery path keeps the original bootloader and writes only
the custom application at `0x20000`. Enter ROM download mode, then run:

```bash
python3 tools/auto_flash.py build /dev/cu.usbmodemXXXX
```

The helper validates the target build, writes only the application partition,
and verifies the written bytes. It does not erase or overwrite the bootloader,
partition table, NVS, OTA metadata, or asset storage.

Do not use `idf.py flash` until the replacement bootloader has completed a
separate hardware recovery and rollback review.

See [the firmware architecture](../docs/firmware/README.md) and
[hardware contract](../docs/product/hardware.md) for the full component and
safety model.
