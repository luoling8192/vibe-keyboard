#!/usr/bin/env python3
"""Pure, fail-closed validation primitives for the first-write bootstrap.

This module never opens a serial device and contains no mutation transport.
"""

from __future__ import annotations

import dataclasses
import hashlib
import json
import pathlib
import re
import struct
import zlib
from collections.abc import Mapping
from typing import Any

FLASH_SIZE = 0x1000000
SECTOR_SIZE = 0x1000
OTA_SLOT_SIZE = 0x500000
OTA_0_OFFSET = 0x020000
OTA_1_OFFSET = 0x520000
OTADATA_OFFSET = 0x011000
OTADATA_SIZE = 0x002000
SECONDARY_OTADATA_OFFSET = OTADATA_OFFSET + SECTOR_SIZE
STORAGE_OFFSET = 0xA20000
ESP32S3_CHIP_ID = 9
ESP_IMAGE_MAGIC = 0xE9
ESP_APP_DESC_MAGIC = 0xABCD5432
ESP_OTA_IMG_UNDEFINED = 0xFFFFFFFF
ESP32S3_DROM_LOW = 0x3C000000
ESP32S3_DROM_HIGH = 0x3E000000
ESP32S3_IROM_LOW = 0x42000000
ESP32S3_IROM_HIGH = 0x44000000
ESP32S3_EXTRAM_LOW = 0x3C000000
ESP32S3_EXTRAM_HIGH = 0x3E000000
ESP32S3_DEFAULT_MMU_PAGE_SIZE = 0x10000
ESP32S3_REVIEWED_MMU_PAGE_SIZE = 0x10000
ESP_IDF_UINT32_SHIFT_MAX = 31
PARTITION_TABLE_SIZE = 0xC00
PARTITION_ENTRY_MAGIC = 0x50AA
PARTITION_MD5_MAGIC = 0xEBEB
ERASED_U32 = 0xFFFFFFFF
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")

PARTITIONS = {
    "nvs": (0x009000, 0x004000),
    "nvs_keys": (0x00D000, 0x003000),
    "phy_init": (0x010000, 0x001000),
    "otadata": (OTADATA_OFFSET, OTADATA_SIZE),
    "ota_0": (OTA_0_OFFSET, OTA_SLOT_SIZE),
    "ota_1": (OTA_1_OFFSET, OTA_SLOT_SIZE),
    "storage": (STORAGE_OFFSET, 0x5E0000),
}
PROTECTED_STAGE_REGIONS = {
    "bootloader": (0x000000, 0x008000),
    "partition_table": (0x008000, 0x001000),
    **{name: region for name, region in PARTITIONS.items() if name != "ota_1"},
}
PROTECTED_EVIDENCE_NAMES = frozenset(PROTECTED_STAGE_REGIONS)
ACTIVATION_PROTECTED_EVIDENCE_NAMES = frozenset({
    "bootloader",
    "partition_table",
    "nvs",
    "nvs_keys",
    "phy_init",
    "primary_otadata",
    "ota_0",
    "ota_1",
    "storage",
})


class BootstrapValidationError(ValueError):
    pass


@dataclasses.dataclass(frozen=True)
class AppDescriptor:
    project: str
    version: str
    secure_version: int
    min_efuse_block_revision: int
    max_efuse_block_revision: int
    mmu_page_size: int


@dataclasses.dataclass(frozen=True)
class ImageEvidence:
    size: int
    sha256: str
    chip_id: int
    min_revision: int
    max_revision: int
    checksum: int
    validation_hash: str
    descriptor: AppDescriptor


@dataclasses.dataclass(frozen=True)
class OtaSelectEntry:
    sequence: int
    state: int
    crc: int
    valid: bool
    selected_slot: int | None


def _checked_range(offset: int, length: int) -> tuple[int, int]:
    if isinstance(offset, bool) or not isinstance(offset, int) or isinstance(length, bool) or not isinstance(length, int):
        raise BootstrapValidationError("offset and length must be non-boolean integers")
    if offset < 0 or length <= 0 or offset > FLASH_SIZE or length > FLASH_SIZE - offset:
        raise BootstrapValidationError("range is outside the 16 MiB flash")
    return offset, offset + length


def _overlaps(left: tuple[int, int], right: tuple[int, int]) -> bool:
    return left[0] < right[1] and right[0] < left[1]


