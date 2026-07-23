# USB Protocol Contract

Status: Vendor core framing and USB handshake verified; replacement USB epoch, capability, asset, and staged-update contracts defined for implementation

## Scope

This product uses USB only. Bluetooth and network transports are not fallback paths and are not represented in the client transport API.

The current device is an ESP32-S3 USB serial/JTAG device. macOS exposes its callout path as `/dev/cu.usbmodem*`; the path is assigned dynamically and must never be hard-coded.

## Device Discovery

The vendor app enumerates `IOSerialBSDClient` services and walks each service's parent chain to read USB registry properties.

| Property | Contract |
|---|---|
| Vendor ID | `0x303a` |
| Target product ID | `0x1001` |
| Callout path | `IOCalloutDevice` |
| Serial source | `USB Serial Number`, then `kUSBSerialNumberString` |
| Device ID | Uppercase serial, retain hexadecimal characters only, take the final 12 characters |

For the observed serial `02:00:00:00:00:01`, the device ID is `020000000001`.

VibeBoard.app 0.5.4 filters by vendor ID and does not contain a proven product-ID filter. This project is intentionally narrower and must require both the target VID and PID so another Espressif serial device is not selected.

## Serial Session

VibeBoard.app opens the callout path with:

```text
O_RDWR | O_NONBLOCK | O_NOCTTY
```

It then applies:

```text
tcgetattr
cfmakeraw
c_cflag |= CLOCAL | CREAD
tcsetattr(TCSANOW)
```

No call to `cfsetispeed` or `cfsetospeed` and no fixed baud-rate constant has been found. The client must not claim or require a specific baud rate. A diagnostic override may be introduced only if live evidence proves it necessary.

The session owns one file descriptor, one read source, one incremental receive buffer, and serialized writes. Detach, EOF, unrecoverable read/write error, or explicit disconnect must cancel the source and close the descriptor exactly once.

## Frame Envelope

The inbound parser and ordinary JSON frames use this four-byte envelope. Audio and outbound OTA commands apply the per-type length rules below:

```text
offset  size  type       meaning
0       1     UInt8      protocol version, exactly 0x01
1       1     UInt8      frame type
2       2     UInt16 LE  body length; audio uses fixed-header length 0x0010
4       N     bytes      body
```

There is no sync word beyond version byte `0x01`, no trailing marker, no CRC, and no checksum in VibeBoard.app 0.5.4.

Verified parser type allowlist:

| Type | Meaning | Dispatch status |
|---:|---|---|
| `0x01` | Opus audio frame | Parsed and delivered |
| `0x10` | JSON state event / ordinary JSON command | Parsed and delivered inbound; used outbound |
| `0x20` | OTA begin (host → device) | Accepted inbound by framing parser, not dispatched |
| `0x21` | OTA data (host → device) | Accepted inbound by framing parser, not dispatched |
| `0x22` | OTA finish/verify (host → device) | Accepted inbound by framing parser, not dispatched |
| `0x23` | OTA cancel (host → device) | Accepted inbound by framing parser, not dispatched |
| `0x30` | Firmware OTA state | Parsed by vendor OTA handler |

The maximum accepted total frame length is 4096 bytes, inclusive.

This table is the verified vendor parser allowlist, not a bidirectional authorization list. Replacement firmware uses direction-specific allowlists after negotiation:

| Direction | Allowed types |
|---|---|
| host → replacement firmware | `0x10` typed JSON command; negotiated `0x40` asset chunk; negotiated `0x41` update chunk only while its state machine is active |
| replacement firmware → host | `0x01` AudioFrame and `0x10` typed JSON event |

Types `0x40` and `0x41` are replacement extensions and are legal only after the corresponding available current-epoch feature block is received. Host audio frames, device asset chunks, unknown types, and a frame used in the wrong direction are rejected. The ordinary JSON encoder remains type `0x10`; asset and update frames have dedicated typed encoders. No public raw-frame sender is authorized.

Binary asset chunks use the ordinary body-length envelope with this exact dedicated layout:

```text
offset  size  type       meaning
0       1     UInt8      envelope version, exactly 0x01
1       1     UInt8      frame type, exactly 0x40
2       2     UInt16 LE  body_length = 8 + N
4       4     UInt32 LE  nonzero transfer_id
8       4     UInt32 LE  exact next_offset
12      N     bytes      VKA1 bytes, N in 1...4084
```

The complete frame length is exactly `4 + body_length = 12 + N` and must be at most 4096, so `N <= 4096 - 12 = 4084`. `body_length` covers bytes at offsets 4 through the end and is never a total-length field. The following byte strings are normative lower-bound goldens, with spaces shown only for readability:

