# LED Feedback Contract

## Scope and Evidence

VibeBoard has one 17-pixel addressable chain on GPIO8. The reviewed board profile proves SK6812, three-component GRB order, RMT TX with a 10 MHz tick resolution, DMA disabled, and output inversion disabled. The 10 MHz value is the RMT encoder tick resolution, not the SK6812 wire bit rate. Raw pixels `0...3` are the four key LEDs and raw pixels `4...16` are the thirteen strip LEDs.

The following remain unknown and must not be guessed:

- the mapping from logical `k1...k4` to raw pixels `0...3`;
- the physical direction of raw pixels `4...16`;
- a safe sustained per-channel brightness limit;
- a safe complete-frame channel-sum/current limit;
- vendor color, theme, animation, and priority semantics.

Until separately authorized physical calibration and current-budget evidence produce a reviewed production profile, production firmware keeps every pixel off and advertises LED unavailable. Existing all-off board initialization is a safety baseline, not LED product support.

## Transport and Capability

LED control is a replacement-protocol feature carried only by the ESP32-S3 built-in USB Serial/JTAG service. Bluetooth, BLE, Wi-Fi, network, TinyUSB, USB OTG CDC, and USB Audio Class are prohibited transports or fallbacks.

`led` is an independent key in the complete current-epoch `vk_capabilities.features` object. It does not depend on `assets` or `screen`, and neither feature may supply or override LED limits. Capability snapshots follow the atomic whole-snapshot replacement rules in `usb-protocol.md`.

Before production-profile admission, the exact known block is:

```json
{"version":1,"available":false,"reason":"calibration_required"}
```

The block has exactly those keys. Version 1 unavailable reasons are `calibration_required|hardware_failed|tainted`. Query remains legal while unavailable; every non-query LED command returns `vk_error` code `unavailable` and cannot enqueue a nonzero frame.

After reviewed profile admission, `available:true` requires exactly:

```json
{"version":1,"available":true,"pixel_count":17,"key_pixels":{"k1":K1,"k2":K2,"k3":K3,"k4":K4},"strip_first":4,"strip_count":13,"color_model":"rgb8","wire_order":"grb","tick_ms":30,"max_brightness":N,"max_frame_channel_sum":M}
```

Rules:

- `K1...K4` are distinct integers whose set is exactly `{0,1,2,3}`; their values remain intentionally unspecified until physical calibration.
- `N` is a measured integer in `1...255`; it remains unspecified before calibration.
- `M` is a measured integer in `1...13005` (`17 × 3 × 255`); it remains unspecified before calibration.
- `tick_ms:30` and the internal maximum channel change of 32 per tick are replacement animation policy selected from observed device behavior, not hardware requirements or claims of visual parity.
- Known LED blocks reject missing, extra, Boolean-for-integer, and out-of-range values.

No build may advertise `available:true` merely because the RMT driver initialized or a mapping observation exists. Production admission additionally requires a healthy LED owner and the exact reviewed profile described below.

## Exact USB ABI

Host-to-device commands are ordinary type-`0x10` exact-key JSON objects:

```json
{"event":"vk_led_query","request_id":1}
{"event":"vk_led_config","request_id":2,"enabled":true,"brightness":N}
```

`request_id` is a host-selected nonzero `UInt32` scoped to the current protocol epoch. `vk_led_query` accepts exactly `event,request_id`. `vk_led_config` accepts exactly `event,request_id,enabled,brightness`, where `enabled` is Boolean and `brightness` is an integer. When available, `brightness` is `0...max_brightness`; firmware rejects rather than clamps.

Device state is one of four exact variants:

```json
{"event":"vk_led_state","request_id":1,"source":"query","available":false,"reason":"calibration_required"}
{"event":"vk_led_state","request_id":1,"source":"query","available":true,"enabled":true,"brightness":N,"effective":"off|connected|recording|mutation"}
{"event":"vk_led_state","request_id":2,"source":"applied","available":false,"reason":"hardware_failed"}
{"event":"vk_led_state","request_id":2,"source":"applied","available":true,"enabled":true,"brightness":N,"effective":"off|connected|recording|mutation"}
```

