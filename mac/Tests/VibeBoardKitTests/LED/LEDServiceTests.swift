import Foundation
import Testing
@testable import VibeBoardKit

private final class LEDTestClock: USBMonotonicClock, @unchecked Sendable {
    private let lock = NSLock()
    private var value: UInt64
    init(_ value: UInt64 = 0) { self.value = value }
    func nowNanoseconds() -> UInt64 { lock.withLock { value } }
    func set(_ value: UInt64) { lock.withLock { self.value = value } }
}

private actor LEDWriter {
    var frames: [Data] = []
    func write(_ data: Data) { frames.append(data) }
    func count() -> Int { frames.count }
    func bodies() throws -> [Data] { try frames.map(FrameDecoder.decodeStateBody) }
}

struct LEDServiceTests {
    @Test func unavailableBlocksConfigButAllowsQuery() async throws {
        let writer = LEDWriter()
        let service = LEDService(writer: { await writer.write($0) })
        await service.replaceContext(try context(#"{"version":1,"available":false,"reason":"calibration_required"}"#, epoch: 1, snapshot: 1))
        await #expect(throws: LEDServiceError.unavailable("calibration_required")) {
            try await service.configure(enabled: true, brightness: 1)
        }
        #expect(try await service.query() == 1)
        #expect(await writer.count() == 1)
    }

    @Test func appliedResponseMustBeStrictlyBeforeDeadline() async throws {
        let writer = LEDWriter(); let clock = LEDTestClock(100)
        let service = LEDService(writer: { await writer.write($0) }, clock: clock)
        await service.replaceContext(try context(availableLED, epoch: 1, snapshot: 1))
        let request = try await service.configure(enabled: true, brightness: 32)
        let response = LEDProtocolEvent.state(LEDStateEvent(requestID: request, source: .applied, available: true, reason: nil, enabled: true, brightness: 32, effective: .connected))
        await #expect(throws: LEDServiceError.timedOut) { try await service.consume(response, at: 1_000_000_100) }
        #expect(await service.currentState()?.brightness == 32)
    }

    @Test func queryDoesNotAcknowledgeAndRetryIsByteEquivalent() async throws {
        let writer = LEDWriter(); let service = LEDService(writer: { await writer.write($0) })
        await service.replaceContext(try context(availableLED, epoch: 3, snapshot: 4))
        let request = try await service.configure(enabled: true, brightness: 16)
        try await service.consume(.state(LEDStateEvent(requestID: request, source: .query, available: true, reason: nil, enabled: true, brightness: 16, effective: .connected)), at: 1)
        try await service.retryPending()
        let bodies = try await writer.bodies()
        #expect(bodies.count == 2 && bodies[0] == bodies[1])
    }

    @Test func reconnectInvalidatesPendingAndLimits() async throws {
        let writer = LEDWriter(); let service = LEDService(writer: { await writer.write($0) })
        await service.replaceContext(try context(availableLED, epoch: 1, snapshot: 1))
        _ = try await service.configure(enabled: true, brightness: 16)
        await service.replaceContext(nil)
        await #expect(throws: LEDServiceError.staleEpoch) { try await service.retryPending() }
        await #expect(throws: LEDServiceError.staleEpoch) { try await service.configure(enabled: true, brightness: 1) }
    }

    private let availableLED = #"{"version":1,"available":true,"pixel_count":17,"key_pixels":{"k1":0,"k2":1,"k3":2,"k4":3},"strip_first":4,"strip_count":13,"color_model":"rgb8","wire_order":"grb","tick_ms":30,"max_brightness":64,"max_frame_channel_sum":3264}"#

    private func context(_ led: String, epoch: UInt64, snapshot: UInt64) throws -> ReplacementSessionContext {
        let data = Data("{\"event\":\"vk_capabilities\",\"protocol\":1,\"display\":{\"width\":428,\"height\":142,\"format\":\"rgb565\"},\"features\":{\"led\":\(led)}}".utf8)
        return ReplacementSessionContext(epochGeneration: epoch, snapshotGeneration: snapshot, snapshot: try ReplacementCapabilitySnapshot.decode(data))
    }
}
