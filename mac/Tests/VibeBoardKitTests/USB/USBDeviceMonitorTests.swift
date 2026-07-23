import Foundation
import Testing
@testable import VibeBoardKit

@Suite("USB device monitor")
struct USBDeviceMonitorTests {
    @Test func surfacesFailureRecoversAndCorrelatesAttachDetach() async throws {
        let target = snapshot(id: 1)
        let provider = FakeRegistryProvider(results: [
            .failure(.registryFailure(5)),
            .success([target]),
            .success([])
        ])
        let monitor = USBDeviceMonitor(provider: provider, interval: .milliseconds(2), eventBufferCapacity: 8)
        var iterator = monitor.events().makeAsyncIterator()

        #expect(await iterator.next() == .failed(.discovery(.registryFailure(5))))
        guard case .attached(let descriptor)? = await iterator.next() else {
            Issue.record("Expected attach")
            return
        }
        #expect(descriptor.registryEntryID == 1)
        #expect(await iterator.next() == .detached(registryEntryID: 1))
    }

    @Test func malformedTargetIsSurfacedAndStreamCancellationStopsPolling() async throws {
        let malformed = USBRegistrySnapshot(
            registryEntryID: 2,
            calloutPath: nil,
            properties: [
                "idVendor": .integer(0x303a),
                "idProduct": .integer(0x1001),
                "USB Serial Number": .string("AABBCCDDEEFF")
            ]
        )
        let provider = FakeRegistryProvider(results: [.success([malformed])])
        let monitor = USBDeviceMonitor(provider: provider, interval: .milliseconds(2))
        let task = Task { () -> USBDeviceMonitorEvent? in
            for await event in monitor.events() { return event }
            return nil
        }
        #expect(await task.value == .failed(.discovery(.malformedProperty(entryID: 2, property: "IOCalloutDevice"))))
        try await waitUntil { provider.callCount >= 1 }
        try await Task.sleep(for: .milliseconds(15))
        let stoppedCount = provider.callCount
        try await Task.sleep(for: .milliseconds(15))
        #expect(provider.callCount == stoppedCount)
    }

    @Test func eventOverflowIsBoundedAndTerminal() async throws {
        let provider = AlternatingRegistryProvider()
        let monitor = USBDeviceMonitor(provider: provider, interval: .milliseconds(1), eventBufferCapacity: 1)
        let stream = monitor.events()
        try await Task.sleep(for: .milliseconds(30))

        var received: [USBDeviceMonitorEvent] = []
        for await event in stream { received.append(event) }
        #expect(received.count <= 1)
        #expect(received.contains(.failed(.eventBufferOverflow)))
    }

    private func snapshot(id: UInt64) -> USBRegistrySnapshot {
        USBRegistrySnapshot(
            registryEntryID: id,
            calloutPath: "/dev/cu.test\(id)",
            properties: [
                "idVendor": .integer(0x303a),
                "idProduct": .integer(0x1001),
                "USB Serial Number": .string("AABBCCDDEEFF")
            ]
        )
    }
}

private final class FakeRegistryProvider: USBRegistryProviding, @unchecked Sendable {
    private let lock = NSLock()
    private var results: [Result<[USBRegistrySnapshot], USBDiscoveryError>]
    private var _callCount = 0

    init(results: [Result<[USBRegistrySnapshot], USBDiscoveryError>]) { self.results = results }

    var callCount: Int { lock.withLock { _callCount } }

    func snapshots() throws -> [USBRegistrySnapshot] {
        try lock.withLock {
            _callCount += 1
            let result = results.isEmpty ? .success([]) : results.removeFirst()
            return try result.get()
        }
    }
}

private final class AlternatingRegistryProvider: USBRegistryProviding, @unchecked Sendable {
    private let lock = NSLock()
    private var attached = false

    func snapshots() throws -> [USBRegistrySnapshot] {
        lock.withLock {
            attached.toggle()
            guard attached else { return [] }
            return [USBRegistrySnapshot(
                registryEntryID: 1,
                calloutPath: "/dev/cu.test",
                properties: [
                    "idVendor": .integer(0x303a),
                    "idProduct": .integer(0x1001),
                    "USB Serial Number": .string("AABBCCDDEEFF")
                ]
            )]
        }
    }
}

private func waitUntil(_ predicate: @escaping @Sendable () -> Bool) async throws {
    for _ in 0..<100 {
        if predicate() { return }
        try await Task.sleep(for: .milliseconds(1))
    }
    Issue.record("Timed out waiting for condition")
}
