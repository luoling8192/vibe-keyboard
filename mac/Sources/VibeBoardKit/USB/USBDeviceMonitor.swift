import Foundation

public enum USBDeviceMonitorError: Error, Equatable, Sendable {
    case discovery(USBDiscoveryError)
    case unexpected(String)
    case eventBufferOverflow
}

public enum USBDeviceMonitorEvent: Equatable, Sendable {
    case attached(USBDeviceDescriptor)
    case detached(registryEntryID: UInt64)
    case failed(USBDeviceMonitorError)
}

public protocol USBDeviceMonitoring: Sendable {
    func events() -> AsyncStream<USBDeviceMonitorEvent>
}

public final class USBDeviceMonitor: USBDeviceMonitoring, Sendable {
    private let provider: any USBRegistryProviding
    private let interval: Duration
    private let eventBufferCapacity: Int

    public init(
        provider: any USBRegistryProviding = IOKitUSBRegistryProvider(),
        interval: Duration = .seconds(1),
        eventBufferCapacity: Int = 32
    ) {
        precondition(eventBufferCapacity > 0)
        self.provider = provider
        self.interval = interval
        self.eventBufferCapacity = eventBufferCapacity
    }

    public func events() -> AsyncStream<USBDeviceMonitorEvent> {
        AsyncStream(bufferingPolicy: .bufferingNewest(eventBufferCapacity)) { continuation in
            let task = Task {
                var known: [UInt64: USBDeviceDescriptor] = [:]
                while !Task.isCancelled {
                    do {
                        let snapshots = try provider.snapshots()
                        let descriptors = try USBDeviceDiscovery.descriptors(from: snapshots)
                        let current = Dictionary(uniqueKeysWithValues: descriptors.map { ($0.registryEntryID, $0) })
                        for (id, descriptor) in current where known[id] == nil {
                            guard Self.yield(.attached(descriptor), to: continuation) else { return }
                        }
                        for id in known.keys where current[id] == nil {
                            guard Self.yield(.detached(registryEntryID: id), to: continuation) else { return }
                        }
                        known = current
                    } catch let error as USBDiscoveryError {
                        guard Self.yield(.failed(.discovery(error)), to: continuation) else { return }
                    } catch {
                        guard Self.yield(.failed(.unexpected(String(describing: error))), to: continuation) else { return }
                    }
                    do {
                        try await Task.sleep(for: interval)
                    } catch {
                        break
                    }
                }
                continuation.finish()
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    private static func yield(
        _ event: USBDeviceMonitorEvent,
        to continuation: AsyncStream<USBDeviceMonitorEvent>.Continuation
    ) -> Bool {
        switch continuation.yield(event) {
        case .enqueued:
            return true
        case .dropped:
            _ = continuation.yield(.failed(.eventBufferOverflow))
            continuation.finish()
            return false
        case .terminated:
            return false
        @unknown default:
            continuation.finish()
            return false
        }
    }
}
