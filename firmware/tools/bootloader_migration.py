#!/usr/bin/env python3
"""Offline-only bootloader migration artifact validation.

This module has no serial, reset, flash, or device transport. It validates an
immutable review artifact that must exist before a separately authorized write.
"""

from __future__ import annotations

import dataclasses
import hashlib
import json
import re
import struct
from collections.abc import Mapping
from typing import Any

BOOTLOADER_OFFSET = 0x000000
BOOTLOADER_REGION_SIZE = 0x008000
PARTITION_TABLE_OFFSET = 0x008000
PARTITION_TABLE_SIZE = 0x001000
OTADATA_OFFSET = 0x011000
OTADATA_SIZE = 0x002000
OTA0_OFFSET = 0x020000
OTA1_OFFSET = 0x520000
OTA_SLOT_SIZE = 0x500000
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
EXPECTED_PROTECTED = {
    "partition_table": (PARTITION_TABLE_OFFSET, PARTITION_TABLE_SIZE),
    "nvs": (0x009000, 0x004000),
    "nvs_keys": (0x00D000, 0x003000),
    "phy_init": (0x010000, 0x001000),
    "otadata": (OTADATA_OFFSET, OTADATA_SIZE),
    "ota_0": (OTA0_OFFSET, OTA_SLOT_SIZE),
    "ota_1": (OTA1_OFFSET, OTA_SLOT_SIZE),
    "storage": (0xA20000, 0x5E0000),
}


class MigrationValidationError(ValueError):
    pass


@dataclasses.dataclass(frozen=True)
class MigrationArtifact:
    version: int
    chip: str
    bootloader_offset: int
    bootloader_size: int
    bootloader_sha256: str
    partition_table_sha256: str
    secure_boot: str
    flash_encryption: str
    anti_rollback: str
    rom_recovery: str
    managed_slots: tuple[tuple[str, int, int], ...]
    protected_regions: tuple[tuple[str, int, int], ...]
    rollback_policy: str
    first_boot_tests: tuple[str, ...]
    artifact_sha256: str


def _canonical(value: Mapping[str, Any]) -> bytes:
    try:
        encoded = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    except (TypeError, ValueError, UnicodeError) as error:
        raise MigrationValidationError("artifact is not canonical JSON data") from error
    if len(encoded) > 8192:
        raise MigrationValidationError("artifact exceeds bounded size")
    return encoded


def _sha(value: Any, field: str) -> str:
    if not isinstance(value, str) or not SHA256_RE.fullmatch(value):
        raise MigrationValidationError(f"{field} must be lowercase SHA-256")
    return value


def build_artifact(*, bootloader: bytes, partition_table: bytes, security: Mapping[str, str], rom_recovery: str) -> dict[str, Any]:
    if not bootloader or len(bootloader) > BOOTLOADER_REGION_SIZE:
        raise MigrationValidationError("bootloader does not fit exact reserved region")
    if len(partition_table) != 0xC00:
        raise MigrationValidationError("partition table must be exact reviewed 0xC00 bytes")
    if bootloader[0] != 0xE9 or len(bootloader) < 24:
        raise MigrationValidationError("bootloader image header is invalid")
    segment_count = bootloader[1]
    chip_id = struct.unpack_from("<H", bootloader, 12)[0]
    if segment_count == 0 or segment_count > 16 or chip_id != 9:
        raise MigrationValidationError("bootloader segment count or chip identity is invalid")
    if set(security) != {"secure_boot", "flash_encryption", "anti_rollback"} or any(
        value not in {"disabled", "reviewed_compatible"} for value in security.values()
    ):
        raise MigrationValidationError("security evidence is incomplete or unreviewed")
    if rom_recovery != "reviewed_external_artifact":
        raise MigrationValidationError("ROM recovery evidence is missing")
    artifact: dict[str, Any] = {
        "version": 1,
        "chip": "esp32s3",
        "bootloader": {"offset": BOOTLOADER_OFFSET, "size": len(bootloader), "sha256": hashlib.sha256(bootloader).hexdigest()},
        "partition_table_sha256": hashlib.sha256(partition_table).hexdigest(),
        "security": dict(security),
        "rom_recovery": rom_recovery,
        "managed_slots": [
            {"name": "ota_0", "offset": OTA0_OFFSET, "size": OTA_SLOT_SIZE},
            {"name": "ota_1", "offset": OTA1_OFFSET, "size": OTA_SLOT_SIZE},
        ],
        "protected_regions": [
            {"name": name, "offset": offset, "size": size}
            for name, (offset, size) in EXPECTED_PROTECTED.items()
        ],
        "rollback_policy": "bootloader_pending_verify",
        "first_boot_tests": ["partition_identity", "usb_core", "watchdog", "heap"],
    }
    artifact["artifact_sha256"] = hashlib.sha256(_canonical(artifact)).hexdigest()
    return artifact


