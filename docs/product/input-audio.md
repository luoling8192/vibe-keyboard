# Input and Audio Contract

Status: Vendor input/electrical model and Opus framing/encoding/EOS semantics verified; physical four-key ordering remains under connected capture

## Four-Key Identity

The device exposes four canonical logical keys:

```text
k1  k2  k3  k4
```

Vendor aliases normalize as:

```text
primary   → k4
secondary → k1
k1...k4   → unchanged
other     → invalid
```

The physical left-to-right ordering is not yet proven. UI labels must use canonical IDs until a real key-event capture establishes physical ordering.

## Device Events

Firmware strings prove these vendor JSON state-event templates:

```json
{"event":"button_down","button":"kN","session_id":0}
{"event":"button_up","button":"kN","duration_ms":0,"session_id":0}
{"event":"button_click","button":"kN","duration_ms":0,"session_id":0}
```

Those captured templates are vendor-compatibility evidence, not the replacement encoder schema. The replacement schemas are exact:

```text
button_down without an audio attempt: {"event":"button_down","button":"kN"}
button_down with an audio attempt:    {"event":"button_down","button":"kN","session_id":S}
button_up without an audio attempt:   {"event":"button_up","button":"kN","duration_ms":D}
button_up with an audio attempt:      {"event":"button_up","button":"kN","duration_ms":D,"session_id":S}
button_click without an audio attempt:{"event":"button_click","button":"kN","duration_ms":D}
button_click with an audio attempt:   {"event":"button_click","button":"kN","duration_ms":D,"session_id":S}
```

For replacement events, `button` is exactly `k1|k2|k3|k4`, `S` is an integer in `1...4294967295`, and `D` is an integer in `0...4294967295` measured from the accepted debounced down to the accepted debounced release with unsigned wrap-safe arithmetic. `button_down` forbids `duration_ms`; `button_up` and `button_click` require the same `duration_ms`. `session_id` is the only optional key and is omitted when no audio attempt is associated; zero is forbidden. Missing required keys, unknown or extra keys, Boolean values, non-integers, and out-of-range integers reject the complete event. A vendor-compatibility decoder may separately accept captured vendor `session_id:0` as absent correlation, but the replacement encoder never emits zero and replacement strict validation rejects it. The client treats unknown button identifiers as protocol errors and never executes actions sourced from them.

Gesture derivation must have exactly one owner:

- `button_click` is authoritative for a single click when firmware emits it.
- Double-click may be derived by the host only if live evidence proves firmware does not emit a dedicated double event.
- Long press may be derived from a matched down/up duration only if firmware does not supply a dedicated event. The matched host monotonic interval is authoritative; firmware-reported duration remains diagnostic and cannot change classification.
- A single physical interaction must trigger a configured action at most once.

The vendor button component scans every 5 ms and accepts a level after two consecutive samples (approximately 10 ms debounce). Its library defaults are 180 ms short and 1,500 ms long, but the application registers only press-down and press-up callbacks and emits release-derived click events. There is no proven dedicated firmware long-press event. Host action thresholds remain user/client policy and require connected usability capture rather than blindly inheriting component defaults.

For captured vendor events, `session_id` identifies the vendor audio session observed on that button interaction. Replacement firmware uses the same single nonzero `UInt32` audio-attempt identity on correlated button events and AudioFrames; it creates no separate button-session namespace. On a successful release, that identity becomes the running audio session. On a release failure, the already accepted down and its later up/click retain the same identity even though capture never started. Therefore button `session_id` means correlation with one audio attempt, not proof that recording is active. Firmware omits the field when no audio attempt is associated. The host creates a recording only after the first valid AudioFrame for that identity; button events alone never create or finalize a recording.

## Mapping Model

Each canonical key stores mappings for:

- single click;
- double click;
- long press.

Supported host actions:

- `none`;
- `voiceInput`;
- `sendEnter`;
- `systemCopy`;
- `interruptControlC`;
- `wakeApplication`;
- `pasteText`;
- `customShortcut`;
- `customCommand`;
- `launchApplication`;
- `screenMode`;
- `petInteraction`.

Vendor-compatible actions use these raw values where persisted/imported:

```text
none
voice_input
send_enter
system_copy
interrupt_ctrl_c
wake_claude
paste_text
custom_shortcut
custom_command
```

