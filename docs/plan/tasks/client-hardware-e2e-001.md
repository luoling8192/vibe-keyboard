---
id: client-hardware-e2e-001
scope: connected vendor USB input/audio acceptance
status: in-progress
depends-on: [client-usb-002, client-input-001, client-audio-001]
---

## Objective

Connect the production Swift USB session, input router, and audio recorder to the current VibeBoard over USB and establish real four-key and speech-recording acceptance evidence before application composition.

## Context

- `docs/INDEX.md`
- `docs/product/hardware.md`
- `docs/product/usb-protocol.md`
- `docs/product/input-audio.md`
- `mac/README.md`
- `docs/plan/analysis/usb-runtime.md`

## Path

- `mac/Sources/VibeBoardDiagnostic/`
- `mac/Sources/VibeBoardKit/USB/`
- `mac/Sources/VibeBoardKit/Input/`
- `mac/Sources/VibeBoardKit/Audio/`
- `mac/Tests/VibeBoardKitTests/Integration/`
- `docs/product/input-audio.md`
- `docs/plan/analysis/`

## Contract

- Use only the production USB discovery/session, frame decoder, gesture router, action router, and audio recorder boundaries; no BLE, network, mock transport, raw-serial replacement, or vendor app runtime dependency.
- Add explicit bounded diagnostic modes for observing canonical button events and recording one AudioFrame session. Safe handshake commands remain allowlisted; no raw frame, provisioning, OTA, flash, erase, voice-gain, or asset mutation command is exposed.
- Capture each physical key independently and map physical left-to-right order to canonical `k1...k4` from observed USB events.
- Capture down/up/click timing evidence without executing destructive or shell actions. Use inert configured actions to prove one accepted physical gesture routes exactly once.
- Record real speech from the device microphone, require a valid first frame, monotonic sequence, final EOS, and successful atomic Ogg completion.
- Decode the resulting Ogg with `ffprobe` and `ffmpeg`; verify mono Opus, the original input rate in `OpusHead`, nonzero duration, and nonzero decoded audio energy. Decoder output at 48 kHz is expected.
- Do not persist or commit raw audio, boot logs, serial captures, or device identifiers beyond the already documented normalized evidence. Temporary evidence uses private permissions and is removed after stable non-sensitive results are recorded.
- Detach/reconnect creates a new USB, gesture timestamp, and audio-session epoch and must not emit a pending action or append to the prior recording.
- Replacement LED calibration is explicitly outside this vendor input/audio task and belongs only to `replacement-led-e2e-001` after its firmware, client, bootstrap, and calibration-harness dependencies pass.

## Verification

1. `swift test` and strict-concurrency build pass with production-boundary integration tests.
2. A bounded connected capture identifies all four physical keys and updates the canonical contract without guessing.
3. Four distinct inert mappings each execute once for one physical gesture and never duplicate across firmware click plus host derivation.
4. A key-controlled real-speech recording receives EOS, finalizes atomically, and decodes successfully.
5. `ffprobe`/`ffmpeg` evidence includes codec/channels/duration and a decoded-energy measurement without retaining speech content.
6. USB detach/reconnect clears pending input/audio work and returns to a fresh ready session.
7. Independent review verifies commands, logs, temporary-file permissions, production wiring, and the connected evidence.
8. Independent review confirms this task did not invoke replacement calibration, firmware mutation, raw LED control, or any transport beyond the production USB input/audio boundary.