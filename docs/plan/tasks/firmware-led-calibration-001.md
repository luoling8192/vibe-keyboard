---
id: firmware-led-calibration-001
scope: separately authorized RAM-only LED mapping and profile evidence
status: blocked
depends-on: [firmware-led-ram-harness-001]
---

## Objective

Execute independently reviewed pure-RAM one-stimulus LED harness artifacts under per-step authorization, restore the unchanged production flash boot, and produce canonical mapping/profile evidence without exposing a production raw-pixel API.

## Context

- `docs/product/led.md`
- `docs/product/hardware.md`
- `docs/product/usb-protocol.md`
- `docs/plan/tasks/firmware-led-ram-harness-001.md`
- `docs/plan/tasks/firmware-bootstrap-001.md`

## Path

- `firmware/calibration/led/`
- `firmware/tools/`
- `docs/plan/analysis/`

## Contract

- Start only after `firmware-led-ram-harness-001` independently passes its clean pure-RAM build, segment/map, forbidden-symbol, deterministic-artifact, and fail-dark reviews. No temporary flash calibration image or rollback assumption is allowed.
- Each explicit authorization is the external byte-exact, domain-separated `VKLED-CALIBRATION-AUTH-V1` canonical record from `docs/product/led.md`. It binds both raw `harness_manifest_sha256 = SHA-256(manifest_bytes)` and domain-separated `harness_manifest_identity`, the exact domain-separated `stimulus_identity`, exact device/board identity, production flash/selection identity, recovery evidence, one-shot nonce/scope, and positive expiry. It is generated only after final image/ELF/map/sdkconfig/toolchain hashes exist and is never embedded into or fed back into the harness image. The single known-answer corpus is `docs/product/fixtures/led-calibration-artifacts-v1.json`.
- Before ROM entry, the executor recomputes every uniquely named raw digest and domain-separated identity, rejects any raw/identity substitution, expiry, or mismatch, and atomically records `authorization_identity = SHA-256("VKLED-CALIBRATION-AUTH-V1\0" || authorization_record_bytes)` as consumed in an append-only private ledger. `authorization_record_sha256` is only the raw complete-record digest and is not a ledger key. `uses` is exactly one; any prior attempt, including a failed attempt, rejects reuse.
- One authorization covers exactly one ROM-entry reset, exact ESP32-S3/security re-identification, one `--no-stub load_ram` of that image, one raw-pixel/single-channel/value-1 pulse for `1...250 ms`, one separately authorized recovery reset or power-cycle after release of the download strap, and one post-recovery read-only production identity/all-off verification.
- `load_ram` must not write flash or selection metadata and performs no automatic post-reset. The harness has no command parser, USB LED control, raw/general sender, persistence, capability advertisement, BLE, Wi-Fi, network, TinyUSB, USB OTG CDC, or UAC.
- If ROM/download identity, security evidence, artifact hash, recovery state, unchanged selection metadata, production image identity, or all-off proof is missing or mismatched, fail closed and do not begin another stimulus.
- Authorization for one descriptor does not authorize another, a general reset, flash/erase/OTA/otadata/NVS mutation, higher value, longer duration, or production profile admission.
- Produce only the canonical non-secret mapping observation artifact from `led.md`. Mapping alone cannot enable LED. A separately documented, authorized, and independently reviewed sustained-current method must establish limits before an exact compiled-in allowlisted production profile can advertise available.

## Verification

- Offline procedure simulation rejects noncanonical authorization bytes, missing/extra/reordered fields, wrong domain identity, expired/used records, missing ledger admission, harness/stimulus/image/ELF/map/sdkconfig/toolchain/tool/device mismatches, stale production identity, download-strap recovery ambiguity, altered selection metadata, and missing post-reset all-off evidence.
- Connected execution, ROM entry, reset, `load_ram`, LED illumination, current measurement, and post-recovery reads each remain externally authorized operations; this task is blocked until those inputs exist.
- Evidence output contains no private backup/NVS contents, device secret, caller path, raw serial log, or arbitrary note.