A binding is a typed enum with action-specific associated data. Missing required associated values are validation errors; for example, `pasteText` requires non-empty text and `customShortcut` requires at least one normalized key.

Shell commands are explicit user-authored configuration. Device payloads, widget text, imported asset metadata, and transcription text can never become shell source.

## Default Mapping

The vendor defaults are retained as an importable initial profile:

| Key | Single | Double | Long |
|---|---|---|---|
| `k1` | wake application | none | none |
| `k2` | paste `继续` | none | none |
| `k3` | Control-C | none | none |
| `k4` | voice input | Enter | none |

These logical defaults do not claim a physical left-to-right order.

## Device Configuration

Verified host-to-device JSON commands:

```json
{"event":"interaction_mode","mode":"hold_to_talk"}
{"event":"interaction_mode","mode":"click_to_talk"}
{"event":"voice_key","key":"kN"}
{"event":"voice_gain","gain":0}
```

The valid numeric range for `gain` remains unverified. The client must not expose or send a guessed range.

### Replacement Input Configuration

Replacement firmware version 1 starts every USB protocol epoch with `interaction_mode:"hold_to_talk"` and canonical `voice_key:"k4"`. `k4` is a logical default only and does not claim a physical front-panel position. The macOS client sends its validated mode/key configuration after each replacement handshake. These values are RAM/current-epoch state: disconnect, lease expiry, reboot, or a new `transport` epoch resets them to the defaults and performs no NVS write.

`interaction_mode` accepts exactly one `mode` field from `hold_to_talk|click_to_talk`; `voice_key` accepts exactly one canonical `key` from `k1|k2|k3|k4`. Unknown/extra fields reject. Configuration changes while audio is active are rejected with `vk_error` operation `input`, code `busy`; they never stop or retarget a recording. No replacement handler accepts `voice_gain`.

The input scanner runs every 5 ms and accepts a transition after two consecutive equal samples. A key must first be observed debounced released before it is armed. Initial boot, new epoch, lease expiry, or disconnect clears pending presses and queued events; a key held across that boundary remains suppressed until debounced release, and firmware emits no synthetic up/click.

The 5 ms scanner task owns only GPIO sampling/debounce and writes semantic transitions to one fixed 32-item FIFO. It never calls USB or audio and never waits for transfer, allocation, pipeline startup, or task join. A separate input owner drains that FIFO, owns epoch-local mode/key/association state, and performs nonblocking typed expected-epoch USB handoff. A third audio-control task owns one fixed four-item ordinary command mailbox, one fixed four-item result mailbox, and one separately reserved single-item lifecycle-abort slot; it alone may call bounded synchronous audio prepare/release/cancel/stop/abort APIs. The reserved slot is not available to ordinary voice transitions and guarantees allocation-free abort admission even when the ordinary mailbox is full. Scanner cadence therefore continues while audio control takes up to the documented 1,500 ms worker-join bound.

USB button handoff is exact and linearized with epoch/queue state:

```c
typedef enum {
    VK_USB_HANDOFF_ACCEPTED,
    VK_USB_HANDOFF_RETRY,
    VK_USB_HANDOFF_EPOCH_CLOSED,
    VK_USB_HANDOFF_OVERFLOW,
} vk_usb_handoff_result_t;
```

`RETRY` means the bounded USB value queue is temporarily full and did not mutate the epoch; the input FIFO retains exact order. `EPOCH_CLOSED` clears old-epoch FIFO, presses, audio commands/results, and association. `OVERFLOW` means a USB-owned reserved terminal already closed that epoch. Local FIFO or either audio-mailbox overflow calls the typed `vk_usb_fail_epoch(expected_epoch, VK_USB_SESSION_ERROR_INPUT_QUEUE_OVERFLOW)`: in the USB synchronization domain it closes old-epoch admission, clears queued button/audio values, and reserves exactly `{"event":"vk_error","operation":"input","code":"input_queue_overflow"}`. That terminal bypasses the full input FIFO; failure to write it does not reopen the epoch. No path overwrites or silently drops an older event.

