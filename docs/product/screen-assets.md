# Screen and Asset Contract

Status: Replacement-firmware protocol contract; vendor firmware supports state text only and has no proven asset protocol

## Ownership and Modes

Firmware owns LVGL objects, animation timing, composition, brightness, storage recovery, and NV3007 writes. macOS performs bounded source decoding/conversion, preview, typed transfer, and low-rate widget updates. It never streams a continuous framebuffer.

Vendor firmware proves only `{"event":"ui_state","state":"ready","text":""}`. Every `vk_` command and type `0x40` below is a negotiated replacement extension. It must not be sent until current-epoch `vk_capabilities.protocol == 1` contains the required available feature block.

| Mode | Persisted screen configuration |
|---|---|
| `image` | one immutable image SHA-256 plus fit/background |
| `pet` | one immutable pet manifest and semantic state map |
| `dashboard` | one immutable layout plus widget declarations |
| `custom` | one immutable allowlisted layout |

`configured_mode` is persisted. `effective_mode` is runtime state. `recording`, `upload`, `firmware_update`, and `error` overlays form a LIFO stack owned by one firmware state task. Removing the top overlay restores the next overlay or configured mode; overlays are never committed or recreated on reconnect.

## Deterministic Pixel Contract

- Logical display is 428×142, top-left origin.
- Embedded source profiles are converted to nonlinear 8-bit sRGB; missing profiles mean sRGB. EXIF orientation is applied first.
- Alpha compositing is integer straight-alpha per channel: `out = floor((src * alpha + bg * (255-alpha) + 127) / 255)`. Background is explicit opaque RGB888; no transparent RGB565 sentinel exists.
- `contain`, `cover`, and `stretch` resize; `center` does not resize. Odd residual/crop pixels put `floor(residual/2)` on top/left and the remainder on bottom/right.
- Bilinear destination pixel `(dx,dy)` maps source centers as `sx=((2*dx+1)*srcW)/(2*dstW)-1/2`, similarly for y. Clamp `sx/sy` to source-center range `[0,srcSize-1]`. Use exact rational weights from adjacent clamped samples; each 8-bit sRGB channel is rounded half-up (`floor(value + 1/2)`) and clamped to `0...255`.
- RGB565 is `((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)` and stored UInt16 little-endian. Version 1 uses no dithering.
- Host and firmware share golden vectors for mapping, resampling, alpha, fit/crop, RGB565, clipping, and RLE.
- GIF/APNG disposal/blending must pass target-macOS golden fixtures. ImageIO output is not assumed composited; failure requires a dedicated bounded parser/compositor.

## VKA1 Container

```text
offset  size  type        meaning
0       4     bytes       ASCII VKA1
4       1     UInt8       kind: 1=image, 2=animation, 3=glyphBitmap
5       1     UInt8       pixel format: 1=RGB565
6       1     UInt8       encoding bits: bit0=raw, bit1=rowRLE
7       1     UInt8       flags: 0
8       2     UInt16 LE   width
10      2     UInt16 LE   height
12      2     UInt16 LE   frame count
14      2     UInt16 LE   header bytes = 56 + frameCount*12
16      4     UInt32 LE   decoded bytes per frame
20      4     UInt32 LE   total container bytes
24      32    bytes       full-container SHA-256 with bytes 24...55 zeroed
56      ...               frame table, then contiguous encoded frames
```

Frame entry:

```text
UInt32LE dataOffset
UInt32LE encodedLength
UInt16LE durationMs
UInt8    frameEncoding: 0=raw, 1=rowRLE
UInt8    reserved: 0
```

Common invariants:

- dimensions are `1...428` by `1...142`; checked decoded size equals `width*height*2`;
- frame count is `1...max_frames`; total size equals received bytes and is within advertised upload limit;
- encoding bits contain only bits 0/1 and equal the union actually used;
- first data offset equals header bytes; every next offset equals previous end; no padding, overlap, alias, reorder, or trailing bytes;
- hash covers the entire container with its own field zeroed and identifies transfer, immutable filename, manifest reference, and stored result;
- unknown enum/bit/reserved value is rejected.

Kind invariants:

| Kind | Frame count | Duration | Dimensions |
|---|---|---|---|
| `image` | exactly 1 | exactly 0 | bounded common dimensions |
| `animation` | at least 1 | every frame within advertised min/max and nonzero | every frame shares header dimensions |
| `glyph_bitmap` | exactly 1 | exactly 0 | bounded common dimensions; used only with explicit glyph metrics in layout |

Raw is exactly one row-major LE RGB565 frame. RowRLE stores runs `UInt16LE count + UInt16LE pixel`; count is `1...width`, runs cannot cross rows, every row decodes to exactly width, every frame to exactly height, and no trailing bytes remain.

The Version-1 RowRLE encoder is canonical. It scans each row left-to-right and emits exactly one run for every maximal sequence of equal UInt16 pixel values. It never splits a maximal run, never merges runs across rows, and resets run detection at every row boundary. Since Version-1 width is at most 428, every canonical run count fits UInt16; an implementation must still use checked conversion and reject an out-of-contract width rather than truncate or split a run. For each frame, the encoder compares the complete canonical RowRLE byte count with the raw frame byte count. It selects rowRLE only when the canonical RowRLE is strictly smaller; equality and larger RowRLE select raw. Header encoding bits equal exactly the union of encodings selected by the frame entries: bit 0 iff at least one frame is raw and bit 1 iff at least one frame is rowRLE. C and Swift must therefore produce byte-identical frame payloads, tables, complete containers, and full-container hashes for identical inputs.