The unavailable variants accept exact reason `calibration_required|hardware_failed|tainted`; available variants contain no `reason`. Query always replies with `source:"query"` and echoes its request ID. A successful config replies with `source:"applied"` and echoes its request ID only after the LED owner has applied the resulting complete frame. Host write completion and a query response are never config acknowledgement.

State invariants are exact:

- `available:false` forbids `enabled`, `brightness`, and `effective`;
- `available:true` requires all three fields;
- `enabled:false` requires `effective:"off"` and retains the requested validated brightness for a later enable within the same epoch;
- `brightness:0` requires `effective:"off"` regardless of `enabled`;
- a new epoch resets to `enabled:false,brightness:0,effective:"off"`.

There is at most one outstanding config. A different config while one is outstanding returns `busy`. A retry with the same request ID and byte-equivalent body coalesces while pending and replays the exact cached applied response after completion without applying twice. Reuse of the same ID with a different body is `invalid_request`. The owner retains only the most recently completed config request/body/response until another config completes or the epoch closes. Query IDs do not occupy the config slot; their `source:"query"` response cannot satisfy it. The Swift LED actor alone owns one non-extending 1,000 ms monotonic absolute deadline per config attempt. At send admission it captures `absolute_deadline = checked(monotonic_now + 1,000 ms)`. For every matching current-epoch `source:"applied"` response it captures `response_linearization_time` in that same actor isolation domain and accepts the response only when `response_linearization_time < absolute_deadline`; equality or a later timestamp is timeout. Timer wake and response-delivery queue order cannot change that timestamp comparison. Timeout atomically clears the outstanding operation before publishing failure. A late matching applied response may update observed device state but cannot complete, revive, or extend the timed-out operation. Disconnect/new epoch invalidates every pending or cached ID.

Errors use:

```json
{"event":"vk_error","operation":"led","request_id":2,"code":"invalid_request"}
```

For a syntactically recoverable request ID, LED errors require that ID; malformed/missing/Boolean/zero request IDs omit it. Exact codes are `invalid_request|wrong_epoch|unavailable|busy|queue_overflow|hardware_failed|tainted`. Optional `message` is diagnostic-only valid UTF-8 of at most 96 bytes. Unknown, extra, missing, Boolean-for-integer, and out-of-range fields are `invalid_request`.

Host configuration is RAM/current-epoch state. New epoch, lease expiry, disconnect, or service stopping clears the host override and converges to an all-zero frame before clean lifecycle acknowledgement. It never writes NVS. No public API accepts a pixel index, arbitrary RGB/GRB bytes, RMT timing, palette, animation program, calibration command, or raw send callback.

## Firmware Ownership

`vk_led` is the sole LED state and animation owner:

```text
vk_input / vk_audio / vk_screen / vk_update / vk_usb
  → typed semantic intents
  → fixed eight-item vk_led ordinary mailbox
  → priority and calibrated-budget evaluation
  → complete 17-pixel logical-RGB frame
  → typed vk_board frame transport
  → set_pixel × 17, then one refresh
```

No producer receives the LED driver handle. `vk_board` exposes only fixed-length all-off and complete-frame operations using 17 logical `{red,green,blue}` values; the pinned driver maps logical RGB to physical GRB. It exposes no raw strip handle, raw byte sender, partial-frame publication, or animation ownership.

The ordinary mailbox stores typed source/state intents, not frames. At most one pending intent per source exists; a newer intent from the same source replaces its older unconsumed intent. Overflow of ordinary intents reports `queue_overflow` and requests fail-dark; it does not close or reopen a USB epoch and does not replace USB control/audio/input terminals.

Safety work does not share one ambiguous slot. The owner has three allocation-free states:

1. a persistent `hardware_failure` latch;
2. one `stopping` lifecycle cell carrying its matching token/generation;
3. one `epoch_off` lifecycle cell carrying its matching token/generation.

Lifecycle safety separates two concepts:

- `cleanup_proof` is retained ownership of tick cancellation, old-generation exclusion, and the in-progress or completed all-off `clear → refresh` proof;
- `ack_obligation` is the single currently live USB lifecycle token/generation sink that may publish `QUIESCENT|TAINTED`.

