---
id: replacement-led-e2e-001
scope: separately authorized connected LED calibration and fail-dark acceptance
status: pending
depends-on: [firmware-bootstrap-001, firmware-led-001, firmware-led-ram-harness-001, firmware-led-calibration-001, client-led-001]
---

## Objective

Execute the reviewed pure-RAM one-stimulus calibration harness step by step, derive independently reviewed mapping/current evidence, admit one exact production profile, and verify the production LED service without coupling this mutation gate to vendor input/audio acceptance.

## Context

- `docs/product/led.md`
- `docs/product/hardware.md`
- `docs/plan/tasks/firmware-led-ram-harness-001.md`
- `docs/plan/tasks/firmware-led-calibration-001.md`
- `docs/plan/tasks/firmware-bootstrap-001.md`
- `docs/plan/tasks/client-led-001.md`

## Contract

- This task starts only after replacement bootstrap stage/activation/recovery acceptance has separately completed and the exact installed image, private recovery hashes, device identity, and authorized pure-RAM harness build, esptool identity, unchanged flash selection, and recovery-reset procedure are revalidated.
- Every raw-pixel/channel stimulus requires separate explicit authorization for one ROM-entry reset, its exact descriptor/build hash and `--no-stub load_ram`, one recovery reset, and post-recovery production identity/all-off verification. Use exact value 1 for at most 250 ms with pre-armed auto-off/watchdog and immediate fail-dark recovery.
- Record only canonical non-secret mapping/current evidence. Do not retain private backup/NVS data, device secrets, raw serial logs, or arbitrary device identifiers.
- Mapping observation does not authorize `available:true`. Current-limit measurement uses a separately reviewed method and authorization; only its independently reviewed output may enter profile admission.
- Rebuild/review production firmware with the exact compiled-in allowlisted profile, then verify capability, query/config request correlation, lifecycle all-off, disconnect, lease expiry, stopping, hardware failure, and reboot fail-dark behavior.
- No BLE, Wi-Fi, network, TinyUSB, USB OTG CDC, UAC, raw production LED command, runtime profile upload, or NVS profile write is allowed.

## Verification

1. Each authorized harness build matches its exact descriptor and independently proves all-off before and after the pulse.
2. Mapping artifact establishes physical order and logical key mapping without guessing.
3. Separately authorized current evidence establishes reviewed limits; profile hash and board/firmware identity match exactly.
4. Production `available:true` appears only with that profile and a healthy owner.
5. Swift query/config uses matching nonzero request IDs and only applied state changes UI state.
6. New epoch, lease expiry, disconnect, stopping, driver failure, task failure, and reboot all prove fail-dark behavior.
7. Independent review confirms every authorization boundary and that no private/raw evidence entered the workspace.