The shared VKA1 binary corpus includes at least: a one-pixel run, alternating pixels, a maximal full-width run, distinct equal-size raw/RowRLE and strictly-smaller RowRLE decisions, row-boundary equal colors that remain separate runs, mixed raw/RowRLE frames with exact encoding-bit unions, and complete-container SHA-256 known answers. Corpus acceptance compares complete bytes and hashes, not only decoded pixels or metadata.

## Capability Envelope

`vk_capabilities` uses the stable envelope in [USB Protocol Contract](usb-protocol.md). Screen features are optional blocks under `features`. A known `assets` or `screen` block rejects unknown/extra keys; the generic rule that a newer unknown feature name may be skipped does not loosen a known block. JSON integer values must be exact unsigned integers in the range stated below; booleans and floating-point values are never integers.

An available assets block has exactly this shape; values are examples, not defaults:

```json
{
  "version": 1,
  "available": true,
  "management": true,
  "storage_state": "ready",
  "free_bytes": 0,
  "reserve_bytes": 1,
  "upload_max_bytes": 0,
  "max_asset_bytes": 1048576,
  "chunk_bytes": 4084,
  "max_assets": 64,
  "max_frames": 1,
  "min_frame_ms": 1,
  "max_frame_ms": 65535,
  "max_active_decoded_bytes": 243104,
  "decoder_scratch_bytes": 4096,
  "encodings": ["raw", "row_rle"],
  "revision": 0
}
```

Exact assets fields:

| Field | Contract |
|---|---|
| `version` | UInt16, exactly `1` |
| `available` | exactly `true` |
| `management` | exactly `true`; means the typed protocol exists, not that every command is currently authorized |
| `storage_state` | `unformatted|ready|corrupt|mount_failed|busy` |
| `free_bytes` | UInt32 current filesystem free bytes, zero outside `ready|busy` |
| `reserve_bytes` | UInt32, positive |
| `upload_max_bytes` | UInt32; zero unless `ready`; exactly the largest currently admissible upload and no greater than `max(0,free_bytes-reserve_bytes)` or `max_asset_bytes` |
| `max_asset_bytes` | UInt32, positive; exact immutable VKA1 file-size ceiling for this profile |
| `chunk_bytes` | UInt16 in `1...4084` |
| `max_assets` | UInt16 in `1...1024`; exact upper bound on all valid immutable VKA1 objects stored at once, on every complete catalog snapshot, and on every assets manifest |
| `max_frames` | UInt16, positive |
| `min_frame_ms`,`max_frame_ms` | UInt16, positive, minimum no greater than maximum |
| `max_active_decoded_bytes` | UInt32, positive aggregate decoded-pixel budget |
| `decoder_scratch_bytes` | UInt32 in `1...max_active_decoded_bytes`; exact maximum live bytes of the one global Version-1 decoder scratch allocation |
| `encodings` | exactly `['raw','row_rle']` in this order for Version 1 |
| `revision` | UInt32 selected commit revision, zero iff no valid commit is selected |

Storage command authorization is derived from the complete block, never from `management` alone:

| State | Authorized operations |
|---|---|
| `unformatted` | `vk_storage_format` only when the same epoch owns fresh verified-erased authorization; no list/upload/delete |
| `ready` | query/list; begin/chunk/end/abort when `upload_max_bytes>0`; delete with revision/reference checks |
| `busy` | query of the already active transfer only; no new begin/list/delete/format |
| `corrupt` | no asset/storage mutation |
| `mount_failed` | no asset/storage mutation |

An unavailable assets block is exactly `{version:1,available:false,reason:R}` where `R` is `display_acceptance_required|storage_unavailable|integrity_unavailable|policy_blocked`. It contains no limits, storage state, management flag, or revision.

An available screen block has exactly this shape; values are examples:

```json
{
  "version":1,
  "available":true,
  "modes":["image","pet","dashboard","custom"],
  "max_commit_bytes":4092,
  "max_layout_bytes":3072,
  "max_assets":64,
  "max_objects":32,
  "max_depth":4,
  "max_widgets":16,
  "max_fonts":4,
  "max_pet_states":6,
  "max_string_bytes":256,
  "max_json_tokens":512,
  "max_widget_value_bytes":256,
  "revision":0,
  "configured":false,
  "fonts":[{"id":"vk-sans","version":1,"metrics_sha256":"<64hex>"}]
}
```