def validate_artifact(artifact: Mapping[str, Any], bootloader: bytes, partition_table: bytes) -> MigrationArtifact:
    required = {"version", "chip", "bootloader", "partition_table_sha256", "security", "rom_recovery", "managed_slots", "protected_regions", "rollback_policy", "first_boot_tests", "artifact_sha256"}
    if set(artifact) != required:
        raise MigrationValidationError("artifact keys are not exact")
    binding = dict(artifact)
    claimed_binding = _sha(binding.pop("artifact_sha256"), "artifact_sha256")
    if hashlib.sha256(_canonical(binding)).hexdigest() != claimed_binding:
        raise MigrationValidationError("artifact binding differs")
    if artifact["version"] != 1 or artifact["chip"] != "esp32s3":
        raise MigrationValidationError("artifact target is unsupported")
    boot = artifact["bootloader"]
    if not isinstance(boot, Mapping) or set(boot) != {"offset", "size", "sha256"}:
        raise MigrationValidationError("bootloader identity is invalid")
    if boot["offset"] != BOOTLOADER_OFFSET or not isinstance(boot["size"], int) or isinstance(boot["size"], bool) or boot["size"] != len(bootloader) or not bootloader or len(bootloader) > BOOTLOADER_REGION_SIZE:
        raise MigrationValidationError("bootloader range is invalid")
    boot_hash = _sha(boot["sha256"], "bootloader.sha256")
    if bootloader[0] != 0xE9 or len(bootloader) < 24 or bootloader[1] == 0 or bootloader[1] > 16 or struct.unpack_from("<H", bootloader, 12)[0] != 9 or hashlib.sha256(bootloader).hexdigest() != boot_hash:
        raise MigrationValidationError("bootloader identity differs")
    table_hash = _sha(artifact["partition_table_sha256"], "partition_table_sha256")
    if len(partition_table) != 0xC00 or hashlib.sha256(partition_table).hexdigest() != table_hash:
        raise MigrationValidationError("partition table identity differs")
    security = artifact["security"]
    if not isinstance(security, Mapping) or set(security) != {"secure_boot", "flash_encryption", "anti_rollback"}:
        raise MigrationValidationError("security evidence is incomplete")
    for key, value in security.items():
        if value not in {"disabled", "reviewed_compatible"}:
            raise MigrationValidationError(f"{key} is not reviewed")
    recovery = artifact["rom_recovery"]
    if recovery != "reviewed_external_artifact":
        raise MigrationValidationError("ROM recovery evidence is missing")
    slots = artifact["managed_slots"]
    expected_slots = [{"name": "ota_0", "offset": OTA0_OFFSET, "size": OTA_SLOT_SIZE}, {"name": "ota_1", "offset": OTA1_OFFSET, "size": OTA_SLOT_SIZE}]
    if slots != expected_slots:
        raise MigrationValidationError("managed slots differ")
    regions = artifact["protected_regions"]
    expected_regions = [{"name": name, "offset": value[0], "size": value[1]} for name, value in EXPECTED_PROTECTED.items()]
    if regions != expected_regions:
        raise MigrationValidationError("protected ranges differ")
    if artifact["rollback_policy"] != "bootloader_pending_verify":
        raise MigrationValidationError("rollback policy differs")
    tests = artifact["first_boot_tests"]
    if tests != ["partition_identity", "usb_core", "watchdog", "heap"]:
        raise MigrationValidationError("mandatory first-boot tests differ")
    return MigrationArtifact(1, "esp32s3", 0, len(bootloader), boot_hash, table_hash,
        security["secure_boot"], security["flash_encryption"], security["anti_rollback"], recovery,
        tuple((item["name"], item["offset"], item["size"]) for item in slots),
        tuple((item["name"], item["offset"], item["size"]) for item in regions),
        artifact["rollback_policy"], tuple(tests), claimed_binding)
