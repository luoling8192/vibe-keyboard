---
id: asset-protocol-001
scope: typed replacement capability and asset protocol
status: in-progress
depends-on: [firmware-usb-001, client-usb-002]
---

## Objective

Implement optional versioned capability blocks, exact typed asset JSON models, and direction-safe type-`0x40` framing on firmware and Swift without storage mutation.

## Context

- `docs/product/usb-protocol.md`
- `docs/product/screen-assets.md`
- `mac/README.md`

## Path

- `firmware/components/vk_usb/`
- `mac/Sources/VibeBoardKit/Protocol/`
- `mac/Sources/VibeBoardKit/USB/`
- `mac/Tests/VibeBoardKitTests/`
- `firmware/tests/`

## Contract

- Implement stable capability envelope with optional `assets/screen/update` blocks, absent/unavailable/available semantics, and zero upload capacity without disabling management.
- This task may model and decode the update block but must not advertise update as available. Before `firmware-bootloader-001` and its first-boot confirmation gate pass, update is absent or `available:false` with `reason:"bootloader_migration_required"`. Swift may create or use the typed `0x41` sender only after the current epoch receives `features.update.available:true`; prior snapshots never authorize it.
- Implement every asset event/list entry/error schema exactly, including `max_asset_bytes`, the 30,000 ms refreshable catalog snapshot deadline, total catalog capacity, and exact screen query/commit/state/error models; handler may report unavailable until storage task.
- Type `0x40` is byte-exact: `01 40`, UInt16LE body length `8+N`, UInt32LE transfer ID, UInt32LE exact offset, then `N` payload bytes; total is `12+N<=4096`, `N` is `1...4084`, and zero/4085 reject. Host→firmware only.
- Decode available assets capability with the required positive `decoder_scratch_bytes`; firmware and Swift use it exactly once with the single serialized decoder owner in identical aggregate-memory admission equations.
- Validate and atomically publish each complete current-epoch capability snapshot without merging prior blocks. `screen.available:true` requires a valid `assets.available:true` block from that same snapshot; assets solely owns the decode-memory profile. Invalid/absent/unavailable assets disables screen preview/commit, while available assets may coexist with unavailable screen.
- Swift exposes typed senders only; ordinary JSON stays type `0x10`; no raw frame escape hatch.

- Contract implementation may not start until an independent review passes `docs/product/screen-assets.md` and its byte-exact shared corpus.

## Verification

- Shared corpus covers optional capabilities, full-storage management, all JSON schemas, UInt32 boundaries, payload 0/1/4084/4085, fragmentation, wrong direction/version/type.
- Vendor sessions never receive replacement types without current-epoch capability.


## Development Evidence: Bounded Firmware JSON Slice

- Added `vk_usb_json` as the only production JSON decoder used by `vk_usb_service`. The parser applies a 4,092-byte body ceiling, depth 12, 1,024 tokens, and 512 decoded UTF-8 bytes per string before accepting deeper/container/token/string state. Object keys count as tokens and decoded duplicate keys reject.
- The caller-owned document workspace is retained in the USB service allocation rather than a small owner-task stack. It has fixed token and scratch capacity and performs no parser allocation.
- The parser accepts bounded object/array/string/number/Boolean/null values, including unknown nested values. It rejects invalid UTF-8, malformed escapes, lone surrogates, duplicate escaped keys, truncation, trailing values, and malformed JSON.
- Numeric nodes retain their source lexeme. Typed unsigned access accepts only ASCII `0|[1-9][0-9]*`, uses checked accumulation, and rejects negative zero, leading zero, decimal, exponent, Boolean, Unicode digits, and UInt32/UInt64 overflow/wrap counterexamples.
- Existing USB command dispatch now uses the bounded parser and exact typed accessors; the former flat parser was removed. Handler registration no longer advertises optional capability readiness. Until the later typed provider slice passes review, production emits an empty optional-feature snapshot and does not authorize asset/screen/update availability.
- Added native adversarial coverage for depth 11/12/13, 1,900-level maximum-size nesting, exact 4,092/4,093-byte boundaries, exact root/key/value token accounting, 1,024/1,025 tokens, 511/512/513-byte strings, nested all-value kinds, duplicate escaped keys, UTF-8/surrogate failures, malformed/truncated/trailing values, and canonical UInt boundaries.
- Offline verification passed: native board/USB/audio/input suite; dedicated ASan and UBSan parser binaries (`ASAN_OPTIONS=detect_leaks=0` because Darwin ASan reports leak detection unsupported); USB TSan; ESP-IDF 5.5.2 clean build (1,960/1,960). The contract suite ran 21 checks with one documented skip and one unrelated failure: its pre-existing assertion still requires `firmware-input-001` to be `status: ready`, while that separately developed task is concurrently `in-progress`; the asset task assertion was updated to its truthful `in-progress` state.
- Offline image: 1,123,216 bytes; SHA-256 `ad401f970da813fbcca84315985c4c87469edbc95529799e74cc478aadbd1cbd`; ESP image checksum and appended validation hash are valid. This is build evidence only and authorizes no flash, device I/O, storage mutation, format, asset transfer, or screen operation.
- First independent firmware JSON review found that parser-time string validation accepted escaped `U+0000`, and that the production USB TSan overflow harness published its write count before the frame bytes and length. The parser now rejects scalar zero before counting or appending any decoded root, key, known, or unknown nested string. Permanent vectors cover escaped and raw-zero root/value/key cases. The fake writer now copies the complete frame under `written_mutex` before atomically publishing the committed write count; no assertion or timeout was weakened.
- Fix verification passed: dedicated UBSan and ASan+UBSan parser suites (`ASAN_OPTIONS=detect_leaks=0` on Darwin), the complete native runner, and all six production USB TSan schedules (default overflow, precommit stop, precommit epoch replacement, concurrent stop, timeout retention, cleanup taint). The contract suite still reports only the unrelated stale `firmware-input-001` status assertion: 21 run, 19 passed, 1 failed, 1 documented skip.
- ESP-IDF 5.5.2 clean fix build passed 1,960/1,960 steps. The offline image is 1,123,136 bytes with SHA-256 `d7b921d0261f34d03646d5c117cefd986b10c752c3edd18caaed3caaeb35daad`; checksum `0xdf` and appended validation hash `7ca4cefe834f9c336cf5ce72ca3f28598d7b81cec7dcd8f8a1b96265e53d437b` are valid. This remains offline build evidence only.
- Remaining task slices: typed atomic current-epoch capability provider; exact asset/screen command/event/error ABI; capability-authorized active-transfer type-`0x40` gate; shared Swift/C corpus and independent implementation review. This task remains `in-progress`.

