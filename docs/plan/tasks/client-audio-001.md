---
id: client-audio-001
scope: macOS Opus recording
status: done
depends-on: [client-usb-001]
---

## Objective

Implement session/sequence validation and a streaming Ogg Opus writer for the device's verified 16 kHz mono, 60 ms Opus frames.

## Context

- `docs/INDEX.md`
- `docs/product/input-audio.md`
- `docs/product/usb-protocol.md`
- `mac/README.md`

## Path

- `mac/Sources/VibeBoardKit/Audio/`
- `mac/Tests/VibeBoardKitTests/Audio/`
- `docs/product/input-audio.md`

## Contract

- Use the verified first=`0x01`, final=`0x02`, 60 ms/960-sample packet, pre-skip 312, and granule increment 2880 contract.
- Validate one active session and monotonic sequence.
- Surface gaps, duplicates, session replacement, no-audio, malformed packet, output I/O, cancellation, and finalization as typed state/errors.
- Emit valid `OpusHead`, `OpusTags`, data pages, CRCs, continuation/EOS flags, lacing, and monotonic granule positions.
- Stream to a private temporary file and atomically activate an optional saved recording.
- Do not log payloads by default.

## Verification

- Golden byte/CRC/page fixtures independent of the implementation.
- `ffmpeg` decodes generated recordings without errors; `ffprobe` reports mono Opus with the standard 48 kHz decoder output timebase, while `OpusHead` records the verified original 16 kHz input rate.
- Tests cover sequence/session failures, page boundaries, maximum lacing, EOS, cancellation, and partial file errors.
- This module task passes on the offline contract above. The production-boundary real-speech, EOS, decodability, and nonzero-audio gate is owned by dependent task `client-hardware-e2e-001`; it remains mandatory for product completion and cannot be replaced by synthetic packets.