Their exact lattice is:

| Existing state | Incoming epoch_off | Incoming stopping | Incoming hardware_failure |
|---|---|---|---|
| none | create cleanup proof and epoch-off obligation | create cleanup proof and stopping obligation | latch failure and start fail-dark |
| epoch_off | coalesce only the matching USB request; any distinct non-superseding token is tainted | invalidate the epoch-off obligation, retain and atomically retarget its cleanup proof to the fresh stopping obligation | latch failure; the currently live obligation resolves `TAINTED` |
| stopping | USB coalesces it before LED; no second obligation exists | coalesce matching token; distinct token is tainted | latch failure; the stopping obligation resolves `TAINTED` |
| hardware_failure | create only a current `TAINTED` obligation | create only a current `TAINTED` stopping obligation | idempotent |

`hardware_failure > stopping > epoch_off` controls execution. USB supersession invalidates the old acknowledgement sink exactly once; no old epoch-off acknowledgement remains publishable. The retained cleanup proof may continue without a second `clear → refresh`, but only the fresh stopping token/generation can receive its result. Completion targeting an invalidated sink is stale and ignored. A later hardware failure upgrades the retained proof to `TAINTED` and only the current live obligation receives that result. A hardware failure can never be downgraded by off or stopping. Pairwise and three-way schedules, including supersession between proof completion and acknowledgement publication, are required under TSan.

Fixed effective visual priority below the safety lattice, highest first:

```text
firmware mutation           → mutation
active audio capture        → recording
current USB epoch ready     → connected
otherwise                   → off
```

Key-pixel overlays remain disabled until the exact logical-key mapping is calibrated. Internal colors and waveforms are replacement policy and are not vendor-compatible claims.

Internal replacement palettes contain base logical-RGB channels in `0...255`. For host brightness `B`, every output channel is exactly `floor(base_channel × B / 255)` using a checked UInt32 multiply before division. `B` is `0...max_brightness`; no gamma curve, rounding-to-nearest, or hidden second scale exists in Version 1.

For every nonzero candidate frame, firmware:

1. computes that exact scale with checked integer arithmetic and rejects overflow;
2. proves each scaled channel is `0...max_brightness`;
3. computes the checked sum of all 51 scaled channels;
4. rejects a sum above `max_frame_channel_sum` without clamping;
5. applies at most 32 channel units of change per 30 ms tick.

Before production-profile admission, the same production path permits only the all-zero frame.

## USB Lifecycle Participation

`vk_led` registers one asynchronous typed lifecycle participant before USB service start. It receives the common request `{kind,token,old_epoch,proposed_epoch,lifecycle_generation}` and a participant-specific one-item acknowledgement sink. `begin` is nonblocking, allocation-free, never calls a USB façade, and returns `ACCEPTED|TAINTED`. It admits `epoch_off` or `stopping` through the dedicated safety cells even when all eight ordinary mailbox entries are occupied.

The USB owner closes old-epoch admission before `begin` and owns the same monotonic absolute 3,250 ms transition deadline used by the complete lifecycle composition. LED starts no independent deadline and cannot extend or restart the USB deadline. It acknowledges exactly `{token,lifecycle_generation,result:QUIESCENT|TAINTED}`. `QUIESCENT` requires the tick cancelled, ordinary intents discarded, a successful all-zero `clear → refresh`, and proof that no old-generation LED work can publish. Inability to prove all-off, safety-cell conflict, driver uncertainty, or exhausted remaining time acknowledges `TAINTED` when possible.

A proposed epoch becomes visible only after every registered participant returns a matching in-deadline `QUIESCENT`. LED `TAINTED`, begin failure, or timeout permanently taints/closes that USB composition and prevents the proposed epoch; it does not reopen the already closed old epoch or fabricate another subsystem terminal. Driver ownership needed for later safe cleanup is retained.