USB exposes one asynchronous typed lifecycle registration before service start. Each request is exactly `{kind:new_epoch|lease_expired|stopping, token:UInt32, old_epoch:UInt32, proposed_epoch:UInt32, lifecycle_generation:UInt32}`. `token` and `lifecycle_generation` are nonzero and allocated by the USB owner, skipping zero on wrap. Epoch zero is reserved only as the lifecycle sentinel for “no epoch”. The first `new_epoch` request requires `{old_epoch:0,proposed_epoch:nonzero}`; later `new_epoch` requests require two distinct nonzero epochs. `lease_expired` requires `{old_epoch:nonzero,proposed_epoch:0}` and cannot be created when no epoch is open. Normal stopping requires `{old_epoch:nonzero,proposed_epoch:0}`. Stopping a started composition before its first successful transport is exactly `{kind:stopping,old_epoch:0,proposed_epoch:0}`. `old_epoch == 0` is legal only for first new-epoch and pre-epoch stopping; only pre-epoch stopping may have both epoch fields zero. A zero `proposed_epoch` remains required for normal lease expiry and stopping when `old_epoch` is nonzero. No audio command, result, button value, or open protocol state may use epoch zero.

The USB owner is the sole deadline owner. It closes old-epoch producer admission under its state lock, records the request token and starts one monotonic absolute deadline exactly 3,250 ms later, releases the lock, and invokes the registered `begin(request, acknowledgement_sink)` callback exactly once. `begin` is nonblocking and returns exactly `ACCEPTED|TAINTED`; `ACCEPTED` means only that cleanup has started and never means quiescence. The sink is a dedicated single-item USB-owned acknowledgement cell accepting exactly one `{token,lifecycle_generation,result:QUIESCENT|TAINTED}`; it is not a USB façade and exposes no send, query, fail, epoch, queue, encoder, or service operation. The callback must not hold a USB lock or synchronously call any USB façade. A matching acknowledgement may be published asynchronously through that cell. A duplicate, mismatched, or late acknowledgement is discarded and cannot mutate USB state. Acknowledgement and timeout linearize under the same USB-owner state lock using the captured monotonic publication time: a matching acknowledgement is accepted only when `ack_linearization_time < absolute_deadline`. At `ack_linearization_time == absolute_deadline` or later, timeout wins: the owner invalidates the cell and taints before considering the acknowledgement. If the timeout wake and acknowledgement publication race, whichever acquires the lock first still applies that same timestamp comparison; lock acquisition order cannot make an exact-deadline acknowledgement valid. Tests schedule one acknowledgement immediately before, exactly at, and immediately after the deadline.

On callback begin, input atomically closes ordinary audio-command and ordinary result-publication admission and clears queued ordinary commands/results as stale. For nonzero `old_epoch` it places `abort(old_epoch,lifecycle_generation,0)` in the separately reserved lifecycle-abort slot. A worker denied ordinary result publication records only tuple-local cleanup state for the abort owner; it cannot enter USB or satisfy lifecycle completion. The slot cannot be occupied by an ordinary command; if it is unexpectedly occupied by a different tuple, or audio control cannot accept the request immediately, `begin` returns `TAINTED` without waiting or entering USB. An ordinary prepare, release, cancel, or stop call already executing when admission closes is not assumed cancellable and retains at most its existing 1,500 ms bound. Audio control starts the reserved abort immediately after that call returns, never starts another ordinary call, and gives abort/join its unchanged maximum 1,500 ms. It then has 250 ms for tuple cleanup and acknowledgement publication. These are consecutive portions of the same USB-owned absolute 3,250 ms deadline: at most 1,500 ms current-call remainder + 1,500 ms abort/join + 250 ms handoff/cleanup. Each step receives only the remaining time to the absolute deadline; no retry restarts or extends any portion. Audio control cancels every queued/current tuple in `old_epoch` and publishes lifecycle completion directly to the acknowledgement sink rather than through the ordinary result mailbox. Thus a full ordinary command or result mailbox cannot prevent abort admission or completion. For either legal request with `old_epoch:0`—the first `new_epoch` or pre-epoch `stopping`—no audio abort is submitted because no protocol epoch can own an audio worker. Input still closes local admission, clears scanner/FIFO/mailbox/association state that was never published, clears any empty reserved slot, and publishes a matching acknowledgement through the same token/generation cell before the same deadline. Pre-epoch stopping never exposes an epoch after `QUIESCENT`; it only permits bounded composition teardown. A lease-expiry request while no epoch is open is impossible and is handled internally as a no-op without allocating a lifecycle token.