| Case | Exact bytes or structural result |
|---|---|
| `transfer_id=0x01020304`, `next_offset=0x05060708`, `N=1`, payload `aa` | `01 40 09 00 04 03 02 01 08 07 06 05 aa` |
| `N=0` | header declares `01 40 08 00 ...`; structurally complete at 12 bytes but rejected by the typed asset decoder before state mutation |
| `N=4084` | body length bytes are `fc 0f`, total length is exactly 4096, accepted only when the active transfer advertises at least 4084 bytes |
| `N=4085` | body length bytes would be `fd 0f`, total length would be 4097, rejected by the envelope limit |

Encoding either UInt32 in big-endian order changes its numeric value and therefore fails the active transfer tuple/offset check; the decoder never probes alternate endianness. A declared body length smaller than the supplied frame leaves the extra bytes to the incremental parser as the start of a separate frame; they are not appended to the chunk. A declared body length larger than currently buffered bytes remains incomplete and causes no callback or mutation. Once the declared number of bytes arrives, exactly one frame is emitted; arbitrary fragmentation across reads must produce the same frame as one contiguous read. Truncation at stream close rejects the incomplete frame. Trailing bytes are parsed independently and must themselves begin a valid allowed-direction envelope. Type `0x40` is never legal device→host, including a byte-identical otherwise-valid golden.

## Incremental Parser

The parser must reproduce the vendor resynchronization contract while also exposing diagnostics to the application:

```swift
while buffer.count >= 4 {
    guard buffer[0] == 0x01 else {
        emit(.discardedByte(reason: .invalidVersion(buffer[0])))
        buffer.removeFirst()
        continue
    }

    let type = buffer[1]
    guard validTypes.contains(type) else {
        emit(.discardedByte(reason: .unknownType(type)))
        buffer.removeFirst()
        continue
    }

    let totalLength: Int
    if type == 0x01 {
        guard buffer.count >= 16 else { return }
        totalLength = 16 + Int(readUInt16LE(buffer, at: 14))
    } else {
        totalLength = 4 + Int(readUInt16LE(buffer, at: 2))
    }

    guard totalLength <= 4096 else {
        emit(.discardedByte(reason: .frameTooLarge(totalLength)))
        buffer.removeFirst()
        continue
    }

    guard buffer.count >= totalLength else { return }
    emit(.frame(buffer.removeFirst(totalLength)))
}
```

Required behavior:

- Arbitrary read boundaries and multiple frames per read are supported.
- Invalid version, type, or oversized length discards exactly one byte and retries synchronization.
- Incomplete data remains buffered.
- An ordinary frame's offset-2 field is its body length and total length is `4 + bodyLength`.
- Audio frame length is the special rule documented below.
- Buffer growth is bounded. If incomplete or hostile input exceeds the configured receive-buffer limit, the session fails with a typed protocol error rather than growing without limit.
- One `append` call accepts at most `receiveBufferLimit` bytes. This also bounds the returned per-byte resynchronization diagnostics; the USB session must read in fixed chunks no larger than that limit.

## JSON State Event

Wire format:

```text
offset  size  type       meaning
0       1     UInt8      0x01
1       1     UInt8      0x10
2       2     UInt16 LE  UTF-8 JSON byte length
4       N     UTF-8      JSON object
```

Model and exact coding keys:

```swift
struct StateEvent: Decodable, Sendable {
    let event: String
    let button: String?
    let sessionID: UInt32?
    let durationMS: UInt32?
    let hardware: String?
    let firmwareVersion: String?
    let buttons: [String]?
    let uiStates: [String]?
    let interactionModes: [String]?
    let message: String?
    let operation: String?
    let code: String?
    let deviceID: String?
    let provisioned: Bool?
    let replacementProtocol: UInt16?

    enum CodingKeys: String, CodingKey {
        case event
        case button
        case sessionID = "session_id"
        case durationMS = "duration_ms"
        case hardware
        case firmwareVersion = "firmware_version"
        case buttons
        case uiStates = "ui_states"
        case interactionModes = "interaction_modes"
        case message
        case operation
        case code
        case deviceID = "device_id"
        case provisioned
        case replacementProtocol = "replacement_protocol"
    }
}
```

`event` is required. The remaining properties are optional. A frame decoder passes only the declared JSON bytes to `JSONDecoder`; trailing bytes are not part of the JSON payload.

`buttons` and `uiStates` are currently modeled as `[String]?` from vendor ABI and upper-layer evidence. Live JSON must be retained as protocol evidence until their element schema is confirmed.

## Audio Frame