`modes` is a nonempty unique array in canonical order `image,pet,dashboard,custom`; each member must be supported. `max_commit_bytes` and `max_layout_bytes` are UInt16 in `1...4092`, with layout no greater than commit. `max_assets` is UInt16 in `1...1024` and must not exceed the available assets block's value when both blocks are present. `max_objects`, `max_widgets`, and `max_fonts` are UInt16 positive; `max_pet_states` is UInt8 in `1...6`; `max_depth` is UInt8 in `1...8` and counts the layout object tree with each root at depth 1. `max_string_bytes` and `max_widget_value_bytes` are UInt16 positive and at most 512. `max_json_tokens` is UInt16 in `32...1024`; a token is each object, array, key, string, number, Boolean, or null encountered by the bounded decoder. The complete control body remains at most 4092 bytes, nesting at most 12 JSON containers, every string at most `max_string_bytes` UTF-8 bytes, and every array is additionally bounded by its named feature limit. `revision` is UInt32 and is zero iff `configured:false`; configured state requires nonzero revision. `fonts.count` is `1...max_fonts`. Each font descriptor has exact keys, ASCII `id` matching `[A-Za-z0-9_-]{1,32}`, nonzero UInt16 version, and 64 lowercase-hex metrics hash; IDs are unique and bytewise sorted.

An unavailable screen block is exactly `{version:1,available:false,reason:R}` where `R` is `display_acceptance_required|panel_unavailable|model_unavailable|storage_unavailable|policy_blocked`. Asset management may be available while screen rendering is unavailable. The converse is forbidden: `screen.available:true` requires a valid `assets.available:true` block in the same complete current-epoch capability snapshot. The available assets block is the sole owner of `max_active_decoded_bytes` and `decoder_scratch_bytes`; the screen block never duplicates or overrides either value. A snapshot containing `screen.available:true` with an absent, unavailable, malformed, or independently invalid assets block invalidates only the screen feature: the host treats screen as unavailable, performs no screen preview or commit, and retains any independently valid asset-management feature. Production firmware must never advertise that invalid combination.

Before the physical test-pattern acceptance proves orientation, color order, panel gaps, and RGB565 byte order, production asset/screen mutation blocks are absent or unavailable with `display_acceptance_required`. Compiling a store, parser, LVGL runtime, or test pattern does not open mutation capability. After that gate, an erased and independently proven storage partition may be advertised through an available assets block with `storage_state:"unformatted"`, zero upload capacity, and only the one-shot explicit-format operation authorized by the current epoch.

Capability validation is performed over the complete snapshot before any feature becomes usable. Known blocks are decoded and validated first without authorizing operations; the cross-feature invariant above is then applied; only then is the complete current-epoch snapshot published atomically to feature consumers. Object key order in `features` has no semantic effect. A duplicate capability snapshot never merges blocks or limits with the prior snapshot: after protocol and immutable display identity match, it independently validates and atomically replaces the prior snapshot as one whole value. Screen preview and commit use only the available assets block from that same published snapshot. A stale prior-epoch assets block, or an assets block retained from the replaced snapshot, cannot satisfy a new screen block. A duplicate with valid assets and unavailable screen keeps assets usable and disables screen; a duplicate with invalid assets and available screen disables both assets and screen rather than retaining old decode-memory limits.

`max_active_decoded_bytes` limits the peak sum of every simultaneously live decoded pixel buffer plus the negotiated decoder scratch allocation; immutable encoded bytes and non-pixel metadata are excluded. Every multiplication and addition below uses checked UInt32 arithmetic and overflow rejects before allocation. Define `asset_charge(A) = decoded_bytes_per_frame(A)` for `image|glyph_bitmap`, and `asset_charge(A) = decoded_bytes_per_frame(A) * min(frame_count(A),2)` for `animation`. `decoder_scratch_bytes` is the exact positive maximum live byte count of one fixed global scratch allocation owned by the single Version-1 decoder owner. Raw and row-RLE decode both charge that complete negotiated value, even if a particular decode uses fewer bytes. Candidate construction, root swap, and pet transition decoding are serialized through that owner; no second decoder or second scratch allocation may be live. Firmware and Swift use the exact current-epoch capability value in the same admission equations, never a local estimate. A single-frame animation therefore charges one frame.

A committed root has one exact steady-state charge. Image mode charges its referenced asset once. A layout charges every instantiated image, icon, glyph label, and pet leaf, including `visible:false` objects because Version 1 instantiates the complete root. An image/icon/glyph leaf charges its referenced asset once per object instance. A pet leaf charge is the maximum, over every ordered old/new state pair permitted by its manifest, of `asset_charge(old) + asset_charge(new)`; missing states are not candidates, explicit `fallback:"idle"` aliases the idle asset, and the two operands remain separately charged. This covers the old state's current/next animation buffers and the new state's current/next buffers during a pet-state transition. No Version-1 decoded-buffer sharing exists: equal SHA references in the same or different object instances are conservatively charged once per reference, and equal old/new pet SHA values are charged twice.

Commit acceptance checks both `candidate_steady + decoder_scratch_bytes <= max_active_decoded_bytes` and the LVGL root-swap peak `current_steady + candidate_steady + decoder_scratch_bytes <= max_active_decoded_bytes`; the scratch term appears exactly once because decoder concurrency is exactly one, and no old-root buffer may be freed before the candidate root becomes render-safe. The no-commit compiled root has decoded charge zero. After swap, the old root is destroyed before any later decode. Runtime pet transitions must stay within the candidate root's precomputed pet charge plus that same single scratch allocation; the pet charge already includes old/new decoded frames and must not include scratch again. These formulas, the exact current-epoch `decoder_scratch_bytes`, exact asset table, and current selected root are evaluated at commit time by both firmware and Swift preview; per-container validation alone never authorizes a screen.

