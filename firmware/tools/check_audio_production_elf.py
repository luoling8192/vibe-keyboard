#!/usr/bin/env python3
import os
import pathlib
import subprocess
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: check_audio_production_elf.py PATH_TO_ELF")

elf = pathlib.Path(sys.argv[1])
if not elf.is_file():
    raise SystemExit(f"missing ELF: {elf}")

nm = os.environ.get("NM", "xtensa-esp32s3-elf-nm")
symbols = subprocess.run([nm, "-g", str(elf)], check=True, capture_output=True, text=True).stdout
names = {line.split()[-1] for line in symbols.splitlines() if line.split()}
required = {
    "vk_audio_init",
    "vk_audio_prepare",
    "vk_audio_release",
    "vk_audio_cancel_prepared",
    "vk_audio_stop",
    "vk_audio_abort",
    "vk_usb_send_audio",
    "i2s_new_channel",
    "i2s_channel_init_pdm_rx_mode",
    "i2s_channel_enable",
    "i2s_channel_read",
    "i2s_channel_disable",
    "i2s_del_channel",
    "afe_config_init",
    "afe_config_free",
    "esp_afe_handle_from_config",
    "opus_encoder_create",
    "opus_encoder_ctl",
    "opus_encode",
    "opus_encoder_destroy",
    "usb_serial_jtag_driver_install",
    "usb_serial_jtag_read_bytes",
    "usb_serial_jtag_write_bytes",
}
missing = sorted(required - names)
if missing:
    raise SystemExit(f"production audio symbols missing from ELF: {missing}")

forbidden = {
    "esp_wifi_init",
    "esp_bt_controller_init",
    "nimble_port_init",
    "httpd_start",
    "esp_http_client_init",
    "socket",
    "tinyusb_driver_install",
    "tusb_init",
    "tud_audio_n_write",
    "tud_audio_write",
}
linked = sorted(forbidden & names)
if linked:
    raise SystemExit(f"forbidden BLE/network/TinyUSB/UAC entry points linked: {linked}")

print("production audio ELF verified: I2S/AFE/Opus/USB Serial-JTAG linked; BLE/network/TinyUSB/UAC entry points absent")