Wire format:

```text
offset  size  type       meaning
0       1     UInt8      version, exactly 0x01
1       1     UInt8      type, exactly 0x01
2       2     UInt16 LE  fixed value 0x0010
4       4     UInt32 LE  session
8       4     UInt32 LE  sequence
12      1     UInt8      flags
13      1     UInt8      reserved; ignored by vendor parser
14      2     UInt16 LE  Opus payload length
16      N     bytes      Opus payload
```

Total length is `16 + payloadLength`; the value at offset 2 is not the payload length. The typed decoder must require a 16-byte header, version/type match, marker `0x0010`, payload bounds, and exact extracted payload range.

```swift
struct AudioFrame: Equatable, Sendable {
    let session: UInt32
    let sequence: UInt32
    let flags: UInt8
    let payload: Data
}
```

The payload is 16 kHz mono Opus. Firmware starts each session at sequence zero, emits 60 ms/960-sample packets, marks the first packet with `flags & 0x01`, and emits the next sequence as an empty EOS frame with `flags & 0x02`. Unknown flag bits remain unsupported. The canonical encoder and Ogg muxing contract, including pre-skip and granule rules, lives in [Input and Audio Contract](input-audio.md).

## Outbound Frames

The verified generic builder is:

```text
0       1     0x01
1       1     frame type
2       2     body byte count, UInt16 little-endian
4       N     body
```

Ordinary application commands serialize a JSON object and call the builder with type `0x10`.

Verified connection sequence and timers:

```text
open serial
  → {"event":"transport","kind":"usb"}
  → {"event":"get_device_info"}
  → {"event":"ui_state","state":"ready","text":""}
  → ping every 2 seconds
  → retry get_device_info every 1.5 seconds until device_info arrives
```

A clean-boot live test completed this sequence using body-length frames. The current `voice_stick` 0.3.8 firmware enabled USB mirror, returned a valid framed `device_info`, stayed active through seven two-second heartbeats, and restored log mode about five seconds after the host closed the descriptor. The captured event reported hardware `vibe_keyboard`, firmware `0.3.8`, firmware device ID `VS-020000000001`, four buttons, two interaction modes, and five UI states. The USB-registry normalized ID remains separately `020000000001`.

## Replacement USB Epoch and Capabilities

Replacement firmware uses the ESP32-S3 built-in USB Serial/JTAG driver only. It does not initialize TinyUSB, USB OTG CDC, USB Audio Class, Bluetooth, Wi-Fi, or a network fallback.

A valid `{"event":"transport","kind":"usb"}` command proposes a new protocol epoch. Before publishing it, the USB owner closes old-epoch producer admission in its synchronization domain and begins every registered asynchronous typed lifecycle participant without holding the USB state lock. The first request uses old-epoch sentinel zero. One exact USB-owned 3,250 ms absolute acknowledgement deadline covers the complete participant set: input/audio may consume at most 1,500 ms for the remainder of an executing ordinary audio call, 1,500 ms for audio abort/join, and 250 ms for callback handoff and cleanup; LED runs concurrently within the same remaining deadline and may not extend it. The owner tracks one matching acknowledgement cell per registered participant. Only matching in-deadline `QUIESCENT` acknowledgements from every participant permit the new epoch to become visible. Any begin failure, timeout, or `TAINTED` result permanently taints/closes that USB composition; a late acknowledgement cannot publish the proposed epoch. A successful transition then clears the incremental parser, heartbeat lease, temporary asset/update state, pending input gestures, active audio session association, and epoch-local LED override. It does not change committed configuration, assets, manifests, NVS, or OTA selection. Duplicate transport commands are idempotent requests but create a fresh epoch only after this exact prior-state quiescence gate.

The host sends `ping` every two seconds. Replacement firmware uses a five-second lease from the last valid `transport` or `ping`. Lease expiry closes temporary transfers and recording state, preserves committed state, and requires a new transport command. This five-second value is replacement policy, not a claim about the vendor timeout. USB SOF/physical attachment alone does not establish a protocol epoch.

Every successful replacement `get_device_info` produces two consecutive type-`0x10` events in the same epoch:

```text
device_info with replacement_protocol: 1
vk_capabilities
```

The captured vendor `device_info` has no `replacement_protocol`; absence is therefore the explicit vendor-compatibility discriminator. A replacement value other than `1` is incompatible. When `replacement_protocol:1` is present, the host must wait for and validate the immediately following current-epoch `vk_capabilities` before entering ready or starting its heartbeat. Missing, malformed, reversed, non-consecutive, stale-across-reconnect, or immutable-identity-mismatched capabilities fail the replacement handshake. The host must not infer vendor/replacement identity from firmware version text, timing, `provisioned`, or whether a capability happened to arrive early.

