---
id: client-app-001
scope: native macOS application
status: in-progress
depends-on: [client-usb-002, client-input-001, client-audio-001, client-assets-001, client-led-001, firmware-input-001, firmware-audio-001, firmware-led-001, firmware-display-001, firmware-update-001, firmware-bootstrap-001]
---

## Objective

Implement the native SwiftUI macOS application shell and connect every UI control to the real USB, screen, key, audio, and firmware services.

## Context

- `docs/INDEX.md`
- `mac/README.md`
- `docs/ui/client.md`
- `docs/product/README.md`

## Path

- `mac/Package.swift`
- `mac/Sources/VibeKeyboardApp/`
- `mac/Tests/VibeKeyboardAppTests/`
- `docs/ui/client.md`

## Contract

- Implement Device, Screen, Pets, Keys, Audio, and Firmware pages from the ASCII layout. LED configuration appears only inside Device capability details after a validated calibrated `led.available:true` snapshot.
- Use one `@MainActor AppModel`; views do not parse/write device bytes.
- Every disabled/loading/error/recording/upload/firmware state is explicit.
- No UI control uses a stub, mock, fake response, or Bluetooth/network fallback in production composition.
- Firmware stage and activate are separate states. Stage requires validated backup/active-image evidence; activation revalidates it and requires explicit confirmation. First bootstrap uses the separately reviewed ROM stage/secondary-otadata activation flow; later update uses RAM-epoch stage/activate. The app never uses vendor immediate-finish OTA for first write.
- Accessibility permissions are requested only when a configured host action needs them.

## Verification

- `swift test` covers app-model integration and major interaction state transitions.
- Build the executable with the available Swift toolchain; record that full Xcode UI tests require Xcode if unavailable.
- Manual connected-device acceptance covers all E2E flows in delivery decomposition.

## Development Evidence

The first production SwiftUI vertical slice is implemented under `mac/Sources/VibeKeyboardApp/` with one `@MainActor AppModel`, the six documented pages, current-epoch asset/screen capability gating, canonical key mapping persistence, key-controlled AudioFrame-to-Ogg state, and disabled firmware/format/LED mutation surfaces. Production composition creates the real USB monitor/session and typed asset/screen services; test fakes exist only in `VibeKeyboardAppTests`.

Offline verification uses the Xcode beta toolchain:

```text
swift build -Xswiftc -strict-concurrency=complete
swift test --filter AppModelTests
```

The strict build passes. Production composition now integrates typed LED query/config and typed widget updates through the single `USBSession` actor writer; `AppModel` consumes current LED availability/state and typed widget acknowledgement/error events. LED controls remain absent while production advertises `calibration_required`; no raw JSON sender or transport fallback exists.

Focused USB/assets/VKA1/LED/widget/App verification passes (`66 tests / 6 library suites` plus `4 AppModel tests`). The complete suite passes three consecutive runs (`187 tests / 24 library suites` plus `4 AppModel tests` per run), including deterministic cancelled-partial-write terminal cleanup. Independent integration review and connected acceptance remain required. No connected device operation was executed.