Lifecycle overlap follows the USB-owned token rules. Duplicate new-epoch/expiry requests coalesce at USB and do not reach LED twice. Stopping supersedes a pending new-epoch/expiry request with a fresh token/generation; LED invalidates the old `ack_obligation`, atomically retargets only the retained `cleanup_proof` to the stopping cell, publishes only through the fresh stopping sink, and treats every old completion or acknowledgement as stale. A late, duplicate, mismatched, superseded, or exact-deadline acknowledgement cannot satisfy the current request. Pre-epoch stopping `{old_epoch:0,proposed_epoch:0}` still cancels local work and proves all-off; if no driver was acquired and all-off is already proven it may acknowledge immediately. Lease expiry without an open epoch remains the USB-level no-op.

## Failure and Stop Semantics

Any `set_pixel` failure abandons the candidate frame and attempts exactly one `clear → refresh`. A refresh failure, or inability to prove the clear refresh, latches `hardware_failure`, rejects all later nonzero intents, and retains ownership needed for safe cleanup. Clear or refresh failure never continues animation.

Stop ordering is exact:

```text
close ordinary intent admission
→ admit stopping through its dedicated cell
→ cancel the tick and pending ordinary intents
→ apply clear
→ refresh the all-zero frame
→ acknowledge the single current matching lifecycle obligation
→ delete/reset the board transport only after all-zero refresh succeeds
```

If all-zero refresh cannot be proved, stop reports tainted and does not claim clean teardown. LED failure may publish a typed LED error only while USB remains open; it never fabricates a control/audio/input terminal or reopens a closed epoch.

## Offline Verification

Required tests cover:

1. GPIO8, 17 pixels, SK6812, GRB, 10 MHz RMT tick, DMA/inversion disabled, and the raw key/strip partitions;
2. startup and every partial-acquisition failure remaining all-off;
3. exactly 17 ordered logical-RGB `set_pixel` calls followed by one refresh;
4. every set/clear/refresh failure, fail-dark attempt, failure latch, and retained cleanup ownership;
5. checked brightness and frame-sum boundaries and arithmetic overflow, with rejection rather than clamping;
6. no nonzero production-driver call without an admitted profile;
7. priority, per-source coalescing, the three safety states, cleanup-proof/ack-obligation separation, and every collision, mailbox overflow, stop-vs-intent, 30 ms wrap-safe ticks, and 32-per-tick interpolation under TSan;
8. exact JSON keys, request correlation/retry, query-vs-applied isolation, unavailable behavior, wrong epoch, and actor-owned 1,000 ms response-before/exact/after-deadline plus queue-race schedules;
9. lifecycle pre-epoch stop, lease expiry, new epoch, stopping supersession, full ordinary mailbox, common-deadline timeout, late acknowledgement, and retained tainted ownership;
10. Swift and firmware shared capability/query/config/state/error JSON goldens;
11. clean production ELF required symbols `led_strip_new_rmt_device`, `led_strip_set_pixel`, `led_strip_refresh`, `led_strip_clear`, and `led_strip_del`;
12. clean production ELF rejection of BLE/NimBLE, Wi-Fi, socket/network, TinyUSB/USB OTG CDC, and USB Audio Class entry points.

## Separate Pure-RAM Calibration Boundary

Physical calibration is not a mode or API of production firmware. Offline harness construction is owned by `firmware-led-ram-harness-001`; connected execution and evidence are owned by `firmware-led-calibration-001`. The design explicitly rejects repeated calibration-image flash staging, OTA selection changes, and assumed rollback.

The harness is an ESP-IDF 5.5.2 ESP32-S3 `CONFIG_APP_BUILD_TYPE_PURE_RAM_APP=y` build loaded in a future separately authorized ROM-download session only through esptool 4.12 `--no-stub load_ram`. This command is documented evidence, not executable authorization. It uses ROM `MEM_BEGIN/MEM_DATA/MEM_END(entrypoint)` and must not call flash write/erase, partition, OTA, NVS, or selection-metadata APIs. The host performs no post-operation reset as part of `load_ram`; recovery reset or power-cycle is a separate authorization and must return through the unchanged flash selection.

### Exact reviewed ELF-to-RAM-image projection

The pinned conversion command is esptool 4.12 `elf2image --chip esp32s3 --use_segments --ram-only-header`; the validator rejects an artifact built by another conversion mode. The reviewed input is an ELF32 little-endian Xtensa executable (`EI_CLASS:1`, `EI_DATA:1`, `e_type:ET_EXEC`, `e_machine:0x005e`) with 32-byte program headers and an entry point that is represented exactly in the image header. It parses every program header, including exact `p_type,p_offset,p_vaddr,p_paddr,p_filesz,p_memsz,p_flags,p_align`, with checked file and address arithmetic.

