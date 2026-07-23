---
id: firmware-update-001
scope: replacement-only staged USB firmware update
status: in-progress
depends-on: [firmware-usb-001, firmware-hardware-001, firmware-bootloader-001]
---

## Objective

Implement the replacement firmware's typed RAM-epoch stage/write/seal/query/cancel/activate service. This task does not bootstrap the first replacement image or migrate the bootloader.

## Context

- `docs/product/hardware.md`
- `docs/product/usb-protocol.md`
- `docs/firmware/README.md`

## Path

- `firmware/components/vk_update/`
- `firmware/main/`
- `firmware/tests/`
- `mac/Sources/VibeBoardKit/`
- `mac/Tests/VibeBoardKitTests/`

## Contract

- Use type-`0x10` exact update JSON schemas and host→firmware type `0x41` chunks of `1...512` bytes; do not reuse vendor `0x20...0x23`.
- Define managed slots as `ota_0 @ 0x020000` and `ota_1 @ 0x520000`. At each decision boundary, require the running partition to be exactly one managed slot and the target to be exactly the other inactive managed slot: running `ota_0` targets `ota_1`, and running `ota_1` targets `ota_0`. Require exact size/SHA/ID/offset and fail closed on every mismatch.
- Recompute update capability from current partition identity on every successful `get_device_info`; a prior epoch or snapshot never authorizes staging. `vk_update_begin`, every chunk write, seal, and activate each re-read and validate the running/target tuple. Any tuple change aborts the handle, invalidates RAM metadata, and prevents further write/seal/selection.
- Keep staged metadata only in current-epoch RAM, including both running and target slot/address identities captured at begin. Timeout/disconnect/new epoch/reboot invalidates it and requires restaging from zero; no NVS/SPIFFS metadata is written.
- Seal requires the current tuple to equal begin, then performs exact byte/digest, `esp_ota_end`, readback, descriptor/chip/revision/size checks without changing otadata or rebooting.
- Activate revalidates epoch, sealed tuple, current running slot, unique inactive target, target readback, and partition selection before selection. Host backup evidence is a local UI gate, never a firmware token.
- This task starts only after `firmware-bootloader-001`. Before that migration and its first-boot confirmation gate pass, the update feature is absent or unavailable with `bootloader_migration_required`; after it, advertise only the reviewed pending-verify rollback policy.
- Pending-verify builds, when later authorized, must run mandatory partition/USB/watchdog/heap tests plus tests for each compiled advertised capability before mark-valid/invalid.

## Verification

- Shared Swift/C wire goldens cover every event/chunk/error/idempotency boundary.
- Native/component tests inject timeout, disconnect, wrong epoch/ID/offset/hash/slot, write/seal/readback/selection failures and prove RAM invalidation and no pre-activate otadata change.
- Clean artifact checks prove target/chip/hash/partition sizes and absence of BLE/network/raw send.
- First-write execution remains exclusively `firmware-bootstrap-001` and separately authorized.

## Offline implementation evidence

- `firmware/components/vk_update/` implements an allocation-free RAM-epoch update owner with exact managed-slot tuple validation, 512-byte chunk admission, exact offsets, durable backend boundaries, seal/readback identity checks, separate activation, idempotent begin/seal/activate behavior, and disconnect/new-epoch invalidation.
- `firmware/tests/native/test_update.c` uses a RAM fake backend and covers both ota_0→ota_1 and ota_1→ota_0, repeated-begin tuple revalidation, wrong running/target tuple, wrong epoch, conflict, offset, durable write failure, incomplete seal, seal/readback terminal invalidation, selection separation, exact activation replay without selecting twice, and the strict 30,000 ms pre-seal idle boundary.
- Every active operation injects monotonic time and revalidates the complete managed-slot tuple. Seal/readback integrity failure cancels backend ownership and clears RAM authorization before returning.
- Production composition does not register this backend and the existing immutable USB boot policy continues to reject `update.available:true` and type `0x41`.
- Status remains `in-progress`: typed USB update ABI integration, shared Swift/C wire corpus, ESP OTA backend, reviewed bootloader evidence, connected readback, activation authorization, and physical rollback acceptance remain absent.
