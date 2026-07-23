#!/usr/bin/env python3
import os
import pathlib
import subprocess
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: check_audio_probe_elf.py PATH_TO_ELF")

elf = pathlib.Path(sys.argv[1])
if not elf.is_file():
    raise SystemExit(f"missing ELF: {elf}")

nm = os.environ.get("NM", "xtensa-esp32s3-elf-nm")
symbols = subprocess.run([nm, "-g", str(elf)], check=True, capture_output=True, text=True).stdout
names = {line.split()[-1] for line in symbols.splitlines() if line.split()}
required = {
    "vk_audio_dependency_probe",
    "afe_config_init",
    "afe_config_free",
    "esp_afe_handle_from_config",
    "opus_encoder_create",
    "opus_encoder_ctl",
    "opus_encode",
    "opus_encoder_destroy",
}
missing = sorted(required - names)
if missing:
    raise SystemExit(f"audio dependency symbols missing from ELF: {missing}")

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
}
linked = sorted(forbidden & names)
if linked:
    raise SystemExit(f"forbidden BLE/network/UAC entry points linked: {linked}")

print("audio dependency probe ELF verified: named AFE/Opus symbols linked; BLE/network/UAC entry points absent")
