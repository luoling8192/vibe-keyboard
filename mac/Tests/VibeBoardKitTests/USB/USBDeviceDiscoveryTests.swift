import Foundation
import Testing
@testable import VibeBoardKit

@Suite("USB discovery")
struct USBDeviceDiscoveryTests {
    @Test func filtersExactTargetAndNormalizesSerial() throws {
        let target = snapshot(id: 2, vendor: 0x303a, product: 0x1001, serial: "02:00:00:00:00:01")
        let wrongProduct = snapshot(id: 1, vendor: 0x303a, product: 0x9999, serial: "02:00:00:00:00:01")
        let wrongVendor = snapshot(id: 3, vendor: 0x9999, product: 0x1001, serial: "02:00:00:00:00:01")
        let matches = try USBDeviceDiscovery.descriptors(from: [wrongProduct, wrongVendor, target])
        #expect(matches.count == 1)
        #expect(matches[0].normalizedDeviceID == "020000000001")
        #expect(matches[0].calloutPath == "/dev/cu.test2")
    }

    @Test func usesSerialFallback() throws {
        let value = USBRegistrySnapshot(
            registryEntryID: 1,
            calloutPath: "/dev/cu.test",
            properties: [
                "idVendor": .integer(0x303a),
                "idProduct": .integer(0x1001),
                "kUSBSerialNumberString": .string("AA-BB-CC-DD-EE-FF")
            ]
        )
        #expect(try USBDeviceDiscovery.descriptors(from: [value])[0].normalizedDeviceID == "AABBCCDDEEFF")
    }

    @Test func malformedTargetAndSelectionErrorsAreTyped() throws {
        var missingPath = snapshot(id: 1, vendor: 0x303a, product: 0x1001, serial: "AABBCCDDEEFF")
        missingPath = USBRegistrySnapshot(registryEntryID: missingPath.registryEntryID, calloutPath: nil, properties: missingPath.properties)
        #expect(throws: USBDiscoveryError.malformedProperty(entryID: 1, property: "IOCalloutDevice")) {
            try USBDeviceDiscovery.descriptors(from: [missingPath])
        }
        #expect(throws: USBDiscoveryError.deviceNotFound) { try USBDeviceDiscovery.selectOne(from: []) }
        let two = [snapshot(id: 1, vendor: 0x303a, product: 0x1001, serial: "AABBCCDDEEFF"), snapshot(id: 2, vendor: 0x303a, product: 0x1001, serial: "112233445566")]
        #expect(throws: USBDiscoveryError.ambiguousDevices(try USBDeviceDiscovery.descriptors(from: two))) {
            try USBDeviceDiscovery.selectOne(from: two)
        }
    }

    private func snapshot(id: UInt64, vendor: UInt64, product: UInt64, serial: String) -> USBRegistrySnapshot {
        USBRegistrySnapshot(registryEntryID: id, calloutPath: "/dev/cu.test\(id)", properties: [
            "idVendor": .integer(vendor), "idProduct": .integer(product), "USB Serial Number": .string(serial)
        ])
    }
}
