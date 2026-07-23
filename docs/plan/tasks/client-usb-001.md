---
id: client-usb-001
scope: macOS protocol core
status: done
depends-on: []
---

## Objective

Create a Swift package and implement the pure `VibeBoardKit` frame/model/command codec for the verified vendor USB contract. No hardware I/O or UI belongs in this task.

## Context

- `docs/INDEX.md`
- `docs/product/usb-protocol.md`
- `docs/product/input-audio.md`
- `mac/README.md`
- `docs/plan/analysis/vendor-model-contracts.md`

## Path

- `mac/Package.swift`
- `mac/Sources/VibeBoardKit/Protocol/`
- `mac/Tests/VibeBoardKitTests/Protocol/`
- `docs/product/usb-protocol.md`

## Contract

Implement:

- frame type allowlist;
- incremental parser with one-byte resynchronization diagnostics;
- ordinary body-length framing, 4096-byte total maximum, and bounded receive buffer;
- special 16-byte audio header parsing;
- `StateEvent` with exact snake-case coding keys;
- `AudioFrame` model;
- JSON type-`0x10` encoder with UInt16 and total-frame bounds;
- typed commands for `transport`, `get_device_info`, `ui_state`, `ping`, `interaction_mode`, `voice_key`, and `voice_gain` only where value constraints are verified;
- OTA wire structures only if all per-type length rules are represented explicitly; no generic assumption may flatten their inconsistent vendor encodings.

Do not implement guessed flag semantics, gain range, OTA chunk size, screen commands, or protocol acknowledgements.

## Verification

Run `swift test` and cover:

1. one-byte and arbitrary-chunk incremental input;
2. multiple frames in one append;
3. invalid version/type/oversize one-byte resynchronization;
4. incomplete ordinary and audio frames retained;
5. 4096 boundary and receive-buffer exhaustion;
6. state JSON success, optional fields, exact coding keys, invalid UTF-8/JSON/missing event;
7. audio fixed-header validation, little-endian fields, payload bounds, reserved-byte tolerance;
8. golden outbound bytes for all implemented commands;
9. rejection of unrepresentable body/frame length and invalid typed values;
10. no checksum, tail marker, BLE, or baud-rate concepts in the protocol API.