If USB stopping supersedes a lifecycle request for the same old epoch, input atomically retargets the one reserved abort's eventual quiescence proof to the new stopping token/generation and acknowledgement sink without submitting a second abort; the old sink is invalid. A different-epoch slot, failed retarget, or uncertain state returns `TAINTED`. If old cleanup already proved quiescence, input revalidates that retained quiescent state before acknowledging the fresh stopping tuple. Input otherwise publishes `QUIESCENT` only after audio control has completed bounded abort/join, proved that the old worker can no longer submit an AudioFrame or publish a non-stale result, released or safely retained every old resource according to the audio taint contract, cleared the reserved slot, both ordinary mailboxes, FIFO, presses, and association, and reset mode/key to defaults. Abort sends no EOS through the closed epoch. Any current-call overrun, abort failure/timeout, exhausted remaining deadline, or resource uncertainty publishes `TAINTED` if time remains; otherwise USB reaches the same tainted result at its absolute deadline. The legal worst case is exactly 1,500 ms current-call remainder, 1,500 ms abort/join, and 250 ms handoff/cleanup inside one 3,250 ms total; none is an extension. A synchronous `TAINTED`, matching asynchronous `TAINTED`, or absence of matching `QUIESCENT` at the 3,250 ms absolute deadline permanently taints/closes the USB composition. USB invalidates the acknowledgement cell, never exposes the proposed epoch, and retains any unproved-owned context. Only a matching in-deadline `QUIESCENT` permits new-epoch publication or clean stop completion.

Canonical simultaneous ordering is `k1` through `k4`; each release emits `button_up` immediately followed by `button_click` for that key. The input owner may stall FIFO handoff behind one voice-transition barrier but the scanner continues sampling. No later voice or non-voice transition may overtake the retained FIFO head. If the bounded FIFO fills while audio control or USB backpressure is pending, the fail-epoch rule above applies.

### Replacement Voice-Key Ordering

`vk_input` owns epoch-local interaction mode, voice key, pending semantic transitions, command generation, and the button-to-audio-attempt association. `vk_audio` alone owns the real prepared/running capture session. There is no second `vk_state` owner for these version-1 values. Button `session_id` is the nonzero audio-attempt identity defined above, assigned immediately before the event's first USB handoff attempt and immutable across retry. Successful handoff to the common typed USB queue defines event order.

Every audio-control command and result carries the exact correlation tuple `{epoch:UInt32, generation:UInt32, session:UInt32}`. Ordinary epochs and all generations are nonzero. The input owner allocates a new monotonically advancing generation for every ordinary command, skipping zero on wrap; the USB lifecycle owner supplies the abort generation. Commands contain exactly `kind:prepare|release|cancel_prepared|stop|abort` plus the tuple. Results contain exactly `kind:prepared|running|stopped|cancelled|runtime_failed|failed|tainted` plus the tuple; `failed` additionally contains exactly `command:prepare|release|cancel_prepared|stop|abort`. There are no optional or extra fields. `prepare` is the only ordinary command with `session:0`; its `prepared` result introduces a previously unused nonzero audio-attempt identity. `release`, `cancel_prepared`, and `stop` repeat it. `abort` uses `session:0` to cover every session in its exact old epoch. No field is implied.

The exact audio state machine is:

