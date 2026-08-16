# Hardware Contract

Status: Partially verified from device descriptors, boot logs, and connected validation

## Verified Device Identity

| Property | Value | Evidence |
|---|---:|---|
| USB vendor | Espressif | USB descriptor |
| VID | `0x303a` | USB descriptor |
| PID | `0x1001` | USB descriptor |
| Vendor/ROM product | USB JTAG/serial debug unit | connected USB descriptor |
| Replacement application product | VibeBoard Microphone + Control (CDC + UAC2) | source/build contract; physical enumeration pending |
| Observed serial | `02:00:00:00:00:01` | USB descriptor |
| Normalized device ID | `020000000001` | vendor app normalization |
| Current macOS path | `/dev/cu.usbmodemXXXX` | device enumeration; path is not stable |
| MCU | ESP32-S3 QFN56 revision 0.2 | ROM loader and boot log |
| Flash | 16 MiB, quad, 3.3 V | ROM loader and boot log |
| PSRAM | 8 MiB octal | ROM loader and boot log |
| Firmware project | `voice_stick` | ESP image metadata |
| Firmware app version | `0.3.8` | ESP image metadata |
| Vendor release version | `0.5.0` | signed-by-hash download manifest; separate version domain |
| ESP-IDF | `v5.5.2` | ESP image metadata |
| Display controller | NV3007 | firmware boot log and strings |
| Display resolution | 428×142 | LVGL boot log and display init constants |
| Pixel format | RGB565 | firmware display evidence |
| LVGL | major version 9 | firmware components/strings |
| Microphone bus | I2S0 PDM RX, GPIO41 clock, GPIO40 data, two 16-bit slots | firmware channel/config construction |
| Audio sample rate | 16 kHz | firmware constants and vendor Ogg Opus muxer |
| Host audio formats | 16 kHz/16-bit/mono UAC2 PCM plus legacy AFE-processed mono Opus sessions | replacement source/build contract; connected UAC validation pending |
| LEDs | one GPIO8 SK6812/GRB chain: 4 key LEDs + 13 strip LEDs | firmware configuration and boot log |

## Verified Display Bus

| Signal/parameter | Value | Confidence |
|---|---:|---|
| SPI MOSI | GPIO21 | high |
| SPI MISO | disabled (`-1`) | high |
| SPI clock | GPIO14 | high |
| LCD CS | GPIO11 | high |
| LCD DC | GPIO13 | high |
| LCD reset | GPIO12, active-low, 100 ms low + 100 ms high delay | high |
| LCD backlight | GPIO9, active-high LEDC PWM, 5 kHz/8-bit | high |
| Pixel clock | 40 MHz, SPI mode 0 | high |
| Panel width | 428 | high |
| Panel height | 142 | high |
| Controller gap | x=0, y=14 | high |
| Orientation | MADCTL `0x60`; correct physical orientation verified on device | high |
| Color | RGB565, LVGL byte swap enabled | high |
| Transaction queue depth | 10 | high |
| Maximum SPI transfer | 121,552 bytes | high |

The complete 119-entry initialization table, startup order, panel power commands, and LVGL display-buffer configuration are in [NV3007 Panel Profile](../firmware/nv3007.md). The table ends with `SLPOUT 0x11` plus 220 ms and does not contain `DISPON 0x29`; the replacement board lifecycle invokes the display-on method separately. GPIO8 is not a display pin.

## Verified Buttons

Firmware parallel tables and their indexed initialization loop prove:

| Logical key | GPIO |
|---|---:|
| `k1` | 0 |
| `k2` | 18 |
| `k3` | 17 |
| `k4` | 16 |

All four keys are active-low inputs with internal pull-ups. The button component scans at 5 ms, requires two stable samples (approximately 10 ms debounce), and uses a low-level GPIO wake interrupt only to resume the periodic scanner from its power-save idle state. The application registers press-down and press-up callbacks; it does not emit events directly from the ISR. Physical left-to-right order still requires connected validation.

## Verified LEDs

The board uses one 17-pixel addressable chain:

```text
GPIO8 → SK6812 / GRB / RMT TX at 10 MHz
raw pixels 0...3  → four key LEDs
raw pixels 4...16 → thirteen strip LEDs
```

RMT DMA and output inversion are disabled. Vendor state animation refreshes every 30 ms and limits each channel's per-tick change to 32, but these are animation policy rather than hardware requirements. Physical chain direction, key-to-front-panel order, and safe total-current/full-brightness limits remain unverified. Initial replacement-firmware acceptance keeps all pixels off, then uses a single low-brightness, one-color test pixel; it must not assume that channel value 255 is safe for 17-pixel full white. Product ownership, USB ABI, fail-dark behavior, and the separately authorized calibration procedure are defined only by [LED Feedback Contract](led.md).

## USB-Only Product Contract

The replacement application software-switches the internal USB PHY to USB OTG
device mode and exposes a composite device: CDC carries the existing framed
control protocol and UAC2 exposes the microphone as 16 kHz, 16-bit mono PCM.
The ROM download path remains USB Serial/JTAG because no USB PHY eFuse or
bootloader is changed.

The macOS client and firmware operate completely over USB and expose no Bluetooth or Wi-Fi dependency or fallback.

Legacy button-triggered recordings remain custom Opus frames over CDC. UAC is a
second host-facing path and is mutually exclusive with a legacy Opus capture
session; opening UAC never disables screen, key, LED, asset, or widget CDC use.

## Flash Layout

Verified partition table:

| Partition | Offset | Size | Purpose |
|---|---:|---:|---|
| `nvs` | `0x009000` | `0x004000` | device configuration |
| `nvs_keys` | `0x00d000` | `0x003000` | encrypted/sensitive device material |
| `phy_init` | `0x010000` | `0x001000` | PHY data |
| `otadata` | `0x011000` | `0x002000` | OTA selection/state |
| `ota_0` | `0x020000` | `0x500000` | active application slot |
| `ota_1` | `0x520000` | `0x500000` | alternate application slot |
| `storage` | `0xa20000` | `0x5e0000` | SPIFFS data partition |

Replacement firmware owns asset storage formatting, filesystem versioning, and recovery. NVS and NVS-key material must never be copied into source control, logs, test fixtures, or shared artifacts.

## Unverified Hardware Details

The following hardware details remain unverified:

- Four-key physical left-to-right order.
- LED physical chain direction, key-to-front-panel order, and measured current/brightness constraints.
- Physical microphone count/slot-to-aperture mapping, exact ESP-SR version, named meanings of vendor AFE config overrides, and runtime AFE chunk sizes.

No firmware implementation may guess these values.

## Flash Safety

The supported installer is `firmware/tools/auto_flash.py`. It validates the
ESP32-S3 application image, writes only `ota_0 @ 0x20000`, and verifies the
written bytes. It must not erase or overwrite the bootloader, partition table,
NVS, NVS keys, PHY data, OTA metadata, or asset storage.

Keep any device backup outside this repository. Broader bootloader or partition
changes are outside the supported workflow.