def validate_stage_range(offset: int, length: int, candidate_size: int) -> None:
    requested = _checked_range(offset, length)
    if isinstance(candidate_size, bool) or not isinstance(candidate_size, int) or candidate_size < 1 or candidate_size > OTA_SLOT_SIZE:
        raise BootstrapValidationError("candidate size is outside ota_1")
    if offset != OTA_1_OFFSET or length != candidate_size:
        raise BootstrapValidationError("stage must write exact candidate bytes at ota_1")
    for name, (start, size) in PROTECTED_STAGE_REGIONS.items():
        if _overlaps(requested, (start, start + size)):
            raise BootstrapValidationError(f"stage overlaps protected region: {name}")


def validate_activation_range(offset: int, length: int) -> None:
    _checked_range(offset, length)
    if OTADATA_OFFSET + OTADATA_SIZE != SECONDARY_OTADATA_OFFSET + SECTOR_SIZE:
        raise BootstrapValidationError("otadata partition geometry is inconsistent")
    if offset != SECONDARY_OTADATA_OFFSET or length != SECTOR_SIZE:
        raise BootstrapValidationError("activation may write only the secondary otadata sector")


def _uint(value: Any, bits: int, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0 or value > (1 << bits) - 1:
        raise BootstrapValidationError(f"{field} must be a non-boolean UInt{bits}")
    return value


def _canonical_sha256(value: Mapping[str, Any]) -> str:
    try:
        encoded = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    except (TypeError, ValueError, UnicodeError) as error:
        raise BootstrapValidationError("evidence is not canonical JSON") from error
    if len(encoded) > 4096:
        raise BootstrapValidationError("canonical evidence exceeds bounded size")
    return hashlib.sha256(encoded).hexdigest()


def _validated_sha256_bytes(raw: bytes, expected_sha256: str, field: str) -> str:
    if not isinstance(expected_sha256, str) or not SHA256_RE.fullmatch(expected_sha256):
        raise BootstrapValidationError(f"reviewed {field} hash is invalid")
    digest = hashlib.sha256(raw).hexdigest()
    if digest != expected_sha256:
        raise BootstrapValidationError(f"{field} differs from reviewed identity")
    return digest


def parse_partition_table(partition_table: bytes) -> dict[str, tuple[int, int]]:
    """Parse the exact ESP-IDF binary table and enforce the reviewed full manifest."""
    if len(partition_table) != PARTITION_TABLE_SIZE:
        raise BootstrapValidationError("partition table must contain exact 0xC00 reviewed bytes")
    parsed: dict[str, tuple[int, int]] = {}
    parsed_entries: list[tuple[int, int, int, int, str, int]] = []
    saw_md5_marker = False
    for offset in range(0, PARTITION_TABLE_SIZE, 32):
        entry = partition_table[offset : offset + 32]
        magic = struct.unpack_from("<H", entry, 0)[0]
        if magic == 0xFFFF:
            raise BootstrapValidationError("partition table has no MD5 marker before erased padding")
        if magic == PARTITION_MD5_MAGIC:
            if saw_md5_marker:
                raise BootstrapValidationError("partition table has duplicate MD5 marker")
            if entry[2:16] != b"\xff" * 14:
                raise BootstrapValidationError("partition table MD5 marker reserved bytes are invalid")
            expected_md5 = hashlib.md5(partition_table[:offset], usedforsecurity=False).digest()
            if entry[16:32] != expected_md5:
                raise BootstrapValidationError("partition table MD5 checksum mismatch")
            saw_md5_marker = True
            if partition_table[offset + 32 :] != b"\xff" * (PARTITION_TABLE_SIZE - offset - 32):
                raise BootstrapValidationError("partition table has bytes after MD5 marker")
            break
        if magic != PARTITION_ENTRY_MAGIC:
            raise BootstrapValidationError("invalid partition table entry magic")
        _, type_value, subtype, part_offset, size, raw_label, flags = struct.unpack("<HBBII16sI", entry)
        label_raw = raw_label.split(b"\0", 1)[0]
        try:
            label = label_raw.decode("ascii")
        except UnicodeDecodeError as error:
            raise BootstrapValidationError("partition label is not ASCII") from error
        if not label or label in parsed:
            raise BootstrapValidationError("partition label is empty or duplicated")
        _checked_range(part_offset, size)
        parsed[label] = (part_offset, size)
        parsed_entries.append((type_value, subtype, part_offset, size, label, flags))
    if not saw_md5_marker:
        raise BootstrapValidationError("partition table has no MD5 marker")
    expected_entries = [
        (1, 2, 0x009000, 0x004000, "nvs", 0),
        (1, 2, 0x00D000, 0x003000, "nvs_keys", 0),
        (1, 1, 0x010000, 0x001000, "phy_init", 0),
        (1, 0, 0x011000, 0x002000, "otadata", 0),
        (0, 0x10, 0x020000, OTA_SLOT_SIZE, "ota_0", 0),
        (0, 0x11, OTA_1_OFFSET, OTA_SLOT_SIZE, "ota_1", 0),
        (1, 0x82, STORAGE_OFFSET, 0x5E0000, "storage", 0),
    ]
    if parsed != PARTITIONS or parsed_entries != expected_entries:
        raise BootstrapValidationError("partition table manifest differs from reviewed identity")
    return parsed


def stage_evidence_binding(
    *,
    image: bytes,
    expected_sha256: str,
    security: Mapping[str, Any],
    partition_table: bytes,
    expected_partition_table_sha256: str,
) -> dict[str, Any]:
    if not image or len(image) > OTA_SLOT_SIZE:
        raise BootstrapValidationError("candidate evidence length is invalid")
    if not isinstance(expected_sha256, str) or not SHA256_RE.fullmatch(expected_sha256):
        raise BootstrapValidationError("reviewed candidate hash is invalid")
    candidate_sha = hashlib.sha256(image).hexdigest()
    if candidate_sha != expected_sha256:
        raise BootstrapValidationError("candidate differs from reviewed identity")
    validate_security_evidence(security)
    parse_partition_table(partition_table)
    security_sha = _canonical_sha256(security)
    partition_sha = _validated_sha256_bytes(partition_table, expected_partition_table_sha256, "partition table")
    evidence = {
        "candidate_sha256": candidate_sha,
        "candidate_size": len(image),
        "security_evidence_sha256": security_sha,
        "partition_table_sha256": partition_sha,
        "partition_table_size": len(partition_table),
    }
    evidence["binding_sha256"] = _canonical_sha256(evidence)
    return evidence


def _mapped_segment(load_addr: int) -> bool:
    return (
        ESP32S3_IROM_LOW <= load_addr < ESP32S3_IROM_HIGH
        or ESP32S3_DROM_LOW <= load_addr < ESP32S3_DROM_HIGH
        or ESP32S3_EXTRAM_LOW <= load_addr < ESP32S3_EXTRAM_HIGH
    )


def _field_is_set(revision: int) -> bool:
    # ESP-IDF 5.5.2 bootloader_common_loader.c IS_FIELD_SET().
    return revision not in (0, 0xFFFF)


def _decode_fixed_ascii(raw: bytes, field: str) -> str:
    value = raw.split(b"\0", 1)[0]
    if not value or any(byte < 0x20 or byte > 0x7E for byte in value):
        raise BootstrapValidationError(f"invalid app descriptor {field}")
    return value.decode("ascii")


def decode_esp_idf_mmu_page_size(exponent: int) -> int:
    """Decode ESP-IDF 5.5.2 app descriptor semantics without project policy."""
    exponent = _uint(exponent, 8, "app descriptor MMU page exponent")
    if exponent == 0:
        return ESP32S3_DEFAULT_MMU_PAGE_SIZE
    # The pinned C verifier shifts 1UL. Reject values that cannot be represented
    # safely by its uint32_t metadata instead of reproducing undefined behavior.
    if exponent > ESP_IDF_UINT32_SHIFT_MAX:
        raise BootstrapValidationError("app descriptor MMU page exponent is unsafe")
    return 1 << exponent


def validate_candidate_image(
    image: bytes,
    *,
    expected_sha256: str,
    chip_revision: int,
    efuse_block_revision: int,
    disable_wafer_version_major: bool,
    disable_efuse_block_version_major: bool,
    expected_project: str | None = None,
    expected_version: str | None = None,
    enforce_reviewed_mmu_policy: bool = True,
) -> ImageEvidence:
    if not image or len(image) > OTA_SLOT_SIZE:
        raise BootstrapValidationError("candidate size must be 1...0x500000")
    if not isinstance(expected_sha256, str) or not SHA256_RE.fullmatch(expected_sha256):
        raise BootstrapValidationError("expected SHA-256 must be 64 lowercase hex characters")
    actual_sha256 = hashlib.sha256(image).hexdigest()
    if actual_sha256 != expected_sha256:
        raise BootstrapValidationError("candidate SHA-256 does not match reviewed bytes")
    chip_revision = _uint(chip_revision, 16, "chip revision")
    efuse_block_revision = _uint(efuse_block_revision, 16, "eFuse block revision")
    if not isinstance(disable_wafer_version_major, bool) or not isinstance(disable_efuse_block_version_major, bool):
        raise BootstrapValidationError("revision-disable evidence must be explicit booleans")
    if len(image) < 24:
        raise BootstrapValidationError("truncated ESP image header")

    magic, segment_count, _, _, _ = struct.unpack_from("<BBBBI", image, 0)
    chip_id = struct.unpack_from("<H", image, 12)[0]
    min_revision = struct.unpack_from("<H", image, 15)[0]
    max_revision = struct.unpack_from("<H", image, 17)[0]
    hash_appended = image[23]
    if magic != ESP_IMAGE_MAGIC or not 1 <= segment_count <= 16:
        raise BootstrapValidationError("invalid ESP application image header")
    if chip_id != ESP32S3_CHIP_ID:
        raise BootstrapValidationError("candidate is not an ESP32-S3 image")
    # ESP-IDF always applies the minimum chip revision comparison. Only maximum
    # uses IS_FIELD_SET(), and the hardware disable bit bypasses that maximum.
    if chip_revision < min_revision or (
        _field_is_set(max_revision) and chip_revision > max_revision and not disable_wafer_version_major
    ):
        raise BootstrapValidationError("candidate is incompatible with the reviewed chip revision")
    if hash_appended != 1:
        raise BootstrapValidationError("candidate does not contain an appended validation hash")

    cursor = 24
    checksum = 0xEF
    first_segment: bytes | None = None
    mmu_page_size: int | None = None
    for index in range(segment_count):
        if cursor + 8 > len(image):
            raise BootstrapValidationError("truncated segment header")
        load_addr, data_length = struct.unpack_from("<II", image, cursor)
        cursor += 8
        segment_data_offset = cursor
        if data_length % 4 != 0:
            raise BootstrapValidationError("segment length is not 4-byte aligned")
        if data_length >= FLASH_SIZE or data_length > OTA_SLOT_SIZE or cursor + data_length > len(image):
            raise BootstrapValidationError("truncated or oversized segment")
        segment = image[cursor : cursor + data_length]
        if index == 0:
            first_segment = segment
            if len(segment) < 256:
                raise BootstrapValidationError("missing complete 256-byte app descriptor")
            if struct.unpack_from("<I", segment, 0)[0] != ESP_APP_DESC_MAGIC:
                raise BootstrapValidationError("invalid app descriptor magic")
            mmu_page_exponent = segment[180]
            mmu_page_size = decode_esp_idf_mmu_page_size(mmu_page_exponent)
        if mmu_page_size is None:
            raise BootstrapValidationError("missing application MMU page size")
        if _mapped_segment(load_addr) and segment_data_offset % mmu_page_size != load_addr % mmu_page_size:
            raise BootstrapValidationError("mapped segment flash/load MMU page offsets differ")
        for byte in segment:
            checksum ^= byte
        cursor += data_length

    # Structural ESP-IDF relation checks above intentionally accept descriptor-derived
    # page sizes. This separate bootstrap policy permits only the reviewed 64-KiB build.
    if enforce_reviewed_mmu_policy and mmu_page_size != ESP32S3_REVIEWED_MMU_PAGE_SIZE:
        raise BootstrapValidationError("bootstrap policy requires reviewed 64-KiB MMU page size")

    checksum_offset = cursor + ((15 - (cursor % 16)) % 16)
    if checksum_offset >= len(image):
        raise BootstrapValidationError("missing image checksum")
    if image[checksum_offset] != checksum:
        raise BootstrapValidationError("invalid image checksum")
    digest_offset = checksum_offset + 1
    if digest_offset + 32 != len(image):
        raise BootstrapValidationError("trailing, missing, or signed image data is not reviewed")
    calculated_digest = hashlib.sha256(image[:digest_offset]).digest()
    stored_digest = image[digest_offset : digest_offset + 32]
    if stored_digest != calculated_digest:
        raise BootstrapValidationError("invalid appended validation hash")

    if first_segment is None or mmu_page_size is None:
        raise BootstrapValidationError("missing application descriptor")
    secure_version = struct.unpack_from("<I", first_segment, 4)[0]
    version = _decode_fixed_ascii(first_segment[16:48], "version")
    project = _decode_fixed_ascii(first_segment[48:80], "project")
    min_efuse_block_revision, max_efuse_block_revision = struct.unpack_from("<HH", first_segment, 176)
    if _field_is_set(min_efuse_block_revision) and efuse_block_revision < min_efuse_block_revision:
        raise BootstrapValidationError("candidate requires a newer eFuse block revision")
    if (
        _field_is_set(max_efuse_block_revision)
        and efuse_block_revision > max_efuse_block_revision
        and not disable_efuse_block_version_major
    ):
        raise BootstrapValidationError("candidate requires an older eFuse block revision")
    if expected_project is not None and project != expected_project:
        raise BootstrapValidationError("app descriptor project mismatch")
    if expected_version is not None and version != expected_version:
        raise BootstrapValidationError("app descriptor version mismatch")

    return ImageEvidence(
        size=len(image),
        sha256=actual_sha256,
        chip_id=chip_id,
        min_revision=min_revision,
        max_revision=max_revision,
        checksum=checksum,
        validation_hash=stored_digest.hex(),
        descriptor=AppDescriptor(
            project=project,
            version=version,
            secure_version=secure_version,
            min_efuse_block_revision=min_efuse_block_revision,
            max_efuse_block_revision=max_efuse_block_revision,
            mmu_page_size=mmu_page_size,
        ),
    )


def validate_exact_readback(candidate: bytes, readback: bytes, expected_sha256: str) -> str:
    if len(readback) != len(candidate):
        raise BootstrapValidationError("readback length is not exact")
    if candidate != readback:
        raise BootstrapValidationError("readback bytes differ from candidate")
    digest = hashlib.sha256(readback).hexdigest()
    if digest != expected_sha256:
        raise BootstrapValidationError("readback SHA-256 differs from reviewed candidate")
    return digest


def validate_protected_region_evidence(before: Mapping[str, str], after: Mapping[str, str]) -> None:
    if frozenset(before) != PROTECTED_EVIDENCE_NAMES or frozenset(after) != PROTECTED_EVIDENCE_NAMES:
        raise BootstrapValidationError("protected-region evidence set is incomplete or contains extras")
    for name in sorted(PROTECTED_EVIDENCE_NAMES):
        if not SHA256_RE.fullmatch(before[name]) or not SHA256_RE.fullmatch(after[name]):
            raise BootstrapValidationError(f"invalid protected-region hash: {name}")
        if before[name] != after[name]:
            raise BootstrapValidationError(f"protected region changed: {name}")


def _require_exact_keys(value: Mapping[str, Any], required: set[str]) -> None:
    if set(value) != required:
        raise BootstrapValidationError("evidence fields are missing or unknown")


def validate_security_evidence(value: Mapping[str, Any]) -> None:
    required = {
        "chip",
        "chip_revision",
        "efuse_block_revision",
        "disable_wafer_version_major",
        "disable_efuse_block_version_major",
        "secure_boot_enabled",
        "flash_encryption_enabled",
        "anti_rollback_enabled",
        "efuse_summary_reviewed",
        "rom_download_available",
        "port_identity_reviewed",
        "partition_table_reviewed",
        "private_backup_reviewed",
        "recovery_reviewed",
    }
    _require_exact_keys(value, required)
    if value["chip"] != "esp32s3":
        raise BootstrapValidationError("reviewed ESP32-S3 chip identity is required")
    _uint(value["chip_revision"], 16, "chip revision")
    _uint(value["efuse_block_revision"], 16, "eFuse block revision")
    for field in required - {"chip", "chip_revision", "efuse_block_revision"}:
        if not isinstance(value[field], bool):
            raise BootstrapValidationError(f"security evidence must be an explicit boolean: {field}")
    for unsupported in ("secure_boot_enabled", "flash_encryption_enabled", "anti_rollback_enabled"):
        if value[unsupported]:
            raise BootstrapValidationError(f"unsupported enabled security policy: {unsupported}")
    for gate in (
        "efuse_summary_reviewed",
        "rom_download_available",
        "port_identity_reviewed",
        "partition_table_reviewed",
        "private_backup_reviewed",
        "recovery_reviewed",
    ):
        if not value[gate]:
            raise BootstrapValidationError(f"required bootstrap gate is not reviewed: {gate}")


def ota_select_crc(sequence: int) -> int:
    sequence = _uint(sequence, 32, "otadata sequence")
    return zlib.crc32(struct.pack("<I", sequence)) & 0xFFFFFFFF


def decode_ota_entry(raw: bytes, ota_partition_count: int = 2) -> OtaSelectEntry:
    if len(raw) < 32 or ota_partition_count <= 0:
        raise BootstrapValidationError("invalid otadata entry input")
    sequence, state, crc = struct.unpack_from("<I20xII", raw, 0)
    valid = sequence != ERASED_U32 and crc == ota_select_crc(sequence) and state not in (3, 4)
    selected_slot = (sequence - 1) % ota_partition_count if valid else None
    return OtaSelectEntry(sequence=sequence, state=state, crc=crc, valid=valid, selected_slot=selected_slot)


def select_active_ota_entry(primary: bytes, secondary: bytes) -> int | None:
    entries = (decode_ota_entry(primary), decode_ota_entry(secondary))
    valid = [index for index, entry in enumerate(entries) if entry.valid]
    if not valid:
        return None
    if len(valid) == 1:
        return valid[0]
    return 0 if entries[0].sequence >= entries[1].sequence else 1


def make_secondary_otadata_sector(sequence: int) -> bytes:
    if sequence in (0, ERASED_U32) or sequence >= 0xFFFFFFFE or (sequence - 1) % 2 != 1:
        raise BootstrapValidationError("golden sequence must safely select ota_1 without UInt32 wrap")
    entry = struct.pack("<I20sII", sequence, b"\xff" * 20, ESP_OTA_IMG_UNDEFINED, ota_select_crc(sequence))
    sector = entry + b"\xff" * (SECTOR_SIZE - len(entry))
    decoded = decode_ota_entry(sector)
    if not decoded.valid or decoded.selected_slot != 1:
        raise BootstrapValidationError("generated otadata does not select ota_1")
    return sector


def validate_partition_identity(value: Mapping[str, Any]) -> None:
    expected = {
        "otadata": {"offset": OTADATA_OFFSET, "size": OTADATA_SIZE},
        "ota_0": {"offset": OTA_0_OFFSET, "size": OTA_SLOT_SIZE},
        "ota_1": {"offset": OTA_1_OFFSET, "size": OTA_SLOT_SIZE},
    }
    _require_exact_keys(value, set(expected))
    for name, geometry in expected.items():
        item = value[name]
        if not isinstance(item, Mapping):
            raise BootstrapValidationError(f"partition identity is not an object: {name}")
        _require_exact_keys(item, {"offset", "size"})
        if _uint(item["offset"], 32, f"{name} offset") != geometry["offset"] or _uint(
            item["size"], 32, f"{name} size"
        ) != geometry["size"]:
            raise BootstrapValidationError(f"partition identity mismatch: {name}")


def validate_activation_inputs(primary: bytes, secondary: bytes, golden: bytes) -> OtaSelectEntry:
    if len(primary) != SECTOR_SIZE or len(secondary) != SECTOR_SIZE or len(golden) != SECTOR_SIZE:
        raise BootstrapValidationError("otadata evidence must contain exact 0x1000-byte sectors")
    primary_entry = decode_ota_entry(primary)
    if not primary_entry.valid or primary_entry.selected_slot != 0:
        raise BootstrapValidationError("primary vendor otadata record must validly select ota_0")
    if secondary != b"\xff" * SECTOR_SIZE:
        raise BootstrapValidationError("secondary otadata sector is not erased")
    golden_entry = decode_ota_entry(golden)
    if not golden_entry.valid or golden_entry.selected_slot != 1:
        raise BootstrapValidationError("golden otadata does not select ota_1")
    if golden_entry.sequence <= primary_entry.sequence:
        raise BootstrapValidationError("golden otadata sequence does not supersede primary")
    if golden[32:] != b"\xff" * (SECTOR_SIZE - 32):
        raise BootstrapValidationError("golden otadata sector contains unexpected bytes")
    if select_active_ota_entry(primary, golden) != 1:
        raise BootstrapValidationError("golden otadata is not selected by ESP-IDF ordering")
    return golden_entry


def activation_evidence_binding(
    *,
    primary: bytes,
    secondary: bytes,
    ota0: bytes,
    ota1: bytes,
    expected_primary_sha256: str,
    expected_ota0_sha256: str,
    expected_ota1_sha256: str,
    partition_table: bytes,
    expected_partition_table_sha256: str,
) -> dict[str, str]:
    if len(primary) != SECTOR_SIZE or len(secondary) != SECTOR_SIZE:
        raise BootstrapValidationError("activation input sectors must be exactly 0x1000 bytes")
    if not ota0 or len(ota0) > OTA_SLOT_SIZE or not ota1 or len(ota1) > OTA_SLOT_SIZE:
        raise BootstrapValidationError("OTA image evidence length is invalid")
    if any(not isinstance(value, str) or not SHA256_RE.fullmatch(value) for value in
           (expected_primary_sha256, expected_ota0_sha256, expected_ota1_sha256)):
        raise BootstrapValidationError("reviewed activation hashes are invalid")
    primary_sha = hashlib.sha256(primary).hexdigest()
    ota0_sha = hashlib.sha256(ota0).hexdigest()
    ota1_sha = hashlib.sha256(ota1).hexdigest()
    if primary_sha != expected_primary_sha256:
        raise BootstrapValidationError("primary otadata differs from reviewed identity")
    if ota0_sha != expected_ota0_sha256:
        raise BootstrapValidationError("ota_0 differs from reviewed identity")
    if ota1_sha != expected_ota1_sha256:
        raise BootstrapValidationError("ota_1 differs from reviewed staged identity")
    if secondary != b"\xff" * SECTOR_SIZE:
        raise BootstrapValidationError("secondary otadata sector is not erased")
    parse_partition_table(partition_table)
    partition_table_sha = _validated_sha256_bytes(partition_table, expected_partition_table_sha256, "partition table")
    return {
        "primary_otadata_sha256": primary_sha,
        "secondary_before_sha256": hashlib.sha256(secondary).hexdigest(),
        "ota_0_sha256": ota0_sha,
        "ota_1_sha256": ota1_sha,
        "partition_table_sha256": partition_table_sha,
        "partition_table_size": str(len(partition_table)),
    }


def validate_activation_post_evidence(
    *,
    primary_before: bytes,
    primary_after: bytes,
    secondary_before: bytes,
    secondary_after: bytes,
    golden: bytes,
    ota0_before: bytes,
    ota0_after: bytes,
    ota1_before: bytes,
    ota1_after: bytes,
    protected_before: Mapping[str, str],
    protected_after: Mapping[str, str],
    flash_before: bytes,
    flash_after: bytes,
) -> dict[str, str]:
    if len(flash_before) != FLASH_SIZE or len(flash_after) != FLASH_SIZE:
        raise BootstrapValidationError("activation flash evidence must cover exact 16-MiB flash")
    authorized_start = SECONDARY_OTADATA_OFFSET
    authorized_end = authorized_start + SECTOR_SIZE
    if flash_before[:authorized_start] != flash_after[:authorized_start] or flash_before[authorized_end:] != flash_after[authorized_end:]:
        raise BootstrapValidationError("flash changed outside authorized secondary otadata sector")
    if flash_before[authorized_start:authorized_end] != secondary_before or flash_after[authorized_start:authorized_end] != secondary_after:
        raise BootstrapValidationError("secondary otadata evidence is not bound to full flash")
    if flash_before[OTADATA_OFFSET:OTADATA_OFFSET + SECTOR_SIZE] != primary_before or flash_after[OTADATA_OFFSET:OTADATA_OFFSET + SECTOR_SIZE] != primary_after:
        raise BootstrapValidationError("primary otadata evidence is not bound to full flash")
    if flash_before[OTA_0_OFFSET:OTA_0_OFFSET + len(ota0_before)] != ota0_before or flash_after[OTA_0_OFFSET:OTA_0_OFFSET + len(ota0_after)] != ota0_after:
        raise BootstrapValidationError("ota_0 evidence is not bound to full flash")
    if flash_before[OTA_1_OFFSET:OTA_1_OFFSET + len(ota1_before)] != ota1_before or flash_after[OTA_1_OFFSET:OTA_1_OFFSET + len(ota1_after)] != ota1_after:
        raise BootstrapValidationError("ota_1 evidence is not bound to full flash")
    if len(primary_before) != SECTOR_SIZE or len(primary_after) != SECTOR_SIZE:
        raise BootstrapValidationError("primary otadata readback length is not exact")
    if len(secondary_before) != SECTOR_SIZE or len(secondary_after) != SECTOR_SIZE:
        raise BootstrapValidationError("secondary otadata readback length is not exact")
    if primary_before != primary_after:
        raise BootstrapValidationError("primary otadata changed during activation")
    if secondary_before != b"\xff" * SECTOR_SIZE or secondary_after != golden:
        raise BootstrapValidationError("secondary otadata readback differs from authorized golden")
    if ota0_before != ota0_after or not ota0_before:
        raise BootstrapValidationError("ota_0 changed during activation")
    if ota1_before != ota1_after or not ota1_before:
        raise BootstrapValidationError("ota_1 changed during activation")
    if frozenset(protected_before) != ACTIVATION_PROTECTED_EVIDENCE_NAMES or frozenset(
        protected_after
    ) != ACTIVATION_PROTECTED_EVIDENCE_NAMES:
        raise BootstrapValidationError("activation protected evidence set is incomplete or contains extras")
    for name in sorted(ACTIVATION_PROTECTED_EVIDENCE_NAMES):
        before = protected_before[name]
        after = protected_after[name]
        if not isinstance(before, str) or not SHA256_RE.fullmatch(before) or not isinstance(after, str) or not SHA256_RE.fullmatch(after):
            raise BootstrapValidationError(f"invalid activation protected hash: {name}")
        if before != after:
            raise BootstrapValidationError(f"activation protected region changed: {name}")
    exact_hashes = {
        "primary_otadata": hashlib.sha256(primary_before).hexdigest(),
        "ota_0": hashlib.sha256(ota0_before).hexdigest(),
        "ota_1": hashlib.sha256(ota1_before).hexdigest(),
    }
    for name, digest in exact_hashes.items():
        if protected_before[name] != digest:
            raise BootstrapValidationError(f"activation protected evidence is not bound to exact bytes: {name}")
    validate_activation_inputs(primary_before, secondary_before, golden)
    return {
        "primary_otadata_sha256": hashlib.sha256(primary_after).hexdigest(),
        "secondary_after_sha256": hashlib.sha256(secondary_after).hexdigest(),
        "ota_0_sha256": hashlib.sha256(ota0_after).hexdigest(),
        "ota_1_sha256": hashlib.sha256(ota1_after).hexdigest(),
        "flash_before_sha256": hashlib.sha256(flash_before).hexdigest(),
        "flash_after_sha256": hashlib.sha256(flash_after).hexdigest(),
    }


def safe_plan(
    operation: str,
    *,
    address: int,
    length: int,
    sha256: str,
    authorization_sha256: str,
    evidence_sha256: str,
) -> dict[str, Any]:
    if operation not in {"stage", "activate"}:
        raise BootstrapValidationError("operation is not allowlisted")
    if not SHA256_RE.fullmatch(sha256) or not SHA256_RE.fullmatch(authorization_sha256):
        raise BootstrapValidationError("review and authorization hashes are required")
    if not isinstance(evidence_sha256, str) or not SHA256_RE.fullmatch(evidence_sha256):
        raise BootstrapValidationError("evidence binding hash is required")
    if operation == "stage":
        validate_stage_range(address, length, length)
    else:
        validate_activation_range(address, length)
    result = {
        "schema": 1,
        "operation": operation,
        "transport": "unavailable_offline",
        "executable": False,
        "address": f"0x{address:06x}",
        "length": length,
        "sha256": sha256,
        "authorization_sha256": authorization_sha256,
        "next_gate": "independent_review_then_explicit_connected_authorization",
    }
    result["evidence_sha256"] = evidence_sha256
    return result


def safe_recovery_plan(artifacts: Mapping[str, Mapping[str, Any]]) -> dict[str, Any]:
    allowlist = {
        "ota_0": (OTA_0_OFFSET, OTA_SLOT_SIZE),
        "otadata": (OTADATA_OFFSET, OTADATA_SIZE),
        "full_flash": (0, FLASH_SIZE),
    }
    if not artifacts or not set(artifacts).issubset(allowlist):
        raise BootstrapValidationError("recovery artifacts are empty or not allowlisted")
    entries: list[dict[str, Any]] = []
    for name in sorted(artifacts):
        value = artifacts[name]
        _require_exact_keys(value, {"size", "sha256"})
        expected_offset, maximum_size = allowlist[name]
        size = value["size"]
        digest = value["sha256"]
        if isinstance(size, bool) or not isinstance(size, int) or size <= 0 or size > maximum_size:
            raise BootstrapValidationError(f"invalid recovery artifact size: {name}")
        if name in {"otadata", "full_flash"} and size != maximum_size:
            raise BootstrapValidationError(f"recovery artifact must have exact size: {name}")
        if not isinstance(digest, str) or not SHA256_RE.fullmatch(digest):
            raise BootstrapValidationError(f"invalid recovery artifact hash: {name}")
        entries.append({"name": name, "address": f"0x{expected_offset:06x}", "size": size, "sha256": digest})
    return {
        "schema": 1,
        "operation": "recovery",
        "transport": "unavailable_offline",
        "executable": False,
        "artifacts": entries,
        "next_gate": "independent_review_then_explicit_recovery_authorization",
    }


def bounded_json(value: Mapping[str, Any]) -> str:
    output = json.dumps(value, indent=2, sort_keys=True)
    if len(output.encode()) > 4096:
        raise BootstrapValidationError("output exceeds bounded diagnostic size")
    lowered = output.lower()
    forbidden = ("nvs_keys.bin", "device_secret", "current-nvs-keys", "/dev/cu.", "esptool.py write_flash")
    if any(marker in lowered for marker in forbidden):
        raise BootstrapValidationError("output contains private or executable material")
    return output