## Development Evidence: Typed Capability Provider Slice

- Added caller-owned typed `assets`, `screen`, and `update` capability models with explicit absent, unavailable, and available states. Available assets and screen fields cover the complete Version-1 limits, storage state, decoder profile, canonical encoding/mode order, revision/configured relation, and immutable sorted font descriptors. Update availability remains deliberately unrepresentable until the bootloader/update owner supplies an evidence-bound running/target/rollback tuple.
- Added bounded deterministic capability validation and JSON encoding. Validation checks every numeric range, storage/free/reserve/upload relation, same-snapshot screen-to-assets dependency, font identity/order, and immutable update boot policy. Encoding uses a checked append builder, emits ordinary type-`0x10` JSON within 4,092 bytes, and accepts no raw JSON from providers.
- `get_device_info` recomputes the provider snapshot outside the USB state lock, validates it, and atomically replaces the complete current-epoch snapshot. Provider failure or invalid output publishes an empty feature object and never retains an older block. New epoch, lease expiry, stop, and terminal epoch failure clear the prior snapshot. Asset control dispatch derives authorization and upload limits from the complete validated snapshot; binary chunks remain unconditionally closed until the active-transfer slice.
- A registered available block must also have its typed handler gate. Production registers no capability provider and therefore continues to emit empty optional features. Update handler registration and `update.available:true` are rejected in every current build; unavailable `bootloader_migration_required` and absent remain representable. Asset type `0x40`, end, and abort remain fail-closed until the active-transfer owner pins an exact epoch/generation/tuple/offset.
- Added native capability validation/encoding tests and service tests for provider lock exclusion, repeated whole-snapshot replacement without merge, provider failure fail-closed behavior, unavailable pre-migration update, zero-upload management behavior, handler readiness, fail-closed chunks, and provider refresh versus stop/context release. Stop closes provider admission and refuses cleanup while a lock-free callback remains pinned; callback completion makes retry cleanup safe. Complete native regression, sanitizer runs, standalone USB service TSan, and all six production USB TSan schedules passed.
- ESP-IDF 5.5.2 clean build passed 1,961/1,961 steps after capability-review fixes. Offline image: 1,126,688 bytes; SHA-256 `96435e59233e873dcf9c213c9ebbc7ca60caade63f1e1d86cf15c786e093d3d5`; checksum `0xd1` and appended validation hash `c80fe574a7899d77b95786084bcf77be8c94b2cba98ad4ce7d9bc04ceb7907b7` are valid. `validate_build.py` confirmed ESP32-S3, 16 MiB flash, 8 MiB PSRAM, no selected forbidden transport entrypoints, and no sensitive markers.
- Contract tests remain 21 run, 19 passed, 1 failed, and 1 documented skip solely because the existing cross-task assertion still requires `firmware-input-001` status `ready`; that separately implemented task truthfully remains `in-progress`. This slice does not claim a green repository-wide contract suite.
- This capability slice awaits independent review and does not complete the exact asset/screen ABI, active-transfer tuple/offset gate, Swift session integration, storage mutation, formatting, rendering, firmware update, or any connected acceptance. `asset-protocol-001` remains `in-progress`.

## Development Evidence: Firmware Typed Asset/Screen ABI Slice

