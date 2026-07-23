---
id: vka1-core-001
scope: cross-language VKA1 codec and validator
status: in-progress
depends-on: [asset-protocol-001]
---

## Objective

Implement allocation-bounded VKA1 parsing/validation in firmware and deterministic VKA1 construction/self-validation in Swift using one shared golden corpus.

## Context

- `docs/product/screen-assets.md`

## Path

- `firmware/components/vk_assets/`
- `firmware/tests/`
- `mac/Sources/VibeBoardKit/Assets/`
- `mac/Tests/VibeBoardKitTests/Assets/`

## Contract

- Enforce full-container SHA-256 with zeroed hash field, exact contiguous frame ranges, negotiated limits, little-endian RGB565, and per-frame raw/rowRLE.
- The canonical RowRLE encoder scans each row left-to-right and emits exactly one run per maximal sequence of equal UInt16 pixels. It never splits a maximal run or merges across rows. Checked conversion rejects an out-of-contract run count; Version-1 width `1...428` guarantees a valid canonical run fits UInt16.
- For each frame, select canonical rowRLE only when its complete byte count is strictly smaller than raw; equality selects raw. Header encoding bits equal exactly the union used by frame entries. Identical C/Swift inputs must produce byte-identical payloads, frame tables, complete containers, and hashes.
- Reject all overflow, alias, padding, truncation, unknown enum/bit, malformed RLE, trailing byte, and decoded-size mismatch.
- Pure validation performs no filesystem, LVGL, USB, or source-image decoding. Per-container acceptance does not imply screen acceptance; the screen task applies the checked per-instance/root-swap aggregate decoded-memory formula.

## Verification

- Shared complete binary/hash goldens are accepted identically by C and Swift. The corpus covers one-pixel, alternating, full-width maximal, row-boundary, equal-size raw selection, strictly-smaller rowRLE selection, mixed-encoding union bits, and complete-container SHA-256 known answers; verification compares complete bytes, not only decoded pixels.
- Mutation/property tests change every header/table field and cover integer limits, non-maximal/split encodings, cross-row runs, and other malformed runs.
- Fuzzing has bounded memory and no crash/out-of-bounds behavior.

## Implementation Evidence

Implemented the first complete cross-language codec slice on 2026-07-22:

- Swift deterministic writer, reader, hash verification, canonical raw/RowRLE selection, and decoded-frame model under `mac/Sources/VibeBoardKit/VKA1/`.
- C allocation-free validator and caller-buffer decoder under `firmware/components/vk_vka1/`.
- Shared complete binary corpus under `mac/Tests/VibeBoardKitTests/Fixtures/VKA1/`, including one-pixel, equal-size raw selection, full-width run, row-boundary reset, and mixed encoding-bit union cases.
- C native and ASan/UBSan corpus verification passed with `ASAN_OPTIONS=detect_leaks=0`.
- Full firmware native runner passed with the VKA1 corpus integrated.
- Swift strict-concurrency build passed. The focused VKA1 suite passed `4 tests / 1 suite`.
- ESP-IDF 5.5.2 clean build passed. Offline image size is `1,135,776` bytes and SHA-256 is `d637ddab4620da8d95a2a5f400fbbcd28557d1b7e857bc70e211ab3ad7316338`.
- Two complete Swift-suite attempts reached an unrelated concurrent `USBSession` test change and terminated in the Swift Testing runtime with `Not enough bits to represent the passed value`; no full-suite pass is claimed for this slice.

The task remains `in-progress`: exhaustive mutation/property/fuzz coverage and independent integrated review are still required. No filesystem, USB, LVGL, format, storage mutation, or connected operation was performed.