Each nonempty `PT_LOAD` must satisfy all of:

- `p_paddr == p_vaddr`; Version 1 rejects a physical/virtual alias rather than guessing which address startup uses;
- `0 < p_filesz <= p_memsz`, `p_align` is zero, one, or a power of two, and both `p_offset + p_filesz` and `p_paddr + p_memsz` are checked without wrap;
- `PF_X` is set only for an executable segment wholly inside half-open IRAM `[0x40370000,0x403E0000)`; executable segments reject `PF_W`;
- `PF_X` is clear for a data segment wholly inside half-open DRAM `[0x3FC88000,0x3FD00000)`; data may be read-only or writable;
- classification comes only from the reviewed ELF `p_flags`; an image address heuristic cannot upgrade or reclassify it;
- define `padded_filesz = checked((p_filesz + 3) & ~3)` and require `padded_filesz <= p_memsz`; the projected bytes are exactly `ELF[p_offset ..< p_offset+p_filesz] || zero^(padded_filesz-p_filesz)`, so every esptool-added alignment byte is exactly `0x00`;
- the loaded padded range is `[p_paddr,p_paddr+padded_filesz)` and the remaining startup zero-fill tail is `[p_paddr+padded_filesz,p_paddr+p_memsz)`; padding consumes the first bytes of the ELF zero-fill tail rather than extending the reviewed memory range. A segment whose four-byte padding would exceed `p_memsz`, leave the approved half-open interval, or overlap any other loaded or zero-fill range is rejected.

For every approved range, `start < end`, `end = checked(start + length)`, and admission is exactly `allow_start <= start && end <= allow_end`; an end equal to the half-open upper bound is legal, while a start equal to it is not. Loaded padded ranges and remaining startup zero-fill ranges must be pairwise disjoint across all segments. Empty, BSS-only `PT_LOAD` segments are rejected in Version 1, as are extra `PT_LOAD` segments, IROM/DROM, PSRAM/EXTRAM, RTC memory, overlap, alias, wrap, padding beyond `p_memsz`, and a remaining zero-fill range not covered by reviewed map/startup symbols. The reviewed startup must zero exactly the remaining `[p_paddr+padded_filesz,p_paddr+p_memsz)` tail; it must not overwrite the file-backed bytes or treat the already-zero esptool padding as an independent initialization range.

Esptool 4.12 first applies `ImageSegment.pad_to_alignment(4)` to every accepted nonzero-address projection. The independent projection is therefore `(load_address=p_paddr, padded_bytes=ELF[p_offset..<p_offset+p_filesz] || zero^(padded_filesz-p_filesz))`. `--ram-only-header` sorts these padded projections by load address, then `merge_adjacent_segments()` merges only projections for which `previous.load_address + len(previous.padded_bytes) == next.load_address`, the esptool memory type matches, and checksum inclusion matches. It inserts no bytes for a gap and no additional BSS bytes. All adjacency, overlap, allowlist, image segment-length, merge, and checksum calculations use `padded_filesz`, never raw `p_filesz`.

The independent validator reproduces that exact pinned algorithm from parsed ELF program headers and compares the ordered image segment count, each load address, each complete padded or merged byte string, image entry point, aligned checksum byte, and complete file end byte-for-byte. RAM-only output must contain no hidden flash segment, appended digest, or trailing bytes. A split, missing, extra, reordered, differently merged, differently padded, or byte-mismatched image is rejected. It also runs pinned esptool as a differential offline check and requires complete regenerated-image byte equality and SHA-256 equality with the reviewed image; invoking esptool is generation/validation only and never `load_ram` or transport.

Required known-answer projections include:

```text
p_filesz=3, p_memsz=4, bytes=41 42 43
  → padded_filesz=4, projected bytes=41 42 43 00

next load address == previous load address + 4 and compatible type/checksum
  → merge using 41 42 43 00 as the complete first projection

next load address == previous load address + 3
  → reject overlap with the padded range

p_filesz=3, p_memsz=3
  → reject because padded_filesz > p_memsz

next load address > previous load address + 4
  → preserve two image segments; never fill the gap
```

