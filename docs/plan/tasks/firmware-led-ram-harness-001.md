---
id: firmware-led-ram-harness-001
scope: offline pure-RAM one-stimulus LED calibration harness and artifact validator
status: blocked
depends-on: [firmware-hardware-001, firmware-led-001, firmware-bootstrap-001]
---

## Objective

Build and independently review an ESP32-S3 pure-RAM, one-stimulus LED harness and its fail-closed artifact validator without writing flash, changing OTA selection metadata, exposing a production calibration API, or authorizing connected execution.

## Context

- `docs/product/led.md`
- `docs/product/hardware.md`
- `docs/product/usb-protocol.md`
- `docs/plan/tasks/firmware-bootstrap-001.md`

## Path

- `firmware/calibration/led/`
- `firmware/tools/`
- `firmware/tests/`
- `docs/plan/analysis/`

## Contract

- Pin ESP-IDF 5.5.2, target ESP32-S3, and `CONFIG_APP_BUILD_TYPE_PURE_RAM_APP=y`. Esptool 4.12 `--chip esp32s3 --no-stub load_ram` is only the future reviewed execution mechanism; this task never opens a port, enters ROM download, resets, loads RAM, or accesses a device.
- Build one immutable canonical descriptor per image: one raw pixel `0...16`, one logical channel, exact value `1`, duration `1...250 ms`, reviewed board-profile hash, and a public 128-bit nonce. No authorization hash is embedded and no runtime command changes it.
- Require ELF32 little-endian Xtensa `ET_EXEC` and parse every full `PT_LOAD` header. Version 1 requires `p_paddr == p_vaddr`, `0 < p_filesz <= p_memsz`, checked file/address ends, valid alignment, and classification only from `p_flags`. `PF_X` file-backed and zero-fill ranges must fit half-open IRAM `[0x40370000,0x403E0000)`; non-executable data ranges must fit half-open DRAM `[0x3FC88000,0x3FD00000)`. Reject writable executable, empty/BSS-only load segments, IROM/DROM, PSRAM/EXTRAM, RTC, overlap, alias, wrap, and out-of-range ranges.
- Generate with pinned esptool 4.12 `elf2image --chip esp32s3 --use_segments --ram-only-header`. Reproduce `ImageSegment.pad_to_alignment(4)` exactly: `padded_filesz = checked((p_filesz + 3) & ~3)`, require `padded_filesz <= p_memsz`, and project `(p_paddr, ELF[p_offset..<p_offset+p_filesz] || zero^(padded_filesz-p_filesz))`. Apply half-open allowlist, overlap, adjacency, merge, image-length, and checksum checks to padded ranges. Address-sort and merge only padded-adjacent same-memory-type/equal-checksum segments; never fill a gap or append additional BSS bytes. Compare every ordered image segment/address/complete padded byte string, entry point, aligned checksum, complete file end, and SHA-256 against both the independent reproduction and pinned esptool differential image. Reject missing, extra, split, reordered, differently merged/padded, trailing, or mismatched bytes. Prove the reviewed startup zeros only `[p_paddr+padded_filesz,p_paddr+p_memsz)` and cannot overwrite file-backed/padding bytes.
- Produce the byte-exact canonical descriptor, harness manifest, and external authorization record defined by `docs/product/led.md` and its single known-answer fixture. Raw `*_sha256` fields always equal `SHA-256(complete bytes)`; `stimulus_identity`, `harness_manifest_identity`, and `authorization_identity` always use their exact NUL-terminated domain separators. Bind complete image, ELF, map, sdkconfig, toolchain/ESP-IDF/esptool identity, and stimulus bytes without paths or secrets. The later authorization record is external and never feeds back into the image.
- Prove from an actual clean build and link map that RMT plus the pinned `led_strip` implementation fits internal RAM and introduces no flash-mapped segment. Tool support alone is not acceptance.
- Reject flash write/erase, partition, OTA, NVS, selection-metadata, USB command/parser, BLE, Wi-Fi, network/socket, TinyUSB, USB OTG CDC, and UAC symbols. Do not fall back to a temporary flash image.
- Before any nonzero call, prove initial all-off and arm an absolute auto-off deadline plus independent watchdog. Completion performs `clear → refresh` by the descriptor deadline and enters an inert no-drive loop. A watchdog reset is not restoration proof.
- Produce only offline build artifacts and validators. Independent offline review is required before `firmware-led-calibration-001` can request any connected authorization.

## Implementation Attempt

The reviewed ESP-IDF 5.5.2 pure-RAM target was attempted offline with the pinned `espressif/led_strip` 3.0.3 RMT backend and no device transport. Compilation succeeded through the harness and RMT sources, but final linking failed because the ESP32-S3 pure-RAM configuration excludes the cache implementation required by `esp_driver_rmt`: `cache_hal_get_cache_line_size` and `esp_cache_msync` remained unresolved from `rmt_tx.c`. `esp_mm/CMakeLists.txt` conditionally omits `esp_cache_msync.c` for pure-RAM ESP32-S3 because `CONFIG_SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE` is absent.

This is the contract's explicit RMT-fit gate failing, not an authorization or tooling inconvenience. The incomplete experimental target was removed. The task is blocked; it must not fall back to a temporary flash image, add unreviewed cache stubs, or execute ROM `load_ram`. A future revision requires either a separately reviewed cache-independent RMT/LED transport or the later bootloader/update calibration path.

No serial port, ROM entry, reset, `load_ram`, flash operation, or LED stimulus was executed.

## Verification

- Clean pure-RAM builds cover representative pixel/channel descriptors and prove byte-identical rebuilds for the same inputs.
- Image/ELF/map validators reject wrong ELF class/data/type/machine/entry, every program-header field mismatch, `p_paddr != p_vaddr`, invalid flags/alignment, disallowed half-open address edge, file or memory wrap, overlap/alias, BSS-only loads, map/BSS disagreement, `padded_filesz > p_memsz`, nonzero alignment padding, padding collision, missing/extra/split/reordered/differently merged/padded/image-byte mismatch, stale hash, descriptor mismatch, forbidden symbol, IROM/DROM output, and non-pure-RAM sdkconfig.
- Adversarial tests pair individually valid but unrelated ELF/image/map/manifest files and require rejection. Pinned esptool differential generation and independent projection must produce byte-identical complete images and the same complete image SHA-256. Known vectors include `41 42 43 → 41 42 43 00`, padded adjacency that merges, raw adjacency that collides after padding, a gap that stays unfilled, and padding beyond `p_memsz`.
- Canonical-byte tests load the single fixture from `docs/product/fixtures/led-calibration-artifacts-v1.json`, reproduce descriptor/manifest/authorization bytes including final LF, and prove every raw digest and domain-separated identity. Raw-for-identity and identity-for-raw substitutions reject.
- Failure injection proves timer/watchdog arming precedes the sole value-1 pulse and every failure path converges to fail-dark/inert behavior.
- The task performs no serial, USB device, ROM entry, reset, `load_ram`, flash, LED illumination, or other device I/O.
