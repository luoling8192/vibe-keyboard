---
id: firmware-input-001
scope: replacement firmware four-key scanner and USB events
status: in-progress
depends-on: [firmware-hardware-001, firmware-usb-001, firmware-audio-001]
---

## Objective

Implement a bounded active-low four-key scanner, ordered input owner, and correlated audio-control owner that emit canonical button/audio interactions through the real USB service.

## Context

- `docs/product/hardware.md`
- `docs/product/input-audio.md`
- `docs/product/usb-protocol.md`
- `docs/firmware/README.md`

## Path

- `firmware/components/vk_input/`
- `firmware/components/vk_audio/`
- `firmware/components/vk_usb/`
- `firmware/main/`
- `firmware/tests/`

## Contract

- Scan k1=GPIO0, k2=GPIO18, k3=GPIO17, k4=GPIO16 every 5 ms with two stable samples, active-low pull-ups. A low-level interrupt may wake scanning but emits no application event.
- Use exactly three owners: the scanner owns only GPIO/debounce and a fixed 32-item semantic FIFO; the input owner owns ordered USB handoff, epoch-local mode/key, command generation, and button/audio-attempt association; the audio-control owner alone calls bounded synchronous audio prepare/release/cancel/stop/abort APIs and owns fixed four-item ordinary command/result mailboxes plus one separately reserved lifecycle-abort slot.
- Implement the exact command/result table. Every tuple carries `{epoch,generation,session}`; successful release retains its generation for asynchronous runtime-failure correlation until stop/runtime-failure/abort/taint termination. Stale tuples receive bounded idempotent cleanup only and cannot mutate current state, release capture, emit a current USB value/error, resolve a current barrier, or acknowledge lifecycle quiescence.
- Emit exact typed button variants through the expected-epoch façade: down forbids duration; up/click require `UInt32 duration_ms`; optional `session_id` is nonzero `UInt32` and absent rather than zero when sessionless. Reject missing/extra/Boolean/out-of-range fields. Retry preserves the complete immutable value and FIFO order. Local FIFO/mailbox overflow uses the typed input reserved-terminal fail-epoch operation; it never overwrites or silently drops an older value.
- Follow the exact paused preparation order. In hold-to-talk, successful prepared state is handed off as `button_down(session)` before release permits capture. The button field is one audio-attempt identity, not proof of active recording. Prepare failure emits one ordinary down without a session. Release failure never emits another down; only successful cancel permits up/click to retain the prepared identity without AudioFrame/EOS. Cancel failure/taint closes the epoch, retains uncertain ownership, and discards not-yet-accepted values. The host establishes recording only on the first valid AudioFrame.
- Treat prepare, release, cancel, stop, and abort as voice-transition barriers. No FIFO entry overtakes a barrier. A third click-to-talk interaction during stop remains ordered, submits no duplicate stop, does not use the stopping session, and becomes a new start only after the matching stopped result. Proven-quiescent stop failure or current runtime failure resolves the barrier by stably removing all not-yet-accepted configured voice-key transitions, preserving every non-voice transition's relative order, and disabling new voice transitions for that epoch.
- Register the asynchronous lifecycle before USB start. USB owns token/generation and one exact 3,250 ms absolute deadline: 1,500 ms maximum current ordinary-call remainder, 1,500 ms abort/join, and 250 ms cleanup. `old_epoch == 0` is legal only for first new-epoch, whose proposed epoch is nonzero, and pre-epoch stopping, whose two epoch fields are zero; normal lease expiry and stopping require a nonzero old epoch and zero proposed epoch. Pre-epoch stopping performs bounded local cleanup and never exposes an epoch. USB invokes nonblocking begin without its lock and accepts a matching acknowledgement only when its lock-linearized monotonic publication time is strictly before the deadline; equality or later taints. Input admits abort through the reserved slot even when ordinary mailboxes are full. The callback never re-enters USB. Exactly one lifecycle request is pending: duplicates coalesce, transport during pending expiry rejects, and stopping supersedes with a fresh token/deadline while invalidating old acknowledgement. `QUIESCENT` requires bounded abort/join proof; timeout/taint invalidates late acknowledgements, retains uncertain ownership, closes the composition, and forbids the proposed epoch.
- Every epoch defaults to `hold_to_talk` and logical `k4`; the client may replace both with exact typed commands. Configuration is RAM-only, rejects mutation during prepare/release/run/stop/abort/cancel, never writes NVS, and does not accept `voice_gain`.
- Lease expiry/disconnect clears pending presses only after audio quiescence and never synthesizes up/click/EOS. EOS is ordered after the final accepted voice-key click on normal stop only.
- Do not guess physical left-to-right order or LED mapping. Use only ESP32-S3 built-in USB Serial/JTAG; do not add BLE, Wi-Fi, network, TinyUSB, USB OTG CDC, or USB Audio Class fallback.