| Source | Command tuple | Legal result tuple | Effect after consumption |
|---|---|---|---|
| `idle` | `prepare(E,G,0)` | `prepared(E,G,S≠0)` | enter `prepared(S)` and retain the start barrier |
| `idle` | `prepare(E,G,0)` | `failed(E,G,0,prepare)` | remain idle; emit `audio_start_failed`; resolve with the documented single sessionless interaction |
| `idle` | `prepare(E,G,0)` | `tainted(E,G,0)` | fail epoch; retain uncertain ownership; admit no later FIFO value |
| `prepared(S)` | `release(E,G,S)` | `running(E,G,S)` | enter `running(S,runtime_generation=G)`; permit AudioFrames; resolve start barrier |
| `prepared(S)` | `release(E,G,S)` | `failed(E,G,S,release)` | remain prepared; emit `audio_start_failed`; submit cancel as the same unresolved barrier |
| `prepared(S)` | `release(E,G,S)` | `tainted(E,G,S)` | fail epoch; retain uncertain ownership; admit no later value |
| `prepared(S)` | `cancel_prepared(E,G,S)` | `cancelled(E,G,S)` | prove no AudioFrame/EOS; enter idle; hold mode keeps `S` only until the already accepted down's up/click are accepted, while click mode clears it immediately; resolve barrier |
| `prepared(S)` | `cancel_prepared(E,G,S)` | `failed(E,G,S,cancel_prepared)` or `tainted(E,G,S)` | fail epoch, retain context, keep barrier closed, discard every not-yet-accepted FIFO value including queued up/click |
| `running(S,R)` | `stop(E,G,S)` | `stopped(E,G,S)` | EOS follows accepted click; enter idle; invalidate `R`; clear association; resolve barrier |
| `running(S,R)` | `stop(E,G,S)` | `failed(E,G,S,stop)` | legal only after proven quiescence; enter idle; invalidate `R`; clear association; emit `audio_stop_failed`; resolve the stop barrier; remove every not-yet-accepted transition for the configured voice key from the FIFO while preserving the relative order of all remaining non-voice-key transitions; latch voice disabled for the epoch |
| `running(S,R)` | `stop(E,G,S)` | `tainted(E,G,S)` | fail epoch and retain uncertain ownership |
| `running(S,R)` with or without pending stop | no command | `runtime_failed(E,R,S)` | accept asynchronously only for exact retained runtime tuple; audio is quiescent without EOS; invalidate `R`, clear association, make pending stop stale, emit `audio_runtime_failed`, resolve any stop barrier; remove every not-yet-accepted transition for the configured voice key from the FIFO while preserving the relative order of all remaining non-voice-key transitions; latch voice disabled for the epoch |
| old epoch `E` after admission close | reserved `abort(E,L,0)` | `cancelled(E,L,0)` | prove every old worker quiescent without EOS; clear old state; lifecycle `QUIESCENT` |
| same | reserved `abort(E,L,0)` | `failed(E,L,0,abort)` or `tainted(E,L,0)` | retain uncertain ownership; lifecycle `TAINTED`; never publish new epoch |
| first `new_epoch(0→E)` or pre-epoch `stopping(0→0)` | no command | no result | close local admission and clear unpublished scanner/FIFO/mailbox/association state; matching lifecycle `QUIESCENT`; first new epoch may publish `E`, while stopping only completes teardown and never publishes an epoch |

`E`, `G`, `L`, `S`, and `R` are exact values. Each command permits only its listed alternatives. `running(E,G,S)` retains release generation `G` as runtime generation `R` through a later stop request until stopped, proven-quiescent stop failure, current runtime failure, abort, or taint terminates it. Thus asynchronous `runtime_failed` needs no outstanding release command. If current runtime failure wins a stop race, that stop result is stale; if stop wins, `R` is invalidated and a later runtime failure is stale.

An ordinary result is current only when it is a listed alternative for the exact outstanding command tuple. Preparation alone matches `prepared(E,G,S)` to `prepare(E,G,0)` by epoch/generation and introduces `S`. The retained-runtime rule alone accepts an asynchronous result without an outstanding command. Every other mismatched or illegal result is stale: bounded idempotent cleanup may touch only its tuple and cannot mutate current state, emit USB output, release current capture, resolve a barrier, or acknowledge lifecycle. Clearing a mailbox is not worker synchronization.

Audio control splits start into a paused preparation and release:

```text
prepare(epoch, generation, session=0) → allocate/validate a non-capturing session
release(epoch, generation, session)   → permit capture and first AudioFrame
cancel_prepared(epoch, generation, session) → unwind without AudioFrame/EOS
stop(epoch, generation, session)      → normal single EOS
abort(old_epoch, generation, session=0) → closed-epoch cleanup without EOS
```

Only successful `release` permits I2S capture or AudioFrame handoff. Every `prepare`, `release`, `cancel_prepared`, `stop`, and reserved `abort` is a voice-transition barrier. A barrier retains its triggering transition or association and prevents every later FIFO entry from overtaking it until the table resolves it. A table path marked fail epoch invokes `vk_usb_fail_epoch(E,VK_USB_SESSION_ERROR_INPUT_TAINTED)`, which closes admission and reserves exactly `{"event":"vk_error","operation":"input","code":"tainted"}`. It emits no additional ordinary error afterward.

