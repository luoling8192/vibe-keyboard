# macOS Client Scope

## Purpose

Own the native macOS application that discovers the target VibeBoard over USB, applies user configuration, transfers display resources, routes four physical keys, and records device Opus audio.

## Build and Launch

Package the release build as a standard macOS application:

```bash
cd mac
./scripts/package_app.sh
```

Launch it from Finder or:

```bash
open "dist/Vibe Keyboard.app"
```

The distributable archive is `dist/VibeKeyboard-macOS-arm64.zip`. Only one
Vibe Keyboard or diagnostic process may own the USB serial device at a time.

## Boundary

The client does not:

- use Bluetooth or network transport to communicate with the device;
- own final LVGL rendering or animation timing;
- expose device secrets or backup contents;
- guess unsupported firmware commands or hardware capabilities.

## Package Structure

```text
VibeKeyboardApp
  └─ VibeBoardKit
      ├─ Protocol
      │   ├─ incremental frame parser
      │   ├─ state/audio models
      │   └─ outbound command encoder
      ├─ USB
      │   ├─ IOKit discovery
      │   ├─ serial session
      │   └─ connection/handshake state machine
      ├─ Input
      │   ├─ persisted mappings
      │   └─ host action router
      ├─ Audio
      │   ├─ session ordering
      │   └─ Ogg Opus writer
      ├─ Assets
      │   ├─ source decoding/conversion
      │   ├─ manifest/revision cache
      │   └─ upload state machine
      ├─ LED
      │   └─ capability/query/config/state service
      └─ AppModel
          └─ observable integration state
```

The first delivery uses one library target, `VibeBoardKit`, to keep package boundaries small. Directory boundaries remain explicit and may become targets only if build or dependency evidence requires it.

## Ownership and Creation

```text
VibeKeyboardApp
  creates AppModel
    creates USBDeviceMonitor
      yields matching USBDeviceDescriptor
    creates USBSession for one descriptor
      owns FrameStreamParser
      publishes typed StateEvent / AudioFrame
    creates KeyActionRouter
    creates AudioRecorder
    creates AssetTransferController
    creates LEDController only for validated replacement capability
```

`AppModel` is the sole owner of the active session and global device-operation state. Views never open file descriptors or encode protocol bytes.

## State Lifecycle

```text
disconnected
  → deviceFound
  → opening
  → announcingUSBTransport
  → requestingDeviceInfo
  → synchronizingConfiguration
  → ready
  → disconnected | incompatible | failed
```

There is no fallback state. A handshake timeout remains a typed failure with retained diagnostics.

The vendor USB mirror handshake is runtime-verified over USB: after `transport/usb`, the client requests device info, sends `ui_state ready`, then maintains two-second pings. `ready` requires a decoded `device_info` event from the matching USB session; timeouts remain typed failures with retained bounded diagnostics. Closing the descriptor lets vendor firmware return to log mode after approximately five seconds.

For replacement firmware, `transport/usb` starts a new epoch and the two-second ping maintains a five-second lease. Each `get_device_info` response is `device_info` immediately followed by `vk_capabilities`. The client may enter vendor-compatible ready state after `device_info`, but screen/assets/update controls remain disabled until valid replacement capabilities arrive in that same epoch. Reconnect invalidates all prior capabilities, transfer offsets, widget sequences awaiting send, and staged-operation handles.

## Concurrency

- IOKit callbacks and file-descriptor reads enter one dedicated serial executor.
- Parser mutation and writes are serialized by the session.
- USB event delivery is explicitly bounded. Lifecycle/control overflow terminates the session with a typed error; audio delivery uses a bounded policy owned by the recording integration and can never grow without limit or silently claim a complete recording after loss.
- Typed events cross to `@MainActor AppModel`.
- Asset conversion and Ogg writing run off the main actor.
- Disconnect cancels all per-session tasks before the file descriptor is closed.

## Persistence

User configuration is stored in Application Support as versioned Codable data written atomically. Secrets and sensitive firmware backup material are not part of this store.

Device-specific settings use normalized 12-hex-character device IDs. Unknown schema versions fail visibly; migrations are explicit and tested.

## Public Entry Points

Initial library surface:

```swift
public struct USBDeviceDescriptor
public protocol USBDeviceMonitoring
public actor USBSession
public struct FrameStreamParser
public enum ParsedFrame
public struct StateEvent
public struct AudioFrame
public enum ControlCommand
```

Concrete API signatures evolve only through the module contracts and task review.

## Errors

Errors are typed at their owning boundary:

- `ProtocolError` for framing/model violations;
- `USBDiscoveryError` for registry failures and ambiguous devices;
- `USBSessionError` for open/termios/read/write/detach/handshake failures;
- `ConfigurationError` for invalid mappings or migrations;
- `AudioRecordingError` for session/order/mux/file errors;
- `AssetError` for conversion/validation/transfer failures;
- `LEDError` for capability, validation, acknowledgement, and current-epoch failures.

User-visible layers retain the underlying diagnostic context without displaying raw audio, firmware contents, NVS values, secrets, or host environment variables.

## Cross-Module Rules

- USB session emits typed data; downstream modules never parse raw serial bytes.
- Key routing accepts only canonical validated device events.
- Audio recorder accepts only ordered frames from the current session.
- Asset transfer uses a current-epoch optional `features.assets` block. Unsupported/temporarily unavailable/full storage disables upload without disabling advertised list/delete management; vendor firmware never receives extension commands.
- Type `0x40` uses a typed `AssetChunkFrame` encoder and serialized session write. The library exposes no public raw-frame sender.
- Asset identity is the VKA1 full-container SHA-256 with its hash field zeroed during calculation. Host preview and firmware share fit/color/layout golden vectors.
- SPIFFS recovery is device-owned. The client treats immutable content as stored only after `vk_asset_stored` and screen state as active only after a validated revision commit; disconnect or timeout retains the previously known active revision until re-query.
- First replacement bootstrap is a separate host ROM-download operation with independently authorized ota_1 stage and secondary-otadata activation. Later replacement updates use RAM-epoch staged update and separate activate. `AppModel` requires local verified backup/active-image evidence; it never transmits a fake backup token or uses vendor immediate-finish OTA for first write.
- LED uses an independent current-epoch capability block and does not depend on assets/screen. Before reviewed production-profile admission or when unavailable, the client exposes no config control and may only query typed state. Query/config uses nonzero current-epoch request IDs; at most one config is outstanding, query responses never acknowledge config, and state changes only after the matching `source:"applied"` response whose actor-captured monotonic time is strictly earlier than the one non-extending 1,000 ms absolute deadline. Equality/later timestamps time out; queue order cannot change that comparison, and a late applied response may update observed state but cannot complete the expired operation.
- During firmware or asset mutation, conflicting device operations are rejected rather than queued silently.

## Tests

- Pure protocol, configuration, audio, and asset contracts use Swift unit tests.
- USB discovery has injected registry/session boundaries plus a real-device diagnostic executable.
- Every connected-device gate records descriptor, protocol/event evidence, and timestamps without secrets.
- UI behavior follows [macOS Client Layout](../docs/ui/client.md).
