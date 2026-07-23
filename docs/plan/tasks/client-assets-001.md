---
id: client-assets-001
scope: macOS asset conversion, preview, and resumable transfer
depends-on: [vka1-core-001, firmware-asset-transfer-001, firmware-screen-001]
status: in-progress
---

## Objective

Implement deterministic source conversion, VKA1 self-validation, layout/pet/widget preview models, and typed resumable USB transfer against real replacement firmware.

## Context

- `docs/product/screen-assets.md`
- `mac/README.md`
- `docs/ui/client.md`
- `docs/plan/tasks/asset-protocol-001.md`

## Path

- `mac/Sources/VibeBoardKit/Assets/`
- `mac/Tests/VibeBoardKitTests/Assets/`
- `mac/Package.swift`

## Contract

- Decode PNG/JPEG and GIF/APNG with bounded ImageIO/CoreGraphics integration. Prove target-macOS disposal/blend behavior with goldens or use a dedicated bounded compositor; never assume composited frames.
- Apply the exact EXIF/sRGB/alpha integer formula, pixel-center bilinear rational mapping/rounding, fit/crop odd-pixel rules, no dithering, and RGB565 packing from the product contract.
- Produce full-container-hash VKA1 with per-frame raw/RLE and pass the shared validator before transfer.
- Validate current-epoch optional feature blocks; treat zero upload capacity as full/unavailable while retaining advertised management operations.
- Preview uses the same fixed row/column placement, exact widget-target binding, canonical numeric formatting, immutable hashed font metrics fixture, glyph rejection, aggregate decoded-memory admission, geometry/color vectors, and nearest-neighbor presentation of canonical 428×142 pixels.
- Transfer sends only typed JSON and `AssetChunkFrame`, resumes from exact durable device offset, and preserves prior active revision on error/disconnect.
- Widget providers emit bounded typed values with monotonic sequence; provider failure is stale/error, never fabricated zero.

## Verification

- Golden source/color/alpha/EXIF/fit/RGB565/raw/RLE/hash and malformed-source tests.
- GIF/APNG disposal/blend/timing fixtures on the supported macOS baseline.
- Shared layout geometry, clipping, revision, widget, and pet preview vectors match firmware.
- Real transfer integration covers interruption/resume/hash/no-space/reboot persistence and preview-to-device captures.

## Development Evidence

Implemented the first complete offline client-assets vertical slice:

- bounded ImageIO/CoreGraphics source admission and sRGB RGBA extraction;
- EXIF orientation, integer alpha compositing, pixel-center bilinear sampling, fit placement, and RGB565 conversion;
- deterministic VKA1 production through the shared codec;
- layout row/column geometry and deterministic widget numeric formatting;
- typed `AssetTransferService` and `ScreenConfigurationService` over the session-owned typed APIs, with storage formatting remaining unavailable without a verified-erased authorization.

Offline checks using the Xcode beta toolchain:

```text
swift build -Xswiftc -strict-concurrency=complete: passed
AssetConversionTests + ScreenPreviewTests + VKA1Tests: 12 tests / 3 suites passed
```

This task remains `in-progress`. Target-macOS GIF/APNG disposal/blend goldens, complete font/glyph preview rendering, full transfer fake-session integration, the final repository suite after concurrent changes settle, independent review, and connected replacement-firmware evidence remain outstanding. No device I/O or mutation was performed.

### Completed offline follow-up

The client now owns bounded GIF/APNG canvas composition rather than assuming ImageIO returns composited frames. The parser reads GIF frame rectangles/disposal and APNG `fcTL` rectangles/disposal/blend, accepts either patch-sized or target-macOS canvas-sized ImageIO frames, and applies source/over plus keep/background/previous deterministically. Checked-in target-macOS GIF/APNG binary fixtures cover frame rectangles, background/previous disposal, source blend, over blend, and variable timing.

Font preview now decodes the immutable canonical `vk-sans-v1.metrics.json`, verifies its complete SHA-256 against the negotiated `(id,version,metrics_sha256)` descriptor, rejects invalid/noncharacter/unsorted/unsupported scalars, and computes bounded baseline/advance placements without fallback glyphs. A language-neutral `preview-geometry-color-font-v1.json` fixture covers RGB565/alpha, row geometry, widget formatting, and font identity for later firmware consumption.

Asset completion is no longer inferred from disappearance of the active handle. `USBSession` retains one typed single-delivery outcome per transfer (`stored`, `aborted`, device rejection, or invalidation), and `AssetTransferService` waits for correlated `stored` or returns the exact terminal failure. Snapshot replacement, disconnect, and other replacement-state teardown record invalidation before clearing authorization. Dashboard widget transmission remains intentionally unopened because the production protocol/session does not yet expose a reviewed typed widget-update sender; no raw JSON fallback was introduced.

Offline checks using the Xcode beta toolchain:

```text
strict-concurrency build: passed
focused asset/preview/VKA1/USB session suite: passed
full suite run 1: 176 tests / 21 suites passed
full suite run 2: 176 tests / 21 suites passed
```

The task remains `in-progress` pending firmware consumption of the shared preview fixture, a reviewed typed widget-update session API, integrated firmware transfer/screen service, independent whole-chain review, and connected USB acceptance. No device I/O, format, storage/asset/screen mutation, flash, reset, BLE, network, TinyUSB, or UAC operation was performed.