Hold-to-talk:

```text
idle + debounced voice-key down
  → retain the voice transition at FIFO head; submit prepare
  → prepare failure: handoff exactly one ordinary button_down without session;
    emit audio_start_failed; resolve the barrier
  → prepared: assign session and handoff button_down(session)
  → only after that handoff is ACCEPTED: submit release
  → release success: permit capture; resolve the start barrier
  → release failure: never send another button_down; emit audio_start_failed;
    submit cancel_prepared and retain the prepared session as the immutable
    button association; only cancelled permits this physical press's queued
    up/click to be accepted, while cancel failure/taint fails the epoch and
    discards those not-yet-accepted values

release while prepare/down/release is pending
  → scanner records release behind the retained down
  → after successful release, handoff up then click with that session
  → after click is ACCEPTED, submit stop; EOS follows the click
  → after prepare failure, handoff down/up/click once each without session
  → after release failure, wait for matching cancelled, then handoff up and click
    with the prepared audio-attempt identity; no AudioFrame/EOS exists
  → cancel failure/taint closes the epoch before either queued value is accepted

active session + debounced voice-key release
  → handoff button_up with the active session
  → handoff button_click with the same session
  → after click is ACCEPTED, submit stop; audio queues the single empty EOS
    after that click
```

The button `session_id` after release failure retains the contract's single audio-attempt identity; it is not a second namespace and does not prove recording started. The host establishes a recording only on the first valid AudioFrame. Therefore a down/up/click sequence may legally carry one consistent nonzero identity while producing no recording, AudioFrame, or EOS.

Click-to-talk:

```text
idle + complete voice-key down/up/click
  → handoff all three events without a session
  → after click is ACCEPTED, submit prepare
  → prepared: submit release; association begins before later FIFO events
    are assigned
  → prepare failure: emit audio_start_failed and resolve without association
  → release failure: emit audio_start_failed and submit cancel_prepared;
    only cancelled clears association and resolves without duplicating an event
  → cancel failure/taint: fail the epoch, retain uncertain audio ownership,
    and discard all not-yet-accepted FIFO values

active session + complete voice-key down/up/click
  → handoff all three events with that session
  → after click is ACCEPTED, submit stop; EOS follows the click

stop pending + a third voice interaction
  → scanner appends its down/up/click in normal FIFO order
  → the stop barrier prevents those events and all later events from overtaking
  → no duplicate stop is submitted and the events do not use the stopping session
  → after matching stopped, clear the old association, then process the retained
    interaction from idle as one new click-to-talk start gesture
  → after matching stop failure/tainted, emit audio_stop_failed/tainted,
    fail closed for voice transitions in that epoch, and never reinterpret the
    retained interaction as an ordinary button gesture
```

The same barrier rule covers a later interaction while prepare, release, cancel, or abort is pending: it remains in FIFO order until the table resolves the current tuple. Non-voice keys never start or stop audio, but cannot overtake an earlier barrier. On proven-quiescent stop failure or current runtime failure, the input owner performs one stable FIFO filter: discard every not-yet-accepted transition whose key equals the epoch's configured voice key, including a retained third interaction, resolve the barrier, and keep every other transition in its existing relative order for ordinary handoff. Newly sampled transitions for the disabled voice key are suppressed until the next epoch; other keys continue normally. Already `ACCEPTED` button values are never retracted or repeated, discarded transitions emit no up/click/error, and the retained voice interaction is never reinterpreted as an ordinary gesture. A clean prepare/release/stop/runtime failure does not retract an already accepted canonical button event and never creates a synthetic click; cancel failure/taint instead closes the epoch and discards every not-yet-accepted value. A current retained-runtime failure follows the table and does not synthesize release. Epoch loss uses the reserved abort, clears association only after quiescence, and forbids EOS/button values from the closed epoch. Host long/double gesture derivation remains unchanged.

