---
id: firmware-led-001
scope: calibrated VibeBoard LED feedback owner and typed USB service
status: in-progress
depends-on: [firmware-hardware-001, firmware-usb-001]
---

## Objective

Implement the offline-testable `vk_led` single-owner service, typed USB capability/query/config/state ABI, bounded animation policy, calibrated budgets, and fail-dark behavior without guessing physical mapping or safe current limits.

## Context

- `docs/product/led.md`
- `docs/product/hardware.md`
- `docs/product/usb-protocol.md`
- `docs/firmware/README.md`

## Path

- `firmware/components/vk_board/`
- `firmware/components/vk_led/`
- `firmware/components/vk_usb/`
- `firmware/main/`
- `firmware/tests/`

## Contract

- Use only the evidence-backed GPIO8, 17-pixel SK6812/GRB chain and pinned `led_strip` RMT encoder. Treat 10 MHz as RMT tick resolution, not wire bit rate.
- `vk_led` is the only state/animation/frame owner. Other components submit typed semantic intents through a fixed eight-item ordinary mailbox. Hardware failure is a persistent latch; stopping and epoch-off have distinct allocation-free lifecycle cells. Keep retained `cleanup_proof` separate from the single live `ack_obligation`; stopping invalidates the old epoch-off sink and atomically retargets only its proof to the fresh token.
- Extend `vk_board` only with fixed 17-pixel logical-RGB complete-frame/all-off transport. Never expose `led_strip_handle_t`, raw GRB, partial frames, pixel commands, arbitrary animation, or a raw sender.
- Before separately authorized calibration, keep every pixel off, advertise exact `led.available:false` reason `calibration_required`, permit query only, and reject every non-query command as unavailable.
- Do not fill logical key-pixel mapping, `max_brightness`, or `max_frame_channel_sum` with guessed values. A future reviewed calibration profile is required for `available:true`.
- Implement exact current-epoch request-correlated `vk_led_query`, `vk_led_config`, `vk_led_state`, and typed error schemas from `docs/product/led.md`. Config is RAM-only; an applied response echoes the nonzero request ID only after the complete frame. Query responses cannot satisfy config, and same-ID retry is byte-equivalent and idempotent.
- Enforce fixed priority, per-source coalescing, 30 ms replacement-policy tick, maximum 32 channel units per tick, checked brightness/frame-sum arithmetic, and rejection rather than clamping.
- Any set/clear/refresh uncertainty fails dark and latches tainted ownership. Stop closes admission, cancels ticks, clears, refreshes all-off, then releases only proven-clean transport.
- Register LED as an asynchronous USB lifecycle participant before service start. Admission is allocation-free even with a full ordinary mailbox; all-off proof uses the common token/generation and remaining 3,250 ms absolute deadline. LED taint prevents a proposed epoch and never reopens the old epoch.
- Add LED capability independently from assets/screen within the atomic complete current-epoch snapshot. `available:true` requires an exact compiled-in reviewed profile; mapping evidence alone is insufficient.
- Keep calibration outside production. `firmware-led-ram-harness-001` owns the independently reviewed pure-RAM artifact; `firmware-led-calibration-001` owns only separately authorized ROM-entry/load/recovery execution. Production exposes no raw calibration operation.
- Use ESP32-S3 built-in USB Serial/JTAG only. Do not add BLE, Wi-Fi, network, TinyUSB, USB OTG CDC, or USB Audio Class fallback.

## Verification

- Native/TSan tests cover arbitration, mailbox/coalescing, every pairwise/three-way safety-state collision, lifecycle token supersession/deadline/late acknowledgement, timing, stop races, request-correlated exact schemas, epoch reset, checked budgets, and failure injection.
- Board adapter tests cover exactly 17 ordered logical-RGB calls then one refresh, all partial failures, fail-dark, and retained cleanup ownership.
- A clean ESP-IDF 5.5.2 production build contains required `led_strip` symbols and continues rejecting BLE/NimBLE, Wi-Fi/network, TinyUSB/USB OTG CDC, and UAC entry points.
- Offline pass keeps LED unavailable/all-off. It does not claim physical mapping, current safety, connected behavior, or authorization to illuminate hardware.

## Implementation status

- Added the offline `vk_led` single-owner typed intent/profile/state core, fail-dark lifecycle, bounded native fake transport tests, and exact typed LED USB capability/command/state/error codecs.
- Production composition remains deliberately unregistered and `vk_led_init_fail_dark` performs no GPIO, RMT, `led_strip`, or device operation. No compiled-in reviewed calibration profile exists.
- Native and contract suites pass offline. The owner now retains asynchronous cleanup proof separately from the retargetable acknowledgement obligation, enforces the USB-owned strict deadline, and ignores superseded acknowledgement sinks.
- Added a reviewed-profile-only complete-frame board adapter: exactly 17 logical RGB writes followed by one refresh; any partial write/refresh failure attempts all-off and latches taint. No raw strip handle or partial-frame API is exposed.
- The screen capability provider now publishes the exact current-epoch `led.available:false,reason:"calibration_required"` block while no profile is admitted.
- Focused LED UBSan, ASan+UBSan (`detect_leaks=0`), and TSan pass. A fresh combined ESP-IDF build was attempted but is currently blocked by concurrent `vk_screen` `-Werror=misleading-indentation` failures outside this task; no new image is claimed here.
- **Blocked for availability:** reviewed mapping artifact, sustained brightness/current evidence, complete canonical profile hash/identity allowlist, and separately authorized physical acceptance do not exist. Production must advertise `available:false,reason:"calibration_required"`.
- A new independent contract review is required before implementation begins. Offline implementation keeps LED unavailable/all-off. Non-production calibration belongs to `firmware-led-calibration-001`; connected acceptance belongs to `replacement-led-e2e-001` and remains separately authorized.