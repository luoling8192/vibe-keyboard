#!/usr/bin/env python3
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
BUILD = ROOT / (sys.argv[1] if len(sys.argv) > 1 else "build")
artifacts = [
    BUILD / "bootloader/bootloader.bin",
    BUILD / "partition_table/partition-table.bin",
    BUILD / "vibe_keyboard.bin",
]
for artifact in artifacts:
    if not artifact.is_file():
        raise SystemExit(f"missing build artifact: {artifact}")

app_size = artifacts[-1].stat().st_size
app_partition_size = 0x500000
if app_size > app_partition_size:
    raise SystemExit(f"app image exceeds OTA slot: {app_size} > {app_partition_size}")

compiled_sdkconfig = (BUILD / "config/sdkconfig.h").read_text()
required_definitions = [
    '#define CONFIG_IDF_TARGET "esp32s3"',
    "#define CONFIG_ESPTOOLPY_FLASHSIZE_16MB 1",
    "#define CONFIG_SPIRAM 1",
    "#define CONFIG_SPIRAM_MODE_OCT 1",
    "#define CONFIG_SPIRAM_SPEED_80M 1",
    "#define CONFIG_SPIRAM_XIP_FROM_PSRAM 1",
    "#define CONFIG_SPIRAM_FETCH_INSTRUCTIONS 1",
    "#define CONFIG_SPIRAM_RODATA 1",
    "#define CONFIG_PARTITION_TABLE_CUSTOM 1",
    "#define CONFIG_ESP_CONSOLE_NONE 1",
    "#define CONFIG_ESP_CONSOLE_SECONDARY_NONE 1",
    "#define CONFIG_BOOTLOADER_LOG_LEVEL_NONE 1",
    "#define CONFIG_LOG_DEFAULT_LEVEL_NONE 1",
    "#define CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT 1",
    "#define CONFIG_FREERTOS_HZ 1000",
    "#define CONFIG_ESP_MAIN_TASK_STACK_SIZE 8192",
    "#define CONFIG_SPIFFS_OBJ_NAME_LEN 96",
]
missing = [item for item in required_definitions if item not in compiled_sdkconfig]
if missing:
    raise SystemExit(f"missing compiled sdkconfig contracts: {missing}")
if "#define CONFIG_BT_ENABLED" in compiled_sdkconfig:
    raise SystemExit("Bluetooth is enabled in the compiled sdkconfig")

image_info = subprocess.run(
    [sys.executable, "-m", "esptool", "--chip", "esp32s3", "image_info", str(artifacts[-1])],
    check=True,
    capture_output=True,
    text=True,
).stdout
if "Checksum:" not in image_info or "(valid)" not in image_info or "Validation Hash:" not in image_info:
    raise SystemExit("application image checksum/hash validation failed")

nm = os.environ.get("NM", "xtensa-esp32s3-elf-nm")
symbols = subprocess.run([nm, "-g", str(BUILD / "vibe_keyboard.elf")], check=True, capture_output=True, text=True).stdout
forbidden_symbol_prefixes = (
    "esp_wifi_", "esp_bt_", "esp_ble_", "nimble_", "ble_",
    "esp_http_", "httpd_", "mqtt_", "esp_mqtt_",
    "tinyusb_", "tusb_", "tud_", "tuh_", "usb_host_", "uac_",
)
forbidden_symbols = {
    "socket", "socketpair", "bind", "listen", "accept", "connect",
    "send", "sendto", "recv", "recvfrom", "getaddrinfo", "freeaddrinfo",
    "lwip_socket", "lwip_connect", "lwip_send", "lwip_recv",
}
linked_names = {line.split()[-1] for line in symbols.splitlines() if line.split()}
linked_forbidden = sorted(name for name in linked_names
                          if name in forbidden_symbols or
                          name.startswith(forbidden_symbol_prefixes))
if linked_forbidden:
    raise SystemExit(f"forbidden network/Bluetooth entry points linked: {linked_forbidden}")

