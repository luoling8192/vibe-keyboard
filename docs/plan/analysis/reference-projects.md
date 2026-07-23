# Reference Project Analysis

## esp8266-ai

Useful patterns:

- Native macOS controller boundaries: device client, monitors, asset pipeline, preview.
- Separate configured screen mode from effective rendering mode.
- Device owns final rendering and animation timing.
- Host owns expensive content preparation such as GIF slicing, CJK bitmap rendering, music artwork, and system sampling.
- Revisioned resources avoid duplicate transfers (`sprite_rev`, `artwork_rev`, `text_rev`).
- Asset upload is streamed to temporary storage, verified, and revision-published. The adopted SPIFFS design uses immutable commit recovery rather than inheriting an unproven atomic-replace claim.
- Network samples carry monotonic sequence numbers and are consumed on a fixed device schedule.
- PetDex input pipeline: manifest → WebP sheet → row/frame extraction → aspect fit → deterministic animation upload.

Not applicable:

- Wi-Fi discovery, HTTP APIs, bridge address management, and `/24` scanning.
- ESP8266, ST7789, TFT_eSPI, 240×240 coordinates, and fixed 120×120 sprite slots.
- Low-memory ESP8266 constraints as hard limits; the VibeBoard has 8 MiB PSRAM but transfers still require bounds.
- Claude/Codex credential and quota logic unless explicitly added as a widget provider.

## second-state/vibekeys_firmware

The inspected current main branch targets ST7789 layouts (284×78 or 320×172). It contains no verified NV3007, 428×142, or target VibeBoard hardware profile. Its GPIO, panel gap, byte order, and display init values cannot be used for this product.

## Adopted Boundary

```text
Swift client
├─ USB device/session
├─ resource converter and revision cache
├─ information providers
├─ declarative layout editor + preview
├─ key action router
└─ audio sink/ASR

VibeBoard firmware
├─ NV3007 hardware profile
├─ configured/effective mode state
├─ verified asset store
├─ LVGL renderers
├─ four-key scanner
└─ USB audio/control endpoint
```

The device capability handshake drives limits, formats, protocol version, and supported modes. No reference-project screen constants are hard-coded into the shared contract.
