---
id: firmware-audio-deps-001
scope: pin ESP-SR and ESP32-target Opus dependencies
status: done
depends-on: [firmware-hardware-001]
---

## Objective

Select and lock exact ESP-SR and ESP32-target Opus releases compatible with ESP-IDF 5.5.2, then document their named ABI before production audio implementation.

## Context

- `docs/product/input-audio.md`
- `docs/firmware/README.md`
- `docs/plan/tasks/firmware-audio-001.md`

## Path

- `firmware/components/vk_audio/`
- `firmware/components/vk_audio/idf_component.yml`
- `firmware/dependencies.lock`
- `firmware/tests/`
- `docs/product/input-audio.md`

## Contract

- Record exact versions, hashes, licenses, target/IDF compatibility, and named AFE create/reset/feed/fetch/chunk/channel fields.
- Prove an ESP32-S3 target Opus build; Homebrew/macOS arm64 libraries are test-only and never firmware dependencies.
- Use no recovered raw `afe_config_t` offsets. Input format is named `MM`, no playback/reference/model list, with runtime chunk/channel queries.
- Version 1 uses unity post-AFE gain and makes no vendor-gain parity claim.

## Verification

- Clean ESP-IDF compile/link test uses only pinned target dependencies and named fields.
- Dependency lock/hash is reproducible and final ELF has no BLE/network/UAC entry point.
- If no compatible dependency can be pinned, this task remains blocked and production audio does not claim completion.

## Implementation Evidence

- `firmware/components/vk_audio/idf_component.yml` pins ESP-IDF `5.5.2`, ESP-SR `2.1.4`, micro-opus `0.4.1`, and target `esp32s3`.
- `firmware/dependencies.lock` records registry hashes `3903f0880cc3065765bd4038e01cbfa7907c8052ecf0a4f7a70c4444a26c1737` (ESP-SR) and `c4cec51b6e45b9b660bf8725a10c65f46485ff8b37ff664e4da3fd738301c71e` (micro-opus), plus all transitive managed dependencies.
- Managed manifests resolve source commits `85a1c634325cecf99377e6fdb385b03a5c3363ce` and `8354085908683c6130e32a832aeec8a7ca115c51` respectively.
- `firmware/tools/check_audio_dependencies.py` verifies lock versions/hashes, managed-component hashes/commits, target/IDF, and saved license notices.
- `firmware/tests/idf/audio_dependency_probe` compiles and links the named AFE interface/config fields and public Opus encoder calls. The real 2.1.4 ESP32-S3 `afe_config_t` has no `voice_communication_init`, `voice_communication_agc_init`, or `voice_communication_agc_gain`; the probe does not invent them. Fixed-point, Xtensa optimization, thread-safe pseudostack, and PSRAM preference are explicit sdkconfig inputs.
- `firmware/tools/check_audio_probe_elf.py` requires the linked AFE/Opus probe symbols and rejects BLE/network/UAC entry points.
- Implementation build used ESP-IDF `v5.5.2`, target `esp32s3`, and completed all `1400/1400` Ninja steps. This is compile/link evidence only; no device I/O occurred and real-time/acoustic behavior remains unverified.

## Review

- Independent review passed the exact version/hash/commit, actual license/notice, named ABI, fixed-point/Xtensa/PSRAM configuration, clean ESP32-S3 build, required/forbidden ELF symbol, production validation, and USB-only isolation gates.
- The pass authorizes dependent offline implementation only. Real-time performance, acoustic slot mapping, AFE quality, USB AudioFrame E2E, physical speech capture, vendor-gain parity, and every device write remain unverified or separately gated.