objdump = os.environ.get(
    "OBJDUMP",
    nm.removesuffix("-nm") + "-objdump" if nm.endswith("-nm") else "xtensa-esp32s3-elf-objdump",
)
instruction_dump = subprocess.run(
    [objdump, "-d", str(BUILD / "vibe_keyboard.elf")],
    check=True,
    capture_output=True,
    text=True,
).stdout
stack_frame_limits = {
    "production_task": 256,
    "app_main": 512,
    "vk_usb_service_poll": 512,
    "vk_screen_product_prepare": 1536,
    "validate_revision": 512,
    "validate_commit": 768,
    "vk_asset_store_recover": 256,
    "manifest_references": 512,
    "vk_asset_store_catalog": 512,
    "consume": 512,
    "dispatch_json": 768,
    "dispatch_screen": 1280,
    "dispatch_chunk": 512,
    "send_device_info": 1280,
    "vk_screen_service_handle_screen": 256,
    "manifest_node": 768,
    "manifest_object": 1792,
    "publish_screen_revision": 256,
    "vk_asset_store_publish_revision": 768,
    "revision_references_valid": 768,
    "remove_audio_session_locked": 512,
    "vk_screen_commit": 512,
    "vk_asset_transfer_handle_command": 3072,
    "vk_asset_transfer_catalog_page": 3072,
    "vk_audio_backend_capture": 4608,
}
stack_frames = {}
for name, maximum in stack_frame_limits.items():
    body_match = re.search(
        rf"^[0-9a-f]+ <{re.escape(name)}>:\n(.*?)(?=^\s*$)",
        instruction_dump,
        re.MULTILINE | re.DOTALL,
    )
    if body_match is None:
        raise SystemExit(f"missing stack-audited symbol: {name}")
    body = body_match.group(1)
    if re.search(r"\bmovsp\s+a1\b", body):
        raise SystemExit(f"extended/dynamic stack frame in audited symbol: {name}")
    entry_match = re.search(r"\bentry\s+a1,\s*(0x[0-9a-f]+|\d+)", body)
    if entry_match is None:
        raise SystemExit(f"missing stack frame prologue: {name}")
    frame_bytes = int(entry_match.group(1), 0)
    if frame_bytes > maximum:
        raise SystemExit(f"stack frame exceeds contract: {name} {frame_bytes} > {maximum}")
    stack_frames[name] = frame_bytes

usb_source = (ROOT / "components/vk_usb/vk_usb.c").read_text()
usb_stack_match = re.search(r"#define\s+VK_USB_TASK_STACK_BYTES\s+(\d+)U", usb_source)
if usb_stack_match is None:
    raise SystemExit("missing USB task stack contract")
usb_task_stack_bytes = int(usb_stack_match.group(1))
usb_screen_stack_estimate = sum(stack_frames[name] for name in (
    "production_task", "vk_usb_service_poll", "consume", "dispatch_json", "dispatch_screen",
    "vk_screen_service_handle_screen", "vk_screen_commit", "publish_screen_revision",
)) + 8 * (stack_frames["manifest_node"] + stack_frames["manifest_object"])
if usb_task_stack_bytes < usb_screen_stack_estimate + 4096:
    raise SystemExit(
        f"USB task stack lacks screen recursion margin: "
        f"{usb_task_stack_bytes} < {usb_screen_stack_estimate + 4096}"
    )

app_bytes = artifacts[-1].read_bytes()
forbidden_patterns = {
    "colon MAC literal": re.compile(
        rb"(?i)(?<![0-9a-f])(?:[0-9a-f]{2}:){5}[0-9a-f]{2}(?![0-9a-f])"
    ),
    "firmware device ID literal": re.compile(
        rb"(?i)(?<![0-9a-f])VS-[0-9a-f]{12}(?![0-9a-f])"
    ),
    "serial device path": re.compile(rb"/dev/cu\.usbmodem"),
    "device secret marker": re.compile(rb"device_secret"),
}
found_markers = [
    name for name, pattern in forbidden_patterns.items() if pattern.search(app_bytes)
]
if found_markers:
    raise SystemExit(f"sensitive markers found in application image: {found_markers}")

result = {
    "target": "esp32s3",
    "configured_flash_bytes": 16 * 1024 * 1024,
    "configured_psram_bytes": 8 * 1024 * 1024,
    "app_partition_bytes": app_partition_size,
    "forbidden_linked_entrypoints": linked_forbidden,
    "stack_frames_bytes": stack_frames,
    "usb_screen_stack_estimate_bytes": usb_screen_stack_estimate,
    "usb_task_stack_bytes": usb_task_stack_bytes,
    "sensitive_markers_found": found_markers,
    "artifacts": {
        str(path.relative_to(BUILD)): {
            "size": path.stat().st_size,
            "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        }
        for path in artifacts
    },
}
print(json.dumps(result, indent=2, sort_keys=True))