Accepted `interaction_mode` or `voice_key` commands reply with exact `{"event":"vk_input_state","interaction_mode":"hold_to_talk|click_to_talk","voice_key":"k1|k2|k3|k4"}` after state mutation. Input failures use `vk_error`, `operation:"input"`, and exact code `invalid_request|wrong_epoch|busy|input_queue_overflow|audio_start_failed|audio_stop_failed|audio_runtime_failed|tainted`; optional `message` is diagnostic-only UTF-8 of at most 96 bytes. Unknown/extra command fields are `invalid_request`. Configuration mutation while prepare/release/run/stop/abort/cancel is pending is `busy`. The state acknowledgement, not successful host write alone, confirms configuration.

## Recording Lifecycle

The device configures I2S0 PDM RX at 16 kHz with two 16-bit slots on GPIO41 clock/GPIO40 data, feeds Espressif AFE using input format `MM`, selects the processed mono PCM output, encodes Opus, and mirrors framed packets over USB. The two slots do not by themselves prove the physical microphone count. The host does not receive USB Audio Class PCM.

Host UI state commands drive the vendor recording interaction:

```json
{"event":"ui_state","state":"listening","text":""}
{"event":"ui_state","state":"thinking","text":""}
{"event":"ui_state","state":"processing","text":"..."}
{"event":"ui_state","state":"ready","text":""}
{"event":"ui_state","state":"error","text":"..."}
```

Required lifecycle:

```text
idle
  → listening requested
  → first valid AudioFrame establishes session
  → ordered Opus packets are accepted
  → final/end evidence closes session
  → Ogg Opus output is finalized
  → ready
```

Hold-to-talk and click-to-talk are both supported by firmware configuration. The client must surface explicit `ready`, `recording`, `finalizing`, `completed`, and `failed` states.

## Audio Dependency Gate

The target dependencies are pinned for ESP-IDF 5.5.2 and ESP32-S3 in `firmware/dependencies.lock`:

| Component | Version | Registry component hash | Source commit | License |
|---|---:|---|---|---|
| `espressif/esp-sr` | `2.1.4` | `3903f0880cc3065765bd4038e01cbfa7907c8052ecf0a4f7a70c4444a26c1737` | `85a1c634325cecf99377e6fdb385b03a5c3363ce` | ESPRESSIF MIT, restricted to Espressif products |
| `esphome/micro-opus` | `0.4.1` | `c4cec51b6e45b9b660bf8725a10c65f46485ff8b37ff664e4da3fd738301c71e` | `8354085908683c6130e32a832aeec8a7ca115c51` | Apache-2.0 wrapper; BSD-style upstream Opus and patches |

The ESP-SR 2.1.4 named AFE interface is `esp_afe_handle_from_config`, followed by `create_from_config`, `destroy`, `reset_buffer`, `feed`, `fetch`, `get_feed_chunksize`, `get_fetch_chunksize`, `get_channel_num`, `get_feed_channel_num`, `get_fetch_channel_num`, and `get_samp_rate`. Configuration starts with `afe_config_init("MM", NULL, AFE_TYPE_SR, AFE_MODE_HIGH_PERF)` and uses named `afe_config_t` fields only: `aec_init`, `se_init`, `vad_init`, `wakenet_init`, `agc_init`, `memory_alloc_mode`, `afe_perferred_core`, `afe_perferred_priority`, `afe_ringbuf_size`, `afe_linear_gain`, and `debug_init`. The researched candidate fields `voice_communication_init`, `voice_communication_agc_init`, and `voice_communication_agc_gain` do not exist in the real 2.1.4 ESP32-S3 `afe_config_t`; they must not be invented or accessed by recovered offsets. `"MM"` contains no playback/reference channel, and the null model list plus disabled WakeNet/VAD means production version 1 loads no speech model list. Raw recovered `afe_config_t` offsets are forbidden.

The micro-opus target build uses its public `opus.h` API: `opus_encoder_create`, `opus_encoder_ctl`, `opus_encode`, and `opus_encoder_destroy`. ESP32-S3 configuration is fixed-point (`CONFIG_OPUS_FLOATING_POINT` disabled), Xtensa optimizations enabled, thread-safe 120,000-byte pseudostack, and PSRAM-preferred state/pseudostack allocation. A host Homebrew/macOS arm64 Opus library may support native tests but is not a firmware dependency.

