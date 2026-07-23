# Delivery Decomposition

## Objective

Deliver a native Swift macOS client and VibeBoard-specific USB-only firmware that is independently reviewed and verified on the connected hardware without guessing protocol, dependency, filesystem, or safety behavior.

## Module Tasks

### Completed Swift Core

1. `client-usb-001`: vendor-compatible pure Swift frame/model codec.
2. `client-usb-002`: IOKit discovery, serial session, bounded lifecycle, and diagnostic executable.
3. `client-input-001`: canonical mappings, gesture classification, and host action routing.
4. `client-audio-001`: ordered AudioFrame session and Ogg Opus writer.

Physical key order and real speech remain in `client-hardware-e2e-001`; offline completion does not substitute for that connected evidence.

### Firmware and Replacement Protocol

1. `firmware-hardware-001`: exact ESP32-S3/NV3007/key/LED/microphone constants and bounded board initialization.
2. `firmware-usb-001`: USB Serial/JTAG framing, epoch/lease, handshake, capability core, and log isolation.
3. `firmware-led-001`: single-owner calibrated LED feedback, typed USB configuration, lifecycle participation, and fail-dark policy.
4. `firmware-led-ram-harness-001`: offline ESP32-S3 pure-RAM one-stimulus build/map/image validator.
5. `firmware-led-calibration-001`: per-step authorized ROM load/recovery and profile evidence gate.
6. `replacement-led-e2e-001`: separately authorized replacement-only mapping/current/profile and fail-dark acceptance.
7. `firmware-input-001`: four-key scanner/debounce and USB events.
8. `firmware-audio-deps-001`: exact ESP-SR and ESP32-target Opus dependency/ABI gate.
9. `firmware-audio-001`: PDM → AFE → Opus → USB lifecycle.
10. `asset-protocol-001`: typed capabilities/control models and type-`0x40` framing on Swift and firmware.
11. `vka1-core-001`: shared full-container-hash VKA1 codec/validator.
12. `firmware-asset-store-001`: explicit-format SPIFFS, immutable revisions, and boot recovery.
13. `firmware-asset-transfer-001`: real USB-to-store transfer/resume integration.
14. `firmware-screen-001`: validated screen model and LVGL image/pet/dashboard/custom runtime.
15. `firmware-display-001`: integration/acceptance umbrella for reviewed screen components.
16. `firmware-bootloader-001`: separately authorized rollback-enabled bootloader migration.
17. `firmware-update-001`: post-migration RAM-epoch stage/write/seal and separate activate.
18. `firmware-bootstrap-001`: host ROM-download first app stage/readback and separate secondary-otadata activation.

### Client Product

1. `client-assets-001`: deterministic source conversion, VKA1, preview/layout/widget models, and resumable transfer.
2. `client-led-001`: typed capability/query/config/state integration after firmware LED review.
3. `client-app-001`: one production `AppModel` and SwiftUI Device/Screen/Pets/Keys/Audio/Firmware pages.

## Integration Enumeration

| Creation/call chain | Task that breaks the stub boundary | Required real evidence |
|---|---|---|
| IOKit monitor → USB session → Swift codec | `client-usb-002` | live vendor handshake/reconnect |
| Firmware USB driver → parser → typed state owner → Swift session | `firmware-usb-001` offline + `firmware-bootstrap-001` connected gate | cross-language goldens first; post-bootstrap replacement handshake, capability, lease, no log bytes |
| Typed LED intents → firmware owner → calibrated board frame → Swift state | `firmware-led-001` + `client-led-001` | offline all-off/fail-dark and request/lifecycle schedules first |
| Pure-RAM one-stimulus artifact → authorized ROM load/recovery → mapping/current evidence → compiled profile | `firmware-led-ram-harness-001` + `firmware-led-calibration-001` + `replacement-led-e2e-001` | byte-exact ELF `PT_LOAD`→esptool image projection, domain-separated non-circular manifests, per-build one-shot ROM/reset authorization, value-1 auto-off, separate current method, exact profile admission |
| GPIO scanner → firmware USB event → Swift gesture/action router | `firmware-input-001` | four human-labeled keys, exactly-once inert actions |
| I2S PDM → pinned AFE → pinned Opus → firmware USB → Swift Ogg | `firmware-audio-001` | first/sequence/EOS and real-speech decode |
| Swift typed asset frame → firmware USB handler | `asset-protocol-001` | shared `0x40` byte corpus, no raw sender |
| Swift VKA1 writer → firmware VKA1 validator | `vka1-core-001` | cross-language complete binary/hash goldens |
| Firmware USB transfer → SPIFFS temporary writer → immutable commit | `firmware-asset-transfer-001` | interrupt/resume/hash/no-space/reboot |
| Boot commit scan → selected manifest → LVGL root | `firmware-screen-001` | previous revision survives all candidate failures |
| Mac source decoder → canonical pixels → VKA1 → preview | `client-assets-001` | color/fit/disposal/layout shared goldens |
| Host widget provider → typed update → existing LVGL object | `client-assets-001` + `firmware-screen-001` | time + one metric without layout resend |
| Host ROM path → ota_1 exact stage/readback → secondary otadata activation | `firmware-bootstrap-001` | protected regions unchanged; explicit ROM recovery |
| Replacement USB update → RAM stage → seal → activate | `firmware-update-001` | epoch invalidation; current running slot/otadata unchanged pre-activate; unique inactive managed target |
| App views → AppModel → production services | `client-app-001` | no production mock/fake/BLE/network fallback |