The entry point must equal both ELF `e_entry` and the image entry point and lie inside one downloaded `PF_X` file-backed IRAM range. The validator also binds and audits the map's load ranges and BSS/zero-fill symbols against the same ELF ranges; map disagreement rejects rather than overriding ELF evidence.

### Canonical offline artifact and external authorization

One separately hashed non-production build embeds exactly one immutable stimulus descriptor and has no command parser, USB LED control, raw/general sender, persistence, log protocol, or capability advertisement. The descriptor is canonical UTF-8 JSON with bytewise-ascending exact keys, no whitespace, lowercase 64-hex SHA-256 strings, decimal integers without leading zeros, no escapes outside JSON-required string escaping, and a final LF:

```json
{"board_profile_sha256":"<64hex>","duration_ms":1,"logical_channel":"red","nonce":"<32 lowercase hex>","raw_pixel":0,"schema":1,"value":1}
```

The exact key set is `board_profile_sha256,duration_ms,logical_channel,nonce,raw_pixel,schema,value`; the displayed serialization order is canonical byte order. `logical_channel` is `red|green|blue`, `raw_pixel` is `0...16`, `value` is exactly `1`, `duration_ms` is `1...250`, and the 128-bit public nonce prevents accidental descriptor reuse. The descriptor contains no authorization hash, device secret, path, or future evidence. Hash names have one meaning only:

```text
descriptor_sha256 = SHA-256(descriptor_bytes)
stimulus_identity = SHA-256("VKLED-STIMULUS-V1\0" || descriptor_bytes)
```

`descriptor_sha256` is a raw complete-byte digest; `stimulus_identity` is the domain-separated identity. The harness embeds the exact descriptor bytes and `stimulus_identity`; neither value is later rewritten. No field named `stimulus_sha256` is used because that name would be ambiguous.

After building the final ELF/image/map/sdkconfig, the offline harness manifest is canonical UTF-8 JSON under the same scalar/ordering/final-LF rules with exact keys:

```json
{"descriptor_sha256":"<64hex>","elf_sha256":"<64hex>","esp_idf":"5.5.2","esptool":"4.12","image_sha256":"<64hex>","map_sha256":"<64hex>","schema":1,"sdkconfig_sha256":"<64hex>","stimulus_identity":"<64hex>","toolchain_sha256":"<64hex>"}
```

Every `*_sha256` field is only a raw complete-file or complete-byte digest:

```text
descriptor_sha256 = SHA-256(descriptor_bytes)
elf_sha256         = SHA-256(complete ELF bytes)
image_sha256       = SHA-256(complete RAM image bytes)
map_sha256         = SHA-256(complete map bytes)
sdkconfig_sha256   = SHA-256(complete sdkconfig bytes)
toolchain_sha256   = SHA-256(the reviewed canonical toolchain-identity bytes)
```

The manifest must contain the separately computed `stimulus_identity` from the descriptor equation above, and both descriptor fields must match the exact embedded descriptor. Manifest hashes are:

```text
harness_manifest_sha256 = SHA-256(manifest_bytes)
harness_manifest_identity = SHA-256("VKLED-HARNESS-MANIFEST-V1\0" || manifest_bytes)
```

Neither value is a field of the manifest or image. A field ending in `_sha256` never contains a domain-separated identity, and a field ending in `_identity` never contains a raw digest. Paths are forbidden. Changing any descriptor, ELF, image, map, sdkconfig, toolchain, ESP-IDF, or esptool field changes the appropriate digest/identity or rejects the artifact. The validator recomputes every equation, the ELF-to-image projection, and complete regenerated image equality before accepting it.

Before any nonzero driver call the harness proves initial all-off, starts the monotonic absolute auto-off deadline, and arms an independent watchdog. It drives only the selected pixel/channel at exact value 1, performs `clear → refresh` no later than the descriptor deadline, then enters an inert loop that never drives LEDs again. Descriptor mismatch or any driver/timer/watchdog error follows fail-dark. A watchdog reset is not itself proof of all-off or production restoration.

