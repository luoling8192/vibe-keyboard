---
id: firmware-display-001
scope: compatibility umbrella for the split screen/asset delivery
status: in-progress
depends-on: [firmware-screen-001]
---

## Objective

Close the combined firmware display deliverable after the independently reviewed VKA1, SPIFFS store, USB transfer, and LVGL screen tasks pass. This task adds no second implementation path.

## Context

- `docs/product/screen-assets.md`
- `docs/firmware/README.md`
- `docs/plan/tasks/vka1-core-001.md`
- `docs/plan/tasks/firmware-asset-store-001.md`
- `docs/plan/tasks/firmware-asset-transfer-001.md`
- `docs/plan/tasks/firmware-screen-001.md`

## Path

- `firmware/components/vk_assets/`
- `firmware/components/vk_screen/`
- `firmware/components/vk_display/`
- `firmware/main/`
- `firmware/tests/`

## Contract

- Do not duplicate codec, storage, transfer, or render logic; integrate only the reviewed real components.
- Advertised screen modes and limits exactly match implemented behavior.
- Static image, pet, dashboard, custom layout, recovery, and previous-state preservation all execute through production paths.

## Verification

- Run the full firmware suite and clean image validation.
- Connected device renders every mode, survives reboot, rejects invalid/interrupted data, and remains within measured RAM/PSRAM/stack/storage limits.
- Independent review traces every capability to a real component and no stub/mock/fake remains in production composition.

## Development Evidence

The offline display-integration slice adds `vk_display` as a bounded owner over the
already recovered `vk_board` NV3007/LVGL transport. It admits only the exact
428×142 RGB565 product profile: 40 MHz SPI, 14/13 controller gaps, two DMA/PSRAM
10-row buffers, queue depth 10, byte swap enabled, and no software rotation,
full-refresh, or direct mode. Flush admission validates rectangles and byte
counts before incrementing a bounded in-flight count. Transport failure taints
the owner and disables screen availability; stop with an outstanding flush
returns timeout without pretending teardown completed.

`app_main` now performs bounded reverse cleanup of this display owner. Production
screen availability remains false: `vk_display_product.c` requires store,
screen-owner, font-profile, and separately admitted physical-display evidence,
and the physical acceptance constant is deliberately false. This build does not
advertise a screen capability or illuminate/test the connected panel.

Offline verification completed without device I/O:

```text
Display native suite: passed
Display ASan + UBSan: passed (ASAN_OPTIONS=detect_leaks=0)
Display TSan: passed
Complete native firmware runner: passed
```

The full native firmware runner and contract suite also pass (`21` passed and one
documented skip). A clean ESP-IDF 5.5.2 build completed after the concurrent
`firmware-screen-001` source stabilized. The resulting offline image is
`1,141,392` bytes with SHA-256
`6d05355d7c780439f50434a31182f87ba837a83c2a41d602f5d07b06ccbcde24`;
ESP image checksum `0x52` and appended validation hash
`7765acedc6ac5e9de6c4946fae74862ac6e49249a52a1ac1f6a1ffa6c1b552f1`
are valid. `tools/validate_build.py` confirms ESP32-S3, 16 MiB flash, 8 MiB
PSRAM, no forbidden linked entrypoints, and no sensitive markers. The production
ELF contains `vk_display_product_init` and `vk_display_start`.

Connected rendering, measured runtime RAM/PSRAM/stack, panel illumination, reboot
persistence, screen-owner production registration, and physical capability
admission remain separately gated; this task therefore stays `in-progress`.

## Production composition follow-up (2026-07-22)

- Display product now exposes only a typed instance/dependency boundary to the screen composition. Availability still requires store recovery, screen owner, exact font profile, transport readiness, and the immutable physical acceptance gate.
- The production gate remains false; no panel illumination or connected display operation occurred. Native firmware and Swift suites passed; ESP-IDF was unavailable in this agent environment. Task remains `in-progress`.

## Integrated clean-build follow-up (2026-07-22)

- Durable boot recovery now reconstructs the complete screen model/root and falls back from an invalid current candidate to the previous immutable revision; failure of both retains the safe root and unavailable capability.
- The complete native runner, permanent RAM-filesystem recovery integration test, focused ASan/UBSan tests, and contract suite pass. A fresh ESP-IDF 5.5.2 build completes all `1,982` steps and links `vk_led_init_fail_dark`, `vk_screen_restore`, `vk_asset_store_load_revision`, `vk_update_init`, `vk_vka1_validate`, input, and audio entry points.
- Integrated image: `1,223,248 bytes`, SHA-256 `bc1c43c3c66a57398fd9a99648d526d5a25860faef439a6e33e8d709f4d0b45a`; ESP checksum `0xb9` and validation hash `0a93a7788fef839c64fc9e2690d0060e3372e9a529c34a3bb20cf284483ce536` are valid. `validate_build.py` reports ESP32-S3, 16 MiB flash, 8 MiB PSRAM, no forbidden linked entrypoints, and no sensitive markers.
- This remains offline evidence. Physical screen admission, runtime RAM/PSRAM/stack measurements, panel rendering, and connected reboot persistence remain gated.
