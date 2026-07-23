# USB Runtime Evidence

## Scope

Non-destructive tests against the connected VibeBoard at observed path `/dev/cu.usbmodemXXXX`. The path is dynamic; identity is VID `0x303a`, PID `0x1001`, serial `02:00:00:00:00:01`.

No test performed provisioning, OTA, flash erase/write, persistent configuration, or asset mutation. File descriptors were closed after each run.

## Serial Configuration

Tests reproduced the vendor app:

```text
O_RDWR | O_NONBLOCK | O_NOCTTY
cfmakeraw
CLOCAL | CREAD
no baud setter
```

The inherited termios speed displayed 9600, but USB CDC may ignore it. This is not a protocol baud requirement.

## Incorrect-Length Baseline

An early test sent ordinary JSON with total-frame length at offset 2 instead of the statically proven body length. It received only boot/application text and no framed response. Those bytes cannot validate protocol behavior.

## Mixed Total-Length Sequence

A later run sent a total-length `transport` frame immediately followed by more frames. Firmware logged `enable USB mirror`, then disabled it about five seconds later. This does not prove total-length semantics: if firmware waited for four extra bytes under body-length rules, the next frame's first four bytes could complete the read, while cJSON could still stop at the prior `}`.

No binary state/audio/OTA frame was received.

## Isolated A/B Attempt

Four isolated trials sent only one `transport` frame then no bytes for eight seconds:

```text
A1 body length 0x0022
B1 total length 0x0026
B2 total length 0x0026
A2 body length 0x0022
```

None produced an observable mirror-enable result. The firmware parser/session could not be proven reset between trials because the safety constraints prohibited device reset in that test. The result is inconclusive and does not override the arm64/x86_64 app and firmware parser disassembly proving body-length framing.

## Firmware-Side Static Cross-Check

The target firmware USB mirror RX at `0x4200dbf0` reads exactly four header bytes, decodes offset 2 as `body_len`, rejects `body_len > 4091`, then reads exactly `body_len` more bytes. Firmware JSON TX at `0x4200dde4` writes `strlen(json)` at offset 2 and sends `4 + strlen(json)` bytes.

Correct handshake frames therefore use body lengths:

```text
transport/usb:   body 34 = 0x0022
get_device_info: body 27 = 0x001b
ui_state ready:  body 46 = 0x002e
ping:            body 16 = 0x0010
```

## Clean-Boot Successful Handshake

A later clean-reset test used only a non-mutating `esptool --no-stub flash-id` operation to return the device to a known application boot. It then opened the CDC path with vendor termios behavior and sent body-length frames in order:

```text
transport/usb
→ wait for mirror switch
→ get_device_info
→ ui_state ready
→ ping every 2 seconds
```

The firmware enabled USB mirror, returned a valid type-`0x10` `device_info`, remained connected through seven heartbeats, and restored log mode about five seconds after the host closed its file descriptor. No Bluetooth path was used.

Captured frame:

```text
header: 01 10 1f 01
body length: 287
total length: 291
```

Decoded payload:

```json
{
  "event": "device_info",
  "hardware": "vibe_keyboard",
  "firmware_version": "0.3.8",
  "device_id": "VS-020000000001",
  "provisioned": true,
  "buttons": ["k1", "k2", "k3", "k4"],
  "interaction_modes": ["hold_to_talk", "click_to_talk"],
  "ui_states": ["ready", "recording", "thinking", "pending_confirmation", "error"]
}
```

The firmware-reported ID has prefix `VS-`; the USB-registry normalized ID remains `020000000001`. The client retains both instead of silently rewriting one into the other.

This proves vendor USB discovery/handshake/heartbeat readiness. The replacement firmware still removes the mirror/bootstrap/log-sharing split so protocol startup is deterministic.

## Evidence Artifacts

Temporary raw artifacts and their hashes:

```text
/tmp/vibeboard-usb-capture/
  chunks.jsonl              a5226b7424fb45de65cd716e9114edb3a51db6d3059372bdc0a1aed3b42f9489
  rx.raw.bin                af2cbe479eacfed2c7ab112091a355bb54698506072cf188f99b619e2234e7b0
  report.txt                6e8e9c7fa8cc93cf381aadfee64e84275d8b418ae12cca0f3ad9b7ed541eb735
  parser-validation.json    ba748b89915b97564d4b0081520142ca50c320c5ca2004de21dbae100f290bcd

/tmp/vibeboard-usb-handshake-total/
  events.jsonl              1742effec91e760efa0465659924a27b06d0c7a8d1ce64d72cce4db853b9b6a3
  tx.raw.bin                e38164f6904162e80f05dc282e0b44b4364b5a85bee2c5756f46a1c214f93d3b
  rx.raw.bin                a6b8539e12ac6faf132d8a9593c919e12b27aeba0c29dbfdf0603814e25f3cc0
  session.json              8ce7aa0133f0fda26927d3fa89ca11dad60c878eff80ee13c7de16b4ff561743

/tmp/vibeboard-usb-clean-handshake/
  events.jsonl              216843523f23321cfb5f5d70984e56d0aa5fada9aa084544ce7056e5dfcc71b5
  rx.raw.bin                58c7c00fa4d38db9bf21e6a76af73654d543ba57035fbfcd48817f43c30a4b49
  tx.raw.bin                4826b0c3792608fc95516f64f6d1c8e8c4802f6b5103b8f10db408c536ead173
  report.json               f570a6cb87c1e26eaa2dfe2b114be94455f326b89f914c8c8d0a4d5da6ba32a5
```

These `/tmp` artifacts are investigation evidence, not production fixtures or persistent storage. Stable protocol bytes are encoded as source tests and documented in the canonical contracts.
