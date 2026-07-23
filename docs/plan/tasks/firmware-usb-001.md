---
id: firmware-usb-001
scope: replacement firmware USB framing, epoch, and capability core
status: done
depends-on: [firmware-hardware-001, client-usb-001]
---

## Objective

Implement the bounded ESP32-S3 USB Serial/JTAG framing/session service and make the existing Swift session interoperate with replacement `device_info` and `vk_capabilities` without adding asset storage, input scanning, audio capture, or OTA state machines.

## Context

- `docs/INDEX.md`
- `docs/product/usb-protocol.md`
- `docs/product/screen-assets.md`
- `docs/product/hardware.md`
- `docs/firmware/README.md`

## Path

- `firmware/components/vk_usb/`
- `firmware/main/`
- `firmware/tests/`
- `mac/Sources/VibeBoardKit/Protocol/`
- `mac/Sources/VibeBoardKit/USB/`
- `mac/Tests/VibeBoardKitTests/`

## Contract

- Use only the built-in USB Serial/JTAG driver. Do not link TinyUSB, USB Audio Class, Bluetooth, Wi-Fi, HTTP, sockets, or another transport.
- Implement fixed/bounded incremental parsing, direction-specific type allowlists, byte-at-a-time resynchronization, checked lengths, bounded JSON depth/strings/lists, exact ASCII protocol keys, and valid UTF-8/JSON Unicode values under a single serialized TX owner. Reject `U+0000` in every protocol string, including `\u0000`; no decoded string may contain an embedded C-string terminator.
- Support vendor-compatible typed JSON handshake commands and replacement `device_info` with `replacement_protocol:1`; reject provisioning, `voice_gain`, shell, raw memory/register/panel/LVGL, and unknown commands. Swift uses the explicit replacement discriminator, then requires the consecutive current-epoch capability before ready; vendor compatibility is only the proven missing-discriminator path.
- `transport/usb` starts an epoch and drops all bytes remaining in that read/append boundary. Only an exact valid `transport` or `ping` sets/refreshes the five-second lease; expiry clears temporary state and preserves committed state.
- Send `device_info` then `vk_capabilities` for every successful `get_device_info`. Capabilities are idempotent and advertise only implemented, nonzero limits.
- Interpret replacement `provisioned: true` only as compatibility, never backup/update authorization.
- Disable primary/secondary consoles, boot/application logs, VFS console, and panic text from production CDC. No raw-frame public API exists on either side.
- Provide minimal typed registration/send interfaces for canonical input events, bounded AudioFrame values, exact asset commands/chunks, and exact update commands/chunks. Downstream handlers never receive raw JSON/frame bytes and no arbitrary event/type/body send exists; an unavailable handler returns typed unsupported and is not advertised. The complete update decoder/handler path is compiled but requires an immutable boot-policy gate plus a currently available typed provider; current pre-migration production fixes that gate disabled.
- Put producer admission and installed/epoch/expiry/stop/overflow/in-flight state under one synchronization domain. Control overflow queues one reserved `session/control_queue_overflow` terminal, clears queues, and invalidates the epoch. Audio overflow queues one reserved `audio/audio_queue_overflow` terminal with session ID, clears queued frames for that session, marks it truncated, and never fabricates EOS. A dequeued item stays cancellable until the USB owner atomically rechecks terminal state and commits the transport write; pending terminal wins over uncommitted work. Pending audio terminal rejects every session so it cannot be replaced. A new transport epoch resets queue/terminal state.
- Production stop signalling uses an atomic/notification or equivalent synchronization boundary, never `volatile`. Driver uninstall occurs only after the sole poll/read/write owner is proven quiescent; concurrency tests compile the exact production stop-loop owner or its extracted shared implementation.

## Verification

- Native parser/property/fuzzer tests cover every split boundary, multiple frames, invalid version/type/direction, 4096/4097, hostile declared length, JSON bounds, and monotonic parser progress.
- Injected transport tests cover partial/zero/failed writes, bounded queues, install/start failure rollback, lease expiry, duplicate handshake, shutdown, and new epoch cleanup.
- Cross-language golden frames make firmware `device_info`/capabilities decode through production Swift boundaries.
- Clean ELF/source checks find no console producer, BLE, Wi-Fi, network, TinyUSB, or UAC entry point.
- This task is completed by independent offline implementation review and performs no firmware write or device I/O. Replacement connected acceptance cannot precede first bootstrap; `firmware-bootstrap-001` owns the post-activation handshake, capability negotiation, heartbeat, host-close lease expiry, and reconnect acceptance without interleaved log bytes.