## SPIFFS Names and Persistent Records

The verified storage partition remains SPIFFS and flat; slash is a filename character. Production must configure `CONFIG_SPIFFS_OBJ_NAME_LEN >= 78`, which includes the longest `/assets/<64hex>.vka` name plus NUL. Tests compute every name length; truncation is forbidden.

```text
/assets/<64-lowercase-hex>.vka
/tmp/<8hex-transfer>.part
/tmp/<8hex-transfer>.meta
/config/screen-r<8hex>.json
/config/assets-r<8hex>.json
/config/commit-r<8hex>.vkc
```

All persisted JSON is canonical UTF-8 with no BOM or trailing newline: object keys are bytewise ascending UTF-8, arrays retain schema-defined order, there is no insignificant whitespace, and unknown fields reject. The canonical encoder emits integers as exactly `0` or `-?[1-9][0-9]*`. Its decimal output starts from checked signed-64-bit coefficient plus scale `0...3`, strips trailing fractional zeros and a now-empty decimal point, and emits every zero as `0`. The persisted canonical decoder accepts only already-minimal output: it rejects `-0`, `-0.0`, `1.0`, `1.20`, plus signs, leading zeros, exponent notation, NaN, and Infinity rather than normalizing before hashing. Canonical decimal lexemes are at most 24 ASCII bytes.

An `integer` declaration/update accepts only a canonical scale-0 lexeme in Int64 range. A `number` or `progress` declaration/update accepts canonical numeric lexemes with scale `0...3`, including an integer-looking scale-0 lexeme such as `42`; type is declaration-driven, not inferred from the presence of a decimal point. Formatting rounds half away from zero, then uses the same canonical zero rule. Text never accepts a JSON number.

The shared corpus is the repository file `docs/product/fixtures/screen-assets-canonical-v1.json`. It contains the normative sidecar, assets manifest, screen manifest, layout, and pet manifest records. Each value under `canonical_records` is the complete canonical UTF-8 record without newline. C and Swift tests must compare emitted bytes directly with the UTF-8 bytes of that string, then parse and re-encode to the same bytes; decoded-object equality alone is insufficient. The corpus records are normative for field order and numeric spelling.

`assets-r....json` has exact keys `assets,previous_revision,revision,schema` and canonical shape:

```json
{"assets":[{"bytes":1,"kind":"image","sha256":"<64hex>"}],"previous_revision":0,"revision":1,"schema":1}
```

Entries have exact keys `bytes,kind,sha256`, are sorted by SHA bytes, are unique, and include exact immutable file byte length and kind. `screen-r....json` has exact keys `configured_mode,image,layout,pet,previous_revision,revision,schema` and canonical image-mode shape:

```json
{"configured_mode":"image","image":{"background_rgb888":0,"fit":"contain","sha256":"<64hex>"},"layout":null,"pet":null,"previous_revision":0,"revision":1,"schema":1}
```

Exactly the selected mode's payload is non-null. `image` uses the exact keys shown. `pet` embeds the bounded pet manifest defined below. `layout` embeds the complete Layout Version 1 object for dashboard/custom. Pet animation/image/glyph SHA references must all exist in the assets manifest; pet/layout JSON is protected by the exact screen-manifest hash in VKC1 rather than masquerading as a VKA1 kind. Revision zero means no previous commit only for the first revision; otherwise both manifests name the same previous revision.

A transfer `.meta` sidecar has exact keys `kind,next_offset,schema,sha256,total_bytes,transfer_id` and canonical shape:

```json
{"kind":"image","next_offset":0,"schema":1,"sha256":"<64hex>","total_bytes":1,"transfer_id":1}
```

The sidecar is rewritten after each durable progress checkpoint. Its complete canonical bytes are included in the shared corpus.
Commit `VKC1` is fixed 112 bytes:

```text
offset size meaning
0      4    ASCII VKC1
4      1    version=1
5      3    reserved=0
8      4    revision UInt32LE
12     4    previous revision UInt32LE
16     32   SHA-256 exact screen manifest bytes
48     32   SHA-256 exact assets manifest bytes
80     32   SHA-256 bytes 0...79 of this commit
```

A valid committed revision requires: valid commit self-hash; filename revision equals record; both exact manifest files exist and hashes match; schemas/revisions/previous fields agree; every referenced immutable file exists, filename/content VKA1 hash agrees, byte size/kind agree; screen references are included in assets; all limits/schema versions are compatible; and candidate render allocation succeeds before runtime switch.

SPIFFS rename is not considered replacement-atomic or power-loss atomic. Publication writes/fsyncs/closes destination-absent immutable files and writes the commit last. Boot scans a bounded set newest-to-oldest using UInt32 serial arithmetic and selects the newest fully valid commit, else one previous fully valid commit. It never fabricates empty state.

An asset whose filename hash does not match bytes, or an incomplete destination not referenced by any valid retained commit, is invalid orphan content and may be deleted after scan before retransmission. Valid matching content is deduplicated. Temporary/manifest/commit orphans may be removed only after current and previous valid revisions are retained.

