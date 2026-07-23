---
id: firmware-audio-001
scope: replacement firmware PDM, pinned AFE, and Opus pipeline
status: done
depends-on: [firmware-hardware-001, firmware-usb-001, firmware-audio-deps-001]
---

## Objective

Implement the evidence-backed I2S0 PDM → pinned ESP-SR AFE → pinned ESP32 Opus pipeline and bounded USB AudioFrame lifecycle.

## Context

- `docs/INDEX.md`
- `docs/product/hardware.md`
- `docs/product/input-audio.md`
- `docs/product/usb-protocol.md`
- `docs/firmware/README.md`

## Path

- `firmware/components/vk_audio/`
- `firmware/main/`
- `firmware/tests/`
- `docs/product/input-audio.md`

## Contract

- Use ESP-IDF 5.5.2 named I2S APIs: I2S0 PDM RX master, 16 kHz, 16-bit stereo/both slots, GPIO41 clock/GPIO40 data, default clock, MCLK 256, down-sample 8S, BCLK divider 8, four DMA descriptors and 512 frames.
- Treat physical microphone count/slot mapping as unknown until connected measurement.
- Use only dependency-gate-approved ESP-SR named fields: `MM`, no playback/reference/model list, runtime feed/fetch chunk/channel sizes. Raw ABI offsets are forbidden.
- Use the pinned ESP32-target Opus component at 16 kHz mono VOIP, VBR 16 kbps, DTX off, complexity 4, voice signal, 960 samples/60 ms, maximum 220 bytes. Host arm64 Opus is never linked into firmware.
- Allocate/check the 64,000-byte ring and all bounded buffers/tasks. Version 1 applies unity post-AFE gain and makes no vendor-gain parity claim; no `voice_gain` command is exposed.
- Session IDs are monotonic and skip zero; each start rebuilds AFE/Opus and resets sequence. Sequence zero carries first. A data sequence increments only after the typed frame is successfully handed to the bounded current-epoch USB queue; the USB service has no physical-write completion acknowledgement. Normal stop emits exactly one empty final frame at the next sequence.
- Partial start/transport failure unwinds in reverse order. Worker join is bounded around 1,500 ms before explicit straggler cleanup. No raw PCM/audio log or retained recording exists on device.

## Verification

- Native tests cover session/sequence/flags/EOS, chunk accumulation, start failure rollback, concurrent/repeated stop, 1,500 ms timeout, encoder negative/zero/>220 rejection, TX overflow/failure, and exactly-once finalization.
- Component tests inject I2S/AFE/Opus/USB/task/allocator adapters at every acquisition and runtime failure.
- Clean ESP-IDF build proves exact dependency locks and named ABI; final ELF has no BLE/network/UAC path.
- Connected acceptance measures slot/processed RMS, DC/clipping, packet histogram, resource high-water marks, then completes key-controlled real-speech Ogg decode without retaining raw audio.

## Implementation Evidence (Independent Review 1 Blocked)

- `vk_audio` now owns the production I2S0 PDM RX, named ESP-SR `MM` AFE, single-owner micro-opus encoder, 64,000-byte bounded microphone ring, 960-sample accumulator, and typed current-epoch USB AudioFrame handoff.
- `vk_audio_init()` creates only idle synchronization state. `vk_audio_start()` and `vk_audio_stop()` are typed controls reserved for `vk_input`; startup does not auto-record.
- The production USB façade exposes only `vk_usb_current_epoch()` and typed `vk_usb_send_audio()`. The private service pointer, raw frames, raw JSON, and arbitrary frame types remain inaccessible.
- Runtime ownership uses serialized public control, reverse-order cleanup, a 1,500 ms join, and taint-with-retained-context on unproven worker termination or cleanup failure.
- Native pure/injected tests cover session zero skipping/wrap, arbitrary chunk accumulation, first/sequence/EOS, encoder negative/zero/oversize rejection, queue handoff failure without sequence advancement, acquisition failure rollback, repeated stop, runtime/cleanup errors, and join timeout without premature release. Native audio tests also pass ASan and UBSan.
- The dependency checker now compares all saved notices byte-for-byte with the pinned managed sources. The production ELF checker requires I2S/AFE/Opus/built-in USB Serial/JTAG symbols and rejects selected BLE, Wi-Fi, network, TinyUSB, and UAC entry points.
- Offline native tests, 10 contract tests, 122 Swift tests, and an ESP-IDF 5.5.2 ESP32-S3 build pass. No device I/O occurred.
- Physical PDM slot mapping, processed RMS, acoustic quality, 60 ms real-time deadline, runtime memory/stack high-water, sustained USB queue behavior, key-controlled connected capture, real-speech decode, and vendor gain parity remain unverified hardware acceptance items.

