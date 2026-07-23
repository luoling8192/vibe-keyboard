---
id: firmware-bootstrap-001
scope: first replacement application ROM-download bootstrap
status: done
depends-on: [firmware-hardware-001, firmware-usb-001]
---

## Objective

Define, review, and only after explicit authorization execute the first host-controlled write of the replacement application to inactive `ota_1`, its exact readback verification, and a separate minimal otadata activation while preserving the vendor recovery image.

## Context

- `docs/product/hardware.md`
- `docs/product/usb-protocol.md`
- `docs/firmware/README.md`

## Path

- `firmware/tools/`
- `firmware/tests/`
- `docs/plan/analysis/`

Private backup and eFuse outputs remain outside the workspace.

## Contract

- Use only the independently verified ESP32-S3 ROM download/esptool path; vendor OTA and replacement `vk_update` cannot bootstrap the first image.
- Fail closed unless read-only Secure Boot, flash-encryption, anti-rollback, eFuse, chip, port, partition, private backup, and recovery checks all match reviewed expectations.
- Stage writes exactly the reviewed app bytes at `ota_1 @ 0x520000`, then reads back exact length and verifies byte identity, SHA-256, checksum, appended hash, app descriptor, chip, revision, and maximum size.
- Stage never writes bootloader, partition table, `ota_0`, NVS, NVS keys, PHY, storage, or otadata.
- Activation is separately authorized. It preserves the verified primary vendor otadata sector and writes only the still-erased secondary sector using a hard-coded reviewed ESP-IDF 5.5.2-compatible golden record selecting ota_1; readback must be byte-identical and decode/CRC checks must pass.
- This task does not install a replacement bootloader and does not claim automatic rollback or `PENDING_VERIFY`. Recovery depends on the proven ROM path and private vendor backup/ota_0/otadata restore procedure.
- No command executes until independent review passes and the user explicitly authorizes the connected mutation.

## Verification

- Pure tooling tests validate address ranges, exact write lengths, image metadata, golden otadata decoding/CRC/slot selection, command allowlists, readback mismatch, and fail-closed unknown security state.
- Dry-run output contains no private bytes, secrets, NVS values, or executable write command without an explicit mutation flag.
- Independent review verifies every command/address/hash and confirms stage/activate remain separate.
- Connected evidence records only non-sensitive hashes/results and proves all protected regions unchanged.
- After separately authorized activation and boot, run the replacement USB acceptance through the production Swift session: exact VID/PID discovery, `transport/usb`, `device_info` then current-epoch `vk_capabilities`, two-second heartbeat/five-second lease behavior, host close, and reconnect with no interleaved console/log/panic bytes. Failure invokes the reviewed ROM recovery path; it never triggers an unreviewed retry or broader write.

## Implementation Evidence (Offline Review Passed)

