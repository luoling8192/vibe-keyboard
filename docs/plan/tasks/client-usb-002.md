---
id: client-usb-002
scope: macOS USB transport
status: done
depends-on: [client-usb-001]
---

## Objective

Implement VibeBoard-specific IOKit discovery, serial file-descriptor lifecycle, framed read/write integration, and a non-destructive diagnostic executable.

## Context

- `docs/INDEX.md`
- `docs/product/hardware.md`
- `docs/product/usb-protocol.md`
- `mac/README.md`

## Path

- `mac/Package.swift`
- `mac/Sources/VibeBoardKit/USB/`
- `mac/Sources/VibeBoardDiagnostic/`
- `mac/Tests/VibeBoardKitTests/USB/`
- `mac/README.md`

## Contract

- Enumerate `IOSerialBSDClient` and read USB identity from the parent chain.
- Require VID `0x303a` and PID `0x1001`.
- Read dynamic `IOCalloutDevice`; never hard-code `/dev/cu.usbmodemXXXX`.
- Normalize serial/device ID according to the USB contract.
- Open `O_RDWR | O_NONBLOCK | O_NOCTTY`; apply raw mode and `CLOCAL | CREAD`; do not set or expose a fixed baud rate.
- Serialize reads, parser mutation, and partial writes.
- Handle `EINTR` and nonblocking `EAGAIN` explicitly with bounded wait/cancellation.
- Close exactly once on detach, EOF, cancellation, or error.
- Preserve mixed boot-log bytes as bounded diagnostics while parser resynchronization remains deterministic.
- Bound every public async event queue. An event-buffer overflow is observable and terminal when dropping it could invalidate lifecycle/control/audio correctness; it never silently produces a truncated recording.
- Surface monitor registry/discovery failures as typed events and recover explicitly after transient failures; never treat a failure as an empty device set.
- The CLI defaults to read-only inspection. Sending transport/device-info commands requires an explicit non-destructive flag. It must not expose provisioning or OTA commands.

## Verification

- `swift test` covers registry-property normalization, target filtering, ambiguous devices, partial writes, EINTR/EAGAIN, EOF/error/cancel cleanup, and parser integration through injected system-call boundaries.
- `swift run VibeBoardDiagnostic list` identifies the connected target by VID/PID/serial/path.
- A connected read-only run records bounded text/protocol diagnostics without firmware writes or secret output.
- If the vendor handshake remains incomplete, the test reports a typed timeout and evidence; it must not claim readiness.