The stable capability envelope is:

```json
{
  "event": "vk_capabilities",
  "protocol": 1,
  "display": {"width": 428, "height": 142, "format": "rgb565"},
  "features": {}
}
```

`features` is an object whose keys are versioned feature names. An absent key means unsupported by this build. A present block always contains `version` and `available`. `available:false` requires a bounded enum `reason` and means the feature exists but is temporarily unusable; feature-specific limits may be omitted. `available:true` requires every field in that feature contract. The USB-core-only build legally sends an empty `features` object. Current feature keys are `assets`, `screen`, `led`, and `update`; unknown keys are ignored only after their complete JSON value passes parser bounds. `led` is independent from assets/screen and is defined only by `led.md`; before reviewed production-profile admission it is exactly unavailable with reason `calibration_required`. Mapping observation alone is insufficient: `available:true` requires the compiled-in, exact board/firmware/current-evidence-bound profile from `led.md`. The exact known `assets` and `screen` capability keys, including `max_asset_bytes`, catalog limits, font identity, and decoded-memory limits, are defined only by `screen-assets.md`; a known block rejects extras rather than applying the unknown-feature rule.

The exact asset/screen control ABI is likewise owned by `screen-assets.md`. In particular, `vk_screen_query|state|commit|committed` use exact key sets and typed screen errors; `vk_asset_begin.total_bytes` is bounded by the same current-epoch `min(upload_max_bytes,max_asset_bytes)`; catalog snapshot idle expiry is 30,000 ms refreshed only by a successful page; and type `0x40` remains host→firmware only. USB registration exposes typed models for these events, never raw JSON or a raw frame sender.

The response is idempotent and may be repeated for retries. A duplicate capability snapshot replaces the prior snapshot only when protocol and immutable display identity match. Capability decoding validates every known block and then all documented cross-feature invariants before atomically publishing the complete snapshot; JSON object-key order does not affect validation. Snapshots never merge with prior snapshots, and no prior-epoch or replaced block or limit may satisfy a block in the new snapshot. Missing or invalid fields normally disable only their owning feature, not USB core or management features advertised separately. The exact exception is the screen-to-assets dependency: `screen.available:true` requires a valid `assets.available:true` block in the same snapshot because that assets block solely owns the decode-memory profile. If assets is absent, unavailable, malformed, or invalid, assets is unavailable and screen is also unavailable; the host performs no screen preview or commit, and production firmware must never advertise that combination. Assets may remain available while screen is absent or unavailable.

Replacement `device_info.replacement_protocol` is exactly `1`; it is only a handshake discriminator and is not a capability or authorization. Replacement `device_info.provisioned` is fixed to `true` only for compatibility with the vendor model. It means “this replacement firmware accepts the documented USB protocol” and is not evidence that a vendor secret exists, that vendor provisioning succeeded, or that an update is authorized. Security and backup gates must never read either field as trust state.

### Production Byte-Stream Isolation

The application protocol is the only producer on USB Serial/JTAG CDC after startup. Production configuration must disable primary and secondary consoles, application and bootloader log output, VFS console output, and panic text on this CDC stream. Project source must not route `printf`, ROM prints, `ESP_LOG*`, or panic output into protocol bytes. Development diagnostics use JTAG or a separately reviewed build mode. Parser resynchronization tolerates pre-application noise but does not authorize application log interleaving.

One service task owns driver install/uninstall on one core and one serialized TX boundary writes complete typed frames. Control and audio queues are bounded, and producer admission plus installed/epoch/expiry/stop/overflow state use one synchronization domain. Control overflow schedules the reserved terminal event `{"event":"vk_error","operation":"session","code":"control_queue_overflow"}`, clears queued control/audio values, and invalidates the epoch; only the USB owner encodes/writes that terminal. Audio overflow schedules the reserved terminal event `{"event":"vk_error","operation":"audio","code":"audio_queue_overflow","session_id":N}`, clears every queued AudioFrame for that session, marks that recording truncated, and rejects further frames for it; it never fabricates EOS or lets a queued prefix continue draining as a complete recording. Input FIFO or ordinary audio-control mailbox overflow invokes the typed `vk_usb_fail_epoch(expected_epoch, VK_USB_SESSION_ERROR_INPUT_QUEUE_OVERFLOW)`, which schedules `{"event":"vk_error","operation":"input","code":"input_queue_overflow"}`. Unproved audio ownership after prepare/release/cancel/stop invokes `vk_usb_fail_epoch(expected_epoch, VK_USB_SESSION_ERROR_INPUT_TAINTED)`, which schedules `{"event":"vk_error","operation":"input","code":"tainted"}`. These are the only input terminal reasons. Both close that exact epoch; lifecycle callback code never calls either operation.