### Explicit First Format

Mount failure never auto-formats. Firmware may advertise `storage_state:"unformatted"` only after a full read-only partition scan proves every byte is `0xff`, the partition identity/offset/size matches the hardware contract, and no valid filesystem exists. In that same USB epoch only, host may send:

```json
{"event":"vk_storage_format","confirmation":"verified_erased_spiffs"}
```

Firmware rechecks all preconditions immediately, formats once, replies `vk_storage_formatted`, and invalidates the command/epoch authorization. Duplicate/late/wrong-epoch commands fail. `corrupt` or `mount_failed` can never use this path and require explicit external recovery; no error erases storage automatically.

## Asset Transfer ABI

Kinds are strings `image|animation|glyph_bitmap`. Host generates a nonzero UInt32 transfer ID. SHA is exactly 64 lowercase hex. File sizes, offsets, cursors, snapshot IDs, and revisions are UInt32. Idle timeout is 30 seconds. Every known command/event rejects unknown or extra fields; `event` plus the listed fields is the exact key set. A parsed Boolean/floating value never satisfies an integer field.

Exact storage events:

```json
{"event":"vk_storage_format","confirmation":"verified_erased_spiffs"}
{"event":"vk_storage_formatted","revision":0}
```

The response revision is the selected commit revision and may be nonzero only if a valid selected commit still exists after a separately authorized recovery path; the explicit first-format path requires zero.

Exact transfer events:

| Event | Direction and exact fields after `event` |
|---|---|
| `vk_asset_begin` | host→device: `transfer_id,sha256,total_bytes,kind` |
| `vk_asset_query` | host→device: `transfer_id` |
| `vk_asset_ready` | device→host: `transfer_id,sha256,total_bytes,kind,next_offset,chunk_bytes` |
| `vk_asset_progress` | device→host: `transfer_id,next_offset` |
| `vk_asset_end` | host→device: `transfer_id,sha256,total_bytes,kind` |
| `vk_asset_stored` | device→host: `transfer_id,sha256,total_bytes,kind` |
| `vk_asset_abort` | host→device: `transfer_id` |
| `vk_asset_aborted` | device→host: `transfer_id` |
| `vk_asset_list` | host→device: `snapshot_id,cursor,limit` |
| `vk_asset_page` | device→host: `snapshot_id,cursor,entries,next_cursor,revision` |
| `vk_asset_delete` | host→device: `sha256,expected_revision` |
| `vk_asset_deleted` | device→host: `sha256,revision` |

List begins with `snapshot_id:0,cursor:0`; any other pair with zero snapshot is invalid. Firmware captures one complete current-epoch catalog snapshot containing at most `max_assets` valid immutable objects, assigns a nonzero ID, sorts entries by SHA bytes, and returns that ID. If storage contains more valid immutable objects than `max_assets`, assets capability is unavailable with `integrity_unavailable`; firmware must not expose a truncated catalog. Continuation must echo the ID and exact previous `next_cursor`; only one snapshot exists per epoch. The snapshot has a 30,000 ms monotonic idle deadline from publication of each successful page; each successful page refreshes it to 30,000 ms, while invalid requests do not. New upload publication, delete, format, reconnect, lease expiry, or deadline expiry invalidates it; continuation then returns `snapshot_expired`. `limit` is UInt8 in `1...64`, but firmware returns at most the number of whole entries that keeps the complete JSON body within 4092 bytes. It never splits an entry. If entries remain, `next_cursor` is a nonzero UInt32 index; at end it is JSON null. An implementation unable to encode one otherwise valid entry returns typed `internal` and no partial page.

A list entry is exactly `{sha256,total_bytes,kind,referenced}` where `referenced` means retained current or previous selected commit. Page `revision` and assets capability revision are the current selected commit revision, zero in no-commit state. Delete uses that same revision, rechecks both retained commits immediately before deletion, and may remove only an unreferenced immutable object. Successful deletion does not publish a commit and therefore does not advance revision. Stored means immutable content exists; it never means active. Screen activation occurs only through a complete revision commit.

Begin accepts `total_bytes` only in `1...min(upload_max_bytes,max_asset_bytes)` from the same current-epoch capability snapshot. Begin with an existing active transfer ID and exact tuple is an idempotent query. Any tuple mismatch is `conflict`. Existing valid matching content returns stored without rewrite. Type `0x40` chunks are host→device only and use the byte-exact layout in the USB contract: `[0]=0x01`, `[1]=0x40`, `[2...3]=UInt16LE body_length=8+N`, `[4...7]=UInt32LE transfer_id`, `[8...11]=UInt32LE exact offset`, and `[12...12+N)=payload`, where `N` is `1...min(current vk_asset_ready.chunk_bytes,4084)` and total length is exactly `12+N <= 4096`. The values must match the active current-epoch transfer tuple and `next_offset`. Zero/oversize, wrong-endian tuple values, wrong declared length, wrong direction, duplicate, stale, or out-of-order chunks reject without advancing state. Incremental fragmentation causes no callback until the full declared frame exists; trailing bytes belong to a subsequent independently validated frame and never extend the chunk. Writes advance only after complete durable checkpoint publication.