After the RAM harness passes independent offline review, every connected stimulus still requires one external authorization record. It is canonical UTF-8 JSON under the same ordering/scalar/final-LF rules with exact keys:

```json
{"authorization_nonce":"<32 lowercase hex>","board_identity_sha256":"<64hex>","device_identity_sha256":"<64hex>","expires_unix":1,"harness_manifest_identity":"<64hex>","harness_manifest_sha256":"<64hex>","production_flash_sha256":"<64hex>","production_selection_sha256":"<64hex>","recovery_evidence_sha256":"<64hex>","schema":1,"scope":"rom_reset+load_ram+one_pulse+recovery_reset+readonly_verify","stimulus_identity":"<64hex>","uses":1}
```

`expires_unix` is a positive canonical UInt64 UTC epoch second, `uses` is exactly `1`, and all other fields are exact non-secret lowercase hashes/enums. The two manifest fields must satisfy both manifest equations above, and `stimulus_identity` must satisfy the descriptor domain-separated equation. Raw evidence fields satisfy `field_sha256 = SHA-256(complete referenced evidence bytes)`. Authorization hashes are:

```text
authorization_record_sha256 = SHA-256(authorization_record_bytes)
authorization_identity = SHA-256("VKLED-CALIBRATION-AUTH-V1\0" || authorization_record_bytes)
```

Only `authorization_identity` is the ledger key; `authorization_record_sha256` is a raw transport/storage integrity digest and is never substituted for it. The identity is never embedded in or fed back into the descriptor, ELF, image, or manifest. The executor recomputes all referenced complete-byte hashes and identities, checks exact device/board/production/recovery evidence and expiry, and atomically marks the authorization identity consumed in its append-only private execution ledger before the first ROM-entry action. Missing ledger admission, a prior attempt (including a failed attempt), or any mismatch rejects; one record can cause at most one execution attempt.

[`fixtures/led-calibration-artifacts-v1.json`](fixtures/led-calibration-artifacts-v1.json) is the single Version 1 known-answer corpus. Its `*_bytes_utf8` values include the required final LF; its expected raw digests and domain-separated identities are normative complete-byte results. Validators must reproduce every value and reject substitution of a raw digest for an identity or an identity for a raw digest.

One authorization covers exactly:

```text
one ROM-entry reset
→ re-identify the exact ESP32-S3 and security state
→ one exact esptool 4.12 --no-stub load_ram image
→ one value-1 bounded pulse
→ one separately authorized recovery reset or power-cycle after release of the download strap
→ one post-recovery read-only verification of the original production image/selection identity and all-off state
```

It authorizes no flash/erase/OTA/otadata/partition/NVS mutation, no second descriptor, no higher value or duration, and no general reset. No step assumes automatic rollback. If reset returns to ROM download, production identity differs, selection metadata cannot be proven unchanged, or all-off cannot be observed, the run fails closed and no next stimulus begins.

The bounded mapping run produces a canonical observation artifact containing only schema version, SHA-256 of the normalized non-secret board/device identity, board-profile hash, RAM-harness artifact hash, ordered descriptor hashes/results, observed physical labels, and reviewer identity/hash. It contains no raw backup, NVS, device secret, path, arbitrary notes, or guessed limits. This mapping artifact alone can never make LED available.

Production admission requires a separately reviewed canonical profile artifact that hash-binds all of:

- the exact mapping observation artifact;
- exact production board-profile and firmware-policy hashes;
- independently reviewed sustained-current evidence and its method hash;
- measured nonzero `max_brightness` and `max_frame_channel_sum`;
- the complete `k1...k4` mapping and strip direction;
- schema version, reviewer authorization hash, and complete canonical artifact SHA-256.

The value-1/250 ms harness does not establish sustained-current safety. Until a separate current-measurement method is documented, authorized, executed, and independently reviewed, profile admission fails closed and production remains `available:false`. Production accepts only a compiled-in profile whose complete canonical hash is on the reviewed build allowlist and whose board/firmware identities match exactly; there is no USB upload, runtime mutation, NVS write, or fallback profile.
