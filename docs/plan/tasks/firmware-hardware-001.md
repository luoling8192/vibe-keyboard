---
id: firmware-hardware-001
scope: VibeBoard firmware hardware profile
status: done
depends-on: []
---

## Objective

Implement an offline-buildable ESP-IDF hardware profile for this VibeBoard from the recovered evidence, without flashing the device. Physical display acceptance remains a first-write gate, not a reason to guess or omit recovered profile behavior.

## Context

- `docs/INDEX.md`
- `docs/product/hardware.md`
- `docs/product/screen-assets.md`
- `docs/plan/analysis/reference-projects.md`

## Path

- `firmware/`
- `docs/product/hardware.md`
- `docs/firmware/`

## Contract

- Target ESP32-S3 revision-compatible configuration, 16 MiB flash and 8 MiB PSRAM.
- Implement only evidence-backed NV3007 SPI/init/orientation/backlight/reset settings.
- Reproduce startup as reset low/high with 100 ms delays, the exact 119-entry table ending in `SLPOUT 0x11` plus 220 ms, LVGL display creation, then backlight enable; do not add startup `DISPON 0x29`.
- Implement the recovered display on/off and sleep/wake command methods, propagate GPIO/SPI errors, and make the wake caller own the proven 220 ms delay.
- Preserve the verified partition layout or document a rollback-safe migration that preserves private NVS/NVS keys.
- Define key GPIOs `k1=0`, `k2=18`, `k3=17`, `k4=16` as active-low inputs with pull-ups, a 5 ms scan and two-sample debounce. A low-level wake interrupt may resume scanning but never emits application events directly.
- Define the GPIO8 17-pixel SK6812/GRB RMT chain at 10 MHz with key raw indices `0...3` and strip raw indices `4...16`. Initial runtime behavior clears the chain and leaves it off; physical order and power policy wait for low-brightness connected tests.
- Define microphone GPIO41/40 and 16 kHz only until the recovered PDM/AFE implementation contract is complete.
- Initialize LVGL 9 at 428×142 RGB565.
- Do not add BLE initialization, GATT services, advertising, or Bluetooth configuration.
- Do not flash hardware in this task.

## Verification

- ESP-IDF build succeeds for ESP32-S3.
- Image/partition metadata fits verified flash boundaries and passes offline image validation.
- Unit/host tests cover hardware constants and panel command-table parsing where possible.
- Independent review traces every pin, panel command, polarity, orientation, and timing value to binary/boot/runtime evidence.
- No `write-flash`, OTA install, erase, or device mutation occurs.