The `.meta` sidecar uses the exact canonical schema above after each durable progress checkpoint. Timeout/disconnect closes handles but retains valid part+sidecar. Query in a later epoch may resume only after sidecar/file length/tuple validation; mismatch deletes invalid temporary state and returns an error. Reboot performs the same bounded validation. End requires exact length/hash/VKA1 validation before destination-absent publication. Abort removes only temporary part/meta, emits `vk_asset_aborted`, and never changes committed content.

Asset/storage failure is exactly `vk_error` with `operation:"asset"|"storage"`, code from `invalid_request|unavailable|wrong_epoch|busy|conflict|not_found|bad_offset|bad_size|bad_hash|kind_mismatch|write_failed|incomplete|invalid_asset|timeout|no_space|referenced|revision_conflict|snapshot_expired|partition_mismatch|not_erased|format_failed|internal`, optional safely parsed `transfer_id`, `next_offset`, or `sha256`, and optional diagnostic-only `message` of at most 96 UTF-8 bytes. Unknown operation/code/extra fields reject on the host. Error text never controls retry or mutation.

### Screen Revision Commit ABI

Stored content becomes configured/active only through a complete revision commit. Every known event rejects missing or unknown fields; `event` plus the fields below is the exact key set. Control messages are type-`0x10` JSON.

| Event | Direction | Exact fields after `event` |
|---|---|---|
| `vk_screen_query` | host→device | none |
| `vk_screen_state` unconfigured | device→host | `assets_manifest_sha256:null,configured:false,configured_mode:null,revision:0,screen_manifest_sha256:null` |
| `vk_screen_state` configured | device→host | `assets_manifest_sha256:<64hex>,configured:true,configured_mode:image|pet|dashboard|custom,revision:<nonzero UInt32>,screen_manifest_sha256:<64hex>` |
| `vk_screen_commit` | host→device | `assets,expected_revision,revision,screen` |
| `vk_screen_committed` | device→host | `assets_manifest_sha256,previous_revision,revision,screen_manifest_sha256` |

Before any valid commit exists, the exact state bytes are `{"assets_manifest_sha256":null,"configured":false,"configured_mode":null,"event":"vk_screen_state","revision":0,"screen_manifest_sha256":null}`. The local compiled boot/USB-wait root is not persisted configured content and is never represented by manifest hashes. A configured state uses nonzero revision, a supported mode, and lowercase hashes. Query is idempotent and always reports the selected persisted revision, not an in-progress candidate. A configured image state with zero-placeholder hashes has exact canonical shape `{"assets_manifest_sha256":"<64hex>","configured":true,"configured_mode":"image","event":"vk_screen_state","revision":1,"screen_manifest_sha256":"<64hex>"}`. The exact query is `{"event":"vk_screen_query"}`. A commit has exact canonical shape `{"assets":{"assets":[]},"event":"vk_screen_commit","expected_revision":0,"revision":1,"screen":{"configured_mode":"image","image":{"background_rgb888":0,"fit":"contain","sha256":"<64hex>"},"layout":null,"pet":null}}`. The corresponding success shape is `{"assets_manifest_sha256":"<64hex>","event":"vk_screen_committed","previous_revision":0,"revision":1,"screen_manifest_sha256":"<64hex>"}`.

The host commit includes `expected_revision`, candidate `revision`, and two embedded manifest payloads. The host `assets` object includes exactly `assets`; it must not include `schema`, `revision`, or `previous_revision`. The host `screen` object includes exactly `configured_mode,image,layout,pet`; it must not include `schema`, `revision`, or `previous_revision`. Firmware derives and inserts exact `schema:1`, command `revision`, and current selected `previous_revision=expected_revision` into both persisted manifests, canonicalizes them by the persisted rules, and validates that the host payload is already exact-key and canonical-value compliant. No other field is derived. A first commit requires `expected_revision:0` and nonzero `revision`.

`max_layout_bytes` is the byte length of the complete canonical embedded `layout` object alone, from its opening `{` through closing `}`; it excludes the enclosing `screen` object and JSON string escaping does not apply because layout is embedded, not string-valued. It is checked only when mode is dashboard/custom. `max_commit_bytes` is the complete encoded type-`0x10` JSON body including `event`; it remains at most 4092. Both checks occur before allocation.

`revision` must be newer by UInt32 serial arithmetic and `expected_revision` must equal the selected revision. If the selected revision already equals command `revision`, firmware reconstructs both derived canonical manifests: exact byte-for-byte/hash equality returns the same `vk_screen_committed` response with the originally recorded `previous_revision`; any difference returns `conflict`. It performs no write, allocation, root swap, or revision advance.

Firmware validates canonical manifests, every stored reference, catalog and aggregate decoded-memory bounds, fonts, candidate allocations, and previous revision before writing immutable manifests and VKC1 last. `vk_screen_committed` is emitted only after validation and render-safe selection. Any failure leaves the previous selected revision/render unchanged. `vk_asset_stored` alone never changes screen state.

