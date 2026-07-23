# Vibe Keyboard Product Scope

Status: Active design; USB core verified, replacement-firmware hardware/audio details still being recovered

## Purpose

Build a native macOS Swift client and matching firmware capabilities for the connected VibeBoard hardware. The result must support:

1. USB-only device discovery and communication.
2. Device-side LVGL rendering on the built-in 428×142 display.
3. User-selected static images and animated pets stored on the device.
4. Host-fed information widgets rendered by the device.
5. Configurable mappings for all four physical keys.
6. Existing microphone recording behavior over USB.
7. Calibrated device-status LED feedback and bounded host enable/brightness configuration.

## Boundary

- The target is this VibeBoard hardware only: USB VID `0x303a`, PID `0x1001`, observed serial `02:00:00:00:00:01`.
- BLE is outside scope. No client feature may require Bluetooth.
- The ESP8266 reference project contributes product patterns only; its Wi-Fi/HTTP transport is not reused.
- The existing `/Applications/VibeBoard.app` is a behavioral and protocol reference, not a runtime dependency.

## Ownership

```text
macOS Swift client
  ├─ discovers the USB device
  ├─ stores user configuration
  ├─ uploads assets and screen layouts
  ├─ feeds widget values
  ├─ maps device key events to host actions
  ├─ configures calibrated LED feedback when capability permits
  └─ receives and stores/forwards microphone audio

VibeBoard firmware
  ├─ owns display drivers and LVGL objects
  ├─ stores uploaded assets/layout state
  ├─ scans four physical keys
  ├─ captures and encodes microphone audio
  ├─ owns calibrated LED feedback and fail-dark behavior
  └─ exposes all functions through one USB protocol
```

## Lifecycle

```text
USB attach
  → client discovers device
  → protocol handshake
  → capability/version check
  → configuration sync
  → ready
  → events/assets/widgets/audio flow over USB
  → USB detach and deterministic cleanup
```

## Constraints

- Never silently substitute BLE or network transport.
- Unknown frame types, malformed payloads, incompatible protocol versions, and failed asset validation must surface typed errors.
- Firmware updates require a verified backup and explicit integrity checks before writing the device.
- API secrets remain in macOS Keychain and are never sent to the device or logged.