## Storage and Update Decisions

- Keep the verified SPIFFS partition. Increase object-name length for full immutable names; do not change partition layout to solve naming.
- SPIFFS rename is not a power-loss atomic replacement. Publish destination-absent immutable files and a commit record last; boot validates newest-to-oldest and retains current plus previous valid revision.
- Mount/corruption failure never formats automatically. First format has an explicit verified-erased gate.
- VKA1 identity hashes the complete container with its hash field zeroed. Frame encoding is selected per frame.
- Vendor OTA finish cannot satisfy first-write safety. First bootstrap is host ROM stage/readback plus separate secondary-otadata activation and retains vendor bootloader; it does not claim automatic rollback.
- Later replacement update uses typed type-0x10 controls/type-0x41 chunks, RAM-only epoch metadata, seal without selection, and explicit activate.

## Execution Order

```text
firmware-hardware-001 (done; offline review passed)
  ├─ firmware-usb-001
  │    ├─ firmware-input-001
  │    ├─ firmware-led-001 → client-led-001
  │    │    └─ firmware-led-ram-harness-001
  │    │         → firmware-led-calibration-001
  │    │         → replacement-led-e2e-001 (after bootstrap; separately authorized)
  │    ├─ asset-protocol-001
  │    │    → vka1-core-001
  │    │       → firmware-asset-store-001
  │    │          → firmware-asset-transfer-001
  │    │             → firmware-screen-001
  │    │                → firmware-display-001
  │    │                → client-assets-001
  │    ├─ firmware-audio-deps-001
  │    │    → firmware-audio-001
  │    └─ firmware-bootloader-001 → firmware-update-001
  └─ firmware-bootstrap-001 after hardware + USB independent reviews

client-hardware-e2e-001 continues against vendor firmware with human input and has no replacement LED dependency

replacement-led-e2e-001 waits for firmware-bootstrap-001 + firmware-led-001 + firmware-led-ram-harness-001 + firmware-led-calibration-001 + client-led-001 and separate per-step authorization

client-app-001
  waits for all production firmware input/audio/LED/display/update/bootstrap services + client-assets + client-led + completed Swift core
  → replacement connected E2E acceptance
```

The workspace is not a Git repository, so overlapping implementation paths are serialized even when the dependency graph would otherwise allow parallel work.

## E2E Acceptance

1. Discover exactly VID `0x303a`/PID `0x1001`, handshake, capability-negotiate, maintain lease, and reconnect without Bluetooth.
2. Label all four physical keys and prove distinct configured actions exactly once.
3. Through `replacement-led-e2e-001`, separately authorize each exact pure-RAM build/descriptor plus ROM-entry/load/recovery sequence, pulse one LED pixel/channel at value 1 for at most 250 ms, prove fail-dark behavior, then use a separately reviewed current method before admitting an exact compiled profile.
4. Record key-controlled real speech and decode valid mono Ogg Opus with nonzero energy.
5. Upload/activate a static image and retain it across reboot.
6. Upload pet states and verify semantic transitions/variable frame timing.
7. Render time plus one live Mac metric without full layout resend.
8. Preview and device custom-layout geometry/color match documented tolerance.
9. Interrupted/invalid/no-space asset operations preserve previous committed/rendered revision.
10. Bootstrap only ota_1, prove protected regions unchanged, separately write verified secondary otadata, and prove ROM recovery; later update uses explicit RAM-stage activation and truthful rollback capability.
11. No secret, backup content, raw PCM, or speech file enters workspace/default logs.

## Gates

- USB Serial/JTAG CDC only; no BLE, Wi-Fi, network, TinyUSB, or UAC fallback.
- No hardware/protocol/dependency value without binary/runtime/header evidence.
- No production AFE/Opus claim until exact ESP32-target dependencies and named ABI are locked.
- No firmware write before hardware/bootstrap reviews, read-only security/eFuse checks, private backup revalidation, inactive-slot proof, exact readback, and recovery command review.
- No task is done until independent review checks docs, code, tests, and required real integration paths.
