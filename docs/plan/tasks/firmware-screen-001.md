---
id: firmware-screen-001
scope: typed screen model and LVGL image/layout/pet/widget runtime
status: in-progress
depends-on: [firmware-asset-transfer-001, firmware-hardware-001]
---

## Objective

Implement validated configured/effective screen state and device-side LVGL image, pet, dashboard, and custom rendering from committed assets.

## Context

- `docs/product/screen-assets.md`
- `docs/firmware/nv3007.md`
- `docs/firmware/README.md`

## Path

- `firmware/components/vk_screen/`
- `firmware/main/`
- `firmware/tests/`

## Contract

- Validate exact screen query/commit/state/error ABI, byte-canonical screen/asset/commit manifests, layout-v1 fixed container placement, one-to-one widget binding, canonical numeric formatting, immutable versioned font metrics corpus/glyph policy, limits, references, revision, and sequence rules before LVGL mutation.
- LVGL context owns object creation/swap/delete. Enforce the checked per-instance decoded charge, pet old/new transition peak, and `current + candidate + scratch` root-swap peak with no same-SHA sharing. Candidate allocation/render failure retains the current root.
- Implement allowlisted objects only; reject arbitrary LVGL/style/callback/panel commands.
- Pet scheduler keeps bounded current/next decoded frames, uses monotonic deadlines, skips overdue frames, and allows only explicit idle fallback.
- Runtime overlays restore configured state without persisting temporary state.

## Verification

- Pure model tests cover schema/bounds/depth/revision/widget/pet/overlay and animation deadlines.
- Injected LVGL/allocator tests prove lock context, render-safe swap, object cleanup, descriptor lifetime, and old-root preservation.
- Connected acceptance renders static, pet, time + one metric, and custom layout within measured memory/stack/PSRAM limits.

## Offline development evidence (2026-07-22)

- Added `vk_screen` with immutable candidate models, owner admission, render-safe root swaps, configured/effective overlay separation, UInt32 serial revision/sequence checks, widget updates, bounded pet deadlines, exact aggregate decoded-memory admission, and font metrics digest binding.
- Added a side-effect-free LVGL adapter boundary. Production does not construct/register a screen service, and capability remains unavailable until display + storage + product profile admission has connected evidence.
- Native fake renderer covers image/custom layout, widget binding/update, overlay LIFO, candidate failure preservation, pet deadline skipping, half-range rejection, memory rejection, and the shared geometry/format/font fixture values.
- Native suite passed, including screen concurrency. Focused ASan/UBSan and TSan screen binaries passed.
- ESP-IDF 5.5.2 clean build passed. Image: `1,141,360 bytes`, SHA-256 `494bd5f304923f5139ca3bc9ec99a9d7a0e2c34c2fd42eb9a127a89d5a06ea1e`; esptool checksum and validation hash are valid.
- Offline contract suite: `21 passed / 1 documented skip`.
- This is offline development evidence only. Boot recovery wiring, durable revision publication/service facade completion, compiled production font adoption, and connected display acceptance remain open; task intentionally remains `in-progress`.

## Production offline composition evidence (2026-07-22)

- Added bounded store→resolver→screen owner→display→typed USB composition and exact widget update facade. Screen commit now invokes immutable VKC1 commit-last durable publication before root swap; failed candidate/publication retains the old root and selected revision.
- Production boot mounts SPIFFS with `format_if_mount_failed=false`, recovers current/previous commits, registers typed asset/screen/widget handlers before USB start, and tears down in reverse order. The compiled-font and physical-acceptance gates remain false, so assets/screen stay unavailable and no production mutation path is authorized.
- Complete native firmware runner passed; focused new sources compile with `-Wall -Wextra -Werror`; Swift Xcode-beta suite passed `183 tests / 23 suites`. ESP-IDF was not available in this agent environment, so no new image/hash is claimed.
- No device, SPIFFS, panel, flash, reset, format, or USB I/O was performed. Status remains `in-progress` pending independent integrated review and physical acceptance.

## Integration stabilization evidence (2026-07-22)

- Added bounded `vk_asset_store_load_revision()` and `vk_screen_restore()`. Boot now reloads immutable current manifests into a fully validated render model/root; if the current candidate fails asset/font/VKA1/model/render validation, the service tries the validated previous revision. If both fail, the owner retains the safe empty root and marks recovery unavailable.
- Added a permanent RAM-filesystem recovery integration executable proving current-to-previous render-root fallback, plus sanitizer coverage for restore and store loading. Recovery never republishes a durable revision and never formats storage.
- Complete native runner and contract suite pass (`21 passed / 1 documented skip`). Clean ESP-IDF 5.5.2 build links screen restore, VKA1, store, input, audio, update, and fail-dark LED components.
- Integrated image: `1,223,248 bytes`, SHA-256 `bc1c43c3c66a57398fd9a99648d526d5a25860faef439a6e33e8d709f4d0b45a`; checksum `0xb9` and validation hash `0a93a7788fef839c64fc9e2690d0060e3372e9a529c34a3bb20cf284483ce536` are valid.
- Physical/font acceptance gates remain false. No connected panel, filesystem, USB, flash, reset, format, or mutation was performed.
