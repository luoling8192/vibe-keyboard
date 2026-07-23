#!/usr/bin/env python3
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
LOCK = ROOT / "dependencies.lock"
ESP_SR = ROOT / "managed_components/espressif__esp-sr"
MICRO_OPUS = ROOT / "managed_components/esphome__micro-opus"

EXPECTED = {
    "espressif/esp-sr": ("2.1.4", "3903f0880cc3065765bd4038e01cbfa7907c8052ecf0a4f7a70c4444a26c1737"),
    "esphome/micro-opus": ("0.4.1", "c4cec51b6e45b9b660bf8725a10c65f46485ff8b37ff664e4da3fd738301c71e"),
}
EXPECTED_COMMITS = {
    ESP_SR / "idf_component.yml": "85a1c634325cecf99377e6fdb385b03a5c3363ce",
    MICRO_OPUS / "idf_component.yml": "8354085908683c6130e32a832aeec8a7ca115c51",
}


def locked_component(text: str, name: str) -> tuple[str, str]:
    match = re.search(
        rf"^  {re.escape(name)}:\n(?P<body>(?:    .*\n)+?)(?=^  [^ ].*:\n|^direct_dependencies:)",
        text,
        re.MULTILINE,
    )
    if match is None:
        raise SystemExit(f"missing locked component: {name}")
    body = match.group("body")
    version = re.search(r"^    version: ['\"]?([^'\"\n]+)", body, re.MULTILINE)
    component_hash = re.search(r"^    component_hash: ([0-9a-f]{64})$", body, re.MULTILINE)
    if version is None or component_hash is None:
        raise SystemExit(f"incomplete lock entry: {name}")
    return version.group(1), component_hash.group(1)


text = LOCK.read_text()
for name, expected in EXPECTED.items():
    actual = locked_component(text, name)
    if actual != expected:
        raise SystemExit(f"lock mismatch for {name}: expected {expected}, got {actual}")

if "version: 5.5.2" not in text or "target: esp32s3" not in text:
    raise SystemExit("dependency lock is not ESP-IDF 5.5.2 / ESP32-S3")

for component, expected_hash in ((ESP_SR, EXPECTED["espressif/esp-sr"][1]), (MICRO_OPUS, EXPECTED["esphome/micro-opus"][1])):
    actual_hash = (component / ".component_hash").read_text().strip()
    if actual_hash != expected_hash:
        raise SystemExit(f"managed component hash mismatch: {component.name}")

for manifest, expected_commit in EXPECTED_COMMITS.items():
    if expected_commit not in manifest.read_text():
        raise SystemExit(f"managed component commit mismatch: {manifest.parent.name}")

required_notices = {
    ROOT / "third_party/notices/ESP-SR-2.1.4-LICENSE.txt": ESP_SR / "LICENSE",
    ROOT / "third_party/notices/micro-opus-0.4.1-Apache-2.0.txt": MICRO_OPUS / "LICENSE",
    ROOT / "third_party/notices/Opus-BSD-COPYING.txt": MICRO_OPUS / "lib/opus/COPYING",
}
missing = [str(path.relative_to(ROOT)) for path in required_notices if not path.is_file()]
if missing:
    raise SystemExit(f"missing dependency notices: {missing}")
for notice, source in required_notices.items():
    if notice.read_bytes() != source.read_bytes():
        raise SystemExit(
            f"dependency notice differs from pinned source: {notice.relative_to(ROOT)}"
        )

print("audio dependency lock verified: ESP-SR 2.1.4, micro-opus 0.4.1, IDF 5.5.2, esp32s3")
