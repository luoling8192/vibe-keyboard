# VibeBoard Firmware Scope

## Purpose

Provide a VibeBoard-specific ESP-IDF firmware that replaces the vendor BLE-primary architecture with one USB-only service while preserving the proven display, four-key, microphone/Opus, and rollback behavior.

## Boundary

The firmware targets only:

```text
ESP32-S3 revision-compatible target
16 MiB flash
8 MiB octal PSRAM
NV3007 428×142 RGB565 display
four keys on verified GPIOs
I2S0 PDM microphone bus on GPIO41/40, 16 kHz/two input slots
  USB OTG composite transport: CDC control plus UAC2 microphone
```

It does not initialize Bluetooth, Wi-Fi, GATT, advertising, or network services. macOS owns user configuration, source asset conversion, host actions, widget sampling, saved recordings, and credentials.

## Component Structure

```text
main
  ├─ vk_board       exact pins, panel, keys, microphone, storage mounts
  ├─ vk_usb         TinyUSB composite descriptors, CDC protocol, and UAC source lifecycle
  ├─ vk_state       typed device/configured/effective state
  ├─ vk_input       key scan, debounce, events, voice interaction
  ├─ vk_audio       PDM → direct UAC PCM or AFE → legacy Opus → framed CDC
  ├─ vk_led         calibrated feedback arbitration and fail-dark owner
  ├─ vk_assets      SPIFFS transfers, VKA1 validation, immutable revisions/recovery
  ├─ vk_screen      LVGL objects, modes, widgets, pet animation
  ├─ vk_update      replacement-only RAM-epoch staged update
  └─ host bootstrap separate ROM-download first-write procedure
```

Components depend on interfaces, not globals. `main` constructs concrete components and owns startup/shutdown ordering.

## Startup

```text
validate hardware/build identity and, only when the installed bootloader capability requires it, pending-verify OTA state
  → initialize NVS without erasing on recoverable error
  → mount storage; first-format only through the explicit verified-erased gate
  → scan immutable commit revisions and retain current + previous valid state
  → initialize board/display/LVGL
  → show local boot/USB-wait state
  → initialize pinned PDM/AFE/Opus control in idle/non-capturing state
  → prepare keys and register typed USB input/config/lifecycle handlers while scanning remains stopped
  → start TinyUSB CDC + UAC2 composite service with all consoles/logs disabled from CDC
  → start key scanner and input/audio-control owners
  → recover bounded temporary transfers without activating them
  → render configured mode
  → answer get_device_info with device_info then vk_capabilities
```

A component failure produces a local bounded error state and a typed USB event when USB is available. It never triggers an NVS erase or asset manifest replacement as a fallback.

## State Ownership

Version 1 uses typed component owners rather than an unimplemented global `vk_state` singleton:

- `vk_usb` owns protocol epoch, lease, capability snapshot emission, and typed queue admission;
- `vk_input` owns epoch-local interaction mode, canonical voice key, debounce/FIFO state, and its association to an audio session;
- `vk_audio` owns the real prepared/running capture session and reports typed lifecycle results;
- `vk_led` alone owns LED state, animation timing, calibrated limits, and complete-frame publication; other components submit typed semantic intents;
- `vk_screen` owns configured/effective screen state and overlay stack;
- a shared mutation coordinator owns mutually exclusive asset/update leases.

Cross-component mutation uses typed commands/results with bounded mailboxes. The 5 ms scanner never invokes synchronous USB/audio operations. LVGL calls execute only on the LVGL context through `esp_lvgl_port` locking/callback rules.

## USB and Logs

The only device transport is the ESP32-S3 internal USB PHY in USB OTG device
mode. TinyUSB exposes CDC for the framed product protocol and a microphone-only
UAC2 function. Bluetooth, Wi-Fi, USB host mode, and network fallback are
excluded. ROM download recovery continues to use the chip's USB Serial/JTAG
path; application startup does not burn eFuses or replace the bootloader.

The USB CDC byte stream is a binary protocol after startup. Primary/secondary console, bootloader/application logs, VFS console, and panic text are disabled in production; development logs use JTAG or a separately reviewed build. One service task owns install/uninstall and one serialized TX boundary. A `transport/usb` command starts an epoch; two-second pings maintain a five-second lease. Lease expiry clears only temporary session state.

The parser uses fixed/bounded storage, direction-specific type allowlists, checked integer arithmetic, and exact-length dispatch. Protocol keys are exact ASCII identifiers; string values require valid UTF-8, valid JSON escapes, and valid Unicode scalar/surrogate-pair handling under size/depth/string/list limits. Unicode scalar `U+0000` is forbidden in every protocol string, including `\u0000`, so no length-bearing value can be truncated through a C-string boundary. Asset bytes are streamed to storage and never buffered as one unbounded allocation. The public API registers and sends only typed button, AudioFrame, asset-command/chunk, and staged-update values; downstream components never receive raw JSON/frame bytes and no raw send API exists.

