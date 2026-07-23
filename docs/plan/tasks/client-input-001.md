---
id: client-input-001
scope: macOS key configuration and action routing
status: done
depends-on: [client-usb-001]
---

## Objective

Implement versioned four-key configuration, deterministic gesture routing, and safe host actions for canonical `k1...k4` events.

## Context

- `docs/INDEX.md`
- `docs/product/input-audio.md`
- `docs/product/usb-protocol.md`
- `mac/README.md`

## Path

- `mac/Sources/VibeBoardKit/Input/`
- `mac/Tests/VibeBoardKitTests/Input/`
- `docs/product/input-audio.md`

## Contract

- Persist independent single/double/long mappings for all four canonical keys.
- Preserve vendor defaults as the initial profile without claiming physical order.
- Reject unknown keys, unknown action variants, invalid associated values, duplicate down/up state, and unsupported schema versions.
- Consume firmware click events as authoritative and derive only gestures the firmware does not emit.
- Ensure one physical gesture executes at most one action.
- Keep action execution behind injected permission/input/process/application/screen interfaces.
- Never interpolate device-controlled values into a shell command.
- Custom commands require explicit user-authored configuration, bounded execution, cancellation-aware SIGTERM→SIGKILL escalation, guaranteed child reaping, and exit-status reporting.
- Disconnect unconditionally clears pending gestures and resets the timestamp epoch before validating any new-session timestamp.
- Persistence and router update boundaries revalidate complete four-key/action invariants before mutation.
- Host monotonic down/up duration is authoritative for derived long presses; device duration remains diagnostic.

## Verification

Run `swift test` and cover model validation, persistence migration/atomic replacement, every action variant, permission failures, gesture timing boundaries, duplicate/out-of-order events, disconnect cancellation, and the invariant that one gesture executes at most once.
