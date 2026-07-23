# Delivery Plan

## Workflow

Design contracts live under `docs/product/`, `mac/`, `docs/firmware/`, and `docs/ui/`. Investigation results live under `docs/plan/analysis/`. Each implementation task has an independent contract under `docs/plan/tasks/` and must pass separate verify review before completion.

Development changes are tracked from the repository root on the `main` branch.

## Delivery Order

See [Delivery Decomposition](analysis/delivery-decomposition.md) for every creation/call integration. The critical path is:

```text
firmware-hardware-001
  → firmware-usb-001
     ├→ firmware-input-001
     ├→ firmware-led-001 → client-led-001
     │   └→ firmware-led-ram-harness-001 → firmware-led-calibration-001
     │      → replacement-led-e2e-001 (separately authorized)
     ├→ firmware-audio-deps-001 → firmware-audio-001
     ├→ asset-protocol-001 → vka1-core-001
     │   → firmware-asset-store-001 → firmware-asset-transfer-001
     │   → firmware-screen-001 → firmware-display-001 → client-assets-001
     └→ firmware-bootloader-001 → firmware-update-001

firmware-hardware-001 + firmware-usb-001
  → firmware-bootstrap-001 (separate first-write gate)

completed Swift core + all production firmware services + client-assets-001 + client-led-001 + firmware-bootstrap/update
  → client-app-001
  → replacement connected E2E
```

`client-hardware-e2e-001` may continue against vendor firmware because it performs no persistent mutation, but physical key labeling and real speech require human participation.

## Tasks

| Task | Status | Gate |
|---|---|---|
| [client-usb-001](tasks/client-usb-001.md) | done | independent review passed |
| [client-usb-002](tasks/client-usb-002.md) | done | independent review and live vendor USB acceptance passed |
| [client-input-001](tasks/client-input-001.md) | done | offline review passed; physical order remains E2E |
| [client-audio-001](tasks/client-audio-001.md) | done | offline review passed; real speech remains E2E |
| [client-hardware-e2e-001](tasks/client-hardware-e2e-001.md) | in-progress | human-labeled keys and real speech |
| [firmware-hardware-001](tasks/firmware-hardware-001.md) | done | third independent offline review passed; no flash or physical acceptance |
| [firmware-usb-001](tasks/firmware-usb-001.md) | done | sixth independent offline review passed; connected gate remains bootstrap-owned |
| [firmware-input-001](tasks/firmware-input-001.md) | in-progress | 14 deterministic production schedules + 4 USB+input integration schedules; async audio failure discovery, abort no-EOS, release-taint table, deadline composition; all sanitizers pass; production physical acceptance gated |
| [firmware-led-001](tasks/firmware-led-001.md) | in-progress | async generation-guarded obligation/publish protocol, FreeRTOS owner task, fail-dark, independent capability provider, 17-pixel adapter failure matrix, multi-threaded TSan; physical calibration/profile gated |
| [firmware-led-ram-harness-001](tasks/firmware-led-ram-harness-001.md) | blocked | contract passed, but the offline ESP-IDF 5.5.2 pure-RAM build proved the pinned RMT backend has unresolved cache dependencies; no fallback or connected execution is authorized |
| [firmware-led-calibration-001](tasks/firmware-led-calibration-001.md) | blocked | waits for reviewed pure-RAM artifacts and per-step ROM-entry/load/recovery authorization |
| [client-led-001](tasks/client-led-001.md) | pending | request-correlated typed Swift service after firmware LED review |
| [replacement-led-e2e-001](tasks/replacement-led-e2e-001.md) | pending | replacement-only mapping/current/profile gate after bootstrap and all LED dependencies |
| [firmware-audio-deps-001](tasks/firmware-audio-deps-001.md) | done | independent dependency/ABI/license/ELF review passed |
| [firmware-audio-001](tasks/firmware-audio-001.md) | done | eighth independent offline review passed; connected physical audio acceptance remains gated |
| [asset-protocol-001](tasks/asset-protocol-001.md) | in-progress | fourth independent review passed; bounded JSON, typed capabilities, 0x40 authorization, 0x41 fail-closed, Swift↔firmware schema parity |
| [vka1-core-001](tasks/vka1-core-001.md) | in-progress | Swift/C shared codec with 512-case mutation corpus, aggregate memory admission, row-RLE selection; production font gate gated |
| [firmware-asset-store-001](tasks/firmware-asset-store-001.md) | in-progress | 24-cell deterministic failure/recovery matrix, append/checkpoint rollback, canonical parsed GC, corrupt detection; SPIFFS adapter EINTR loop pending |
| [firmware-asset-transfer-001](tasks/firmware-asset-transfer-001.md) | in-progress | active transfer state machine, durable offset, seal, typed begin/end, correlated outcomes; production USB acceptance gated |
| [firmware-screen-001](tasks/firmware-screen-001.md) | in-progress | real VKA1-backed LVGL adapter, z/source ordering, image fit, font binding gate, tick scheduler, overlay content; production font/physical gated |
| [firmware-display-001](tasks/firmware-display-001.md) | in-progress | token-correlated flush FIFO, transport completion hook, LVGL START/FINISH, panel failure propagation path; physical acceptance gated |
| [client-assets-001](tasks/client-assets-001.md) | in-progress | Swift asset transfer, converter, preview, 0x40 send correlation, actor reentrancy, service outcome schedules; connected screen/widget acceptance gated |
| [firmware-bootloader-001](tasks/firmware-bootloader-001.md) | in-progress | rollback bootloader migration tests pass offline; connected migration/activation requires separate authorization |
| [firmware-update-001](tasks/firmware-update-001.md) | in-progress | 12-scenario failure matrix including write/seal/selection invalidation, idle semantics, slot switching; production backend NULL; 0x41 unsupported |
| [firmware-bootstrap-001](tasks/firmware-bootstrap-001.md) | done | fifth independent offline tooling review passed; connected stage/activation remain separately unauthorized |
| [client-app-001](tasks/client-app-001.md) | in-progress | monotonic gesture, asset write-before correlation, screen revision advancement, snapshot invalidation; 202 Swift tests; physical USB/keys/screen acceptance gated |