## Display

The exact NV3007 profile is a validation gate. `vk_board` owns panel creation and `vk_screen` owns LVGL objects. Host commands cannot send raw panel commands or create arbitrary LVGL object types.

One RGB565 full-screen buffer is 121,552 bytes. Buffer count and chunk lines must be selected from measured PSRAM/internal-DMA constraints, not from the ESP8266 reference.

## Input and Audio

Key GPIO identity follows [Hardware Contract](../product/hardware.md). `vk_input` is the only debounce owner and emits canonical `k1...k4` events.

The microphone pipeline reproduces the verified vendor capture and encoder
contract from [Input and Audio Contract](../product/input-audio.md): ESP-IDF
5.5.2 named I2S0 PDM RX APIs at 16 kHz with two 16-bit slots. UAC bypasses the
speech AFE, selects the input slot with greater block energy, and publishes it
as 16-bit mono PCM. Legacy voice sessions retain the pinned ESP-SR AFE using
named `MM` fields, the pinned Opus encoder, and the existing framed CDC
protocol. UAC and legacy Opus sessions arbitrate the single I2S owner and never
run concurrently. Other CDC features remain available while UAC streams.

Production AFE/Opus adapters remain gated until exact versions, hashes, ESP-IDF compatibility, named AFE ABI, and target linkage are recorded. A host Homebrew arm64 Opus library may test pure logic but can never satisfy the ESP32 dependency. Raw `afe_config_t` offsets are forbidden. Version 1 applies unity post-AFE gain and does not claim vendor gain parity; no host `voice_gain` command is exposed.

## LED Feedback

The exact product contract is [LED Feedback Contract](../product/led.md). `vk_led` is the sole owner; `vk_board` exposes only fixed 17-pixel logical-RGB frame/all-off transport and never exposes the strip handle. Before exact production-profile admission, the only production frame is all-zero and `led.available` remains false with reason `calibration_required`. LED capability is independent from assets/screen, but uses the same complete current-epoch capability snapshot. LED registers an asynchronous lifecycle participant, uses request-correlated config acknowledgement, and maintains distinct hardware-failure/stopping/epoch-off safety states. Failure to prove all-off taints the USB composition awaiting that participant and never reopens the old epoch. Raw value-1 pulses exist only in the separately reviewed ESP32-S3 pure-RAM harness loaded by a future per-step-authorized ROM `--no-stub load_ram` operation; they are never flashed calibration images or a production USB API.

## Storage and Updates

- Sensitive vendor NVS/NVS keys and the verified partition layout are preserved.
- `storage` remains SPIFFS with an object-name bound large enough for complete immutable names. SPIFFS path separators are logical filename characters, not directories.
- Mount failure never triggers format. First format requires the documented proof that the verified initially erased partition has no valid filesystem. Corruption produces an error, never erase fallback.
- Asset/config publication uses destination-absent temporary and immutable files, fsync/close, a commit record written last, and bounded boot scanning. SPIFFS rename is not claimed as replacement-atomic or power-loss atomic. At least current and previous valid revisions are retained.
- First replacement bootstrap is a separately reviewed host ROM-download procedure: stage only exact app bytes into `ota_1 @ 0x520000`, read back and validate, then separately write only the erased secondary otadata sector. It preserves vendor bootloader, partition table, ota_0, primary otadata, NVS/NVS keys, PHY, and storage.
- First bootstrap must not claim automatic rollback. The vendor bootloader remains installed; recovery uses the proven ROM path and private vendor backup.
- Before a reviewed replacement-bootloader migration, update capability must be absent/unavailable and firmware running from ota_1 must not overwrite vendor ota_0. After migration, `vk_update` uses type-0x10 controls plus type-0x41 chunks, current-epoch RAM metadata, seal without selection, and revalidation before activation.
- A future production build may claim pending-verify rollback only after a separately reviewed rollback-enabled replacement bootloader migration. Mandatory self-tests are capability-aware and must match compiled services. No update path may fall back to the running slot.

## Build and Test

The baseline toolchain matches vendor ESP-IDF v5.5.2 until evidence requires a change.

Required layers:

1. native/pure tests for byte/container/state logic where ESP-IDF allows;
2. ESP-IDF component tests or Unity tests for drivers/services;
3. offline image and partition validation;
4. connected USB/display/key/audio tests;
5. reboot, detach, interrupted transfer, invalid data, low-space, and rollback tests.

No connected test may write firmware until the First-Write Safety Gate in [Hardware Contract](../product/hardware.md) is complete.
