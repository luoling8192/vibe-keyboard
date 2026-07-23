import Foundation

public actor LEDService {
    public typealias FrameWriter = @Sendable (Data) async throws -> Void
    public typealias CommandWriter = @Sendable (LEDCommand) async throws -> Void

    private struct Pending: Sendable {
        let requestID: UInt32
        let command: LEDCommand
        let body: Data
        let epochGeneration: UInt64
        let snapshotGeneration: UInt64
        let deadline: UInt64
    }

    private let writer: FrameWriter?
    private let commandWriter: CommandWriter?
    private let clock: any USBMonotonicClock
    private var context: ReplacementSessionContext?
    private var pending: Pending?
    private var lastCompleted: (requestID: UInt32, body: Data)?
    private var observedState: LEDStateEvent?
    private var nextRequestID: UInt32 = 1

    public init(writer: @escaping FrameWriter, clock: any USBMonotonicClock = ContinuousUSBClock()) {
        self.writer = writer
        self.commandWriter = nil
        self.clock = clock
    }

    public init(commandWriter: @escaping CommandWriter, clock: any USBMonotonicClock = ContinuousUSBClock()) {
        self.writer = nil
        self.commandWriter = commandWriter
        self.clock = clock
    }

    public static func connected(to session: USBSession, clock: any USBMonotonicClock = ContinuousUSBClock()) async -> LEDService {
        let service = LEDService(commandWriter: { command in try await session.sendLEDCommand(command) }, clock: clock)
        _ = await session.addLEDServiceConsumer { [weak service] event, responseTime in
            try? await service?.consume(event, at: responseTime)
        }
        return service
    }

    public func replaceContext(_ context: ReplacementSessionContext?) {
        self.context = context
        pending = nil
        lastCompleted = nil
        observedState = nil
    }

    public func currentState() -> LEDStateEvent? { observedState }

    public func synchronize(with context: ReplacementSessionContext?) {
        replaceContext(context)
    }

    @discardableResult
    public func query() async throws -> UInt32 {
        guard let context else { throw LEDServiceError.staleEpoch }
        guard context.snapshot.led != nil else { throw LEDServiceError.unavailable("absent") }
        let requestID = allocateRequestID()
        try await write(.query(requestID: requestID))
        try requireCurrent(context)
        return requestID
    }

    @discardableResult
    public func configure(enabled: Bool, brightness: UInt8) async throws -> UInt32 {
        guard let context else { throw LEDServiceError.staleEpoch }
        guard case .available(let capability)? = context.snapshot.led else {
            if case .unavailable(let unavailable)? = context.snapshot.led {
                throw LEDServiceError.unavailable(unavailable.reason)
            }
            throw LEDServiceError.unavailable("absent")
        }
        guard brightness <= capability.maxBrightness else {
            throw ReplacementProtocolError.invalidValue(field: "brightness")
        }
        guard pending == nil else { throw LEDServiceError.busy }
        let requestID = allocateRequestID()
        let body = try LEDProtocolCodec.encode(.config(requestID: requestID, enabled: enabled, brightness: brightness))
        let now = clock.nowNanoseconds()
        let deadline = Self.addingClamped(1_000_000_000, to: now)
        let command = LEDCommand.config(requestID: requestID, enabled: enabled, brightness: brightness)
        pending = Pending(requestID: requestID, command: command, body: body, epochGeneration: context.epochGeneration,
                          snapshotGeneration: context.snapshotGeneration, deadline: deadline)
        do {
            try await write(command)
            try requireCurrent(context)
            return requestID
        } catch {
            pending = nil
            throw error
        }
    }

    public func retryPending() async throws {
        guard let pending else {
            if context == nil { throw LEDServiceError.staleEpoch }
            throw LEDServiceError.busy
        }
        guard let context, pending.epochGeneration == context.epochGeneration,
              pending.snapshotGeneration == context.snapshotGeneration else {
            self.pending = nil
            throw LEDServiceError.staleEpoch
        }
        try await write(pending.command)
    }

    public func consume(_ event: LEDProtocolEvent, at responseTime: UInt64? = nil) throws {
        guard context != nil else { throw LEDServiceError.staleEpoch }
        switch event {
        case .state(let state):
            observedState = state
            guard state.source == .applied, let pending, pending.requestID == state.requestID else { return }
            let time = responseTime ?? clock.nowNanoseconds()
            self.pending = nil
            guard time < pending.deadline else { throw LEDServiceError.timedOut }
            lastCompleted = (pending.requestID, pending.body)
        case .error(let error):
            guard let requestID = error.requestID, pending?.requestID == requestID else { return }
            pending = nil
            throw LEDServiceError.rejected(error.code)
        }
    }

    public func expire(at now: UInt64? = nil) throws {
        guard let pending else { return }
        let time = now ?? clock.nowNanoseconds()
        guard time >= pending.deadline else { return }
        self.pending = nil
        throw LEDServiceError.timedOut
    }

    private func write(_ command: LEDCommand) async throws {
        if let commandWriter {
            try await commandWriter(command)
        } else if let writer {
            try await writer(try LEDProtocolCodec.encode(command))
        } else {
            throw LEDServiceError.staleEpoch
        }
    }

    private func allocateRequestID() -> UInt32 {
        let value = nextRequestID == 0 ? 1 : nextRequestID
        nextRequestID = value == UInt32.max ? 1 : value + 1
        return value
    }

    private func requireCurrent(_ captured: ReplacementSessionContext) throws {
        guard let context, context.epochGeneration == captured.epochGeneration,
              context.snapshotGeneration == captured.snapshotGeneration else {
            throw LEDServiceError.staleEpoch
        }
    }

    private static func addingClamped(_ value: UInt64, to base: UInt64) -> UInt64 {
        let (sum, overflow) = base.addingReportingOverflow(value)
        return overflow ? .max : sum
    }
}