## Safety Gates

- LED remains all-off and `available:false` until the independently reviewed pure-RAM harness, per-step ROM-entry/`--no-stub load_ram`/recovery authorizations and value-1/250 ms limits, mapping evidence, separately reviewed sustained-current method/evidence, and exact compiled-in production profile all pass. Mapping alone never enables LED.
- Device transport is ESP32-S3 USB Serial/JTAG CDC only. BLE, Wi-Fi, network, TinyUSB, and USB Audio Class are not fallback paths.
- No hardware pin, panel command, protocol field, dependency ABI, audio timing, or flag value is accepted without binary/runtime/header evidence.
- Full flash and sensitive partition backups remain private outside the workspace.
- First replacement write waits for hardware/bootstrap review pass, offline image validation, read-only security/eFuse checks, private backup/hash recheck, exact ota_1 readback, and recovery command review.
- Bootstrap stage leaves vendor bootloader, ota_0, both otadata sectors, NVS/NVS keys, PHY, partition table, and storage unchanged. Activation separately writes only the erased secondary otadata sector. Vendor automatic rollback is not claimed.
- After the bootloader migration gate, replacement update stage/seal writes only the unique inactive managed slot and leaves the current running slot plus otadata unchanged; RAM metadata dies with the epoch. Activation is separate.
- SPIFFS mount/corruption failure never auto-formats. Publication uses immutable revisions and boot recovery, not a false atomic-rename claim.
- Provisioning secrets, NVS/NVS keys, raw PCM/audio, speech recordings, and backup contents never enter source control or default logs.
- No task is done until independent review checks design, code, tests, artifacts, and specified real integrations.

## End-to-End Acceptance

1. USB attach discovers exactly VID `0x303a`/PID `0x1001`, handshakes, reports current-epoch capabilities, maintains lease, and reconnects without Bluetooth.
2. Static image upload validates, commits, renders, and persists after reboot.
3. Pet upload maps semantic states and animates device-side with bounded memory.
4. Dashboard renders time plus one live Mac metric without framebuffer streaming.
5. Custom layout preview and device render match shared geometry/color goldens and documented tolerance.
6. Each physical key triggers one distinct configured host action exactly once.
7. A key-controlled recording produces valid decodable 16 kHz-input mono Ogg Opus containing real speech.
8. Interrupted/invalid/no-space asset transfer preserves previous committed/rendered revision.
9. First bootstrap stages only ota_1 and separately activates through secondary otadata with ROM recovery available; after the bootloader migration gate, each later replacement update preserves the current running slot and otadata before explicit activation, targets only the unique inactive managed slot, and advertises its reviewed pending-verify rollback policy.
10. Full recovery backup hashes remain verifiable and no secret/raw recording appears in logs/workspace.
