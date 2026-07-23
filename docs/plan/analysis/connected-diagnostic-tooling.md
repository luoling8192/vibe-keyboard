# Connected Diagnostic Tooling

## Status

Production-boundary tooling is implemented and offline-verified. Real key order and real speech evidence still require a user-operated connected run.

## Commands

```text
VibeBoardDiagnostic keys --allow-safe-commands --duration 20
VibeBoardDiagnostic record --allow-safe-commands --output /absolute/private/file.ogg --timeout 30
```

Both commands require the explicit safety flag. The CLI has no raw frame, provisioning, OTA, flash, erase, asset, voice-gain, BLE, or network surface.

## Key Capture

The key command owns the single `USBSession.events` consumer, passes canonical state events through `GestureRouter`, and invokes `KeyActionRouter` with four distinct inert in-memory screen-mode markers. Its adapters reject permission, input injection, application, voice, shell, shortcut, paste, and pet operations.

Output is one sorted JSON object containing:

- raw canonical down/up/click events with bounded metadata;
- per-key gesture counts;
- per-key inert marker counts.

It deliberately does not infer physical order. Stable physical order is documented only after the operator presses the four front keys from left to right in one capture.

## Recording

The recording command owns one event consumer and one actor-confined `AudioRecordingSession` graph. After the verified USB handshake it sends the verified `ui_state=listening` command, then waits for the user to operate the device's currently configured voice key. It does not change interaction mode, voice key, or gain. Completion requires a first frame, ordered sequence, EOS, atomic Ogg commit, and mode `0600` from the production sink.

After commit, the command:

1. checks `OpusHead` original input rate;
2. runs `/opt/homebrew/bin/ffprobe` for codec, decoder rate, channels, and duration;
3. runs `/opt/homebrew/bin/ffmpeg` with `astats` for RMS and peak;
4. reports only summary values as sorted JSON.

No decoded PCM file is created. On timeout, disconnect, cancellation, or validation failure, the temporary/output recording is removed and `ui_state=ready` is attempted before disconnect.

## Offline Verification

- Connected-diagnostic boundary tests: 6 tests passed.
- Full package: 115 tests in 15 suites passed.
- Swift 6 complete strict-concurrency build passed.
- Safety-flag and invalid-output CLI smoke tests returned exit status 2.

These results do not substitute for real physical-key or speech acceptance.