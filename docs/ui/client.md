# macOS Client Layout

Status: Draft

## Design Principles

- Native macOS SwiftUI interface.
- Device status and destructive firmware actions remain visually distinct.
- Screen preview is always 428:142 and mirrors the firmware layout model.
- Asset upload, key mapping, and audio state expose progress and recoverable errors.

## Overall Structure

```text
┌──────────────────────────────────────────────────────────────────────┐
│ Vibe Keyboard                                              Connected │
├──────────────┬───────────────────────────────────────────────────────┤
│ Device       │                                                       │
│ Screen       │                   Active page                         │
│ Pets         │                                                       │
│ Keys         │                                                       │
│ Audio        │                                                       │
│ Firmware     │                                                       │
├──────────────┴───────────────────────────────────────────────────────┤
│ VB-020000000001 · USB · firmware/protocol status                    │
└──────────────────────────────────────────────────────────────────────┘
```

## Device Page

```text
┌──────────────────────────────────────────────────────────────────────┐
│ Device                                                              │
│ ┌─────────────────────────────┐ ┌──────────────────────────────────┐ │
│ │ 428×142 live preview        │ │ Connection                     │ │
│ │ ┌─────────────────────────┐ │ │ USB: Connected                 │ │
│ │ │                         │ │ │ Serial: 02:00:00:00:00:01     │ │
│ │ │      device screen      │ │ │ Firmware: …  Protocol: …      │ │
│ │ │                         │ │ │ [Reconnect]                    │ │
│ │ └─────────────────────────┘ │ ├──────────────────────────────────┤ │
│ │ Mode: Pet                   │ │ Capabilities                  │ │
│ └─────────────────────────────┘ │ Assets: unavailable …         │ │
│                                 │ Screen: available              │ │
│                                 │ LED: calibration required      │
│                                 │ Update: migration required     │ │
│                                 │ [Reconnect] [Details]          │ │
│                                 └──────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────────┘
```

## Screen Page

```text
┌──────────────────────────────────────────────────────────────────────┐
│ Screen                                                  [Send]       │
│ ┌───────────────────────────────────────┐ ┌────────────────────────┐ │
│ │ 428×142 preview                      │ │ Mode                   │ │
│ │ ┌───────────────────────────────────┐ │ │ ( ) Image              │ │
│ │ │                                   │ │ │ (•) Pet                │ │
│ │ │ [Pet] [Status text] [Widget]      │ │ │ ( ) Dashboard          │ │
│ │ │                                   │ │ │ ( ) Custom             │ │
│ │ └───────────────────────────────────┘ │ ├────────────────────────┤ │
│ │                                       │ │ Widgets                │ │
│ │                                       │ │ Time / System / Agent  │ │
│ └───────────────────────────────────────┘ └────────────────────────┘ │
│ Upload state: validating → converting → sending → verifying → active │
└──────────────────────────────────────────────────────────────────────┘
```

## Pets Page

```text
┌──────────────────────────────────────────────────────────────────────┐
│ Pets                                              [Import GIF/APNG]  │
│ Search [________________________]                                    │
│ ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐                 │
│ │ preview  │ │ preview  │ │ preview  │ │ preview  │                 │
│ │ name     │ │ name     │ │ name     │ │ custom   │                 │
│ └──────────┘ └──────────┘ └──────────┘ └──────────┘                 │
│                                                                      │
│ Selected animation states: Idle [▼] Active [▼] Success [▼] Error [▼]│
│                                                   [Upload to device] │
└──────────────────────────────────────────────────────────────────────┘
```

## Keys Page

```text
┌──────────────────────────────────────────────────────────────────────┐
│ Keys                                                                │
│ Physical order: Not calibrated                                      │
│   [ k1 ]           [ k2 ]           [ k3 ]           [ k4 ]         │
│                                                                      │
│ Selected: k1                                                        │
│ Click        [Voice input ▾]                                        │
│ Double click [None ▾]                                               │
│ Long press   [Screen next ▾]                                        │
│                                                                      │
│ [Test action]                                         [Save mapping]│
└──────────────────────────────────────────────────────────────────────┘
```

## Audio Page

```text
┌──────────────────────────────────────────────────────────────────────┐
│ Audio                                                               │
│ State: Ready          Packets: 0                                    │
│ Replacement: Use configured voice key k4                            │
│ Interaction: (•) Hold to talk  ( ) Click to talk                    │
│ Save recordings [ ]   Application Support/VibeKeyboard/Recordings     │
│ Recognition: Unavailable until a reviewed provider contract exists  │
│ Last recording: …                                                    │
└──────────────────────────────────────────────────────────────────────┘
```

