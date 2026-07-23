---
id: client-led-001
scope: Swift LED capability and bounded configuration service
status: in-progress
depends-on: [client-usb-002, firmware-led-001]
---

## Objective

Implement the Swift typed LED capability/query/config/state service without exposing uncalibrated, raw-pixel, color, palette, or animation controls.

## Context

- `docs/product/led.md`
- `docs/product/usb-protocol.md`
- `mac/README.md`
- `docs/ui/client.md`

## Path

- `mac/Sources/VibeBoardKit/Protocol/`
- `mac/Sources/VibeBoardKit/LED/`
- `mac/Tests/VibeBoardKitTests/`

## Contract

- Decode exact `led` capability variants as an independent block in the atomic current-epoch snapshot. Never infer availability from firmware version or assets/screen.
- When absent, unavailable, malformed, uncalibrated, stale, or vendor firmware is active, expose no config operation. Typed query is legal only where the replacement protocol supports it.
- Encode only exact `vk_led_query(request_id)` and `vk_led_config(request_id,enabled,brightness)` with a nonzero current-epoch UInt32 request ID. Validate integer brightness against current `max_brightness`; reject rather than clamp.
- Serialize at most one config, own one non-extending 1,000 ms monotonic timeout, and publish changed state only after a matching current-epoch `source:"applied"` response. The actor captures response monotonic time and accepts acknowledgement only when it is strictly earlier than the absolute deadline. A `source:"query"` response, host write completion, duplicate prior state, exact-deadline response, later response, or stale epoch is not acknowledgement; a late applied response may update observed state but never completes the timed-out operation.
- Retry uses the same request ID only with a byte-equivalent body. Reconnect invalidates request IDs and cached responses.
- Disconnect/reconnect invalidates pending operations and prior limits. Persist no device override in this task; firmware configuration remains RAM/current-epoch state.
- Expose no public raw frame sender, pixel index, RGB/GRB bytes, color/palette editor, animation program, Bluetooth, or network transport.

## Verification

- Shared JSON goldens cover capability variants, request-correlated exact query/config/query-state/applied-state/error keys, unknown/extra/missing fields, Boolean/zero IDs, bounds, state invariants, and unavailable behavior.
- Actor tests cover current-epoch replacement, same-ID byte-equivalent retry, conflicting-ID/body rejection, query-vs-applied isolation, 1,000 ms deadline-before/exact/after cases, timer/response actor-queue races, detach/reconnect, duplicate/stale state, and malformed capability isolation.
- Strict-concurrency build and Swift tests pass.
- Independent review is required before `client-app-001` integration. No connected illumination occurs in this task.

## Implementation status

- Added strict independent LED capability decoding, exact typed query/config/state/error codecs, and an actor service with current epoch/snapshot gating, one fixed 1,000 ms deadline, byte-equivalent retry, applied-vs-query isolation, stale-context invalidation, and no raw pixel/color/animation sender.
- `LEDService` is integrated through `USBSession.sendLEDCommand`; the session actor is the only serialized writer, revalidates the current epoch/snapshot capability before and after the write, and routes typed LED events with the receive-side monotonic timestamp. Disconnect/reconnect context replacement invalidates pending requests and cached state. No raw sender was added.
- `AppModel` derives LED availability and state from the atomic replacement snapshot/events. `calibration_required` remains unavailable and exposes no configuration UI; typed controls can only be eligible for an exact `available:true` profile.
- The deterministic cancellation regression now waits for observed write entry rather than elapsed wall time, and the write loop yields/checks cancellation at EAGAIN boundaries. A cancelled partial write terminates as `.failed(.cancelled)`, clears replacement context/transfer handles, and closes the descriptor exactly once.
- Xcode beta strict-concurrency build passes. Focused USB/assets/VKA1/LED/widget/App tests pass (`66 tests / 6 library suites` plus `4 AppModel tests`). The full suite passes three consecutive runs (`187 tests / 24 library suites` plus `4 AppModel tests` per run). No device I/O occurred.