All three reserved-terminal operations linearize in the same USB synchronization domain. `vk_usb_fail_epoch` first compares the nonzero expected epoch with the current open epoch. A mismatch returns `EPOCH_CLOSED` and mutates nothing. A match atomically closes all producer admission, clears queued button/control/audio values, records the one terminal kind, and returns `OVERFLOW`; repeated calls cannot replace the recorded terminal. The terminal has USB-owned reserved storage and never enters the producer queue. Failure to encode or write it leaves the epoch closed. A dequeued value remains cancellable until the USB owner commits it for transport write while holding the same state synchronization boundary; a pending terminal supersedes every not-yet-committed value. A write already committed at that linearization point may finish before a later overflow, but no producer may insert an overflow between the final terminal check and commit. While an audio terminal is pending, all audio admission is rejected so another session cannot replace it. After the owner emits that terminal, a new first frame may establish a later session only where the audio-specific rule keeps the epoch open; session/input terminals close the whole epoch and require a new successful transport lifecycle.

The typed button façade accepts one of three exact variants and returns exactly `ACCEPTED|RETRY|EPOCH_CLOSED|OVERFLOW`: down is `{expected_epoch,event:button_down,button,session_id?}` and forbids duration; up and click are `{expected_epoch,event:button_up|button_click,button,duration_ms,session_id?}`. `button` is `k1|k2|k3|k4`, `duration_ms` is `UInt32`, and an optional `session_id` is `UInt32` in `1...4294967295`; zero, Boolean, missing required, and extra fields reject. The replacement encoder omits absent session identity and never emits vendor-compatible zero. It copies the complete value before `ACCEPTED`; `RETRY` and `EPOCH_CLOSED` enqueue nothing. Exact wire JSON and immutable retry association are defined in `input-audio.md`. The façade exposes no raw queue, JSON, encoder, service pointer, or send callback.

USB lifecycle registration is asynchronous and complete before service start. Each participant receives exactly `{kind:new_epoch|lease_expired|stopping,token:UInt32,old_epoch:UInt32,proposed_epoch:UInt32,lifecycle_generation:UInt32}` and owns a distinct one-item acknowledgement cell for that token/generation. Token/generation are nonzero. Epoch zero is only the lifecycle “no epoch” sentinel. First new-epoch requires old zero/proposed nonzero; later new-epoch requires distinct nonzero values. Lease expiry requires old nonzero/proposed zero and is an internal no-op with no lifecycle request when no epoch is open. Normal stopping requires old nonzero/proposed zero. Stopping a started composition before its first successful transport is exactly `{kind:stopping,old_epoch:0,proposed_epoch:0}`. `old_epoch == 0` is legal only for first new-epoch and pre-epoch stopping; only pre-epoch stopping may have both epoch fields zero. A zero `proposed_epoch` remains required for normal lease expiry and stopping when `old_epoch` is nonzero. Pre-epoch stopping performs registered local cleanup and bounded acknowledgement but can never publish an epoch. No open protocol value uses epoch zero.

The USB owner alone owns one monotonic absolute 3,250 ms deadline. Under its lock it closes old admission, records the token/deadline, then unlocks and calls nonblocking `begin(request,acknowledgement_sink)`. `begin` returns `ACCEPTED|TAINTED`; accepted means cleanup started, not quiescence. The sink is a dedicated one-item cell accepting only `{token,lifecycle_generation,result:QUIESCENT|TAINTED}` and exposes no USB façade operation. Callback code cannot synchronously send/query/fail or hold a USB lock. Acknowledgement publication and timeout linearize under the same USB-owner state lock. The owner accepts a matching acknowledgement only when its captured monotonic `ack_linearization_time < absolute_deadline`; equality or a later time is timeout, invalidates the cell, and taints before the acknowledgement is considered. Lock acquisition order cannot make an acknowledgement captured exactly at the deadline valid. Duplicate, mismatched, exact-deadline, and late values are discarded. Timeout or taint retains uncertain context, permanently closes the composition, and cannot be reversed by late acknowledgement. Only accepted quiescent acknowledgements from every registered participant publish a proposed epoch or prove clean teardown; pre-epoch stopping has no proposed epoch and only proves teardown. A LED participant begins allocation-free through dedicated off/stopping safety cells even when its ordinary mailbox is full, runs inside this same deadline, and may acknowledge `QUIESCENT` only after proving an all-zero refresh and eliminating old-generation publication. LED uncertainty returns `TAINTED`, blocks the new epoch, retains cleanup ownership, and never reopens the closed old epoch; its collision and retarget rules are defined in `led.md`: a superseded acknowledgement sink is invalidated, while only retained cleanup proof may be retargeted to the fresh stopping token.