- Added exact typed asset command decoding for begin/query/end/abort/list/delete. List now requires `snapshot_id,cursor,limit`, rejects a zero snapshot with a nonzero cursor, and retains canonical UInt32/UInt8 validation from `vk_usb_json`. Storage format is recognized but always rejects with typed `not_erased` because this slice has no independently verified-erased authorization token.
- Added typed screen query/commit decoding. The commit gate enforces exact host assets/screen envelopes, ordered unique immutable asset entries, revision serial arithmetic, mode-specific nullability, image and pet envelopes, negotiated commit/layout/assets/object/widget/pet limits, and fixed 428x142 display identity. Parsed documents are exposed only as immutable node handles scoped to the callback; no raw JSON body is passed to a handler. Full object/widget/font/reference semantics remain owned by the later screen/store implementation and production registers no screen provider.
- Asset and screen callbacks receive nonzero expected epoch and capability snapshot generation. Admission is rechecked and pinned before callback. Stop closes admission and retains registration/context ownership until in-flight capability, asset, screen, or typed outbound callback leases quiesce. Callback completion from a stale epoch/generation cannot publish a response.
- Added bounded canonical typed encoders for storage formatted, asset ready/progress/stored/aborted/page/deleted, screen state/committed, and operation-specific asset/storage/screen errors. Catalog pages encode only whole entries, require sorted unique hashes, carry snapshot/cursor/next-cursor/revision, and reject overflow beyond the ordinary 4,092-byte body budget. No raw sender was added.
- Begin may reach an explicitly registered no-mutation fake for ABI tests. End, abort, and every type-`0x40` chunk remain fail-closed before callback until the active-transfer tuple/offset owner exists. Handler registration does not authorize a command; a valid same-epoch available capability snapshot is still required. Production registers neither asset/screen provider nor mutation handler, so features remain absent/unavailable.
- Firmware-local native vectors cover exact asset/list/screen image schemas, missing/extra/wrong values, canonical integers, lowercase hashes, kind/mode/revision rules, negotiated body limits, canonical event/error bytes, catalog whole-entry ordering, and stale epoch/generation publication. Native regression passed after one unrelated pre-existing flaky audio cleanup run was rerun successfully. Dedicated ABI ASan+UBSan and UBSan passed; all six production USB TSan schedules passed. Contract remains 21 run, 19 passed, 1 failed, 1 documented skip solely because the existing input cross-task assertion still expects `firmware-input-001` status `ready`.
- ESP-IDF 5.5.2 clean build completed 1,962 steps plus the incremental lease fix. Offline image is 1,131,008 bytes with SHA-256 `8a6f7516caf7e387eca3d83c827772708380553ae3b0e340dc7a1f084181f66e`. This is offline build evidence only and authorizes no device I/O, flash, reset, format, asset/storage/screen mutation, or LED operation.
- Remaining slices include independent review of this ABI, the exact active-transfer epoch/generation/tuple/offset state machine, shared Swift/C fixtures, full layout/widget/font/reference semantic validation in the screen owner, and real store/display integration. `asset-protocol-001` remains `in-progress`.

## Development Evidence: Active Asset Transfer Slice

- Added one current-epoch active asset-transfer owner inside the USB service. A successful typed begin callback publishes `vk_asset_ready` and binds the exact epoch, capability snapshot generation, transfer ID, SHA-256, total bytes, kind, negotiated chunk limit, and initial offset. A duplicate identical begin is idempotent; a conflicting begin returns `busy`.
- Type `0x40` now requires that exact authorization. It parses only the documented little-endian transfer ID and offset, rejects zero/unknown IDs, stale or out-of-order offsets, zero/oversized/overrun payloads, and type `0x41`. The immutable callback value carries expected epoch and snapshot generation. A successful backend callback is the durable-checkpoint boundary; only then does the owner advance and emit progress. Failure retains the previous offset and emits the exact expected offset.
- Query returns only the pinned active tuple. End requires a matching complete tuple and successful backend seal callback before emitting stored and invalidating authorization. Abort is idempotent and emits aborted. Capability refresh/replacement, new epoch, lease expiry, terminal epoch failure, and stop all clear admission before stale callbacks can publish or advance.
- Production still registers no asset/capability backend, so this path remains unreachable on hardware and performs no storage mutation. Native tests use only the in-memory fake callbacks.
- Complete native regression and a dedicated UBSan build passed. ESP-IDF 5.5.2 clean build completed 1,967/1,967 steps; the final `vibe_keyboard.bin` is 1,135,712 bytes with SHA-256 `5caa13c7a756ea56eaa47a9dc5b85d77800e01fdba3a0e8eadc45fec05069576`. Repository contract tests remain 21 run, 19 passed, 1 failed, 1 documented skip solely because the stale cross-task input assertion still requires `status: ready` while that task is truthfully `in-progress`.
- This remains development evidence rather than independent review. Shared corpus, real store integration, full screen semantics, and connected USB acceptance remain incomplete.