## Verification

- Native timeline tests cover bounce, simultaneous k1→k4 ordering, wrap-safe duration, held-across-epoch suppression, disconnect, duplicate suppression, and exactly-once click.
- Deterministic owner tests cover every prepare/release/stop/abort barrier, release failure without duplicate down, prepared-session up/click without AudioFrame/EOS, stop-pending third interaction, FIFO non-overtaking, expected-epoch retry, and reserved-terminal overflow.
- Correlation tests cover every table row, inject runtime failure before/after stop and late results across epoch/generation/session changes, and prove stale cleanup cannot mutate or acknowledge current state.
- Lifecycle tests prove first-new-epoch and pre-epoch-stopping zero-sentinel behavior, pre-epoch stop cleanup without epoch publication, no-epoch lease-expiry no-op, callback lock exclusion, asynchronous token matching, no synchronous USB re-entry, abort admission with both ordinary mailboxes full, an adversarial 1,500 ms current-call remainder followed by 1,500 ms abort/join and 250 ms cleanup, old-worker quiescence before acknowledgement, acknowledgement immediately before/exactly at/immediately after the absolute deadline, duplicate/mismatched/late acknowledgement rejection, overlapping request coalescing/rejection/stopping supersession, 3,250 ms absolute timeout/taint behavior, no new-epoch exposure after timeout, and the unchanged 1,500 ms per-call audio join bound.
- Component tests inject GPIO/clock/USB/audio adapters and prove bounded startup, reverse cleanup, and no scanner wait on USB/audio control.
- Independent contract review must pass before implementation starts. Connected human-labeled capture later maps physical positions and confirms one routed inert action per gesture; it is not implied by offline review.

## Development Evidence (Pending Independent Review)

- Added typed asynchronous USB/input lifecycle registration with nonzero token/generation, a USB-owned absolute 3,250 ms deadline, strict-before acknowledgement publication, zero-sentinel first epoch/pre-epoch stop handling, lifecycle taint, and no proposed-epoch publication after timeout.
- Input now owns explicit scanner/input/audio-control boundaries, fixed 32-item FIFO, four-item ordinary command/result mailboxes, one separately reserved abort slot, configuration mailbox, atomic admission/snapshots, and joined task teardown.
- Added correlated runtime-failure delivery, stable voice-key filtering, prepare-failure sessionless down, release-failure cancellation without duplicate down, and stop-barrier FIFO preservation.
- Preserved and regressed asset capability/ABI/active-transfer paths in the full native suite.
- Offline verification passed: full native suite; USB lifecycle native TSan; contract suite `21 passed / 1 documented skip`; ESP-IDF 5.5.2 clean build.
- Offline image: 1,140,512 bytes; SHA-256 `e0f967e7bff3b5500dc35dbc93d171560fb9857d6dda76d23f81c1a9d3b68bd9`.
- Task remains `in-progress` pending the planned integrated independent review. No device I/O, real recording, flash, reset, BLE, Wi-Fi, network, TinyUSB, or UAC operation was performed.