Screen failure is exactly `vk_error` with `operation:"screen"`, code from `invalid_request|unavailable|wrong_epoch|revision_conflict|conflict|invalid_manifest|missing_asset|font_mismatch|limit_exceeded|allocation_failed|render_failed|internal`, and optional diagnostic-only UTF-8 `message` of at most 96 bytes. No other fields are permitted. `revision_conflict` means `expected_revision` is not selected; `conflict` means an already selected candidate revision has different canonical bytes. Error text never controls retry or mutation.

## Layout Version 1

Top-level persisted layout has exactly these keys:

```json
{"background_rgb888":0,"mode":"custom","objects":[],"revision":1,"version":1,"widgets":[]}
```

`version` is UInt16 exactly 1; `revision` is the same nonzero UInt32 screen revision; `mode` is `dashboard|custom` and must equal configured mode; background is UInt24 `0...16777215`; widgets and objects are bounded by capability. IDs are ASCII `[A-Za-z0-9_-]{1,32}` and unique across all objects; widget IDs are separately unique. Every known object/declaration rejects unknown/extra fields.

Every object has these required base keys: `id,type,width,height,z,clip,visible`. Width/height are positive UInt16; z is Int16; clip/visible are Boolean. A root additionally requires Int16 `x,y`. A child of row/column forbids x/y. Ties preserve source order. Object rectangles and container aggregate sizes must fit parent/display by checked arithmetic; overflow rejects. The capability `max_depth` counts the first root as depth 1.

Leaf exact shapes below show child form; a root adds x/y:

```json
{"background_rgb888":0,"clip":true,"fit":"contain","height":100,
 "id":"photo","sha256":"<64hex>","type":"image","visible":true,
 "width":200,"z":0}

{"background_rgb888":0,"clip":true,"fit":"contain","height":100,
 "id":"avatar","pet":{"id":"pet","states":{"idle":{"sha256":"<64hex>"}},"version":1},
 "type":"pet","visible":true,"width":100,"z":0}

{"align":"left","clip":true,"color_rgb888":16777215,
 "font":{"id":"vk-sans","version":1},"height":20,"id":"title",
 "overflow":"clip","text":"Ready","type":"static_label","visible":true,"width":100,"z":0}

{"align":"left","clip":true,"color_rgb888":16777215,
 "font":null,"glyph":{"advance":16,"baseline":12,"bearing_x":0,"bearing_y":12,
 "sha256":"<64hex>"},"height":16,"id":"glyph","overflow":"clip",
 "type":"glyph_label","visible":true,"width":16,"z":0}

{"align":"left","clip":true,"color_rgb888":16777215,
 "font":{"id":"vk-sans","version":1},"height":20,"id":"status",
 "overflow":"clip","type":"dynamic_label","visible":true,"widget_id":"status",
 "width":100,"z":0}

{"background_rgb888":0,"clip":true,"fill_rgb888":65280,"height":8,
 "id":"bar","type":"progress","visible":true,"widget_id":"cpu","width":100,"z":0}

{"clip":true,"color_rgb888":16777215,"font":{"id":"vk-sans","version":1},
 "gap":4,"height":20,"id":"metric","sha256":"<64hex>","type":"icon_text",
 "visible":true,"widget_id":"cpu_text","width":120,"z":0}
```

`fit` is `contain|cover|stretch|center`; RGB values are UInt24. An image/icon SHA must reference `image`; a pet manifest is inline and follows the exact schema below. A `static_label` requires non-null font and exact `text`, forbids `widget_id` and glyph, and can never be a widget target. A `dynamic_label` requires non-null font and exact `widget_id`, forbids literal `text` and glyph, and renders only the declaration fallback until fresh data. A `glyph_label` requires `font:null`, exact glyph object, forbids `text|widget_id`, and can never be a widget target; metrics are signed Int16 `bearing_x,bearing_y,baseline`, positive UInt16 advance, and a `glyph_bitmap` SHA. `overflow` is exactly `clip`. Progress and icon_text require `widget_id`; icon_text has positive/zero UInt16 gap.

Container child form is exact; a root adds x/y:

```json
{"children":[],"clip":true,"cross_align":"center","gap":4,"height":40,
 "id":"row1","main_align":"start","type":"row","visible":true,"width":200,"z":0}
```

`type` is `row|column`; children are `1...max_objects`; gap is UInt16; align is `start|center|end`. Children retain explicit sizes; no flex, stretch, percentage, implicit resize, or space-between exists. Main-axis sizes plus gaps must fit. Remaining space is wholly after/before or split floor/remainder for start/end/center; cross-axis placement uses the same rule.

Widget declarations have exact per-type shapes and include deterministic no-fresh/stale/error presentation:

```json
{"fallback":"—","id":"status","target":"status","type":"text"}
{"fallback":0,"id":"count","target":"count_label","type":"integer"}
{"fallback":0,"format":{"decimals":1},"id":"cpu_text","max":100,"min":0,
 "target":"metric","type":"number"}
{"fallback":0,"format":{"decimals":1},"id":"cpu","max":100,"min":0,
 "target":"bar","type":"progress"}
```