## Firmware Page

```text
┌──────────────────────────────────────────────────────────────────────┐
│ Firmware                                                            │
│ Current: …       Running slot: …       Update: Unavailable          │
│ Reason: bootloader migration required                               │
│ Local backup evidence: Verified / Missing / Stale                    │
│ Candidate image: Not selected                                       │
│                                                                      │
│ [Select image…] [Validate] [Stage…]       [Activate… disabled]       │
│                                                                      │
│ Stage never activates. Activation requires a second confirmation.    │
└──────────────────────────────────────────────────────────────────────┘
```

## Production Composition

```text
VibeKeyboardApplication (@main)
  └─ @MainActor AppModel
       ├─ USBDeviceMonitor (VID 303a / PID 1001)
       ├─ USBSession (USB Serial/JTAG only)
       ├─ AssetTransferService / ScreenConfigurationService
       ├─ KeyMappingRepository
       └─ AudioRecordingSession → Ogg Opus

Views
  └─ render typed AppModel state only
       └─ never parse frames, encode JSON, open descriptors, or fabricate capability success
```

The executable follows the six-page layout above. Screen and asset controls require a current-epoch available capability snapshot. LED configuration is omitted until a calibrated typed LED capability and reviewed service exist. Firmware stage/activate and storage format remain disabled until their separate evidence and authorization gates exist.

## Interactions

| Input | Scope | Behavior |
|---|---|---|
| `Cmd+,` | App | Open settings |
| `Cmd+O` | Screen/Pets | Import asset |
| `Esc` | Modal | Cancel without mutation |
| `Return` | Confirm dialog | Execute only when validation passes |
| Device key event | Keys/action router | Highlight the canonical key, classify the gesture once, then execute the saved typed action |
| Screen mode commit | Screen/Pets | Build a capability-bound typed image, pet, dashboard, or custom commit and show its canonical preview |
| Widget update | Dashboard/Custom | Send one bounded typed update for the configured revision; never resend the layout |
| Save recordings | Audio | Atomically write private `.ogg` files under Application Support; keep no raw PCM |

## State Variants

- **Disconnected**: pages remain inspectable; send/test/record controls are disabled; reconnect is primary.
- **Connecting**: connection card shows handshake stage; no silent fallback.
- **Protocol incompatible**: exact device/client versions and recovery action are shown.
- **Upload busy**: progress includes bytes and verification stage; existing device asset stays active.
- **Upload error**: error and retry are shown; partial asset is never activated.
- **Recording**: global recording indicator stays visible across pages.
- **Firmware write**: all other device actions are disabled until completion/recovery.
- **Replacement recording**: no host start command is invented. When host-start is not negotiated, the page instructs the user to use the configured canonical voice key and only consumes key-controlled `AudioFrame` values. Vendor mode may use only the separately verified `ui_state` lifecycle.
- **Keys before calibration**: labels remain canonical `k1...k4`; no front-panel left-to-right order is claimed.
- **Recognition**: no fake provider is shown. ASR remains unavailable until provider, privacy, cancellation, retention, Keychain, and error contracts pass review.
- **LED before calibration/unavailable**: show the typed reason only in Device capability details; hide enable/brightness controls and never send config. Firmware remains all-off.
- **LED after calibrated capability**: Device capability details may show an enable toggle and bounded brightness control from `max_brightness`. Each query/config uses a nonzero current-epoch request ID; changed state is shown only after the matching `source:"applied"` response, never from a query response or host write completion. The Swift actor accepts an applied response only when its captured monotonic timestamp is strictly before the 1,000 ms absolute deadline; exact-deadline/later responses cannot complete the operation. No pixel, color, palette, animation, or calibration editor exists.
- **Firmware mutation**: bootstrap and replacement staged update are distinct paths. Stage and activate have separate validation, confirmations, evidence, and recovery states; neither `provisioned` nor `replacement_protocol` grants authorization.

## Size Constraints

- Minimum window: 900×620 points.
- Sidebar: 160–210 points.
- Preview keeps a 428:142 aspect ratio and uses nearest-neighbor scaling for pixel assets.
- At narrow widths, page inspectors move below the preview instead of overlaying it.
