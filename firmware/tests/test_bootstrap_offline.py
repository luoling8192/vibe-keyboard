#!/usr/bin/env python3

from __future__ import annotations

import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from bootstrap_core import (  # noqa: E402
    ERASED_U32,
    FLASH_SIZE,
    PARTITIONS,
    ESP_OTA_IMG_UNDEFINED,
    OTA_1_OFFSET,
    OTA_SLOT_SIZE,
    ACTIVATION_PROTECTED_EVIDENCE_NAMES,
    PROTECTED_EVIDENCE_NAMES,
    SECONDARY_OTADATA_OFFSET,
    SECTOR_SIZE,
    BootstrapValidationError,
    activation_evidence_binding,
    bounded_json,
    decode_ota_entry,
    make_secondary_otadata_sector,
    parse_partition_table,
    ota_select_crc,
    safe_plan,
    stage_evidence_binding,
    safe_recovery_plan,
    select_active_ota_entry,
    validate_activation_inputs,
    validate_activation_post_evidence,
    validate_activation_range,
    validate_candidate_image,
    validate_exact_readback,
    validate_protected_region_evidence,
    validate_security_evidence,
    validate_stage_range,
)


def make_image(
    *,
    chip_id: int = 9,
    min_revision: int = 0,
    max_revision: int = 99,
    project: str = "vibe_keyboard",
    version: str = "test",
    hash_appended: int = 1,
    min_efuse_block_revision: int = 0,
    max_efuse_block_revision: int = 0,
    descriptor_size: int = 256,
    load_addr: int = 0x3C000020,
    mmu_page_exponent: int = 0,
) -> bytes:
    descriptor = bytearray(descriptor_size)
    struct.pack_into("<II", descriptor, 0, 0xABCD5432, 0)
    descriptor[16 : 16 + len(version)] = version.encode()
    descriptor[48 : 48 + len(project)] = project.encode()
    if descriptor_size >= 181:
        struct.pack_into("<HHB", descriptor, 176, min_efuse_block_revision, max_efuse_block_revision, mmu_page_exponent)
    header = bytearray(24)
    struct.pack_into("<BBBBI", header, 0, 0xE9, 1, 0, 0, 0x40370000)
    header[8] = 0xEE
    struct.pack_into("<H", header, 12, chip_id)
    header[14] = min(min_revision // 100, 0xFF)
    struct.pack_into("<H", header, 15, min_revision)
    struct.pack_into("<H", header, 17, max_revision)
    header[23] = hash_appended
    image = header + struct.pack("<II", load_addr, len(descriptor)) + descriptor
    checksum = 0xEF
    for byte in descriptor:
        checksum ^= byte
    image += b"\x00" * ((15 - len(image) % 16) % 16) + bytes([checksum])
    return bytes(image + hashlib.sha256(image).digest())


def image_sha(image: bytes) -> str:
    return hashlib.sha256(image).hexdigest()


def make_partition_table(*, mutate_storage_flags: bool = False) -> bytes:
    types = {"nvs": (1, 2), "nvs_keys": (1, 2), "phy_init": (1, 1), "otadata": (1, 0),
             "ota_0": (0, 0x10), "ota_1": (0, 0x11), "storage": (1, 0x82)}
    entries = bytearray()
    for name, (offset, size) in PARTITIONS.items():
        type_value, subtype = types[name]
        flags = 1 if mutate_storage_flags and name == "storage" else 0
        label = name.encode() + b"\0" * (16 - len(name))
        entries.extend(struct.pack("<HBBII16sI", 0x50AA, type_value, subtype, offset, size, label, flags))
    marker = struct.pack("<H", 0xEBEB) + b"\xff" * 14 + hashlib.md5(entries, usedforsecurity=False).digest()
    return bytes(entries + marker + b"\xff" * (0xC00 - len(entries) - len(marker)))


REVIEWED_REVISIONS = {
    "chip_revision": 0,
    "efuse_block_revision": 0,
    "disable_wafer_version_major": False,
    "disable_efuse_block_version_major": False,
}


def ota_sector(sequence: int, *, state: int = ESP_OTA_IMG_UNDEFINED, crc: int | None = None) -> bytes:
    entry = struct.pack("<I20sII", sequence, b"\xff" * 20, state, ota_select_crc(sequence) if crc is None else crc)
    return entry + b"\xff" * (SECTOR_SIZE - len(entry))


class CandidateImageTests(unittest.TestCase):
    def test_valid_candidate(self) -> None:
        image = make_image()
        evidence = validate_candidate_image(
            image,
            expected_sha256=image_sha(image),
            **REVIEWED_REVISIONS,
            expected_project="vibe_keyboard",
            expected_version="test",
        )
        self.assertEqual(evidence.chip_id, 9)
        self.assertEqual(evidence.descriptor.project, "vibe_keyboard")
        self.assertEqual(evidence.validation_hash, image[-32:].hex())

    def assert_invalid(self, image: bytes, **kwargs: object) -> None:
        revisions: dict[str, object] = dict(REVIEWED_REVISIONS)
        for key in tuple(revisions):
            if key in kwargs:
                revisions[key] = kwargs.pop(key)
        with self.assertRaises(BootstrapValidationError):
            validate_candidate_image(image, expected_sha256=image_sha(image), **revisions, **kwargs)

    def test_size_and_truncation_rejected(self) -> None:
        self.assert_invalid(b"")
        self.assert_invalid(b"\xe9")
        oversized = b"\xe9" + b"\0" * OTA_SLOT_SIZE
        self.assert_invalid(oversized)

    def test_reviewed_hash_required(self) -> None:
        image = make_image()
        with self.assertRaises(BootstrapValidationError):
            validate_candidate_image(image, expected_sha256="0" * 64, **REVIEWED_REVISIONS)
        with self.assertRaises(BootstrapValidationError):
            validate_candidate_image(image, expected_sha256=image_sha(image).upper(), **REVIEWED_REVISIONS)

    def test_header_chip_revision_descriptor_rejected(self) -> None:
        self.assert_invalid(make_image(chip_id=0))
        self.assert_invalid(make_image(min_revision=101), chip_revision=100)
        self.assert_invalid(make_image(max_revision=100), chip_revision=101)
        image = make_image(max_revision=0)
        validate_candidate_image(
            image,
            expected_sha256=image_sha(image),
            **{**REVIEWED_REVISIONS, "chip_revision": 101},
        )
        self.assert_invalid(make_image(), expected_project="other")
        self.assert_invalid(make_image(), expected_version="other")
        self.assert_invalid(make_image(hash_appended=0))
        self.assert_invalid(make_image(min_revision=0xFFFF))
        self.assert_invalid(make_image(descriptor_size=252))
        self.assert_invalid(make_image(min_efuse_block_revision=2), efuse_block_revision=1)
        self.assert_invalid(make_image(max_efuse_block_revision=2), efuse_block_revision=3)
        validate_candidate_image(
            make_image(max_efuse_block_revision=2),
            expected_sha256=image_sha(make_image(max_efuse_block_revision=2)),
            **{**REVIEWED_REVISIONS, "efuse_block_revision": 3, "disable_efuse_block_version_major": True},
        )

    def test_checksum_hash_trailing_and_segment_length_rejected(self) -> None:
        image = bytearray(make_image())
        image[-33] ^= 1
        self.assert_invalid(bytes(image))
        image = bytearray(make_image())
        image[-1] ^= 1
        self.assert_invalid(bytes(image))
        self.assert_invalid(make_image() + b"extra")
        image = bytearray(make_image())
        struct.pack_into("<I", image, 28, 0x500001)
        self.assert_invalid(bytes(image))
        self.assert_invalid(make_image(descriptor_size=257))

    def test_mapped_segment_page_offset_and_mmu_page_size(self) -> None:
        self.assert_invalid(make_image(load_addr=0x3C000024))
        image = make_image(mmu_page_exponent=16)
        evidence = validate_candidate_image(image, expected_sha256=image_sha(image), **REVIEWED_REVISIONS)
        self.assertEqual(evidence.descriptor.mmu_page_size, 0x10000)
        for exponent in (15, 17):
            structural = make_image(mmu_page_exponent=exponent)
            evidence = validate_candidate_image(
                structural, expected_sha256=image_sha(structural),
                enforce_reviewed_mmu_policy=False, **REVIEWED_REVISIONS,
            )
            self.assertEqual(evidence.descriptor.mmu_page_size, 1 << exponent)
            with self.assertRaisesRegex(BootstrapValidationError, "bootstrap policy requires reviewed 64-KiB"):
                validate_candidate_image(
                    structural, expected_sha256=image_sha(structural), **REVIEWED_REVISIONS,
                )
        for exponent in (32, 255):
            unsafe = make_image(mmu_page_exponent=exponent)
            with self.assertRaisesRegex(BootstrapValidationError, "exponent is unsafe"):
                validate_candidate_image(
                    unsafe, expected_sha256=image_sha(unsafe),
                    enforce_reviewed_mmu_policy=False, **REVIEWED_REVISIONS,
                )
        mismatched_32k = make_image(load_addr=0x3C004020, mmu_page_exponent=15)
        with self.assertRaisesRegex(BootstrapValidationError, "flash/load MMU page offsets differ"):
            validate_candidate_image(
                mismatched_32k, expected_sha256=image_sha(mismatched_32k),
                enforce_reviewed_mmu_policy=False, **REVIEWED_REVISIONS,
            )
        # Non-mapped DRAM does not use the flash/MMU offset relation.
        image = make_image(load_addr=0x3FC88004)
        validate_candidate_image(image, expected_sha256=image_sha(image), **REVIEWED_REVISIONS)

    def test_app_descriptor_magic_and_ascii_rejected(self) -> None:
        image = bytearray(make_image())
        image[32] ^= 1
        self.assert_invalid(bytes(image))
        image = bytearray(make_image())
        image[32 + 16] = 0
        self.assert_invalid(bytes(image))


class RangeAndReadbackTests(unittest.TestCase):
    def test_exact_stage_range(self) -> None:
        validate_stage_range(OTA_1_OFFSET, 1, 1)
        validate_stage_range(OTA_1_OFFSET, OTA_SLOT_SIZE, OTA_SLOT_SIZE)
        for offset, length, size in (
            (OTA_1_OFFSET - 1, 1, 1),
            (OTA_1_OFFSET, 2, 1),
            (OTA_1_OFFSET, 0, 0),
            (OTA_1_OFFSET, OTA_SLOT_SIZE + 1, OTA_SLOT_SIZE + 1),
        ):
            with self.assertRaises(BootstrapValidationError):
                validate_stage_range(offset, length, size)

    def test_stage_binding_covers_candidate_security_and_partitions(self) -> None:
        image = make_image()
        security = SecurityTests().evidence()
        partition_table = make_partition_table()
        partition_sha = hashlib.sha256(partition_table).hexdigest()
        first = stage_evidence_binding(
            image=image, expected_sha256=image_sha(image), security=security,
            partition_table=partition_table, expected_partition_table_sha256=partition_sha,
        )
        changed_security = dict(security)
        changed_security["chip_revision"] = 1
        second = stage_evidence_binding(image=image, expected_sha256=image_sha(image), security=changed_security, partition_table=partition_table, expected_partition_table_sha256=partition_sha)
        self.assertNotEqual(first["binding_sha256"], second["binding_sha256"])
        changed_table = make_partition_table(mutate_storage_flags=True)
        with self.assertRaises(BootstrapValidationError):
            stage_evidence_binding(
                image=image, expected_sha256=image_sha(image), security=security,
                partition_table=changed_table, expected_partition_table_sha256=partition_sha,
            )

    def test_partition_table_md5_marker_is_structurally_verified(self) -> None:
        table = make_partition_table()
        marker_offset = len(PARTITIONS) * 32
        self.assertEqual(parse_partition_table(table), PARTITIONS)

        malformed: list[bytes] = []

        bad_reserved = bytearray(table)
        bad_reserved[marker_offset + 2] = 0
        malformed.append(bytes(bad_reserved))

        bad_digest = bytearray(table)
        bad_digest[marker_offset + 16] ^= 1
        malformed.append(bytes(bad_digest))

        missing_marker = bytearray(table)
        missing_marker[marker_offset : marker_offset + 32] = b"\xff" * 32
        malformed.append(bytes(missing_marker))

        duplicate_marker = bytearray(table)
        duplicate_marker[marker_offset + 32 : marker_offset + 64] = table[marker_offset : marker_offset + 32]
        malformed.append(bytes(duplicate_marker))

        entry_after_marker = bytearray(table)
        entry_after_marker[marker_offset + 32 : marker_offset + 64] = table[:32]
        malformed.append(bytes(entry_after_marker))

        misplaced_marker = bytearray(table)
        misplaced_marker[marker_offset - 32 : marker_offset] = table[marker_offset : marker_offset + 32]
        misplaced_marker[marker_offset : marker_offset + 32] = b"\xff" * 32
        malformed.append(bytes(misplaced_marker))

        stale_digest = bytearray(table)
        stale_digest[12] ^= 1
        malformed.append(bytes(stale_digest))

        for candidate in malformed:
            with self.subTest(candidate_sha256=hashlib.sha256(candidate).hexdigest()):
                with self.assertRaises(BootstrapValidationError):
                    parse_partition_table(candidate)

    def test_generated_partition_table_regression_when_available(self) -> None:
        candidates = (ROOT / "build" / "partition_table" / "partition-table.bin",)
        existing = [path for path in candidates if path.is_file()]
        if not existing:
            self.skipTest("no generated production partition table is available")
        for path in existing:
            with self.subTest(artifact=path.parent.parent.name):
                raw = path.read_bytes()
                self.assertEqual(parse_partition_table(raw), PARTITIONS)
                marker_offset = len(PARTITIONS) * 32
                corrupted = bytearray(raw)
                corrupted[marker_offset + 16] ^= 1
                with self.assertRaises(BootstrapValidationError):
                    parse_partition_table(bytes(corrupted))

    def test_exact_activation_range(self) -> None:
        validate_activation_range(SECONDARY_OTADATA_OFFSET, SECTOR_SIZE)
        for offset, length in (
            (SECONDARY_OTADATA_OFFSET - 1, SECTOR_SIZE),
            (SECONDARY_OTADATA_OFFSET, SECTOR_SIZE - 1),
            (0x11000, SECTOR_SIZE),
        ):
            with self.assertRaises(BootstrapValidationError):
                validate_activation_range(offset, length)

    def test_readback_exactness(self) -> None:
        candidate = b"candidate"
        digest = hashlib.sha256(candidate).hexdigest()
        self.assertEqual(validate_exact_readback(candidate, candidate, digest), digest)
        for readback in (candidate[:-1], candidate + b"x", b"Candidate"):
            with self.assertRaises(BootstrapValidationError):
                validate_exact_readback(candidate, readback, digest)
        with self.assertRaises(BootstrapValidationError):
            validate_exact_readback(candidate, candidate, "0" * 64)

    def test_protected_regions_unchanged_and_complete(self) -> None:
        hashes = {name: hashlib.sha256(name.encode()).hexdigest() for name in PROTECTED_EVIDENCE_NAMES}
        validate_protected_region_evidence(hashes, dict(hashes))
        changed = dict(hashes)
        changed["ota_0"] = "0" * 64
        with self.assertRaises(BootstrapValidationError):
            validate_protected_region_evidence(hashes, changed)
        incomplete = dict(hashes)
        incomplete.pop("nvs_keys")
        with self.assertRaises(BootstrapValidationError):
            validate_protected_region_evidence(incomplete, hashes)


class SecurityTests(unittest.TestCase):
    def evidence(self) -> dict[str, object]:
        return {
            "chip": "esp32s3",
            "chip_revision": 0,
            "efuse_block_revision": 0,
            "disable_wafer_version_major": False,
            "disable_efuse_block_version_major": False,
            "secure_boot_enabled": False,
            "flash_encryption_enabled": False,
            "anti_rollback_enabled": False,
            "efuse_summary_reviewed": True,
            "rom_download_available": True,
            "port_identity_reviewed": True,
            "partition_table_reviewed": True,
            "private_backup_reviewed": True,
            "recovery_reviewed": True,
        }

    def test_explicit_compatible_evidence(self) -> None:
        validate_security_evidence(self.evidence())

    def test_unknown_missing_extra_and_enabled_fail_closed(self) -> None:
        for field in list(self.evidence()):
            value = self.evidence()
            value.pop(field)
            with self.assertRaises(BootstrapValidationError):
                validate_security_evidence(value)
        extra = self.evidence()
        extra["unknown"] = False
        with self.assertRaises(BootstrapValidationError):
            validate_security_evidence(extra)
        for field in ("secure_boot_enabled", "flash_encryption_enabled", "anti_rollback_enabled"):
            value = self.evidence()
            value[field] = True
            with self.assertRaises(BootstrapValidationError):
                validate_security_evidence(value)
        unknown = self.evidence()
        unknown["secure_boot_enabled"] = None
        with self.assertRaises(BootstrapValidationError):
            validate_security_evidence(unknown)
        for invalid in (True, -1, 65536):
            for field in ("chip_revision", "efuse_block_revision"):
                value = self.evidence()
                value[field] = invalid
                with self.assertRaises(BootstrapValidationError):
                    validate_security_evidence(value)


class OtaDataTests(unittest.TestCase):
    def test_crc_matches_fixed_golden(self) -> None:
        self.assertEqual(ota_select_crc(1), 0x99F8B879)
        self.assertEqual(ota_select_crc(2), 0x8B4D1797)

    def test_golden_selects_ota1_and_preserves_erasure(self) -> None:
        primary = ota_sector(1)
        erased = b"\xff" * SECTOR_SIZE
        golden = make_secondary_otadata_sector(2)
        entry = validate_activation_inputs(primary, erased, golden)
        self.assertEqual(entry.sequence, 2)
        self.assertEqual(entry.selected_slot, 1)
        self.assertEqual(select_active_ota_entry(primary, golden), 1)
        self.assertEqual(golden[32:], b"\xff" * (SECTOR_SIZE - 32))

    def test_erased_invalid_crc_state_and_single_copy_selection(self) -> None:
        erased = b"\xff" * SECTOR_SIZE
        self.assertFalse(decode_ota_entry(erased).valid)
        bad_crc = ota_sector(1, crc=0)
        self.assertFalse(decode_ota_entry(bad_crc).valid)
        invalid = ota_sector(1, state=3)
        aborted = ota_sector(1, state=4)
        self.assertFalse(decode_ota_entry(invalid).valid)
        self.assertFalse(decode_ota_entry(aborted).valid)
        self.assertEqual(select_active_ota_entry(ota_sector(1), erased), 0)
        self.assertEqual(select_active_ota_entry(erased, ota_sector(2)), 1)
        self.assertIsNone(select_active_ota_entry(erased, erased))

    def test_sequence_order_and_wrap_are_fail_closed_for_generation(self) -> None:
        self.assertEqual(select_active_ota_entry(ota_sector(3), ota_sector(2)), 0)
        for sequence in (0, 1, ERASED_U32, 0xFFFFFFFE):
            with self.assertRaises(BootstrapValidationError):
                make_secondary_otadata_sector(sequence)
        with self.assertRaises(BootstrapValidationError):
            validate_activation_inputs(ota_sector(3), b"\xff" * SECTOR_SIZE, make_secondary_otadata_sector(2))

    def test_activation_evidence_binding_and_post_readback(self) -> None:
        primary = ota_sector(1)
        secondary = b"\xff" * SECTOR_SIZE
        ota0 = b"vendor-ota0"
        ota1 = make_image()
        partition_table = make_partition_table()
        partition_sha = hashlib.sha256(partition_table).hexdigest()
        binding = activation_evidence_binding(
            primary=primary,
            secondary=secondary,
            ota0=ota0,
            ota1=ota1,
            expected_primary_sha256=hashlib.sha256(primary).hexdigest(),
            expected_ota0_sha256=hashlib.sha256(ota0).hexdigest(),
            expected_ota1_sha256=hashlib.sha256(ota1).hexdigest(),
            partition_table=partition_table,
            expected_partition_table_sha256=partition_sha,
        )
        self.assertEqual(binding["ota_0_sha256"], hashlib.sha256(ota0).hexdigest())
        golden = make_secondary_otadata_sector(2)
        protected = {name: hashlib.sha256(name.encode()).hexdigest() for name in ACTIVATION_PROTECTED_EVIDENCE_NAMES}
        protected["primary_otadata"] = hashlib.sha256(primary).hexdigest()
        protected["ota_0"] = hashlib.sha256(ota0).hexdigest()
        protected["ota_1"] = hashlib.sha256(ota1).hexdigest()
        flash_before = bytearray(b"\xff" * FLASH_SIZE)
        flash_before[0x011000:0x012000] = primary
        flash_before[0x012000:0x013000] = secondary
        flash_before[0x020000:0x020000 + len(ota0)] = ota0
        flash_before[OTA_1_OFFSET:OTA_1_OFFSET + len(ota1)] = ota1
        flash_after = bytearray(flash_before)
        flash_after[0x012000:0x013000] = golden
        post = validate_activation_post_evidence(
            primary_before=primary,
            primary_after=primary,
            secondary_before=secondary,
            secondary_after=golden,
            golden=golden,
            ota0_before=ota0,
            ota0_after=ota0,
            ota1_before=ota1,
            ota1_after=ota1,
            protected_before=protected,
            protected_after=dict(protected),
            flash_before=bytes(flash_before), flash_after=bytes(flash_after),
        )
        self.assertEqual(post["secondary_after_sha256"], hashlib.sha256(golden).hexdigest())
        for changed in ("primary", "secondary", "ota0", "ota1", "protected"):
            kwargs = {
                "primary_before": primary,
                "primary_after": primary,
                "secondary_before": secondary,
                "secondary_after": golden,
                "golden": golden,
                "ota0_before": ota0,
                "ota0_after": ota0,
                "ota1_before": ota1,
                "ota1_after": ota1,
                "protected_before": protected,
                "protected_after": dict(protected),
                "flash_before": bytes(flash_before),
                "flash_after": bytes(flash_after),
            }
            if changed == "primary": kwargs["primary_after"] = primary[:-1] + b"x"
            elif changed == "secondary": kwargs["secondary_after"] = golden[:-1] + b"x"
            elif changed == "ota0": kwargs["ota0_after"] = b"changed"
            elif changed == "ota1": kwargs["ota1_after"] = ota1[:-1] + b"x"
            else: kwargs["protected_after"]["storage"] = "0" * 64
            with self.assertRaises(BootstrapValidationError):
                validate_activation_post_evidence(**kwargs)
        for changed_offset in (0x013000, 0xF00000):
            changed_flash = bytearray(flash_after)
            changed_flash[changed_offset] ^= 1
            with self.assertRaises(BootstrapValidationError):
                validate_activation_post_evidence(
                    primary_before=primary, primary_after=primary,
                    secondary_before=secondary, secondary_after=golden, golden=golden,
                    ota0_before=ota0, ota0_after=ota0, ota1_before=ota1, ota1_after=ota1,
                    protected_before=protected, protected_after=dict(protected),
                    flash_before=bytes(flash_before), flash_after=bytes(changed_flash),
                )

    def test_activation_binding_rejects_incomplete_or_changed_evidence(self) -> None:
        primary = ota_sector(1)
        secondary = b"\xff" * SECTOR_SIZE
        ota0 = b"vendor"
        ota1 = make_image()
        partition_table = make_partition_table()
        partition_sha = hashlib.sha256(partition_table).hexdigest()
        base = dict(primary=primary, secondary=secondary, ota0=ota0, ota1=ota1,
                    expected_primary_sha256=hashlib.sha256(primary).hexdigest(),
                    expected_ota0_sha256=hashlib.sha256(ota0).hexdigest(),
                    expected_ota1_sha256=hashlib.sha256(ota1).hexdigest(), partition_table=partition_table, expected_partition_table_sha256=partition_sha)
        variants = []
        for key, value in (("primary", primary[:-1]), ("secondary", secondary[:-1]),
                           ("secondary", b"x" + secondary[1:]), ("expected_ota0_sha256", "0" * 64),
                           ("ota1", ota1[:-1]), ("expected_ota1_sha256", "0" * 64)):
            item = dict(base); item[key] = value; variants.append(item)
        item = dict(base); item["partition_table"] = make_partition_table(mutate_storage_flags=True); variants.append(item)
        for item in variants:
            with self.assertRaises(BootstrapValidationError):
                activation_evidence_binding(**item)

    def test_activation_requires_valid_primary_erased_secondary_exact_golden(self) -> None:
        primary = ota_sector(1)
        erased = b"\xff" * SECTOR_SIZE
        golden = make_secondary_otadata_sector(2)
        variants = (
            (erased, erased, golden),
            (primary, ota_sector(2), golden),
            (primary, erased, golden[:-1]),
            (primary, erased, golden[:32] + b"\0" + golden[33:]),
        )
        for args in variants:
            with self.assertRaises(BootstrapValidationError):
                validate_activation_inputs(*args)


class OutputAndCliTests(unittest.TestCase):
    def test_stage_activate_plans_are_separate_non_executable(self) -> None:
        digest = "1" * 64
        authorization = "2" * 64
        stage = safe_plan("stage", address=OTA_1_OFFSET, length=123, sha256=digest, authorization_sha256=authorization, evidence_sha256="3" * 64)
        activate = safe_plan(
            "activate",
            address=SECONDARY_OTADATA_OFFSET,
            length=SECTOR_SIZE,
            sha256=digest,
            authorization_sha256=authorization,
            evidence_sha256="3" * 64,
        )
        self.assertFalse(stage["executable"])
        self.assertFalse(activate["executable"])
        self.assertNotEqual(stage["operation"], activate["operation"])
        self.assertEqual(stage["transport"], "unavailable_offline")
        with self.assertRaises(BootstrapValidationError):
            safe_plan("stage-and-activate", address=OTA_1_OFFSET, length=1, sha256=digest, authorization_sha256=authorization, evidence_sha256="3" * 64)

    def test_recovery_plan_is_allowlisted_redacted_and_non_executable(self) -> None:
        plan = safe_recovery_plan({"ota_0": {"size": OTA_SLOT_SIZE, "sha256": "1" * 64}})
        self.assertFalse(plan["executable"])
        self.assertEqual(plan["transport"], "unavailable_offline")
        self.assertEqual(plan["artifacts"][0]["address"], "0x020000")
        self.assertNotIn("path", bounded_json(plan))
        with self.assertRaises(BootstrapValidationError):
            safe_recovery_plan({"nvs_keys": {"size": 0x3000, "sha256": "1" * 64}})
        with self.assertRaises(BootstrapValidationError):
            safe_recovery_plan({"full_flash": {"size": 1, "sha256": "1" * 64}})

    def test_output_is_bounded_redacted_and_not_a_shell_command(self) -> None:
        rendered = bounded_json(safe_plan("stage", address=OTA_1_OFFSET, length=1, sha256="1" * 64, authorization_sha256="2" * 64, evidence_sha256="3" * 64))
        self.assertNotIn("write_flash", rendered)
        self.assertNotIn("/dev/cu.", rendered)
        self.assertNotIn("nvs_keys", rendered)
        for private in ("current-nvs-keys.bin", "device_secret", "esptool.py write_flash"):
            with self.assertRaises(BootstrapValidationError):
                bounded_json({"value": private})
        with self.assertRaises(BootstrapValidationError):
            bounded_json({"value": "x" * 5000})

    def test_cli_failure_never_discloses_private_path(self) -> None:
        script = ROOT / "tools/bootstrap_offline.py"
        private_path = "/private/backup/customer-secret.json"
        result = subprocess.run(
            [sys.executable, str(script), "validate-security-evidence", private_path],
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertNotIn(private_path, result.stdout + result.stderr)
        self.assertNotIn("customer-secret", result.stdout + result.stderr)
        self.assertIn("security_evidence_unreadable", result.stderr)
        argument = subprocess.run(
            [sys.executable, str(script), private_path],
            check=False, capture_output=True, text=True,
        )
        self.assertEqual(argument.returncode, 2)
        self.assertNotIn(private_path, argument.stdout + argument.stderr)
        self.assertNotIn("customer-secret", argument.stdout + argument.stderr)
        self.assertEqual(argument.stderr, "bootstrap argument error\n")

    def test_cli_validates_and_never_has_execution_option(self) -> None:
        script = ROOT / "tools/bootstrap_offline.py"
        help_output = subprocess.run([sys.executable, str(script), "--help"], check=True, capture_output=True, text=True).stdout
        self.assertNotIn("execute", help_output.lower())
        self.assertNotIn("--port", help_output.lower())
        with tempfile.TemporaryDirectory(prefix="vk-bootstrap-") as directory:
            path = pathlib.Path(directory) / "candidate.bin"
            path.write_bytes(make_image())
            security = pathlib.Path(directory) / "security.json"
            security.write_text(json.dumps(SecurityTests().evidence()))
            partition_table = pathlib.Path(directory) / "partition-table.bin"
            partition_table.write_bytes(make_partition_table())
            partition_sha = hashlib.sha256(partition_table.read_bytes()).hexdigest()
            result = subprocess.run(
                [
                    sys.executable,
                    str(script),
                    "plan-stage",
                    str(path),
                    "--expected-sha256",
                    image_sha(path.read_bytes()),
                    "--authorization-sha256",
                    "2" * 64,
                    "--security-evidence",
                    str(security),
                    "--partition-table", str(partition_table),
                    "--expected-partition-table-sha256", partition_sha,
                    "--expected-project",
                    "vibe_keyboard",
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            value = json.loads(result.stdout)
            self.assertFalse(value["plan"]["executable"])
            self.assertEqual(value["plan"]["address"], "0x520000")
            self.assertEqual(value["plan"]["evidence_sha256"], value["evidence"]["binding_sha256"])

            primary = pathlib.Path(directory) / "primary.bin"
            secondary = pathlib.Path(directory) / "secondary.bin"
            ota0 = pathlib.Path(directory) / "ota0.bin"
            ota1 = pathlib.Path(directory) / "ota1.bin"
            primary.write_bytes(ota_sector(1))
            secondary.write_bytes(b"\xff" * SECTOR_SIZE)
            ota0.write_bytes(b"vendor-ota0")
            ota1.write_bytes(make_image())
            activation = subprocess.run(
                [sys.executable, str(script), "plan-activate",
                 "--primary-sector", str(primary),
                 "--expected-primary-sha256", hashlib.sha256(primary.read_bytes()).hexdigest(),
                 "--secondary-sector", str(secondary),
                 "--ota0", str(ota0),
                 "--expected-ota0-sha256", hashlib.sha256(ota0.read_bytes()).hexdigest(),
                 "--ota1", str(ota1),
                 "--expected-ota1-sha256", hashlib.sha256(ota1.read_bytes()).hexdigest(),
                 "--partition-table", str(partition_table),
                 "--expected-partition-table-sha256", partition_sha,
                 "--security-evidence", str(security),
                 "--authorization-sha256", "3" * 64],
                check=True, capture_output=True, text=True,
            )
            activation_value = json.loads(activation.stdout)
            self.assertFalse(activation_value["plan"]["executable"])
            self.assertEqual(activation_value["plan"]["address"], "0x012000")
            self.assertRegex(activation_value["plan"]["evidence_sha256"], r"^[0-9a-f]{64}$")
            self.assertNotIn(str(primary), activation.stdout)

            golden = pathlib.Path(directory) / "golden.bin"
            golden.write_bytes(make_secondary_otadata_sector(2))
            primary_after = pathlib.Path(directory) / "primary-after.bin"
            secondary_after = pathlib.Path(directory) / "secondary-after.bin"
            ota0_after = pathlib.Path(directory) / "ota0-after.bin"
            ota1_after = pathlib.Path(directory) / "ota1-after.bin"
            primary_after.write_bytes(primary.read_bytes())
            secondary_after.write_bytes(golden.read_bytes())
            ota0_after.write_bytes(ota0.read_bytes())
            ota1_after.write_bytes(ota1.read_bytes())
            protected = {name: hashlib.sha256(name.encode()).hexdigest() for name in ACTIVATION_PROTECTED_EVIDENCE_NAMES}
            protected["primary_otadata"] = hashlib.sha256(primary.read_bytes()).hexdigest()
            protected["ota_0"] = hashlib.sha256(ota0.read_bytes()).hexdigest()
            protected["ota_1"] = hashlib.sha256(ota1.read_bytes()).hexdigest()
            protected_before = pathlib.Path(directory) / "protected-before.json"
            protected_after = pathlib.Path(directory) / "protected-after.json"
            protected_before.write_text(json.dumps(protected))
            protected_after.write_text(json.dumps(protected))
            flash_before_path = pathlib.Path(directory) / "flash-before.bin"
            flash_after_path = pathlib.Path(directory) / "flash-after.bin"
            flash_before = bytearray(b"\xff" * FLASH_SIZE)
            flash_before[0x011000:0x012000] = primary.read_bytes()
            flash_before[0x012000:0x013000] = secondary.read_bytes()
            flash_before[0x020000:0x020000 + len(ota0.read_bytes())] = ota0.read_bytes()
            flash_before[OTA_1_OFFSET:OTA_1_OFFSET + len(ota1.read_bytes())] = ota1.read_bytes()
            flash_after = bytearray(flash_before)
            flash_after[0x012000:0x013000] = golden.read_bytes()
            flash_before_path.write_bytes(flash_before)
            flash_after_path.write_bytes(flash_after)
            post = subprocess.run(
                [sys.executable, str(script), "validate-activation-readback",
                 "--primary-before", str(primary), "--primary-after", str(primary_after),
                 "--secondary-before", str(secondary), "--secondary-after", str(secondary_after),
                 "--golden", str(golden), "--ota0-before", str(ota0), "--ota0-after", str(ota0_after),
                 "--ota1-before", str(ota1), "--ota1-after", str(ota1_after),
                 "--protected-before", str(protected_before), "--protected-after", str(protected_after),
                 "--flash-before", str(flash_before_path), "--flash-after", str(flash_after_path)],
                check=True, capture_output=True, text=True,
            )
            self.assertEqual(json.loads(post.stdout)["validation"], "passed")


if __name__ == "__main__":
    unittest.main()
