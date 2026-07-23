#!/usr/bin/env python3
"""Create non-executable, reviewed bootstrap plans. No device transport exists here."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys

from bootstrap_core import (
    OTA_1_OFFSET,
    SECONDARY_OTADATA_OFFSET,
    SECTOR_SIZE,
    BootstrapValidationError,
    bounded_json,
    activation_evidence_binding,
    decode_ota_entry,
    make_secondary_otadata_sector,
    safe_plan,
    stage_evidence_binding,
    validate_activation_inputs,
    validate_activation_post_evidence,
    validate_candidate_image,
    validate_security_evidence,
)


def _lower_sha(value: str) -> str:
    if len(value) != 64 or value.lower() != value:
        raise argparse.ArgumentTypeError("expected 64 lowercase hex SHA-256 characters")
    try:
        bytes.fromhex(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("invalid SHA-256") from error
    return value


class SafeArgumentParser(argparse.ArgumentParser):
    def error(self, message: str) -> None:
        del message
        self.exit(2, "bootstrap argument error\n")


def parser() -> argparse.ArgumentParser:
    root = SafeArgumentParser(description=__doc__)
    subcommands = root.add_subparsers(dest="command", required=True)

    image = subcommands.add_parser("validate-image", help="validate a reviewed local ESP32-S3 application")
    image.add_argument("image", type=pathlib.Path)
    image.add_argument("--expected-sha256", required=True, type=_lower_sha)
    image.add_argument("--security-evidence", required=True, type=pathlib.Path)
    image.add_argument("--expected-project")
    image.add_argument("--expected-version")

    security = subcommands.add_parser("validate-security-evidence", help="validate explicit read-only evidence JSON")
    security.add_argument("evidence", type=pathlib.Path)

    stage = subcommands.add_parser("plan-stage", help="emit a non-executable ota_1 plan")
    stage.add_argument("image", type=pathlib.Path)
    stage.add_argument("--expected-sha256", required=True, type=_lower_sha)
    stage.add_argument("--authorization-sha256", required=True, type=_lower_sha)
    stage.add_argument("--security-evidence", required=True, type=pathlib.Path)
    stage.add_argument("--partition-table", required=True, type=pathlib.Path)
    stage.add_argument("--expected-partition-table-sha256", required=True, type=_lower_sha)
    stage.add_argument("--expected-project")
    stage.add_argument("--expected-version")

    activate = subcommands.add_parser("plan-activate", help="emit a non-executable secondary-otadata plan")
    activate.add_argument("--primary-sector", required=True, type=pathlib.Path)
    activate.add_argument("--expected-primary-sha256", required=True, type=_lower_sha)
    activate.add_argument("--secondary-sector", required=True, type=pathlib.Path)
    activate.add_argument("--ota0", required=True, type=pathlib.Path)
    activate.add_argument("--expected-ota0-sha256", required=True, type=_lower_sha)
    activate.add_argument("--ota1", required=True, type=pathlib.Path)
    activate.add_argument("--expected-ota1-sha256", required=True, type=_lower_sha)
    activate.add_argument("--partition-table", required=True, type=pathlib.Path)
    activate.add_argument("--expected-partition-table-sha256", required=True, type=_lower_sha)
    activate.add_argument("--security-evidence", required=True, type=pathlib.Path)
    activate.add_argument("--authorization-sha256", required=True, type=_lower_sha)
    activate.add_argument("--output-golden", type=pathlib.Path)

    post = subcommands.add_parser("validate-activation-readback", help="validate offline activation readback evidence")
    for name in ("primary-before", "primary-after", "secondary-before", "secondary-after", "golden", "ota0-before", "ota0-after", "ota1-before", "ota1-after"):
        post.add_argument(f"--{name}", required=True, type=pathlib.Path)
    post.add_argument("--protected-before", required=True, type=pathlib.Path)
    post.add_argument("--protected-after", required=True, type=pathlib.Path)
    post.add_argument("--flash-before", required=True, type=pathlib.Path)
    post.add_argument("--flash-after", required=True, type=pathlib.Path)
    return root


def _read_bytes(path: pathlib.Path, field: str) -> bytes:
    try:
        return path.read_bytes()
    except OSError as error:
        raise BootstrapValidationError(f"{field}_unreadable") from error


def _read_json(path: pathlib.Path, field: str) -> dict[str, object]:
    try:
        text = path.read_text()
    except OSError as error:
        raise BootstrapValidationError(f"{field}_unreadable") from error
    try:
        value = json.loads(text)
    except (json.JSONDecodeError, UnicodeError) as error:
        raise BootstrapValidationError(f"{field}_invalid_json") from error
    if not isinstance(value, dict):
        raise BootstrapValidationError(f"{field}_not_object")
    return value


def _write_bytes(path: pathlib.Path, value: bytes, field: str) -> None:
    try:
        path.write_bytes(value)
    except OSError as error:
        raise BootstrapValidationError(f"{field}_unwritable") from error


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if args.command in {"validate-image", "plan-stage"}:
            image = _read_bytes(args.image, "candidate")
            security_value = _read_json(args.security_evidence, "security_evidence")
            validate_security_evidence(security_value)
            evidence = validate_candidate_image(
                image,
                expected_sha256=args.expected_sha256,
                chip_revision=security_value["chip_revision"],
                efuse_block_revision=security_value["efuse_block_revision"],
                disable_wafer_version_major=security_value["disable_wafer_version_major"],
                disable_efuse_block_version_major=security_value["disable_efuse_block_version_major"],
                expected_project=args.expected_project,
                expected_version=args.expected_version,
            )
            result = {
                "schema": 1,
                "validation": "passed",
                "target": "esp32s3",
                "image": {
                    "size": evidence.size,
                    "sha256": evidence.sha256,
                    "validation_hash": evidence.validation_hash,
                    "min_revision": evidence.min_revision,
                    "max_revision": evidence.max_revision,
                    "project": evidence.descriptor.project,
                    "version": evidence.descriptor.version,
                    "secure_version": evidence.descriptor.secure_version,
                    "min_efuse_block_revision": evidence.descriptor.min_efuse_block_revision,
                    "max_efuse_block_revision": evidence.descriptor.max_efuse_block_revision,
                    "mmu_page_size": evidence.descriptor.mmu_page_size,
                },
            }
            if args.command == "plan-stage":
                partition_table = _read_bytes(args.partition_table, "partition_table")
                binding = stage_evidence_binding(
                    image=image,
                    expected_sha256=args.expected_sha256,
                    security=security_value,
                    partition_table=partition_table,
                    expected_partition_table_sha256=args.expected_partition_table_sha256,
                )
                result["evidence"] = binding
                result["plan"] = safe_plan(
                    "stage",
                    address=OTA_1_OFFSET,
                    length=evidence.size,
                    sha256=evidence.sha256,
                    authorization_sha256=args.authorization_sha256,
                    evidence_sha256=binding["binding_sha256"],
                )
        elif args.command == "validate-security-evidence":
            value = _read_json(args.evidence, "security_evidence")
            validate_security_evidence(value)
            result = {"schema": 1, "validation": "passed", "security_gate": "reviewed_compatible"}
        elif args.command == "plan-activate":
            primary = _read_bytes(args.primary_sector, "primary_sector")
            secondary = _read_bytes(args.secondary_sector, "secondary_sector")
            ota0 = _read_bytes(args.ota0, "ota0")
            ota1 = _read_bytes(args.ota1, "ota1")
            partition_table = _read_bytes(args.partition_table, "partition_table")
            security_value = _read_json(args.security_evidence, "security_evidence")
            validate_security_evidence(security_value)
            binding = activation_evidence_binding(
                primary=primary,
                secondary=secondary,
                ota0=ota0,
                ota1=ota1,
                expected_primary_sha256=args.expected_primary_sha256,
                expected_ota0_sha256=args.expected_ota0_sha256,
                expected_ota1_sha256=args.expected_ota1_sha256,
                partition_table=partition_table,
                expected_partition_table_sha256=args.expected_partition_table_sha256,
            )
            primary_entry = decode_ota_entry(primary)
            if not primary_entry.valid or primary_entry.selected_slot != 0 or primary_entry.sequence >= 0xFFFFFFFD:
                raise BootstrapValidationError("primary sequence cannot produce a reviewed non-wrapping ota_1 record")
            sequence = primary_entry.sequence + 1
            if (sequence - 1) % 2 != 1:
                sequence += 1
            golden = make_secondary_otadata_sector(sequence)
            validate_activation_inputs(primary, secondary, golden)
            if args.output_golden is not None:
                _write_bytes(args.output_golden, golden, "golden_output")
            digest = hashlib.sha256(golden).hexdigest()
            security_binding = hashlib.sha256(
                json.dumps(security_value, sort_keys=True, separators=(",", ":")).encode()
            ).hexdigest()
            combined_binding = hashlib.sha256(
                json.dumps({**binding, "security_evidence_sha256": security_binding}, sort_keys=True, separators=(",", ":")).encode()
            ).hexdigest()
            result = {
                "schema": 1,
                "validation": "passed",
                "evidence": {**binding, "security_evidence_sha256": security_binding},
                "golden": {"sequence": sequence, "slot": "ota_1", "size": len(golden), "sha256": digest},
                "plan": safe_plan(
                    "activate",
                    address=SECONDARY_OTADATA_OFFSET,
                    length=SECTOR_SIZE,
                    sha256=digest,
                    authorization_sha256=args.authorization_sha256,
                    evidence_sha256=combined_binding,
                ),
            }
        else:
            before = _read_json(args.protected_before, "protected_before")
            after = _read_json(args.protected_after, "protected_after")
            result = {
                "schema": 1,
                "validation": "passed",
                "evidence": validate_activation_post_evidence(
                    primary_before=_read_bytes(args.primary_before, "primary_before"),
                    primary_after=_read_bytes(args.primary_after, "primary_after"),
                    secondary_before=_read_bytes(args.secondary_before, "secondary_before"),
                    secondary_after=_read_bytes(args.secondary_after, "secondary_after"),
                    golden=_read_bytes(args.golden, "golden"),
                    ota0_before=_read_bytes(args.ota0_before, "ota0_before"),
                    ota0_after=_read_bytes(args.ota0_after, "ota0_after"),
                    ota1_before=_read_bytes(args.ota1_before, "ota1_before"),
                    ota1_after=_read_bytes(args.ota1_after, "ota1_after"),
                    protected_before=before,
                    protected_after=after,
                    flash_before=_read_bytes(args.flash_before, "flash_before"),
                    flash_after=_read_bytes(args.flash_after, "flash_after"),
                ),
            }
        print(bounded_json(result))
        return 0
    except BootstrapValidationError as error:
        message = str(error)
        if len(message.encode("utf-8", errors="ignore")) > 256:
            message = "bounded_validation_failure"
        print(f"bootstrap validation failed: {message}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