## Independent Review 1 Findings

- Conclusion: blocked.
- The production `vk_usb_current_epoch()` / `vk_usb_send_audio()` façade does not pin the private service allocation against concurrent USB teardown, so send/query can race `release_context()` and dereference freed service state.
- Production audio uses `volatile`/unlocked shared state across the worker and controller/getters; pure pipeline/runtime TSan does not compile or prove the production FreeRTOS ownership path.
- Required injected tests do not execute each production I2S/AFE/Opus/USB/task/allocator acquisition, runtime, cleanup, partial-read, malformed-fetch, overflow, and unwind boundary.
- Task remains `in-progress` until those production ownership and verification blockers are fixed and independently re-reviewed.

## Review 1 Fix Evidence (Pending Independent Review 2)

- `vk_usb_facade` now owns admission closure and counted leases around every production typed façade call. `vk_usb_stop()` closes admission and waits boundedly for all leases before runtime teardown; timeout taints and retains the service context rather than freeing it. Deterministic pthread/TSan tests cover borrow-versus-close, post-close rejection, timeout retention, and clean closure.
- Production audio replaced `volatile` with C11 acquire/release atomics for the worker stop token. Public start/stop/getters serialize through one retained mutex; deinit closes API admission without racing mutex destruction, and tainted contexts remain retained.
- `vk_audio_control_api()` is the typed future `vk_input` composition boundary and keeps start/stop linked without unreachable auto-capture code. `app_main` initializes audio idle and obtains this table without invoking capture.
- `vk_audio_backend` is a production-shaped injected acquisition/capture/release boundary for I2S, AFE, Opus, memory, and typed USB sends. Native tests inject each acquisition failure, partial reads, malformed AFE fetch results, Opus return bounds through the shared pipeline, exact reverse cleanup order, and bounded ring behavior. Existing runtime tests retain join-timeout/no-release and repeated stop coverage.
- Offline regression after fixes: native board/USB/audio tests pass; USB façade TSan and audio backend UBSan pass; contract tests pass `10` with one documented skip; Swift strict-concurrency build and `122 tests / 15 suites` pass; ESP-IDF 5.5.2 build and production audio ELF checker pass. App size is `1,152,128` bytes and SHA-256 is `2bf47b7eea89d9c76e0f2cbb4db1292d71dc418c53c5d2176121367d491a05f1`.
- This evidence remains offline only and does not establish PDM slot mapping, acoustic quality, real-time deadline, runtime high-water, sustained USB E2E, real speech, or vendor gain parity.

## Independent Review 2 Findings

- Conclusion: blocked.
- The façade lease closes the obvious unpinned service-pointer race by inspection, and production audio no longer uses `volatile` as synchronization, but native/TSan tests still do not compile the production `vk_audio.c` owner/public API or `vk_usb.c` wrapper/teardown composition.
- Partial production AFE and Opus construction can allocate adapter-owned resources before the shared backend handle is published; failure then bypasses destruction and leaks those resources.
- Production backend failure injection remains incomplete for I2S read/ring boundaries, feed/fetch contracts, per-control Opus failures, typed USB errors, cleanup failures, task/semaphore allocation, and exact reverse ownership.
- Task remains `in-progress` pending fixes and a fresh independent review.

## Review 2 Fix Evidence (Pending Independent Review 3)

- The native production harness now compiles the exact `vk_audio.c` public API and worker composition with a pthread-backed FreeRTOS adapter. TSan schedules public start/stop/getters, repeated stop, worker startup/runtime/cleanup, AFE/Opus startup failure collection, and deinit while retaining the production mutex/runtime state.
- Production AFE construction now unwinds config/data on every interface, create, chunk, channel, and sample-rate failure before returning. Opus construction destroys the encoder when any of the five production control calls fails. Production tests inject each named validation/control point and assert exact destruction.
- The AFE fetch chunk is retained in `vk_audio_backend_t`; every nonempty fetch must exactly match the queried mono chunk byte count. Backend ASan/UBSan tests retain acquisition rollback, I2S read/timeout/malformed count, feed/fetch, ring, encoder, typed-send, and reverse cleanup coverage.
- The native USB production harness compiles the exact `vk_usb.c` wrapper with `vk_usb_runtime`, owner, and façade layers. TSan schedules current-epoch/send against stop, wrong-epoch/closed admission, clean teardown, and restart without exposing a service pointer or raw sender.
- Offline regression passed native board/USB/audio tests, production audio and USB TSan, backend ASan/UBSan, and contract tests (`13`, one documented skip). A clean ESP-IDF 5.5.2 build passed `1962/1962`; the production ELF checker confirmed required I2S/AFE/Opus/USB Serial/JTAG symbols and absence of selected BLE, Wi-Fi, network, TinyUSB, and UAC entry points. The app is `1,152,672` bytes (`0x1196a0`) with SHA-256 `172b6778972674d9a54fc0522f6df3d38380839279c9ccd6a7a2a39f8836dde9`.
- This evidence is offline only. It does not establish PDM slot mapping, acoustic quality, 60 ms deadline, runtime memory/stack high-water, sustained physical USB behavior, key-controlled capture, real-speech Ogg decode, or vendor gain parity. Task status remains `in-progress` until independent Review 3.

