#!/usr/bin/env python3

from __future__ import annotations

import copy
import pathlib
import struct
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests"))

from bootloader_migration import MigrationValidationError, build_artifact, validate_artifact  # noqa: E402
from test_bootstrap_offline import make_partition_table  # noqa: E402


class BootloaderMigrationTests(unittest.TestCase):
    def setUp(self) -> None:
        header = bytearray(24)
        header[0] = 0xE9
        header[1] = 1
        struct.pack_into("<H", header, 12, 9)
        self.bootloader = bytes(header) + bytes(range(1, 128))
        self.table = make_partition_table()
        self.security = {"secure_boot": "disabled", "flash_encryption": "disabled", "anti_rollback": "disabled"}
        self.artifact = build_artifact(bootloader=self.bootloader, partition_table=self.table,
                                       security=self.security, rom_recovery="reviewed_external_artifact")

    def test_valid_artifact(self) -> None:
        result = validate_artifact(self.artifact, self.bootloader, self.table)
        self.assertEqual(result.rollback_policy, "bootloader_pending_verify")
        self.assertEqual(result.managed_slots[1], ("ota_1", 0x520000, 0x500000))

    def test_tampering_and_wrong_image_fail(self) -> None:
        changed = copy.deepcopy(self.artifact)
        changed["managed_slots"][0]["offset"] = 0x30000
        with self.assertRaises(MigrationValidationError):
            validate_artifact(changed, self.bootloader, self.table)
        with self.assertRaises(MigrationValidationError):
            validate_artifact(self.artifact, self.bootloader + b"x", self.table)

    def test_unknown_security_and_missing_recovery_fail(self) -> None:
        with self.assertRaises(MigrationValidationError):
            build_artifact(bootloader=self.bootloader, partition_table=self.table,
                           security={**self.security, "secure_boot": "unknown"},
                           rom_recovery="reviewed_external_artifact")
        changed = copy.deepcopy(self.artifact)
        changed["rom_recovery"] = "assumed"
        with self.assertRaises(MigrationValidationError):
            validate_artifact(changed, self.bootloader, self.table)

    def test_bootloader_region_and_magic_fail(self) -> None:
        with self.assertRaises(MigrationValidationError):
            build_artifact(bootloader=b"\xe9" * 0x8001, partition_table=self.table,
                           security=self.security, rom_recovery="reviewed_external_artifact")
        with self.assertRaises(MigrationValidationError):
            build_artifact(bootloader=b"bad", partition_table=self.table,
                           security=self.security, rom_recovery="reviewed_external_artifact")


if __name__ == "__main__":
    unittest.main()
