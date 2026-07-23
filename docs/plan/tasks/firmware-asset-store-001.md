---
id: firmware-asset-store-001
scope: SPIFFS asset transfer, immutable revision, and boot recovery
status: in-progress
depends-on: [vka1-core-001]
---

## Objective

Implement the explicit-format SPIFFS asset store, resumable temporary writes, immutable content/revision publication, and deterministic boot recovery.

## Context

- `docs/product/hardware.md`
- `docs/product/screen-assets.md`
- `docs/firmware/README.md`

## Path

- `firmware/components/vk_assets/`
- `firmware/main/`
- `firmware/tests/`
- `firmware/sdkconfig.defaults`

## Contract

- Preserve storage partition layout; increase SPIFFS object-name bound and test the longest full name. Treat slash as a flat-name prefix.
- Never auto-format on mount/corruption failure. First format requires full-FF proof and the exact same-epoch one-shot typed confirmation.
- Implement byte-exact canonical part/meta sidecars and manifest schemas from the shared corpus, fixed 112-byte VKC1 commit, validation graph, destination-absent publication, and commit-last recovery.
- Retain current and previous valid revisions; boot scans bounded commits and recovers newest complete compatible revision.
- Enforce dynamic free/GC/reserve limits, exact `max_asset_bytes`, and `max_assets` as the total stored immutable-object/catalog/manifest bound. No failure erases storage or replaces the selected working revision.

## Verification

- Injected filesystem tests cover partial/EINTR/zero writes, fsync/close/name conflict/no-space/GC/mount/corruption failures and reboot at every publish boundary.
- Filename bound, manifest/commit hashes, invalid destination cleanup, dedupe, orphan cleanup, revision wrap, previous-revision fallback, and repeated recovery are deterministic/idempotent.
- Tests prove no format/erase fallback and no claim of SPIFFS power-loss atomic rename.

## Development Evidence

Implemented the bounded store core and SPIFFS adapter in `firmware/components/vk_assets/` without connecting it to production startup or USB. The production adapter always mounts with `format_if_mount_failed=false`; explicit format requires a caller-owned same-epoch token created only after an exact partition identity and full-FF scan, rechecks the scan immediately, and consumes the token before format. No production composition currently creates this token.

The store uses destination-absent immutable object, manifest, and fixed 112-byte VKC1 commit writes. The commit is written last; recovery scans bounded commit names and validates self-hash, manifest hashes, revision linkage, and the required external VKA1/revision graph callbacks before selection. Current, previous, and in-flight files are protected from collection. Temporary transfer metadata is canonical, durable, and can reconstruct a bounded resume state after reboot. SPIFFS rename is not used or claimed atomic. `CONFIG_SPIFFS_OBJ_NAME_LEN=96`, above the required 78-byte configuration floor.

Offline checks, with no device I/O or filesystem mutation outside native fakes:

```text
Asset-store ASan+UBSan: passed (ASAN_OPTIONS=detect_leaks=0)
Asset-store TSan: passed
Complete native board/JSON/capability/USB/VKA1/asset-store/input/audio runner: passed
ESP-IDF 5.5.2 clean build: 1967/1967
CONFIG_SPIFFS_OBJ_NAME_LEN=96
Image size: 1,136,000 bytes
Image SHA-256: 0679232d9df7f6286a7eb5b859feab5c8bf987fe283b7a5198d85f05220a3bf8
```

The task remains `in-progress`: USB transfer integration, canonical full manifest parsing/reference validation supplied by the screen owner, physical SPIFFS behavior, explicit-format authorization, and independent review remain outstanding.