A clean ESP32-S3 compile/link probe establishes dependency and named-ABI compatibility only. It does not establish real-time 60 ms encoding performance, microphone-slot acoustic mapping, AFE quality, end-to-end USB audio behavior, or vendor gain parity. Replacement firmware version 1 uses unity post-AFE gain (`afe_linear_gain = 1.0`) and does not claim parity with an unverified vendor integer gain. The unknown vendor `voice_gain` range remains excluded from both production firmware and client controls.

## Audio Contract

Each USB `AudioFrame` carries:

- `session: UInt32`;
- `sequence: UInt32`;
- `flags: UInt8`;
- Opus payload bytes.

Verified capture and encoder configuration:

```text
I2S controller: I2S0 PDM RX master
PDM input: 16000 Hz, 16-bit stereo/both slots, GPIO41 clock/GPIO40 data
DMA: 4 descriptors × 512 frames
AFE input format: MM; runtime feed/fetch chunk sizes queried from the AFE interface
microphone ring: 64000 bytes
sample rate: 16000 Hz
channels: 1
application: OPUS_APPLICATION_VOIP
VBR: enabled
bitrate: 16000 bps
DTX: disabled
complexity: 4
signal: OPUS_SIGNAL_VOICE
samples per packet: 960
packet duration: 60 ms
maximum encoded packet: 220 bytes
AFE feed task: stack 8192, priority 20, core 0
AFE fetch task: stack 8192, priority 18, core 0
audio pipeline task: stack 32768, priority 5, core 0
bounded worker join: approximately 1500 ms before straggler deletion
```

Verified flags and ordering:

| Flag | Meaning |
|---:|---|
| `0x01` | first packet; sequence must be zero |
| `0x02` | final/end-of-stream |
| `0x04...0x80` | unknown and not authorized |

A new session resets sequence to zero. Each data packet consumes the current sequence, is handed to the bounded USB queue for the captured transport epoch, then increments only after that handoff succeeds. This is queue-ownership acknowledgement, not physical USB-write completion; the USB service intentionally exposes no physical-write completion callback. Normal end emits the next sequence with `flags=0x02` and an empty payload. The host accepts a non-empty final payload defensively and writes it as the final Opus packet, but current firmware does not produce one.

Verified Ogg Opus output contract:

```text
OpusHead version: 1
channels: 1
pre-skip: 312
input sample rate: 16000
output gain: 0
mapping family: 0
OpusTags vendor: VibeBoard
Ogg stream serial: 0x5653544b
Ogg granule increment: 2880 per 60 ms Opus packet
```

The muxer writes one Opus packet per Ogg page. Head is BOS, the last non-empty packet is EOS when final, and an empty firmware EOS frame causes an empty Ogg EOS page at the current granule. CRC uses polynomial `0x04c11db7`. `OpusHead` records the original 16 kHz input rate, but conforming Ogg Opus decoders expose a 48 kHz output timebase; `ffprobe` reporting 48 kHz is expected and does not contradict the firmware encoder's 16 kHz input.

Required behavior:

- reject malformed audio headers and ordinary empty payloads;
- require first-flag frames to use sequence zero;
- detect session changes, sequence gaps, duplicates, and regression;
- do not silently concatenate different sessions;
- reject frames after EOS and report a session change without prior EOS as truncation;
- treat only `flags & 0x02` as proven EOS; retain unknown bits in diagnostics and reject unsupported combinations;
- optional local save uses atomic temporary-file replacement;
- optional ASR credentials stay in macOS Keychain and are never sent to firmware or logged;
- raw/debug audio logging is disabled by default.

## Action Safety

Accessibility/input-injection permission is requested only when a configured action requires it. A permission failure produces a visible typed error and does not fall back to shell automation.

Custom commands:

- are disabled until explicitly configured;
- display the exact command before save/test;
- run without interpolating device-controlled values;
- have bounded execution and captured exit status;
- never log environment variables or secrets.

## Required Tests

- decode valid/invalid button identifiers and events;
- prove one physical gesture executes at most one action;
- validate associated data for every action variant;
- persistence round-trip and schema migration;
- action router permission and process errors;
- audio session/sequence transitions and gaps;
- golden Ogg Opus header/page CRC/granule tests once packet timing is recovered;
- real four-key USB capture and real speech decode acceptance test.
