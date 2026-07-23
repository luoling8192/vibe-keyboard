#if os(macOS)
import Foundation
import IOKit
import IOKit.serial

public struct IOKitUSBRegistryProvider: USBRegistryProviding {
    public init() {}

    public func snapshots() throws -> [USBRegistrySnapshot] {
        guard let matching = IOServiceMatching(kIOSerialBSDServiceValue) else {
            throw USBDiscoveryError.registryFailure(KERN_FAILURE)
        }
        let dictionary = matching as NSMutableDictionary
        dictionary[kIOSerialBSDTypeKey] = kIOSerialBSDAllTypes

        var iterator: io_iterator_t = 0
        let result = IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator)
        guard result == KERN_SUCCESS else { throw USBDiscoveryError.registryFailure(result) }
        defer { IOObjectRelease(iterator) }

        var snapshots: [USBRegistrySnapshot] = []
        while true {
            let service = IOIteratorNext(iterator)
            guard service != 0 else { break }
            snapshots.append(snapshot(service: service))
            IOObjectRelease(service)
        }
        return snapshots
    }

    private func snapshot(service: io_registry_entry_t) -> USBRegistrySnapshot {
        var entryID: UInt64 = 0
        IORegistryEntryGetRegistryEntryID(service, &entryID)
        let path = stringProperty(service, key: kIOCalloutDeviceKey)
        var values: [String: USBRegistryValue] = [:]
        var current = service
        var ownsCurrent = false
        defer { if ownsCurrent { IOObjectRelease(current) } }

        while current != 0 {
            copyProperty(current, key: "idVendor", into: &values)
            copyProperty(current, key: "idProduct", into: &values)
            copyProperty(current, key: "USB Serial Number", into: &values)
            copyProperty(current, key: "kUSBSerialNumberString", into: &values)

            var parent: io_registry_entry_t = 0
            let status = IORegistryEntryGetParentEntry(current, kIOServicePlane, &parent)
            if ownsCurrent { IOObjectRelease(current) }
            guard status == KERN_SUCCESS else {
                ownsCurrent = false
                break
            }
            current = parent
            ownsCurrent = true
        }
        return USBRegistrySnapshot(registryEntryID: entryID, calloutPath: path, properties: values)
    }

    private func copyProperty(_ entry: io_registry_entry_t, key: String, into values: inout [String: USBRegistryValue]) {
        guard values[key] == nil,
              let property = IORegistryEntryCreateCFProperty(entry, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() else { return }
        if let number = property as? NSNumber {
            values[key] = .integer(number.uint64Value)
        } else if let string = property as? String {
            values[key] = .string(string)
        }
    }

    private func stringProperty(_ entry: io_registry_entry_t, key: String) -> String? {
        guard let property = IORegistryEntryCreateCFProperty(entry, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() else { return nil }
        return property as? String
    }
}
#endif
