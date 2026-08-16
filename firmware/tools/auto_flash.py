#!/usr/bin/env python3
"""Safely flash only the custom application partition.

Usage:
  Activate ESP-IDF 5.5.2 first.
  python3 auto_flash.py [build_directory] [/dev/cu.usbmodem...]
"""
import glob
import hashlib
import os
import subprocess
import sys

if len(sys.argv) > 3:
    raise SystemExit("Usage: auto_flash.py [build_directory] [/dev/cu.usbmodem...]")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = (
    os.path.abspath(os.path.expanduser(sys.argv[1]))
    if len(sys.argv) > 1
    else os.path.join(ROOT, "build")
)
PORT = sys.argv[2] if len(sys.argv) > 2 else None

def resolve_ports():
    if PORT is not None:
        if not os.path.exists(PORT):
            raise SystemExit(f"ERROR: Serial port does not exist: {PORT}")
        candidates = [PORT]
    else:
        candidates = sorted(glob.glob("/dev/cu.usbmodem*"))
        if len(candidates) != 1:
            found = ", ".join(candidates) if candidates else "none"
            raise SystemExit(
                f"ERROR: Expected exactly one USB modem port, found: {found}. "
                "Pass the exact port as the second argument."
            )
    alternate = candidates[0].replace("/dev/cu.", "/dev/tty.")
    if alternate != candidates[0] and os.path.exists(alternate):
        candidates.append(alternate)
    return candidates

def validate_build():
    app = os.path.join(BUILD_DIR, "vibe_keyboard.bin")
    if not os.path.isfile(app):
        print(f"ERROR: Missing build artifact: {app}")
        return False
    sdkconfig = os.path.join(BUILD_DIR, "config", "sdkconfig.h")
    if not os.path.isfile(sdkconfig):
        print(f"ERROR: Missing generated config: {sdkconfig}")
        return False
    with open(sdkconfig, encoding="utf-8") as stream:
        compiled_config = stream.read()
    if "#define CONFIG_FREERTOS_HZ 1000" not in compiled_config:
        print("ERROR: Refusing to flash a build not compiled for a 1000 Hz FreeRTOS tick rate")
        return False
    if "#define CONFIG_ESP_MAIN_TASK_STACK_SIZE 8192" not in compiled_config:
        print("ERROR: Refusing to flash a build without the admitted 8192-byte main task stack")
        return False
    if "#define CONFIG_SPIRAM_XIP_FROM_PSRAM 1" not in compiled_config:
        print("ERROR: Refusing to flash a build without PSRAM XIP")
        return False
    if "#define CONFIG_SPIFFS_OBJ_NAME_LEN 96" not in compiled_config:
        print("ERROR: Refusing to flash a build with truncated SPIFFS asset names")
        return False
    composite_usb_contract = (
        "#define CONFIG_USB_DEVICE_UAC_AS_PART 1",
        "#define CONFIG_UAC_MIC_CHANNEL_NUM 1",
        "#define CONFIG_UAC_SAMPLE_RATE 16000",
        "#define CONFIG_UAC_SUPPORT_MACOS 1",
    )
    if any(item not in compiled_config for item in composite_usb_contract):
        print("ERROR: Refusing to flash a build without the admitted CDC/UAC microphone profile")
        return False
    panic_modes = (
        "#define CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT 1",
        "#define CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT 1",
    )
    if not any(mode in compiled_config for mode in panic_modes):
        print("ERROR: Refusing build without an admitted panic reboot policy")
        return False
    with open(app, "rb") as stream:
        digest = hashlib.sha256(stream.read()).hexdigest()
    print(f"Validated app SHA-256: {digest}")
    return app

def probe_device(port, before):
    result = subprocess.run(
        [sys.executable, "-m", "esptool", "--chip", "esp32s3",
         "--port", port, "--baud", "115200",
         "--before", before, "--after", "no_reset", "--no-stub",
         "chip_id"],
        timeout=15,
    )
    return result.returncode == 0

def check_device(port):
    if probe_device(port, "no_reset"):
        return True
    print(f"Manual ROM probe failed on {port}; trying USB automatic reset.")
    return probe_device(port, "default_reset")

def do_flash(port, app):
    print("\n>>> FLASHING APPLICATION PARTITION ONLY <<<")
    written = subprocess.run(
        [sys.executable, "-m", "esptool", "--chip", "esp32s3",
         "--port", port, "--baud", "460800",
         "--before", "no_reset", "--after", "no_reset",
         "write_flash", "0x20000", app],
    )
    if written.returncode != 0:
        return False
    verified = subprocess.run(
        [sys.executable, "-m", "esptool", "--chip", "esp32s3",
         "--port", port, "--baud", "460800",
         "--before", "no_reset", "--after", "watchdog_reset",
         "verify_flash", "0x20000", app],
    )
    return verified.returncode == 0

app = validate_build()
if not app:
    sys.exit(2)
try:
    port = next((candidate for candidate in resolve_ports() if check_device(candidate)), None)
    if port is None:
        raise SystemExit(
            "ERROR: Automatic ROM entry failed. Hold K1 while reconnecting USB, "
            "then run this command again."
        )
    print(f"Using {port}")
    if not do_flash(port, app):
        raise SystemExit("ERROR: Flash or read-back verification failed.")
except subprocess.TimeoutExpired:
    raise SystemExit(
        "ERROR: Automatic ROM entry timed out. Hold K1 while reconnecting USB, "
        "then run this command again."
    )
print("\nFLASH AND READ-BACK VERIFICATION COMPLETE")
print("If the ROM stub remains active, release K1 and power-cycle once without pressing any button.")
