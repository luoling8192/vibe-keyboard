# Voice Input Integration: BlackHole Virtual Audio

Status: Design — not yet implemented. This document defines the architecture
for routing the device's Opus audio stream into any third-party dictation app
(Typeless, Vokie, Superwhisper, VoiceInk, etc.) via the BlackHole virtual
audio device.

## Problem

The device captures microphone audio, encodes it as Opus, and streams it to
the Mac over USB. The Mac currently only muxes these packets into an Ogg file
on disk. Third-party dictation apps cannot consume this Ogg file as a
microphone source — they expect a live CoreAudio input device. We need to
bridge the device's Opus stream into the CoreAudio HAL so any third-party app
can capture it.

## Architecture

```text
┌──────────┐  Opus   ┌─────────────────┐  PCM   ┌───────────┐  capture  ┌──────────────────┐
│ Device   │  USB    │ vibe-keyboard   │  write  │ BlackHole │ ────────▶ │ Typeless / Vokie │
│ mic      │ ──────▶ │ Opus decode     │ ──────▶ │ (virtual  │           │ / Superwhisper   │
│          │         │ + CoreAudio IO  │         │  device)  │           │ → transcribe     │
└──────────┘         └─────────────────┘         └───────────┘           │ → paste to input │
                                                           ▲             └──────────────────┘
                                                           │
                     ┌─────────────────┐  hotkey  ┌───────────────┐
                     │ vibe-keyboard   │ ─CGEvent▶│ Dictation app │
                     │ voice key route │           │ (global hotkey)│
                     └─────────────────┘           └───────────────┘
```

Two independent paths:

1. **Audio path** — device Opus → decode → BlackHole virtual device →
   third-party app captures from BlackHole as its microphone input.
2. **Trigger path** — voice key press → `toggleVoiceInput()` → CGEvent posts
   the third-party app's configured global hotkey.

### Why not just use the Mac's built-in microphone?

That is the simpler fallback (documented below as Mode A). The BlackHole
path exists specifically to keep the **device's hardware microphone** in the
loop — it is physically closer to the user and already wired through the
keyboard. The trade-off is implementation complexity (Opus decoder +
CoreAudio HAL integration + user-installed BlackHole driver).

## Components

### 1. Opus Decoder

The codebase currently has `OggOpusMuxer` (Opus *encoding* into Ogg
containers) but **no Opus decoding**. `OpusHeadInspector` only reads
metadata, it does not decode PCM.

**Requirement:** real-time Opus → 48 kHz / 16-bit / mono PCM decoding.

**Approach:** add a SwiftPM dependency on libopus. Candidate packages:

- `swift-opus` / `Opus-Swift` bindings
- or vendor the C library via a system-target wrapper

The decoder must:

- accept raw Opus packets (the `AudioFrame.payload` bytes, ≤220 bytes each)
- output interleaved 16-bit PCM at 48 kHz mono
- handle the device's frame flags: `0x01` = first, `0x02` = last

Proposed module:

```text
VibeBoardKit/Audio/
  OpusStreamDecoder.swift    ← wraps libopus, packet-in / PCM-out
```

### 2. CoreAudio HAL Writer

Writes decoded PCM into the BlackHole virtual device so other apps can
capture it.

**Requirement:** push PCM into a CoreAudio output device by device ID.

**Approach:**

- Enumerate CoreAudio devices via `AudioObjectGetPropertyData` /
  `kAudioHardwarePropertyDevices`.
- Locate BlackHole by device name match (`"BlackHole 2ch"`, `"BlackHole 16ch"`,
  `"BlackHole 64ch"`).
- Register an IO proc (`AudioDeviceCreateIOProcID`) that the HAL calls to pull
  samples.
- Buffer decoded PCM in a lock-free ring buffer; the IO proc reads from it.
- Start/stop the device IO (`AudioDeviceStart` / `AudioDeviceStop`) when a
  recording session begins/ends.

Stream format:

```text
sampleRate:     48000.0
channels:       1
bitDepth:       16-bit signed integer
frame flags:    0x01 first → start IO + begin decoding
                0x02 last  → drain + stop IO
```

Proposed module:

```text
VibeBoardKit/Audio/
  BlackHoleAudioWriter.swift   ← CoreAudio HAL device IO, ring buffer
```

### 3. Audio Pipeline in AppModel

Currently `consumeAudio(_ frame: AudioFrame)` feeds `AudioRecordingSession`
which only writes Ogg. The pipeline needs a branch:

```text
AudioFrame received
  ├─ saveRecordings? → AudioRecordingSession → Ogg file (existing)
  └─ BlackHole mode? → OpusStreamDecoder → PCM → BlackHoleAudioWriter
```

`AudioRecordingSession` stays for file saving. A new `LiveAudioPipeline`
owns the decoder + writer pair for the duration of a device audio session
(from first-flag frame to last-flag frame).

Lifecycle:

```text
frame.flags & 0x01 (first)  → create OpusStreamDecoder + start BlackHole IO
frame.payload               → decode → push PCM to ring buffer
frame.flags & 0x02 (last)   → drain remaining PCM → stop BlackHole IO → teardown
error / cancel              → stop IO + teardown decoder/writer
```

### 4. Trigger: Pluggable Voice Input Provider

The `VoiceInputControlling` protocol (`toggleVoiceInput()`) is the hook.
`ProductionHostActionAdapter.toggleVoiceInput()` is currently a no-op comment.
It needs to post a configurable global hotkey via CGEvent.

**Requirement:** when voice key fires, simulate the third-party app's
configured hotkey so it starts/stops capturing from BlackHole.