- `firmware/tools/bootstrap_core.py` is a pure validation module with no serial/ROM transport. It validates exact ESP32-S3 image structure, 4-byte segment alignment and bounds, every mapped segment's ESP32-S3 load-address/flash-offset relation using ESP-IDF 5.5.2 descriptor-derived MMU page sizes, followed by a separate bootstrap policy requiring the reviewed 64 KiB build, XOR checksum, appended SHA-256, the complete 256-byte `esp_app_desc_t`, reviewed candidate SHA-256, exact ESP-IDF 5.5.2 chip/eFuse block revision min/max semantics bound to explicit UInt16 security evidence, `ota_1 @ 0x520000` range, secondary otadata `0x012000/0x1000` range, exact readback, and complete stage protected-region hashes and exact 16 MiB pre/post flash evidence proving every byte outside the authorized secondary otadata sector unchanged.
- The security evidence gate requires an exact named JSON object and fails closed for missing, unknown, non-boolean, enabled Secure Boot, enabled flash encryption, enabled anti-rollback, invalid/non-UInt16 chip or eFuse block revision, unknown revision-disable bits, unreviewed eFuse, missing ROM recovery, stale port/partition identity, missing private-backup review, or missing recovery review. Candidate and activation plans consume and hash-bind this same evidence. The stage plan additionally parses and SHA-256-binds the exact reviewed 0xC00-byte ESP-IDF partition-table binary, cross-checks its complete manifest against reviewed geometry, and requires the pinned ESP-IDF MD5 marker layout: erased reserved bytes, the exact MD5 of every preceding 32-byte entry, one marker after the reviewed entries, and erased trailing bytes. It binds candidate size/hash and does not infer hardware state from `sdkconfig`.
- ESP-IDF 5.5.2 otadata support is reproduced from the named `esp_ota_select_entry_t` layout and `bootloader_common_ota_select_crc()` behavior: 32-byte entry, CRC32 over little-endian `ota_seq`, `(sequence - 1) % ota_partition_count` slot selection, `ESP_OTA_IMG_UNDEFINED` for the retained non-rollback vendor bootloader, and erased remainder. Golden generation rejects zero, erased, wrong-slot, and near-wrap sequences.
- `firmware/tools/bootstrap_offline.py` exposes only local validation and non-executable plan commands. Stage and activate are separate subcommands and require distinct reviewed authorization hashes. `plan-activate` requires the exact primary/erased-secondary sectors, their reviewed identities, current `ota_0` bytes and reviewed hash, exact staged/read-back `ota_1` bytes and reviewed hash, exact partition-table binary/hash, and security evidence; it calls the same activation validator and binds plan evidence by SHA-256. A separate `validate-activation-readback` requires exact golden secondary readback while proving primary otadata, `ota_0`, staged `ota_1`, and the complete 16 MiB flash readback changed only in the authorized secondary sector, while directly cross-binding primary otadata, ota_0, and ota_1 bytes. Plans state `transport: unavailable_offline` and `executable: false`; there is no port, flash, reset, erase, monitor, OTA, or device execution option.
- Recovery plans are allowlisted to bounded `ota_0`, complete `otadata`, or complete full-flash artifact identities; they contain hashes/addresses/sizes only, never artifact paths or bytes, and remain non-executable.
- `firmware/tests/test_bootstrap_offline.py` has 28 offline tests covering candidate truncation/oversize/header/chip/revision/complete-descriptor/checksum/hash failures, unaligned segments, mapped DROM/IROM MMU offset mismatch, legacy-zero/configured-64-KiB and ESP-IDF structural MMU exponents 0/15/16/17, unsafe exponent rejection, separate reviewed-64-KiB policy rejection, `min_chip_rev_full=0xffff`, eFuse min/max and disable-bit behavior, non-boolean/out-of-range revision evidence, exact stage/activation boundaries, stage security/exact partition-table/candidate binding, malformed/missing/duplicate/misplaced partition MD5 markers, stale/wrong partition MD5 digests, generated production partition-table regression, activation evidence binding and exact whole-flash post-readback proof including reserved/unpartitioned ranges and `ota_1`, private-path error redaction, short/long/different readback, fail-closed security state, otadata CRC/state/selection/erasure/order/wrap, stage/activate separation, recovery allowlists, bounded redaction, and CLI absence of execution/port options. All tests are offline; no device I/O occurred.
- Fifth independent offline review passed after differential ESP-IDF 5.5.2 partition-table MD5 validation, 28 bootstrap tests, 15 contract tests with one documented environment-dependent skip, production image/partition regressions, full-flash mutation probes, path-redaction probes, and production import audit.
- A local production application artifact was parsed successfully as ESP32-S3, size `514928`, project `vibe_keyboard`, appended validation hash `34c80d932f1f4aacce4727189caaf1cdb7028942043f4437bfc467dde0175de7`. This is local image-format evidence only, not authorization to stage that build. Connected read-only chip/eFuse/port/partition/backup/recovery evidence requires separate explicit authorization; staging `ota_1` requires a distinct later mutation authorization; secondary-otadata activation requires another separate authorization after exact stage readback. Reset and recovery execution remain unauthorized.