Exactly one lifecycle request may be outstanding. Admission and precedence are linearized by the USB owner: while `new_epoch` is pending, another transport command is coalesced into that same request and produces no token, generation, epoch, or deadline; lease expiry is also coalesced because old admission is already closed. While `lease_expired` is pending, transport is rejected as lifecycle-busy and cannot propose an epoch; duplicate expiry coalesces. A `stopping` request has highest priority: if no request is pending it starts normally; if `new_epoch` or `lease_expired` is pending, USB invalidates that request's acknowledgement cell and proposed epoch, retains its already closed old context, allocates a new token/generation and one new stopping deadline, then invokes stopping begin. When input already owns a reserved abort for the same old epoch, stopping begin atomically retargets only that abort's eventual quiescence proof to the fresh stopping acknowledgement sink; it does not allocate or submit a second abort and does not reuse the old token/generation. A result already published to the invalidated old sink is stale and cannot satisfy stopping; if cleanup had already proved quiescence, stopping begin independently verifies the retained quiescent state before publishing a fresh matching acknowledgement. A reserved abort for a different epoch, failed retarget, or uncertain retained state returns tainted. The superseded cleanup may otherwise continue only as tuple-local cleanup, and any old acknowledgement is discarded. While stopping is pending, every transport/expiry/duplicate stopping request is coalesced without a new token or deadline, all producer admission stays closed, and no epoch can be exposed. Failure or timeout of either the superseded cleanup or stopping cleanup makes the composition fail-closed; stopping never restores an earlier request. Token and generation are never reused for a superseding request.

Input/audio owns a separate single reserved lifecycle-abort slot, unavailable to its four ordinary commands. After ordinary admission closes, callback begin clears queued ordinary work and admits `abort(old_epoch,lifecycle_generation,0)` through that slot even when ordinary command/result mailboxes are full. An already executing ordinary call may consume at most its existing 1,500 ms remainder; audio control then starts no further ordinary work, gives abort/join at most 1,500 ms, and reserves 250 ms for cleanup and acknowledgement. Every step is capped by the remaining time to the one USB-owned 3,250 ms absolute deadline; no retry extends it. Completion reports directly to the acknowledgement sink, not through the result mailbox. Slot conflict or immediate admission failure makes `begin` return tainted without waiting or USB re-entry. For first new-epoch and pre-epoch stopping, old epoch zero requires no audio command but still closes local admission, clears unpublished scanner/FIFO/mailbox/association state, and publishes through the matching bounded acknowledgement cell. Pre-epoch stopping then completes teardown and never exposes an epoch. A nonzero old epoch acknowledges quiescent only after bounded abort/join proves no old AudioFrame/result producer remains. Current-call overrun, abort failure, exhausted remaining time, or timeout acknowledges tainted when possible and otherwise reaches the same fail-closed USB deadline result.

The public integration boundary exposes only typed lifecycle registration and typed sends: canonical button event values, bounded AudioFrame values, exact asset command values, and exact update command values. Downstream handlers never receive raw JSON, arbitrary event names, frame types, byte bodies, a service pointer, or a raw send callback. Unregistered handlers are absent from capabilities and their commands return typed `unsupported`. The replacement transport remains ESP32-S3 built-in USB Serial/JTAG only; BLE, Wi-Fi, network, TinyUSB, USB OTG CDC, and USB Audio Class are prohibited fallbacks.

Verified ordinary commands:

```json
{"event":"transport","kind":"usb"}
{"event":"get_device_info"}
{"event":"ui_state","state":"ready","text":""}
{"event":"ping"}
{"event":"interaction_mode","mode":"hold_to_talk"}
{"event":"interaction_mode","mode":"click_to_talk"}
{"event":"voice_key","key":"k1"}
{"event":"voice_gain","gain":0}
```

`ui_state.state` and `text`, `voice_key.key`, and `voice_gain.gain` are dynamic. Only documented enums may be represented as typed commands. The valid voice-gain range is still unknown, so the production client must not expose or send this command yet.

Provisioning also uses type `0x10` JSON with event `provision` and a `device_secret`. That command is intentionally excluded from the new client's general command surface because it handles sensitive vendor material and is not required for the USB-only replacement firmware.

The new encoder rejects bodies that cannot be represented by UInt16 or would exceed the 4096-byte total frame limit instead of reproducing the vendor builder's truncation behavior.