## Independent Review 3 Findings

- Conclusion: blocked.
- The production files enter native tests, but the old FreeRTOS adapters ignored finite semaphore timeouts, so startup/stop timeout retention and late completion were unreachable.
- Production AFE member/config/oversize and allocation injection was incomplete; backend malformed I2S/ring/AFE/USB and cleanup-retention matrices were incomplete.
- The USB production harness compiled `vk_usb.c` but replaced `vk_usb_service.c` with a simplified stub, leaving terminal and teardown semantics unproved.

## Review 3 Fix Evidence (Pending Independent Review 4)

- The production audio pthread adapter now implements finite timed semaphore waits and task-object reclamation. Separate deterministic processes execute normal/concurrent getter lifecycle, startup timeout with retained tainted session and late worker completion, and stop timeout with retained tainted session and late worker completion.
- Production AFE injection now covers config allocation, null interface, each of the eight required interface members independently, create failure, zero and oversized feed/fetch chunks, wrong feed/fetch channels, and wrong sample rate. Session, both semaphore, task, ring, and feed allocations are activated through production allocation indices; all five Opus controls retain per-point destruction assertions.
- The shared production-used backend matrix now executes timeout/error/odd/oversized/zero/partial reads, repeated drain, exact fetch mismatch, feed failure, ring wrap/full, typed wrong-epoch/overflow/generic sends, and I2S disable/destroy failures. Cleanup tests assert retained ownership after failure and successful retry rather than claiming false release.
- The production USB harness now compiles the exact `vk_usb_service.c` together with `vk_usb.c`, runtime, owner, and façade. It executes the real parser/epoch establishment, queue overflow/audio terminal rejection, production send versus stop, finite stop timeout retention, cleanup-failure taint/restart rejection, and clean teardown. Only the USB Serial/JTAG driver, timer, MAC, FreeRTOS, and transport edges are adapted.
- Offline verification passed the native suite; production audio/USB TSan schedules; backend ASan+UBSan; 16 contract tests with one documented skip; and the pinned audio dependency check. A clean ESP-IDF 5.5.2 build and ELF checker passed; the app is `1,115,776` bytes with SHA-256 `1f41efb347378c0f570e317da2cbaeb79d0c84b67c8ee6e1e923876f23dec0dd`, valid checksum `0xa2`, and valid appended hash `b8ac7a6a6cea48e79ef0148a303ae69105c487976a64344ee01a05c49558f034`.
- This is offline evidence only. PDM slot mapping, acoustic quality, the 60 ms deadline, runtime memory/stack high-water, sustained physical USB, key-controlled capture, real-speech Ogg decode, and vendor gain parity remain unverified. Task status remains `in-progress` pending independent Review 4.

## Independent Review 4 Findings

- Conclusion: blocked.
- The ninth required AFE interface-member null schedule (`get_samp_rate`) was missing.
- Timeout tests proved sticky taint but did not provide a production-used late-session collection/destruction path or an independent asynchronous capture-failure schedule.
- The production USB harness still lacked duplicate epoch replacement and deterministic before-TX-commit cancellation/write-order schedules.
- Public owner TSan coverage still omitted concurrent/repeated starts and start/stop/deinit/getter/runtime-failure combinations.

## Review 4 Fix Evidence (Pending Independent Review 5)