```swift
// ProductionHostActionAdapter
private var voiceHotkey: KeyboardShortcut?   // e.g. Cmd+D for Typeless

func configureVoiceHotkey(_ shortcut: KeyboardShortcut?) {
    voiceHotkey = shortcut
}

func toggleVoiceInput() async throws {
    guard let shortcut = voiceHotkey else { return }
    try sendShortcut(shortcut)   // existing CGEvent post path
}
```

The hotkey is whatever the user configured in their dictation app's
settings. The app UI exposes a single configurable shortcut. No fn key —
fn (keycode 63) cannot be reliably registered as a global hotkey by
third-party apps; CGEvent-posted fn is not honored by Carbon HotKey / NSEvent
monitors. Use a normal modifier+key combo.

### 5. Device Voice Key

The device still needs to capture audio, so `voiceKey` must remain non-`.none`
in BlackHole mode. The `deviceVoiceKey(for:)` logic in `AppModel` already
derives the voice key from the key mapping profile — no change needed. The
device captures and streams Opus; the Mac decodes and forwards to BlackHole.

## Configuration Model

```swift
enum VoiceInputMode: String, CaseIterable {
    case deviceCapture   // current behavior: Opus → Ogg file only
    case blackhole       // Opus → decode → BlackHole → third-party app
}

struct VoiceInputSettings: Codable {
    var mode: VoiceInputMode
    var blackholeDeviceName: String   // default "BlackHole 2ch"
    var triggerHotkey: KeyboardShortcut?   // e.g. Cmd+D
}
```

Persisted in `UserDefaults` under `voiceInput.settings`.

## User Setup

BlackHole mode requires one-time setup:

1. Install [BlackHole](https://github.com/ExistentialAudio/BlackHole)
   (2ch is sufficient for mono voice).
2. In the dictation app (Typeless/Vokie/etc.), set the input/microphone
   device to **BlackHole 2ch**.
3. In the dictation app, note the global hotkey (e.g. `Cmd+D`).
4. In Vibe Keyboard → Audio:
   - Set Voice Input Mode to **BlackHole**.
   - Set Trigger Hotkey to match the dictation app's hotkey.
   - Set BlackHole Device Name (auto-detected if installed).
5. Select a voice key on the device. Press it — the device captures, the Mac
   decodes and pushes PCM into BlackHole, the hotkey triggers the dictation
   app, which captures from BlackHole, transcribes, and pastes.

### Aggregate Device (optional, if app refuses BlackHole as mic)

Some apps only list physical microphones. Workaround:

- Open `Audio MIDI Setup`.
- Create an Aggregate Device combining the Mac's built-in mic + BlackHole.
- Select the Aggregate Device as the dictation app's input.
- BlackHole audio passes through; the built-in mic adds ambient noise but the
  BlackHole signal dominates.

This is a user-side workaround, not a code concern.

## Error Handling

| Condition | Behavior |
|---|---|
| BlackHole not installed / not found | Fall back to Ogg-file-only; surface `BlackHole device not found` in UI |
| Opus decoder init failure | Teardown pipeline; surface `Opus decoder unavailable` |
| BlackHole IO start failure | Teardown; surface `Audio device start failed` |
| Ring buffer underrun (decode slower than playback) | Output silence for underrun frames; log; do not crash |
| Device audio error event | Existing handler cancels recorder; must also teardown BlackHole pipeline |

## File Layout (proposed)

```text
mac/Sources/VibeBoardKit/Audio/
  OggOpusMuxer.swift            ← existing
  AudioRecordingSession.swift   ← existing
  OpusStreamDecoder.swift       ← NEW: libopus wrapper
  BlackHoleAudioWriter.swift    ← NEW: CoreAudio HAL IO
  LiveAudioPipeline.swift       ← NEW: ties decoder + writer to session lifecycle

mac/Sources/VibeKeyboardApp/
  AppModel.swift                ← consumeAudio() branch + settings
  AppActionPipeline.swift       ← toggleVoiceInput() posts hotkey
  AppViews.swift                ← Audio page: mode picker, hotkey config
```

## Open Questions

1. **libopus packaging** — system `libopus.dylib` is not guaranteed on
   stock macOS. Best to vendor via SwiftPM to avoid a system dependency.
   Need to pick a maintained Swift binding or write a minimal C bridge.
2. **Latency budget** — Opus decode + ring buffer + HAL IO adds latency.
   Need to measure end-to-end from device capture to BlackHole output.
   Target: < 100 ms to feel real-time to the dictation app.
3. **Sample rate mismatch** — device Opus original input rate is captured in
   `OpusHeadInspector`. libopus always outputs 48 kHz. If the device encoded
   at a different rate, libopus handles resampling internally — verify.
4. **Multi-app capture** — only one app should capture from BlackHole at a
   time. If the user runs two dictation apps, both will receive the stream.
   Document this as a limitation.
5. **BlackHole channel format** — BlackHole 2ch expects stereo interleaved.
   Mono PCM must be duplicated to both channels before writing. Verify HAL
   stream format negotiation handles this, or pre-duplicate in the writer.

## Fallback: Mode A (Mac Microphone Only)

If BlackHole is not installed or the user wants simplicity:

- Voice key press triggers the hotkey only (no audio path).
- `voiceKey` sent to device as `.none` — device does not capture.
- The dictation app uses the Mac's built-in microphone.
- Simplest path, but device hardware microphone is idle.

This mode requires only the hotkey trigger change in
`ProductionHostActionAdapter.toggleVoiceInput()` — no Opus decoder, no
CoreAudio writer.
