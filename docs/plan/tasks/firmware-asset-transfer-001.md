---
id: firmware-asset-transfer-001
scope: USB asset transfer integration
status: in-progress
depends-on: [firmware-asset-store-001, asset-protocol-001]
---

## Objective

Connect typed USB asset commands/chunks to the real SPIFFS store and expose accurate runtime capabilities.

## Context

- `docs/product/usb-protocol.md`
- `docs/product/screen-assets.md`

## Path

- `firmware/components/vk_usb/`
- `firmware/components/vk_assets/`
- `firmware/main/`
- `firmware/tests/`

## Contract

- Implement begin/query/ready/chunk/progress/end/stored/abort/paged-list/delete with exact tuple idempotency, `1...min(upload_max_bytes,max_asset_bytes)` admission, 30-second transfer idle close, a separate 30,000 ms successful-page-refreshed catalog deadline, sidecar resume, optimistic expected revision, and typed errors.
- Advertise optional/unavailable/available blocks accurately; allow zero upload/free capacity while preserving list/delete management.
- Disconnect/lease expiry closes temporary transfer state without activation. Resume trusts only durable device-reported offset.
- Asset and staged-update mutation are mutually exclusive.

## Verification

- Integration tests cover duplicate/out-of-order/wrong ID, reconnect/resume, maximum chunk, write/hash/no-space failure, concurrent mutation rejection, response ordering, and reboot persistence.
- No transfer buffers a complete asset or changes the previous active revision before commit.

## Offline implementation evidence

The first integrated vertical slice is implemented in `vk_asset_transfer` and remains
`in-progress` pending screen-manifest integration and the combined review.

- Typed capability and asset registrations are installed only before USB runtime start.
  Provider output is derived from mounted store state; production availability remains an
  explicit gate and does not follow handler presence.
- Begin and reconnect query bind USB authorization to the store's durable sidecar tuple and
  exact reported offset. Chunk success is the store append+sidecar sync boundary. End performs
  complete hash/VKA1 validation and immutable publication; abort removes only temporary files.
- Store status, bounded sorted catalog enumeration, retained-reference detection, optimistic
  revision deletion, and a refreshable 30,000 ms catalog snapshot helper are present. Format is
  not registered or invoked.
- Epoch-local USB authorization can be discarded without deleting the durable part/sidecar;
  a later explicit query restores only a validated store tuple.

Offline commands completed without device access:

```text
python3 firmware/tests/run_native_tests.py
# complete native runner passed

cc ... -fsanitize=address,undefined ... test_asset_transfer.c ...
ASAN_OPTIONS=detect_leaks=0 /tmp/test_asset_transfer_san
# passed

python3 firmware/tests/test_contract.py
# 21 run / 20 passed / 1 documented skip

idf.py -B /tmp/vk-transfer-build \
  -D SDKCONFIG=/tmp/vk-transfer-sdkconfig build
# 1969/1969 passed
```

Offline image evidence:

```text
size: 1,140,640 bytes
SHA-256: 4f252c7c1e07df6b6f1e11bc44d4196460747ad1479b1423c107ed87d0bc026b
```

This does not authorize SPIFFS reads/writes/format on hardware, asset mutation, flash, or reset.
Production registration remains deferred until physical storage/display acceptance can make the
capability truthful.

## Production composition follow-up (2026-07-22)

- Asset transfer registration is now composed with the durable store and typed screen/widget handlers. Sealing only publishes an immutable VKA1 asset; screen state changes only through a validated commit whose VKC1 durable publication succeeds.
- Production mount never auto-formats and capability remains unavailable behind font/display/physical gates. Native firmware and Swift suites passed; ESP-IDF was unavailable in this agent environment. No device/storage mutation occurred and status remains `in-progress`.