- `vk_audio_runtime` now records retained ownership on startup/stop timeout and cleanup failure. `vk_audio_runtime_collect()` waits boundedly for proven worker completion, checks cleanup, destroys the retained session/semaphores exactly once, and intentionally preserves sticky taint so a failed composition cannot restart before reboot. `vk_audio_deinit()` owns this explicit late-collection path.
- Production audio native tests independently inject all nine required AFE interface members, including `get_samp_rate`, plus the existing config/create/query/value/control/allocation matrix. Separate TSan processes cover startup-timeout and stop-timeout late collection, asynchronous I2S runtime failure and collection, concurrent/repeated starts, stop, deinit, and getters through the exact public API.
- The production USB harness still compiles the exact service/runtime/owner/facade composition. It now establishes a duplicate transport epoch, races `current_epoch` with stop, and uses the production before-TX-commit hook to force an audio overflow terminal after dequeue but before commit. The captured write sequence proves that the terminal is the sole committed frame and the cancelled audio value is not written.
- Offline verification passed the full native suite, production audio and USB TSan schedules, 19 contract tests with one documented skip, the pinned dependency check, and a clean ESP-IDF 5.5.2 build (`1956/1956`). The production ELF checker confirmed I2S/AFE/Opus/USB Serial/JTAG symbols and rejected selected BLE, Wi-Fi, network, TinyUSB, and UAC entry points. The app is `1,116,032` bytes (`0x110780`) with SHA-256 `095dce38511ab47be9de3b40bcd1922fce3100becc2b58d6a883ced570abf1a7`, valid checksum `0x07`, and valid appended hash `08ddc7a81cb042322f164951f3c13844eb027fbe784cc5a5b96c11fcce7ec067`.
- ASan+UBSan execution was attempted independently; this macOS AddressSanitizer runtime reports that leak detection is unsupported. The normal backend ASan/UBSan runner remains part of the native suite, but no unsupported leak-detector result is claimed.
- This remains offline evidence only. Physical PDM slot mapping, acoustics, 60 ms real-time behavior, device stack/heap high-water, sustained physical USB, key-controlled capture, real-speech Ogg decode, and vendor gain parity remain unverified. Task status stays `in-progress` pending independent Review 5.


## Independent Review 5 Findings

- Conclusion: blocked.
- Timeout late collection was closed, but production cleanup failures had no retry path after the worker's first backend release attempt.
- Public-owner concurrency still lacked deterministic idle concurrent-start and start-versus-deinit schedules with session ownership assertions.
- Production USB still lacked a dequeued-before-commit stop cancellation schedule, and the stop-timeout schedule did not prove that the poll owner had entered the blocked read.

## Review 5 Fix Evidence (Independent Review 8 Passed)

- `vk_audio_runtime_collect()` now calls a production-used `retry_cleanup` operation after the worker has published `stopped`. The production adapter retries `vk_audio_backend_release()` while preserving backend ownership on failure; session semaphores are destroyed only after cleanup succeeds. Sticky taint remains unchanged, and repeated collection cannot double-release.
- Production tests inject transient and persistent I2S disable and destroy failures. They assert the first stop and first collection fail closed, retained semaphore ownership remains exact, a later collection succeeds after the injected fault clears, I2S operations execute the expected number of times, and all per-session semaphores are released exactly once. Runtime unit tests independently cover persistent retry, later success, release-once, and post-release rejection.
- Public-owner concurrency now uses the real blocked AFE construction boundary as a deterministic admission gate. Two idle starts prove exactly one nonzero session ID is admitted, the loser does not receive an ID, and a later start-versus-deinit schedule proves the admitted session has a distinct nonzero ID and is stopped and collected before deinit returns.
- The production USB stop request now closes service admission before notifying the poll owner. Deterministic before-TX-commit gates prove that a dequeued old-epoch AudioFrame is cancelled by both stop and epoch replacement. Stop yields zero post-close writes; replacement rejects the old epoch and then emits exactly one new-epoch AudioFrame. The finite timeout schedule waits for `read_entered` before calling stop, proving the poll-owner block placement.
- Offline verification passed the full native suite, production audio and USB TSan schedules, 20 contract tests with one documented skip, and the pinned dependency check. A clean ESP-IDF 5.5.2 build passed all `1956/1956` steps; the production ELF checker found the required I2S/AFE/Opus/USB Serial/JTAG symbols and no selected BLE, Wi-Fi, network, TinyUSB, or UAC entry points. The application is `1,116,160` bytes (`0x110800`) with SHA-256 `e9c051105fbb33eb05e411a40e5ff12d4593fac8d58baa25257048c49035ce65`, valid checksum `0x29`, and valid appended hash `3e4137a7b217341df332bbdd5706745671a23e0e3bdd7cc90f78cdbc6f29b005`. No device I/O was performed.
- Physical PDM slot mapping, acoustics, 60 ms real-time behavior, device stack/heap high-water, sustained physical USB, key-controlled capture, real-speech Ogg decode, and vendor gain parity remain unverified.

## Independent Review 8

- Conclusion: pass for the offline implementation gate.
- The review independently revalidated complete per-resource cleanup accounting, both deterministic public start/stop linearizations, and production service-domain stop/epoch replacement before TX commit.
- Connected physical audio acceptance remains separately gated and was not performed.
