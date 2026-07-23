# Vendor App Model Contracts

Source: independent read-only arm64/x86_64 static analysis of `/Applications/VibeBoard.app` v0.5.4.

Binary SHA-256:

```text
75a5d92b4db5d479623c66bdb0592dfe27b14084c1a82bcadf162050b3d0f701
```

## USB Framing

The stable envelope, parser, serial session, frame allowlist, and error behavior are canonical in [USB Protocol Contract](../../product/usb-protocol.md).

Both architecture slices confirm:

```text
ordinary frame = 0x01 | type | UInt16LE(body.count) | body
audio frame    = 16-byte fixed header | Opus payload
maximum total frame length = 4096
valid inbound types = { 0x01, 0x10, 0x20, 0x21, 0x22, 0x23, 0x30 }
checksum = none
```

`parseFrames` discards exactly one byte for an invalid version, invalid type, or oversized frame and retains incomplete data.

## State Event

```swift
struct StateEvent: Decodable {
    let event: String
    let button: String?
    let sessionID: UInt32?
    let durationMS: UInt32?
    let hardware: String?
    let firmwareVersion: String?
    let buttons: [String]?
    let uiStates: [String]?
    let message: String?
    let deviceID: String?
    let provisioned: Bool?
}
```

Exact JSON keys:

```text
event
button
session_id
duration_ms
hardware
firmware_version
buttons
ui_states
message
device_id
provisioned
```

`event` is required; all remaining properties are optional. `[String]` for `buttons` and `uiStates` has medium confidence from ABI and upper-layer use; the rest of the model has high confidence.

Observed event/state strings include `device_info`, `idle`, `listening`, `processing`, `ready`, `recording`, `finalizing`, and `error`. This is not proven to be a closed set.

## Audio Frame

Swift ABI model:

```swift
struct AudioFrame {
    let session: UInt32
    let sequence: UInt32
    let flags: UInt8
    let payload: Data
}
```

Wire format:

```text
offset  size  meaning
0       1     version = 0x01
1       1     type = 0x01
2       2     fixed marker = 0x0010, UInt16 little-endian
4       4     session, UInt32 little-endian
8       4     sequence, UInt32 little-endian
12      1     flags
13      1     reserved; vendor parser ignores it
14      2     payload length, UInt16 little-endian
16      N     Opus payload
```

The vendor app muxes payloads into Ogg Opus with 16,000 Hz and one channel. Flag semantics remain open.

## Dispatch

| Type | Vendor behavior |
|---:|---|
| `0x01` | `parseAudioFrame` → audio callback |
| `0x10` | `parseStateEvent` → state callback |
| `0x30` | `parseFirmwareOTAStateEvent` → OTA handler |
| `0x20...0x23` | Framing accepts them; dispatcher returns without a public callback |

Receiving `StateEvent.event == "device_info"` marks device information received and cancels the retry timer.

## Outbound JSON

`UsbCentral.sendFrame(type:body:)` builds the ordinary four-byte envelope. Ordinary app commands serialize JSON objects and send them with frame type `0x10`.

Verified event names:

```json
{"event":"ping"}
{"event":"get_device_info"}
```

The vendor builder writes only the low 16 bits of `body.count` while appending the full body. This is a vendor bug, not a contract to reproduce; the new client must reject unrepresentable body lengths.

## Button Model

Canonical keys are `k1`, `k2`, `k3`, `k4`.

Normalization:

```text
primary   → k4
secondary → k1
k1…k4     → unchanged
other     → absent
```

The physical left-to-right order is not yet proven.

Default bindings:

| Key | Single | Double | Paste text |
|---|---|---|---|
| `k1` | `wake_claude` | `none` | empty |
| `k2` | `paste_text` | `none` | `继续` |
| `k3` | `interrupt_ctrl_c` | `none` | empty |
| `k4` | `voice_input` | `send_enter` | empty |

Supported vendor action raw values:

```text
none
voice_input
send_enter
system_copy
interrupt_ctrl_c
wake_claude
paste_text
custom_shortcut
custom_command
```

Runtime model uses camelCase; persistence helper keys use snake_case:

```text
single
double
custom_shortcut
custom_command
paste_text
paste_text_send_enter
icon_path
```

## USB Device Discovery

The vendor implementation enumerates `IOSerialBSDClient`, walks the parent chain, requires `idVendor == 0x303a`, reads `IOCalloutDevice`, and derives a 12-hex-character device ID from USB serial properties. No product-ID filter was found. The new VibeBoard-specific client intentionally adds `idProduct == 0x1001`.

The serial path is opened with `O_RDWR | O_NONBLOCK | O_NOCTTY`, raw mode, `CLOCAL | CREAD`, and no explicit baud setter.

## Open Items

- Exact meanings of frame types `0x20...0x23`.
- OTA event and outbound schemas.
- Audio flag bits, final-frame behavior, and frame duration.
- Physical left-to-right mapping of `k1...k4`.
- Complete outbound state/configuration commands.
- Existing vendor screen/UI-state payload and capability.