## Application Installation (Not the Update Protocol)

The supported first installation uses the ESP32-S3 ROM download path through
`firmware/tools/auto_flash.py`. The helper validates the application image,
writes only `ota_0 @ 0x20000`, verifies the written bytes, and leaves the
bootloader, partition table, NVS, OTA metadata, and asset storage unchanged.

This path is separate from `vk_update`. Broader bootloader or partition changes
are outside the supported workflow.

## Replacement Staged Update Wire Contract

`vk_update` does not bootstrap the first image. The current production build reports `available:false` with `reason:"bootloader_migration_required"`. The available service below is enabled only after a rollback-enabled replacement bootloader is installed and both slots are managed replacement slots. It uses type-`0x10` typed JSON controls and negotiated binary type `0x41`.

The USB core contains the complete typed update command/chunk decoder but dispatches it only when two independent gates pass: the immutable production boot policy says the reviewed bootloader migration/first-boot gate is complete, and a registered typed update provider successfully recomputes a currently available running/target tuple. The current pre-migration production composition fixes the immutable policy to disabled, rejects registration/dispatch as typed `unsupported`, and never advertises update. No host command can change this policy.

The `features.update` capability block is recomputed for every successful `get_device_info`; a prior epoch or prior snapshot never authorizes a new stage. When available, `target` is the unique inactive managed slot at that instant. For example, a device currently running from `ota_1` reports:

```json
{
  "version": 1,
  "available": true,
  "chunk_bytes": 512,
  "max_image_bytes": 5242880,
  "target": "ota_0",
  "staged_metadata": "ram_epoch",
  "rollback": "bootloader_pending_verify"
}
```

The only valid running/target mappings after migration are:

| Current running slot | Advertised and staged target |
|---|---|
| `ota_0 @ 0x020000` | `ota_1 @ 0x520000` |
| `ota_1 @ 0x520000` | `ota_0 @ 0x020000` |

Any other running partition, an ambiguous/missing partition identity, or a target that is not the other managed slot makes the feature unavailable. `rollback` may be `bootloader_pending_verify` only after a separately reviewed rollback-enabled replacement bootloader is installed and its first-boot confirmation path passes. Otherwise the update feature remains absent or unavailable; an available post-migration implementation must advertise the exact reviewed rollback policy. `available:false` requires `reason` from the complete bounded enum `bootloader_migration_required|busy|wrong_running_slot|target_unavailable|integrity_unavailable|policy_blocked`; free text and substituting `policy_blocked` for the known pre-migration state are forbidden.

All JSON integers below are unsigned and must fit their named width. `transfer_id` is a nonzero host-generated UInt32. `sha256` is exactly 64 lowercase hex characters. Unknown or extra fields are rejected for update commands.

| Event | Direction | Required fields | Result |
|---|---|---|---|
| `vk_update_begin` | host→device | `transfer_id`, `size` UInt32, `sha256` | Create/query RAM stage; reply `vk_update_ready` |
| `vk_update_ready` | device→host | `transfer_id`, `size`, `sha256`, `next_offset`, `chunk_bytes` | Exact active state |
| `vk_update_progress` | device→host | `transfer_id`, `next_offset` | Durable `esp_ota_write` progress |
| `vk_update_seal` | host→device | `transfer_id`, `size`, `sha256` | Validate; reply `vk_update_sealed` or error |
| `vk_update_sealed` | device→host | `transfer_id`, `size`, `sha256`, `project`, `version` | RAM metadata is activatable in this epoch |
| `vk_update_query` | host→device | `transfer_id` | Reply ready/sealed/error from current epoch only |
| `vk_update_cancel` | host→device | `transfer_id` | Abort handle, erase RAM metadata; reply `vk_update_cancelled` |
| `vk_update_cancelled` | device→host | `transfer_id` | No activation remains possible |
| `vk_update_activate` | host→device | `transfer_id`, `sha256` | Reverify and select target; reply activating/error |
| `vk_update_activating` | device→host | `transfer_id`, `reboot_ms` | Selection succeeded; bounded reboot follows |
| `vk_update_error` | device→host | `operation`, `code`, optional `transfer_id`, optional `next_offset` | Typed terminal/nonterminal error |

Error `code` is one of `busy|conflict|not_found|wrong_epoch|wrong_running_slot|wrong_target|bad_size|bad_hash|bad_offset|write_failed|incomplete|image_invalid|readback_mismatch|not_sealed|selection_failed|timeout|internal`. `message`, when present, is diagnostic-only bounded UTF-8 and never controls behavior.