Bindings are exact and one-to-one. A declaration `id` must equal the target object's `widget_id`, `target` must equal that object's object `id`, each declaration names one target, and each target object is named by exactly one declaration. No object accepts multiple declarations. Compatibility is: `text|integer|number → dynamic_label|icon_text`; `progress → progress` only. A progress object can never render text; a separate declaration must target a separate compatible object. Static/glyph labels are never targets. Text fallback is required UTF-8 within `max_widget_value_bytes`; text has no min/max/format. Integer fallback is required Int64; integer has no min/max/format. Number/progress require canonical numeric min/max/fallback with scale `0...3` (scale zero accepted), min<max, and fallback in range; format has exact `decimals` UInt8 `0...3`. Formatting rounds half away from zero and emits canonical zero. Dynamic label/icon_text shows only fallback before a fresh value and after stale/error/reconnect; there is no competing literal text.

Widget update is exact type-`0x10` JSON. `value` is required for fresh and its JSON type must match the declaration; it is forbidden for stale/error. `message` is forbidden for fresh/stale, optional diagnostic-only UTF-8 up to 96 bytes for error, and never becomes display text. Unknown/extra fields reject.

```json
{"event":"vk_widget_update","revision":7,"widget_id":"cpu","sequence":19,
 "state":"fresh","value":42.5}
```

Every validated fresh/stale/error update consumes the nonzero UInt32 sequence. Serial comparison rejects stale/equal/half-range values. Fresh stores/renders the new value. Stale/error discard any prior fresh value and render the declaration fallback; reconnect, lease expiry, reboot, and newer layout do the same and reset sequence ownership to that layout. Updates are RAM-only and never publish a revision. Success emits exact `{"event":"vk_widget_applied","revision":7,"widget_id":"cpu","sequence":19,"state":"fresh"}` only after LVGL-model application, so the host may claim rendered only after that acknowledgement. Failure is exact `vk_error` operation `widget`, code from `not_configured|wrong_revision|not_found|stale_sequence|type_mismatch|out_of_range|too_large|invalid_state|internal`, optional safely parsed widget/sequence, and optional 96-byte diagnostic message.

Fonts are firmware-owned versioned capability descriptors. Each descriptor maps by exact `(id,version)` to one immutable repository file `docs/product/fixtures/fonts/<id>-v<version>.metrics.json`. The production `vk-sans-v1.metrics.json` fixture covers every printable ASCII glyph compiled from LVGL Montserrat 14; its unkerned advances and bearings are the values advertised by firmware and used by Swift preview. The file is canonical UTF-8 without BOM/newline and has exact top-level keys `ascent,descent,glyphs,line_height,version`. Metrics are signed Int16 `ascent,descent`; `line_height` is positive UInt16; `version` equals the descriptor version. `glyphs` is a nonempty array sorted by scalar value, with exact entry keys `advance,bearing_x,bearing_y,scalar`; scalar is uppercase `U+` plus exactly 4 or 6 hexadecimal digits, denotes one valid Unicode scalar (never surrogate/noncharacter), and is unique. Advance is positive UInt16; bearings are Int16. `metrics_sha256` is SHA-256 of the complete file bytes. Firmware generated source and Swift preview are generated from that same immutable file and embed/assert the same digest; neither reconstructs data from the digest. A layout must match advertised ID/version/hash. Unsupported glyphs reject layout/text updates; no implicit fallback or ellipsis exists. Non-ASCII content remains available through uploaded image or animation assets.

Pet manifest Version 1 is an inline canonical JSON object:

```json
{"id":"pet","states":{"active":{"fallback":"idle"},"idle":{"sha256":"<64hex>"},"recording":{"sha256":"<64hex>"}},"version":1}
```

The exact key order/bytes are also in the canonical shared corpus. `id` is ASCII `[A-Za-z0-9_-]{1,32}`. State object keys are bytewise sorted. `states` has at most `features.screen.max_pet_states` unique keys from `idle|active|recording|thinking|success|error`; `idle` is required and must be exactly a SHA object. Each optional value is exactly one of `{sha256:<64-lowercase-hex>}` or `{fallback:"idle"}`. Omitted optional states are unavailable; there is no implicit fallback. Every SHA must refer to an `image` or `animation` in the same assets manifest. Unknown/extra fields reject. The same inline schema is used by configured `pet` mode and a layout `pet` leaf; it is protected by the enclosing canonical screen/layout bytes and is never stored or addressed as a VKA1 object. Scheduler uses monotonic deadlines, skips overdue frames, and keeps only bounded current/next decoded frames within the aggregate decoded-pixel limit.

## Required Tests

- Cross-language complete VKA1/hash and malformed/overflow/RLE/kind vectors.
- Exact sRGB resampling/alpha/EXIF/fit/RGB565 and GIF/APNG disposal goldens.
- Capability optional/unavailable/full-storage management behavior.
- SPIFFS filename bound, format authorization, mount/corruption failure, commit graph/hash, invalid destination cleanup, no-space, interruption, and previous revision recovery.
- Asset event/state/idempotency/timeout/reconnect/reboot/list/delete/revision and chunk `1...4084` boundaries.
- Layout container placement, widget formatting/sequence, font metrics/glyph rejection, screen manifest, overlay, pet deadline/skip goldens shared with preview.
- Connected static image, pet, dashboard, custom preview-match, interruption, storage-full management, and reboot acceptance after the first-write gate.
