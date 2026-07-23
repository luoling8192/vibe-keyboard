import Foundation

public struct USBDeviceDescriptor: Equatable, Sendable {
    public let registryEntryID: UInt64
    public let vendorID: UInt16
    public let productID: UInt16
    public let serialNumber: String
    public let normalizedDeviceID: String
    public let calloutPath: String

    public init(
        registryEntryID: UInt64,
        vendorID: UInt16,
        productID: UInt16,
        serialNumber: String,
        normalizedDeviceID: String,
        calloutPath: String
    ) {
        self.registryEntryID = registryEntryID
        self.vendorID = vendorID
        self.productID = productID
        self.serialNumber = serialNumber
        self.normalizedDeviceID = normalizedDeviceID
        self.calloutPath = calloutPath
    }
}

public struct USBRegistrySnapshot: Equatable, Sendable {
    public let registryEntryID: UInt64
    public let calloutPath: String?
    public let properties: [String: USBRegistryValue]

    public init(registryEntryID: UInt64, calloutPath: String?, properties: [String: USBRegistryValue]) {
        self.registryEntryID = registryEntryID
        self.calloutPath = calloutPath
        self.properties = properties
    }
}

public enum USBRegistryValue: Equatable, Sendable {
    case integer(UInt64)
    case string(String)
}

public enum USBDiscoveryError: Error, Equatable, Sendable {
    case registryFailure(Int32)
    case malformedProperty(entryID: UInt64, property: String)
    case invalidSerialNumber(String)
    case deviceNotFound
    case ambiguousDevices([USBDeviceDescriptor])
}

public protocol USBRegistryProviding: Sendable {
    func snapshots() throws -> [USBRegistrySnapshot]
}

public enum USBDeviceDiscovery {
    public static let targetVendorID: UInt16 = 0x303a
    public static let targetProductID: UInt16 = 0x1001

    public static func normalizedDeviceID(from serialNumber: String) throws -> String {
        let hex = serialNumber.uppercased().filter { $0.isHexDigit }
        guard hex.count >= 12 else { throw USBDiscoveryError.invalidSerialNumber(serialNumber) }
        return String(hex.suffix(12))
    }

    public static func descriptors(from snapshots: [USBRegistrySnapshot]) throws -> [USBDeviceDescriptor] {
        var result: [USBDeviceDescriptor] = []
        for snapshot in snapshots {
            guard case .integer(let rawVendor)? = snapshot.properties["idVendor"],
                  case .integer(let rawProduct)? = snapshot.properties["idProduct"] else { continue }
            guard rawVendor == UInt64(targetVendorID), rawProduct == UInt64(targetProductID) else { continue }
            guard let path = snapshot.calloutPath, !path.isEmpty else {
                throw USBDiscoveryError.malformedProperty(entryID: snapshot.registryEntryID, property: "IOCalloutDevice")
            }
            let serial: String
            if case .string(let value)? = snapshot.properties["USB Serial Number"], !value.isEmpty {
                serial = value
            } else if case .string(let value)? = snapshot.properties["kUSBSerialNumberString"], !value.isEmpty {
                serial = value
            } else {
                throw USBDiscoveryError.malformedProperty(entryID: snapshot.registryEntryID, property: "USB Serial Number")
            }
            result.append(USBDeviceDescriptor(
                registryEntryID: snapshot.registryEntryID,
                vendorID: targetVendorID,
                productID: targetProductID,
                serialNumber: serial,
                normalizedDeviceID: try normalizedDeviceID(from: serial),
                calloutPath: path
            ))
        }
        return result.sorted { $0.registryEntryID < $1.registryEntryID }
    }

    public static func selectOne(from snapshots: [USBRegistrySnapshot]) throws -> USBDeviceDescriptor {
        let matches = try descriptors(from: snapshots)
        guard !matches.isEmpty else { throw USBDiscoveryError.deviceNotFound }
        guard matches.count == 1 else { throw USBDiscoveryError.ambiguousDevices(matches) }
        return matches[0]
    }
}
