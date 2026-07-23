# Hardware Contract

Status: Partially verified from device descriptors, boot log, full flash backup, and ESP32-S3 firmware disassembly

## Verified Device Identity

| Property | Value | Evidence |
|---|---:|---|
| USB vendor | Espressif | USB descriptor |
| VID | `0x303a` | USB descriptor |
| PID | `0x1001` | USB descriptor |
| Product | USB JTAG/serial debug unit | USB descriptor |
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
| Host audio codec | AFE-processed mono Opus | vendor pipeline, `AudioFrame` parser and Ogg Opus muxer |
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

The complete 119-entry initialization table, startup order, panel power commands, and LVGL display-buffer configuration are in [NV3007 Panel Profile](../firmware/nv3007.md). The table ends with `SLPOUT 0x11` plus 220 ms and does not contain `DISPON 0x29`; the replacement board lifecycle then invokes the separate recovered display-on method. GPIO8 is not a display pin; earlier inference from an immediate value `8` confused command-bit/PWM-resolution fields with GPIO identity.

## Verified Buttons

Firmware parallel tables and their indexed initialization loop prove:

| Logical key | GPIO |
|---|---:|
| `k1` | 0 |
| `k2` | 18 |
| `k3` | 17 |
| `k4` | 16 |

All four keys are active-low inputs with internal pull-ups. The vendor button component scans at 5 ms, requires two stable samples (approximately 10 ms debounce), and uses a low-level GPIO wake interrupt only to resume the periodic scanner from its power-save idle state. The application registers press-down and press-up callbacks; it does not emit events directly from the ISR. Static evidence still cannot prove physical left-to-right order.

## Verified LEDs

The board uses one 17-pixel addressable chain:

```text
GPIO8 → SK6812 / GRB / RMT TX at 10 MHz
raw pixels 0...3  → four key LEDs
raw pixels 4...16 → thirteen strip LEDs
```

RMT DMA and output inversion are disabled. Vendor state animation refreshes every 30 ms and limits each channel's per-tick change to 32, but these are animation policy rather than hardware requirements. Physical chain direction, key-to-front-panel order, and safe total-current/full-brightness limits remain unverified. Initial replacement-firmware acceptance keeps all pixels off, then uses a single low-brightness, one-color test pixel; it must not assume that channel value 255 is safe for 17-pixel full white. Product ownership, USB ABI, fail-dark behavior, and the separately authorized calibration procedure are defined only by [LED Feedback Contract](led.md).

## USB-Only Product Contract

The product uses the ESP32-S3 built-in USB Serial/JTAG CDC callout for all host communication.

The vendor firmware internally uses BLE-oriented component names and contains a BLE GATT implementation. It also implements a `/dev/usbserjtag` `usb_mirror` task that reuses the vendor framing. These names and dormant capabilities do not change the product decision: the new macOS client and replacement firmware must operate completely over USB and expose no Bluetooth dependency or fallback.

Audio is custom Opus frames over the USB serial protocol, not USB Audio Class.

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

The current device's entire `storage` partition is erased `0xff`: 6,160,384 bytes, SHA-256 `017a07e77208410239de8aa090548c9611a682425bb4096de64ed2c0c1980064`, with no filesystem metadata or used sectors. The vendor image contains no proven dynamic image/pet asset protocol. Replacement firmware must own first format, filesystem versioning, and recovery.

## Recovery Backup

A complete read-only 16 MiB backup was acquired before any firmware implementation or write:

```text
SHA-256: 09aba4543ae8fdb5b6cd91fe989539a94eb626e197bb02853f43dceead7ab1b1
Size: 16777216 bytes
```

The private copy lives outside the workspace at:

```text
~/Library/Application Support/VibeKeyboard/Backups/020000000001/2026-07-21/
```

The directory is mode `0700`, files are mode `0600`, and a private manifest contains hashes and a recovery command template marked `NOT EXECUTED`. NVS and NVS-key material are sensitive and must never be copied into source control, logs, test fixtures, or shared artifacts.

## Unverified Hardware Details

The following hardware details remain unverified:

- Four-key physical left-to-right order.
- LED physical chain direction, key-to-front-panel order, and measured current/brightness constraints.
- Physical microphone count/slot-to-aperture mapping, exact ESP-SR version, named meanings of vendor AFE config overrides, and runtime AFE chunk sizes.

No firmware implementation may guess these values.

## First-Write Bootstrap Safety Gate

Backup acquisition is complete, but no first write is authorized yet. First replacement application bootstrap follows [USB Protocol Contract](usb-protocol.md#first-write-bootstrap-not-the-update-protocol) and `firmware-bootstrap-001`.

Before staging `ota_1`, all gates are mandatory:

1. reverify the private full-backup manifest and hashes for partition table, bootloader, both otadata sectors, `ota_0`, NVS, NVS keys, PHY, and storage;
2. re-enter the proven ESP32-S3 ROM download path and re-identify exact chip and port;
3. perform named read-only Secure Boot, flash-encryption, anti-rollback, and eFuse inspection; unknown or incompatible state fails closed;
4. independently pass the hardware firmware review and validate candidate target, checksum, appended hash, descriptor, revision, size, and partition boundaries;
5. stage by writing only exact candidate bytes to inactive `ota_1 @ 0x520000`; read back exact length and verify bytes/SHA/image metadata;
6. prove bootloader, partition table, `ota_0`, primary otadata, NVS, NVS keys, PHY, and storage stayed unchanged;
7. separately review and authorize activation, which writes only the still-erased secondary otadata sector using a reviewed ESP-IDF 5.5.2 golden record selecting ota_1 and verifies readback/CRC/decode;
8. retain the verified primary vendor otadata record and vendor `ota_0` recovery image.

The first bootstrap does not replace the vendor bootloader. Vendor automatic rollback is unproven, so success must not be described as `PENDING_VERIFY`; recovery relies on the proven ROM path and private restore artifacts. Installing a rollback-enabled replacement bootloader is a later independent high-risk migration gate. No first-write task may run merely because application firmware or `vk_update` implementation passes.
