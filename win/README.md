# Windows Client Scope

## Purpose

Native Windows application that discovers the target VibeBoard over USB, applies user configuration, transfers display resources, routes four physical keys, and records device Opus audio.

This is the .NET 8 / WPF port of the macOS client (`mac/`), implementing the same USB serial protocol and device features.

## Build and Launch

```bash
cd win
dotnet build -c Release
```

Launch:

```bash
dotnet run --project src/VibeKeyboardApp -c Release
```

## Requirements

- .NET 8 SDK (or newer, with `RollForward` enabled)
- Windows 10 19041+ / Windows 11
- USB cable connected to the VibeBoard device

## Package Structure

```text
VibeBoardKit
  └─ Protocol
      ├─ incremental frame parser
      ├─ state/audio models
      ├─ outbound command encoder
      ├─ BoundedJSON parser (security-critical)
      ├─ replacement capability/event decoder
      ├─ asset/screen/LED/widget protocol
  ├─ USB
  │   ├─ WMI device discovery (VID 0x303a, PID 0x1001)
  │   ├─ SerialPort session
  │   └─ connection/handshake state machine
  ├─ Input
  │   ├─ persisted mappings
  │   ├─ gesture router
  │   └─ host action router
  ├─ Audio
  │   ├─ session ordering
  │   └─ Ogg Opus writer
  ├─ Assets
  │   ├─ source decoding/conversion (System.Drawing)
  │   ├─ VKA1 container codec
  │   └─ upload state machine
  ├─ LED
  │   └─ capability/query/config/state service
  └─ AppModel
      └─ observable integration state
```

## Protocol

Uses USB Serial/JTAG (CDC) only. No Bluetooth or network transport.

Device: ESP32-S3, VID `0x303a`, PID `0x1001`. Display 428×142 RGB565.

See [../docs/product/usb-protocol.md](../docs/product/usb-protocol.md) for the full USB protocol contract.

## Differences from macOS Client

- USB discovery uses WMI (System.Management) instead of IOKit
- Serial port uses `System.IO.Ports.SerialPort` instead of POSIX `open()`/`termios`
- Image decoding uses `System.Drawing` instead of ImageIO/CoreGraphics
- UI uses WPF instead of SwiftUI
- Audio BlackHole mode replaced with WASAPI loopback (planned)
