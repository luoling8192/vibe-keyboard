---
id: firmware-bootloader-001
scope: rollback-enabled replacement bootloader migration
status: in-progress
depends-on: [firmware-bootstrap-001]
---

## Objective

Design, independently review, and only after separate authorization migrate from the retained vendor bootloader/recovery arrangement to a replacement rollback-enabled bootloader and two-slot update policy.

## Context

- `docs/product/hardware.md`
- `docs/product/usb-protocol.md`
- `docs/firmware/README.md`

## Path

- `firmware/sdkconfig.defaults`
- `firmware/tools/`
- `firmware/tests/`
- `docs/plan/analysis/`

## Contract

- Before migration and its first-boot confirmation gate pass, `features.update` is absent or `available:false, reason:"bootloader_migration_required"`; this value belongs to the complete bounded update-unavailable reason enum. Replacement firmware running from ota_1 must not overwrite vendor ota_0.
- Migration requires a separately reviewed immutable bootloader image, Secure Boot/flash-encryption/eFuse compatibility, complete private backup revalidation, ROM recovery proof, exact bootloader-region write/readback, and explicit authorization.
- Migration defines the point at which `ota_0 @ 0x020000` and `ota_1 @ 0x520000` become managed replacement slots and records the loss/change of the vendor ota_0 recovery role. It must never silently repurpose ota_0.
- After migration, update target selection is dynamic: the target is always the unique inactive managed slot outside the current running slot. Capability computation and begin/chunk/seal/activate each re-read and validate this running/target tuple; any change fails closed and invalidates staged RAM state.
- Enable and test ESP-IDF application rollback. Mandatory pending-verify tests are partition identity, USB core startup, watchdog, and heap; compiled available features add their own bounded tests.
- Failure recovery must work from ROM download without relying on either application slot.

## Verification

- Offline bootloader image/config/hash/partition compatibility and OTA state-transition tests.
- Injected interruption tests for every bootloader/otadata mutation boundary.
- Independent review of exact command/address/readback/recovery artifacts before any connected write.
- No migration executes without a second explicit mutation authorization distinct from first app bootstrap.

## Offline implementation evidence

- `firmware/tools/bootloader_migration.py` builds and validates a bounded, immutable review manifest for the exact bootloader region, partition-table identity, protected ranges, managed slots, security-policy evidence, ROM-recovery evidence, rollback policy, and mandatory first-boot tests.
- The validator contains no serial, reset, flash, or device transport and cannot execute migration.
- `firmware/tests/test_bootloader_migration.py` covers exact identity, binding tamper, wrong image, unreviewed security, missing recovery evidence, region overflow, and image magic.
- Status remains `in-progress`: no reviewed physical bootloader image, security/eFuse evidence, private-backup revalidation, ROM recovery execution, connected readback, first-boot confirmation, or mutation authorization exists.