A repeated `vk_update_begin` with the exact same active `transfer_id/size/sha256` is an idempotent query and returns current `vk_update_ready`; any mismatch is `conflict`. A repeated seal of the exact sealed tuple returns the same sealed result. Activate is accepted once; repeats before reboot return the same activating result when available and never re-run partition writes. Idle timeout is 30 seconds before seal. Timeout, cancel, lease expiry, new epoch, USB disconnect, or reboot aborts the handle and destroys all RAM staged metadata. There is no persistent resume and no activation after reconnection; the host must restage from offset zero.

Binary update chunks are:

```text
offset  size  type       meaning
0       1     UInt8      envelope version 0x01
1       1     UInt8      type 0x41
2       2     UInt16 LE  body length = 8 + chunk length
4       4     UInt32 LE  transfer ID
8       4     UInt32 LE  exact offset
12      N     bytes      image bytes, N in 1...512
```

Only host→firmware `0x41` is legal. Transfer ID and offset must match the active RAM stage; offset must equal `next_offset`. Zero-length, duplicate, stale, or out-of-order chunks are rejected and never advance state. Before every chunk write, firmware re-reads the running partition and target identity and requires the same valid running/target mapping captured by begin. A mismatch returns `wrong_running_slot` or `wrong_target`, aborts the OTA handle, invalidates the RAM stage, and never writes the chunk. Firmware writes a complete chunk before emitting progress. No complete image is buffered in RAM.

Begin must require the running partition to be exactly one managed slot, the target to be exactly the other inactive managed slot, their addresses to differ, declared size `1...0x500000`, and no conflicting asset mutation. It captures `{epoch, transfer_id, size, sha256, running slot/address, target slot/address}` in RAM. Seal re-reads both partition identities and requires the mapping to equal that begin tuple; a mismatch aborts and invalidates the stage. It then requires exact bytes/digest, calls `esp_ota_end`, reads back candidate bytes, recomputes SHA-256, and validates app descriptor/chip/revision/size. It stores only the bounded sealed tuple `{epoch, transfer_id, size, sha256, running slot/address, target slot/address, descriptor identity}` in RAM and does not modify otadata or reboot.

Activate re-reads and validates the same epoch and sealed tuple, the current running slot/address, the unique inactive target slot/address, target readback digest/descriptor, and current partition selection before `esp_ota_set_boot_partition`. Any tuple change invalidates the RAM stage and no selection occurs. The macOS application separately revalidates its private backup/active-image evidence and requires explicit confirmation; this host-local gate is not represented as a firmware “backup token”.

A replacement build claims pending-verify rollback only after a separately reviewed rollback-enabled replacement bootloader is installed and the migration's first-boot confirmation path passes. In that build, mandatory first-boot tests are partition identity, USB core startup, watchdog, and heap sanity. Tests for input, audio, display, or storage are required only when their compiled capability blocks are present; failure marks invalid and reboots. Until that complete bootloader gate passes, the update feature remains absent or `available:false` with `reason:"bootloader_migration_required"`; no available `explicit_recovery` state is authorized.

No production staged-update sender is exposed until parser, 512-byte limit, digest/readback checks, RAM-epoch invalidation, selection behavior, and injected interruption tests pass independent review.

## Errors

The Swift boundary uses explicit errors for:

- unsupported protocol version;
- unsupported frame type when decoding a complete typed frame;
- total frame length over 4096;
- malformed audio header or payload length;
- invalid UTF-8 or JSON;
- missing required `event`;
- receive-buffer limit exceeded;
- disconnected write, partial write exhaustion, EOF, and I/O failure.

Byte-by-byte stream resynchronization is an authorized recovery rule, not silent acceptance: each discard is observable through diagnostics.

## Open Protocol Items

- Vendor firmware absolute OTA chunk limit remains unknown; replacement staged update deliberately fixes 512 bytes from the verified host behavior.
- Physical left-to-right order of `k1...k4` requires connected human-labeled capture.
- Valid numeric vendor voice-gain range remains unknown and is not exposed.
- Replacement capability, asset, widget, screen, and staged-update contracts require implementation and connected validation; their wire values are not claims about vendor firmware.

## Validation Gate

Core framing is covered by shared fixtures, strict decoders, and connected-device validation.

The following remain required before user-visible feature completion:

1. Firmware parser evidence or live captures for each additional command used by this project.
2. Real key captures proving physical order and gesture timing, plus a real Opus recording acceptance test for the statically verified audio semantics.
3. A connected-device integration test using the new Swift USB session for discovery, handshake, incremental parsing, heartbeat lifetime, and detach cleanup.
