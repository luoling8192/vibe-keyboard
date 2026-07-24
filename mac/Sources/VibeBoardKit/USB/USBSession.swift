import Foundation
#if os(macOS)
import Darwin
#endif

public struct USBDeviceInfo: Equatable, Sendable {
    public let registryDeviceID: String
    public let firmwareDeviceID: String?
    public let hardware: String
    public let firmwareVersion: String?
    public let buttons: [String]?
    public let interactionModes: [String]?
    public let uiStates: [String]?

    public init(registryDeviceID: String, firmwareDeviceID: String?, hardware: String, firmwareVersion: String?, buttons: [String]?, interactionModes: [String]?, uiStates: [String]?) {
        self.registryDeviceID = registryDeviceID
        self.firmwareDeviceID = firmwareDeviceID
        self.hardware = hardware
        self.firmwareVersion = firmwareVersion
        self.buttons = buttons
        self.interactionModes = interactionModes
        self.uiStates = uiStates
    }

    init(descriptor: USBDeviceDescriptor, event: StateEvent) {
        registryDeviceID = descriptor.normalizedDeviceID
        firmwareDeviceID = event.deviceID
        hardware = event.hardware ?? ""
        firmwareVersion = event.firmwareVersion
        buttons = event.buttons
        interactionModes = event.interactionModes
        uiStates = event.uiStates
    }
}

public enum USBSessionState: Equatable, Sendable {
    case disconnected
    case opening
    case inspecting
    case announcingUSBTransport
    case requestingDeviceInfo
    case synchronizingConfiguration
    case ready(USBDeviceInfo)
    case incompatible(String)
    case failed(USBSessionError)
}

public enum USBSessionError: Error, Equatable, Sendable {
    case alreadyOpen
    case disconnected
    case openFailed(Int32)
    case configureFailed(Int32)
    case readFailed(Int32)
    case writeFailed(Int32)
    case waitFailed(Int32)
    case writeTimedOut
    case handshakeTimedOut
    case endOfFile
    case cancelled
    case eventBufferOverflow
    case eventConsumerTerminated
    case protocolFailure(String)
    case incompatibleHardware(String?)
}

public struct ReplacementSessionContext: Equatable, Sendable {
    public let epochGeneration: UInt64
    public let snapshotGeneration: UInt64
    public let snapshot: ReplacementCapabilitySnapshot

    public init(epochGeneration: UInt64, snapshotGeneration: UInt64, snapshot: ReplacementCapabilitySnapshot) {
        self.epochGeneration = epochGeneration
        self.snapshotGeneration = snapshotGeneration
        self.snapshot = snapshot
    }
}

public enum AssetTransferOutcome: Equatable, Sendable {
    case stored(transferID: UInt32, sha256: String, totalBytes: UInt32, kind: AssetKind)
    case aborted(transferID: UInt32)
    case rejected(transferID: UInt32?, code: String, nextOffset: UInt32?, message: String?)
    case invalidated(transferID: UInt32)

    public var transferID: UInt32? {
        switch self {
        case .stored(let transferID, _, _, _), .aborted(let transferID), .invalidated(let transferID): transferID
        case .rejected(let transferID, _, _, _): transferID
        }
    }
}

public struct ActiveAssetTransfer: Equatable, Sendable {
    public let transferID: UInt32
    public let sha256: String
    public let totalBytes: UInt32
    public let kind: AssetKind
    public let nextOffset: UInt32
    public let chunkBytes: UInt16

    fileprivate let authorizationID: UUID
    fileprivate let epochGeneration: UInt64
    fileprivate let snapshotGeneration: UInt64

    fileprivate init(
        transferID: UInt32,
        sha256: String,
        totalBytes: UInt32,
        kind: AssetKind,
        nextOffset: UInt32,
        chunkBytes: UInt16,
        authorizationID: UUID,
        epochGeneration: UInt64,
        snapshotGeneration: UInt64
    ) {
        self.transferID = transferID
        self.sha256 = sha256
        self.totalBytes = totalBytes
        self.kind = kind
        self.nextOffset = nextOffset
        self.chunkBytes = chunkBytes
        self.authorizationID = authorizationID
        self.epochGeneration = epochGeneration
        self.snapshotGeneration = snapshotGeneration
    }
}

private struct PendingAssetBegin: Equatable, Sendable {
    let transferID: UInt32
    let sha256: String
    let totalBytes: UInt32
    let kind: AssetKind
    let epochGeneration: UInt64
    let snapshotGeneration: UInt64
}

private struct PendingAssetQuery: Equatable, Sendable {
    let transferID: UInt32
    let epochGeneration: UInt64
    let snapshotGeneration: UInt64
}

private struct CurrentScreenSelection: Equatable, Sendable {
    let epochGeneration: UInt64
    let snapshotGeneration: UInt64
    var configured: Bool
    var mode: ScreenMode?
    var revision: UInt32
}

private enum AssetCommandRegistration: Sendable {
    case none
    case begin(PendingAssetBegin)
    case query(PendingAssetQuery)
    case end(authorizationID: UUID)
    case abort(authorizationID: UUID)
}

private struct ActiveAssetTransferState: Sendable {
    var handle: ActiveAssetTransfer
    var awaitingProgress: UInt32?
    var endRequested = false
    var abortRequested = false
}

public enum USBSessionEvent: Equatable, Sendable {
    case stateChanged(USBSessionState)
    case stateEvent(StateEvent)
    case replacementCapabilities(ReplacementSessionContext)
    case replacementEvent(ReplacementProtocolEvent)
    case audioFrame(AudioFrame)
    case protocolDiagnostic(type: UInt8, length: Int)
    case discardedByte(UInt8, reason: FrameDiscardReason)
}

public struct USBDiagnostics: Equatable, Sendable {
    public let text: String
    public let entries: [String]

    public init(text: String, entries: [String]) {
        self.text = text
        self.entries = entries
    }
}

private struct CapabilityIdentity: Equatable, Sendable {
    let protocolVersion: UInt16?
    let display: CapabilityDisplay?
}

public actor USBSession {
    public static let openFlags = Int32(O_RDWR | O_NONBLOCK | O_NOCTTY)
    private static let waitSliceMilliseconds: Int32 = 50

    public nonisolated let descriptor: USBDeviceDescriptor
    public nonisolated let events: AsyncStream<USBSessionEvent>

    private let operations: any SerialSystemOperating
    private let clock: any USBMonotonicClock
    private let readChunkSize: Int
    private let diagnosticByteLimit: Int
    private let diagnosticEntryLimit: Int
    private let heartbeatInterval: Duration
    private let continuation: AsyncStream<USBSessionEvent>.Continuation
    private var parser: FrameStreamParser
    private var fileDescriptor: Int32?
    private var readTask: Task<Void, Never>?
    private var heartbeatTask: Task<Void, Never>?
    private var state: USBSessionState = .disconnected
    private var diagnosticBytes: [UInt8] = []
    private var diagnosticEntries: [String] = []
    private var receivedDeviceInfo: StateEvent?
    private var receivedCapabilities: ReplacementCapabilitySnapshot?
    private var pendingReplacementDeviceInfo: StateEvent?
    private var acceptedCapabilityIdentity: CapabilityIdentity?
    private var epochGeneration: UInt64 = 0
    private var snapshotGeneration: UInt64 = 0
    private var replacementContext: ReplacementSessionContext?
    private var pendingAssetBegin: PendingAssetBegin?
    private var pendingAssetQueries: [UInt32: PendingAssetQuery] = [:]
    private var activeAssetTransfer: ActiveAssetTransferState?
    private var currentScreenSelection: CurrentScreenSelection?
    private var pendingScreenCommit: (previousRevision: UInt32, revision: UInt32, mode: ScreenMode)?
    private var assetTransferOutcomes: [UInt32: AssetTransferOutcome] = [:]
    private var ledEventConsumers: [UUID: @Sendable (LEDProtocolEvent, UInt64) async -> Void] = [:]
    private var widgetEventConsumers: [UUID: @Sendable (WidgetProtocolEvent) async -> Void] = [:]
    private var sawUnpairedCapabilities = false
    private var eventsFinished = false
    private var writeInProgress = false

    public init(
        descriptor: USBDeviceDescriptor,
        operations: any SerialSystemOperating = DarwinSerialSystemOperations(),
        clock: any USBMonotonicClock = ContinuousUSBClock(),
        readChunkSize: Int = 4096,
        receiveBufferLimit: Int = 4096,
        diagnosticByteLimit: Int = 2048,
        diagnosticEntryLimit: Int = 128,
        eventBufferCapacity: Int = 256,
        heartbeatInterval: Duration = .seconds(2)
    ) throws {
        precondition(readChunkSize > 0 && readChunkSize <= receiveBufferLimit)
        precondition(diagnosticByteLimit > 0 && diagnosticEntryLimit > 0)
        precondition(eventBufferCapacity > 0)
        self.descriptor = descriptor
        self.operations = operations
        self.clock = clock
        self.readChunkSize = readChunkSize
        self.diagnosticByteLimit = diagnosticByteLimit
        self.diagnosticEntryLimit = diagnosticEntryLimit
        self.heartbeatInterval = heartbeatInterval
        self.parser = try FrameStreamParser(receiveBufferLimit: receiveBufferLimit)
        let pair = AsyncStream<USBSessionEvent>.makeStream(
            bufferingPolicy: .bufferingNewest(eventBufferCapacity)
        )
        self.events = pair.stream
        self.continuation = pair.continuation
        pair.continuation.onTermination = { [weak self] termination in
            guard case .cancelled = termination else { return }
            Task { await self?.handleEventConsumerTermination() }
        }
    }

    deinit {
        readTask?.cancel()
        heartbeatTask?.cancel()
        if let fd = fileDescriptor { _ = operations.close(fileDescriptor: fd) }
        continuation.finish()
    }

    public func currentState() -> USBSessionState { state }

    public func currentReplacementContext() -> ReplacementSessionContext? { replacementContext }

    public func diagnostics() -> USBDiagnostics {
        let text = String(decoding: diagnosticBytes.map { byte in
            (byte == 0x09 || byte == 0x0a || byte == 0x0d || (0x20...0x7e).contains(byte)) ? byte : UInt8(ascii: ".")
        }, as: UTF8.self)
        return USBDiagnostics(text: text, entries: diagnosticEntries)
    }

    public func openForInspection() throws {
        try open(mode: .inspecting)
    }

    public func connect(handshakeTimeout: Duration = .seconds(10)) async throws -> USBDeviceInfo {
        try open(mode: .opening)
        do {
            try requireState(.announcingUSBTransport)
            try await write(command: .announceUSBTransport)
            try requireState(.requestingDeviceInfo)
            try await write(command: .getDeviceInfo)
            try requireState(.synchronizingConfiguration)
            try await write(command: .uiState(.ready, text: ""))

            let start = clock.nowNanoseconds()
            let deadline = Self.deadline(start: start, duration: handshakeTimeout)
            var nextInfoRequest = Self.addingClamped(1_500_000_000, to: start)
            var nextPing = Self.addingClamped(2_000_000_000, to: start)
            while clock.nowNanoseconds() < deadline {
                try Task.checkCancellation()
                if case .failed(let error) = state { throw error }
                if let event = receivedDeviceInfo {
                    guard event.hardware == "vibe_keyboard" else {
                        let error = USBSessionError.incompatibleHardware(event.hardware)
                        _ = setState(.incompatible(event.hardware ?? "missing hardware"))
                        throw error
                    }
                    if let replacementProtocol = event.replacementProtocol {
                        guard replacementProtocol == 1 else {
                            throw USBSessionError.protocolFailure("unsupported replacement protocol")
                        }
                        guard let capabilities = receivedCapabilities else {
                            try await Task.sleep(for: .milliseconds(25))
                            continue
                        }
                        guard capabilities.protocolVersion == 1,
                              capabilities.display == CapabilityDisplay(width: 428, height: 142, format: "rgb565") else {
                            throw USBSessionError.protocolFailure("invalid replacement capabilities")
                        }
                    } else if sawUnpairedCapabilities {
                        throw USBSessionError.protocolFailure("replacement capabilities without discriminator")
                    }
                    let info = USBDeviceInfo(descriptor: descriptor, event: event)
                    try requireState(.ready(info))
                    startHeartbeat()
                    return info
                }
                let now = clock.nowNanoseconds()
                if now >= nextInfoRequest {
                    try await write(command: .getDeviceInfo)
                    nextInfoRequest = Self.addingClamped(1_500_000_000, to: now)
                }
                if now >= nextPing {
                    try await write(command: .ping)
                    nextPing = Self.addingClamped(2_000_000_000, to: now)
                }
                try await Task.sleep(for: .milliseconds(25))
            }
            throw USBSessionError.handshakeTimedOut
        } catch is CancellationError {
            terminate(.cancelled)
            throw USBSessionError.cancelled
        } catch let error as USBSessionError {
            if case .incompatibleHardware = error {
                disconnectPreservingState()
            } else if error != .eventBufferOverflow {
                terminate(error)
            }
            throw error
        } catch {
            let failure = USBSessionError.protocolFailure(String(describing: error))
            terminate(failure)
            throw failure
        }
    }

    public func send(_ command: ControlCommand, timeout: Duration = .seconds(2)) async throws {
        try await performWrite(timeout: timeout) { try FrameEncoder.encode(command) }
    }

    public func sendAssetCommand(_ command: AssetCommand, timeout: Duration = .seconds(2)) async throws {
        let context = try requireReplacementContext()
        let assets = try requireAssets(in: context)
        try authorize(command, assets: assets, context: context)
        let registration = registerAssetCommand(command, context: context)
        do {
            try await performWrite(timeout: timeout) { try ReplacementCommandEncoder.encode(command) }
            try requireUnchanged(context)
        } catch {
            rollbackAssetCommand(registration)
            throw error
        }
    }

    private func registerAssetCommand(_ command: AssetCommand, context: ReplacementSessionContext) -> AssetCommandRegistration {
        switch command {
        case .begin(let transferID, let sha256, let totalBytes, let kind):
            let pending = PendingAssetBegin(transferID: transferID, sha256: sha256, totalBytes: totalBytes, kind: kind, epochGeneration: context.epochGeneration, snapshotGeneration: context.snapshotGeneration)
            pendingAssetBegin = pending
            return .begin(pending)
        case .query(let transferID):
            let pending = PendingAssetQuery(transferID: transferID, epochGeneration: context.epochGeneration, snapshotGeneration: context.snapshotGeneration)
            pendingAssetQueries[transferID] = pending
            return .query(pending)
        case .end:
            guard let id = activeAssetTransfer?.handle.authorizationID else { return .none }
            activeAssetTransfer?.endRequested = true
            return .end(authorizationID: id)
        case .abort:
            guard let id = activeAssetTransfer?.handle.authorizationID else { return .none }
            activeAssetTransfer?.abortRequested = true
            return .abort(authorizationID: id)
        default:
            return .none
        }
    }

    private func rollbackAssetCommand(_ registration: AssetCommandRegistration) {
        switch registration {
        case .none:
            break
        case .begin(let pending):
            if pendingAssetBegin == pending { pendingAssetBegin = nil }
        case .query(let pending):
            if pendingAssetQueries[pending.transferID] == pending { pendingAssetQueries.removeValue(forKey: pending.transferID) }
        case .end(let authorizationID):
            if activeAssetTransfer?.handle.authorizationID == authorizationID { activeAssetTransfer?.endRequested = false }
        case .abort(let authorizationID):
            if activeAssetTransfer?.handle.authorizationID == authorizationID { activeAssetTransfer?.abortRequested = false }
        }
    }

    public func sendScreenCommand(_ command: ScreenCommand, timeout: Duration = .seconds(2)) async throws {
        let context = try requireReplacementContext()
        let screen = try requireScreen(in: context)
        _ = try requireAssets(in: context)
        var registration: (previousRevision: UInt32, revision: UInt32, mode: ScreenMode)?
        if case .commit(let commit) = command {
            guard let selection = currentScreenSelection,
                  selection.epochGeneration == context.epochGeneration,
                  selection.snapshotGeneration == context.snapshotGeneration else {
                throw USBSessionError.protocolFailure("stale screen capability")
            }
            let expectedLimits = ScreenCommitLimits(
                displayWidth: context.snapshot.display.width,
                displayHeight: context.snapshot.display.height,
                screen: screen.selecting(revision: selection.revision, configured: selection.configured)
            )
            guard pendingScreenCommit == nil,
                  commit.limits == expectedLimits,
                  commit.expectedRevision == selection.revision else {
                throw USBSessionError.protocolFailure("stale screen capability")
            }
            let mode: ScreenMode
            switch commit.payload {
            case .image: mode = .image
            case .pet: mode = .pet
            case .dashboard: mode = .dashboard
            case .custom: mode = .custom
            }
            registration = (commit.expectedRevision, commit.revision, mode)
            pendingScreenCommit = registration
        }
        do {
            try await performWrite(timeout: timeout) { try ReplacementCommandEncoder.encode(command) }
            try requireUnchanged(context)
        } catch {
            if let registration, pendingScreenCommit?.previousRevision == registration.previousRevision,
               pendingScreenCommit?.revision == registration.revision {
                pendingScreenCommit = nil
            }
            throw error
        }
    }

    public func sendAssetChunk(
        _ payload: Data,
        using authorization: ActiveAssetTransfer,
        timeout: Duration = .seconds(2)
    ) async throws {
        let context = try requireReplacementContext()
        let assets = try requireAssets(in: context)
        guard assets.storageState == .ready, assets.uploadMaxBytes > 0,
              var active = activeAssetTransfer,
              active.handle.authorizationID == authorization.authorizationID,
              active.handle == authorization,
              !active.endRequested, !active.abortRequested,
              active.awaitingProgress == nil,
              authorization.epochGeneration == context.epochGeneration,
              authorization.snapshotGeneration == context.snapshotGeneration,
              authorization.nextOffset < authorization.totalBytes else {
            throw USBSessionError.protocolFailure("invalid asset transfer authorization")
        }
        let remaining = Int(authorization.totalBytes - authorization.nextOffset)
        let limit = min(Int(authorization.chunkBytes), Int(assets.chunkBytes), AssetChunkEncoder.maximumPayloadLength, remaining)
        guard !payload.isEmpty, payload.count <= limit else {
            throw USBSessionError.protocolFailure("invalid asset chunk size")
        }
        let next = authorization.nextOffset + UInt32(payload.count)
        active.awaitingProgress = next
        activeAssetTransfer = active
        do {
            try await performWrite(timeout: timeout) {
                try AssetChunkEncoder.encode(
                    transferID: authorization.transferID,
                    nextOffset: authorization.nextOffset,
                    payload: payload
                )
            }
            try requireUnchanged(context)
        } catch {
            if activeAssetTransfer?.handle.authorizationID == authorization.authorizationID,
               activeAssetTransfer?.awaitingProgress == next {
                activeAssetTransfer?.awaitingProgress = nil
            }
            throw error
        }
    }

    public func currentActiveAssetTransfer() -> ActiveAssetTransfer? {
        activeAssetTransfer?.handle
    }

    public func currentAssetTransferOutcome(transferID: UInt32) -> AssetTransferOutcome? {
        assetTransferOutcomes[transferID]
    }

    public func sendLEDCommand(_ command: LEDCommand, timeout: Duration = .seconds(2)) async throws {
        let context = try requireReplacementContext()
        guard context.snapshot.led != nil else {
            throw LEDServiceError.unavailable("absent")
        }
        if case .config(_, _, let brightness) = command {
            guard case .available(let capability)? = context.snapshot.led else {
                if case .unavailable(let unavailable)? = context.snapshot.led {
                    throw LEDServiceError.unavailable(unavailable.reason)
                }
                throw LEDServiceError.unavailable("absent")
            }
            guard brightness <= capability.maxBrightness else {
                throw ReplacementProtocolError.invalidValue(field: "brightness")
            }
        }
        try await performWrite(timeout: timeout) { try LEDProtocolCodec.encode(command) }
        try requireUnchanged(context)
    }

    public func sendWidgetUpdate(_ command: WidgetUpdateCommand, timeout: Duration = .seconds(2)) async throws {
        let context = try requireReplacementContext()
        let screen = try requireScreen(in: context)
        _ = try requireAssets(in: context)
        guard let selection = currentScreenSelection,
              selection.epochGeneration == context.epochGeneration,
              selection.snapshotGeneration == context.snapshotGeneration,
              selection.configured, command.revision == selection.revision else {
            throw USBSessionError.protocolFailure("stale widget revision")
        }
        try await performWrite(timeout: timeout) {
            try WidgetProtocolCodec.encode(command, maximumValueBytes: screen.maxWidgetValueBytes)
        }
        try requireUnchanged(context)
    }

    public func addLEDServiceConsumer(_ consumer: @escaping @Sendable (LEDProtocolEvent, UInt64) async -> Void) -> UUID {
        let id = UUID()
        ledEventConsumers[id] = consumer
        return id
    }

    public func removeLEDServiceConsumer(_ id: UUID) {
        ledEventConsumers.removeValue(forKey: id)
    }

    public func addWidgetConsumer(_ consumer: @escaping @Sendable (WidgetProtocolEvent) async -> Void) -> UUID {
        let id = UUID()
        widgetEventConsumers[id] = consumer
        return id
    }

    public func removeWidgetConsumer(_ id: UUID) {
        widgetEventConsumers.removeValue(forKey: id)
    }

    public func takeAssetTransferOutcome(transferID: UInt32) -> AssetTransferOutcome? {
        assetTransferOutcomes.removeValue(forKey: transferID)
    }

    public func disconnect() async {
        disconnect(reason: nil)
    }

    private func open(mode: USBSessionState) throws {
        guard fileDescriptor == nil else { throw USBSessionError.alreadyOpen }
        guard !eventsFinished else { throw USBSessionError.eventBufferOverflow }
        clearReplacementState()
        assetTransferOutcomes.removeAll(keepingCapacity: true)
        epochGeneration = nextGeneration(epochGeneration)
        receivedDeviceInfo = nil
        pendingReplacementDeviceInfo = nil
        acceptedCapabilityIdentity = nil
        sawUnpairedCapabilities = false
        diagnosticBytes.removeAll(keepingCapacity: true)
        diagnosticEntries.removeAll(keepingCapacity: true)
        parser = try FrameStreamParser(receiveBufferLimit: parser.receiveBufferLimit)
        try requireState(.opening)
        let fd: Int32
        switch operations.open(path: descriptor.calloutPath, flags: Self.openFlags) {
        case .value(let value): fd = value
        case .failure(let code):
            let error = USBSessionError.openFailed(code)
            terminate(error)
            throw error
        }
        fileDescriptor = fd
        switch operations.configureRaw(fileDescriptor: fd) {
        case .value: break
        case .failure(let code):
            let error = USBSessionError.configureFailed(code)
            terminate(error)
            throw error
        }
        do {
            try requireState(mode)
        } catch {
            closeFileDescriptor()
            throw error
        }
        readTask = Task { await self.readLoop() }
    }

    private func readLoop() async {
        while !Task.isCancelled {
            guard let fd = fileDescriptor else { return }
            switch operations.read(fileDescriptor: fd, maximumCount: readChunkSize) {
            case .value(let data):
                if data.isEmpty {
                    terminate(.endOfFile)
                    return
                }
                do { try consume(data) }
                catch let error as USBSessionError {
                    if error != .eventBufferOverflow { terminate(error) }
                    return
                } catch {
                    terminate(.protocolFailure(String(describing: error)))
                    return
                }
            case .failure(let code) where code == EINTR:
                await Task.yield()
                continue
            case .failure(let code) where code == EAGAIN || code == EWOULDBLOCK:
                switch operations.wait(fileDescriptor: fd, events: Int16(POLLIN), timeoutMilliseconds: Self.waitSliceMilliseconds) {
                case .value:
                    await Task.yield()
                    continue
                case .failure(let waitCode) where waitCode == EINTR:
                    await Task.yield()
                    continue
                case .failure(let waitCode):
                    terminate(.waitFailed(waitCode))
                    return
                }
            case .failure(let code):
                terminate(.readFailed(code))
                return
            }
            await Task.yield()
        }
    }

    private func consume(_ data: Data) throws {
        let streamEvents: [FrameStreamEvent]
        do {
            streamEvents = try parser.append(data)
        } catch {
            throw USBSessionError.protocolFailure(String(describing: error))
        }
        for event in streamEvents {
            switch event {
            case .discardedByte(let byte, let reason):
                appendDiagnosticByte(byte)
                try emitOrThrow(.discardedByte(byte, reason: reason))
            case .frame(let raw):
                appendEntry("frame type=0x\(String(raw.type.rawValue, radix: 16)) length=\(raw.bytes.count)")
                try emitOrThrow(.protocolDiagnostic(type: raw.type.rawValue, length: raw.bytes.count))
                do {
                    if raw.type == .state {
                        try consumeStateFrame(raw)
                    } else {
                        switch try FrameDecoder.decode(raw) {
                        case .state:
                            throw USBSessionError.protocolFailure("invalid state routing")
                        case .audio(let frame):
                            try emitOrThrow(.audioFrame(frame))
                        }
                    }
                } catch ProtocolError.unsupportedFrameType {
                    continue
                } catch let error as USBSessionError {
                    throw error
                } catch {
                    throw USBSessionError.protocolFailure(String(describing: error))
                }
            }
        }
    }

    private func consumeStateFrame(_ raw: RawFrame) throws {
        let body = try FrameDecoder.decodeStateBody(raw.bytes)
        let eventName: String
        do {
            let envelope = try BoundedJSON.object(body)
            guard let value = BoundedJSON.string(envelope["event"]) else {
                throw USBSessionError.protocolFailure("invalid state event")
            }
            eventName = value
        } catch let error as USBSessionError {
            throw error
        } catch {
            throw USBSessionError.protocolFailure("invalid state event")
        }
        if eventName == "vk_capabilities" {
            let snapshot: ReplacementCapabilitySnapshot
            do { snapshot = try ReplacementCapabilitySnapshot.decode(body) }
            catch { throw USBSessionError.protocolFailure("invalid replacement capabilities") }
            try consumeCapabilities(snapshot)
            return
        }
        if eventName == "vk_led_state" || (eventName == "vk_error" && (try? LEDProtocolCodec.decode(body)) != nil) {
            guard replacementContext != nil else { throw USBSessionError.protocolFailure("LED event without current capabilities") }
            let led: LEDProtocolEvent
            do { led = try LEDProtocolCodec.decode(body) }
            catch { throw USBSessionError.protocolFailure("invalid LED event") }
            let responseTime = clock.nowNanoseconds()
            try emitOrThrow(.replacementEvent(.led(led)))
            for consumer in ledEventConsumers.values { Task { await consumer(led, responseTime) } }
            return
        }
        if eventName == "vk_widget_applied" || (eventName == "vk_error" && (try? WidgetProtocolCodec.decode(body)) != nil) {
            guard replacementContext != nil else { throw USBSessionError.protocolFailure("widget event without current capabilities") }
            let widget: WidgetProtocolEvent
            do { widget = try WidgetProtocolCodec.decode(body) }
            catch { throw USBSessionError.protocolFailure("invalid widget event") }
            try emitOrThrow(.replacementEvent(.widget(widget)))
            for consumer in widgetEventConsumers.values { Task { await consumer(widget) } }
            return
        }
        if eventName == "vk_input_state" {
            guard replacementContext != nil else {
                throw USBSessionError.protocolFailure("input state without current capabilities")
            }
            let event = try FrameDecoder.decodeState(raw.bytes)
            try emitOrThrow(.stateEvent(event))
            return
        }
        if isKnownReplacementEvent(eventName) {
            guard replacementContext != nil else {
                throw USBSessionError.protocolFailure("replacement event without current capabilities")
            }
            let replacement: ReplacementProtocolEvent
            do {
                replacement = try ReplacementEventDecoder.decode(body)
            } catch {
                let payload = String(decoding: body.prefix(256), as: UTF8.self)
                appendEntry("replacement decode failed event=\(eventName) body=\(payload)")
                throw USBSessionError.protocolFailure("invalid replacement event")
            }
            let event = try FrameDecoder.decodeState(raw.bytes)
            try consumeHandshakeEvent(event)
            try consumeReplacementEvent(replacement)
            try emitOrThrow(.replacementEvent(replacement))
            return
        }
        if eventName.hasPrefix("vk_") {
            throw USBSessionError.protocolFailure("unknown replacement event")
        }
        let event = try FrameDecoder.decodeState(raw.bytes)
        try consumeHandshakeEvent(event)
        try emitOrThrow(.stateEvent(event))
    }

    private func isKnownReplacementEvent(_ event: String) -> Bool {
        event == "vk_error" || event == "vk_storage_formatted" ||
            event.hasPrefix("vk_asset_") || event.hasPrefix("vk_screen_")
    }

    private func consumeHandshakeEvent(_ event: StateEvent) throws {
        if event.event == "device_info" {
            if let protocolVersion = event.replacementProtocol {
                guard protocolVersion == 1 else {
                    throw USBSessionError.protocolFailure("unsupported replacement protocol")
                }
                guard pendingReplacementDeviceInfo == nil, !sawUnpairedCapabilities else {
                    throw USBSessionError.protocolFailure("invalid replacement handshake order")
                }
                clearPublishedReplacementContext()
                pendingReplacementDeviceInfo = event
                receivedDeviceInfo = nil
            } else {
                clearPublishedReplacementContext()
                pendingReplacementDeviceInfo = nil
                receivedDeviceInfo = event
            }
            return
        }
        if pendingReplacementDeviceInfo != nil {
            pendingReplacementDeviceInfo = nil
            receivedCapabilities = nil
            throw USBSessionError.protocolFailure("replacement capabilities not consecutive")
        }
    }

    private func consumeCapabilities(_ snapshot: ReplacementCapabilitySnapshot) throws {
        let identity = CapabilityIdentity(protocolVersion: snapshot.protocolVersion, display: snapshot.display)
        guard identity == CapabilityIdentity(protocolVersion: 1, display: CapabilityDisplay(width: 428, height: 142, format: "rgb565")) else {
            throw USBSessionError.protocolFailure("invalid replacement capabilities")
        }
        if let accepted = acceptedCapabilityIdentity {
            guard identity == accepted, pendingReplacementDeviceInfo == nil else {
                throw USBSessionError.protocolFailure("invalid replacement handshake order")
            }
        } else {
            guard let pending = pendingReplacementDeviceInfo else {
                sawUnpairedCapabilities = true
                clearPublishedReplacementContext()
                return
            }
            acceptedCapabilityIdentity = identity
            receivedDeviceInfo = pending
            pendingReplacementDeviceInfo = nil
        }
        snapshotGeneration = nextGeneration(snapshotGeneration)
        receivedCapabilities = snapshot
        let context = ReplacementSessionContext(epochGeneration: epochGeneration, snapshotGeneration: snapshotGeneration, snapshot: snapshot)
        replacementContext = context
        clearAssetOperations()
        pendingScreenCommit = nil
        if case .available(let screen)? = snapshot.screen {
            currentScreenSelection = .init(epochGeneration: context.epochGeneration, snapshotGeneration: context.snapshotGeneration, configured: screen.configured, mode: nil, revision: screen.revision)
        } else {
            currentScreenSelection = nil
        }
        try emitOrThrow(.replacementCapabilities(context))
    }

    private func consumeReplacementEvent(_ event: ReplacementProtocolEvent) throws {
        guard let context = replacementContext else {
            throw USBSessionError.protocolFailure("replacement event without current capabilities")
        }
        switch event {
        case .asset(.ready(let transferID, let sha256, let totalBytes, let kind, let nextOffset, let chunkBytes)):
            let matchesBegin = pendingAssetBegin.map {
                $0.transferID == transferID && $0.sha256 == sha256 && $0.totalBytes == totalBytes && $0.kind == kind &&
                    $0.epochGeneration == context.epochGeneration && $0.snapshotGeneration == context.snapshotGeneration
            } ?? false
            let matchesQuery = pendingAssetQueries[transferID].map {
                $0.epochGeneration == context.epochGeneration && $0.snapshotGeneration == context.snapshotGeneration
            } ?? false
            guard matchesBegin || matchesQuery,
                  nextOffset <= totalBytes,
                  let assets = try? requireAssets(in: context),
                  chunkBytes <= assets.chunkBytes,
                  totalBytes <= min(assets.uploadMaxBytes, assets.maxAssetBytes),
                  assets.storageState == .ready else {
                throw USBSessionError.protocolFailure("uncorrelated asset ready")
            }
            let handle = ActiveAssetTransfer(
                transferID: transferID,
                sha256: sha256,
                totalBytes: totalBytes,
                kind: kind,
                nextOffset: nextOffset,
                chunkBytes: chunkBytes,
                authorizationID: UUID(),
                epochGeneration: context.epochGeneration,
                snapshotGeneration: context.snapshotGeneration
            )
            pendingAssetBegin = nil
            pendingAssetQueries.removeValue(forKey: transferID)
            activeAssetTransfer = ActiveAssetTransferState(handle: handle, awaitingProgress: nil)
        case .asset(.progress(let transferID, let nextOffset)):
            guard var active = activeAssetTransfer else { return }
            guard active.handle.transferID == transferID,
                  active.awaitingProgress == nextOffset,
                  nextOffset > active.handle.nextOffset,
                  nextOffset <= active.handle.totalBytes else {
                throw USBSessionError.protocolFailure("invalid asset progress")
            }
            active.handle = ActiveAssetTransfer(
                transferID: active.handle.transferID,
                sha256: active.handle.sha256,
                totalBytes: active.handle.totalBytes,
                kind: active.handle.kind,
                nextOffset: nextOffset,
                chunkBytes: active.handle.chunkBytes,
                authorizationID: active.handle.authorizationID,
                epochGeneration: active.handle.epochGeneration,
                snapshotGeneration: active.handle.snapshotGeneration
            )
            active.awaitingProgress = nil
            activeAssetTransfer = active
        case .asset(.stored(let transferID, let sha256, let totalBytes, let kind)):
            guard let active = activeAssetTransfer,
                  active.handle.transferID == transferID,
                  active.handle.sha256 == sha256,
                  active.handle.totalBytes == totalBytes,
                  active.handle.kind == kind,
                  active.handle.nextOffset == totalBytes,
                  active.endRequested else {
                throw USBSessionError.protocolFailure("invalid asset stored")
            }
            assetTransferOutcomes[transferID] = .stored(transferID: transferID, sha256: sha256, totalBytes: totalBytes, kind: kind)
            clearAssetOperations(recordInvalidation: false)
        case .asset(.aborted(let transferID)):
            guard let active = activeAssetTransfer,
                  active.handle.transferID == transferID,
                  active.abortRequested else {
                throw USBSessionError.protocolFailure("invalid asset aborted")
            }
            assetTransferOutcomes[transferID] = .aborted(transferID: transferID)
            clearAssetOperations(recordInvalidation: false)
        case .led, .widget:
            break
        case .error(let error):
            if error.operation == "asset" || error.operation == "storage" {
                if error.code == "bad_offset", let active = activeAssetTransfer,
                   error.transferID == active.handle.transferID,
                   let nextOffset = error.nextOffset,
                   nextOffset <= active.handle.totalBytes {
                    assetTransferOutcomes[active.handle.transferID] = .rejected(
                        transferID: active.handle.transferID,
                        code: error.code,
                        nextOffset: nextOffset,
                        message: error.message
                    )
                    activeAssetTransfer?.awaitingProgress = nil
                } else if let transferID = error.transferID {
                    guard transferID == pendingAssetBegin?.transferID ||
                            pendingAssetQueries[transferID] != nil ||
                            transferID == activeAssetTransfer?.handle.transferID else {
                        throw USBSessionError.protocolFailure("uncorrelated asset error")
                    }
                    assetTransferOutcomes[transferID] = .rejected(
                        transferID: transferID,
                        code: error.code,
                        nextOffset: error.nextOffset,
                        message: error.message
                    )
                    retainActiveAssetTransferForAbort(transferID: transferID)
                } else {
                    if let transferID = activeAssetTransfer?.handle.transferID ?? pendingAssetBegin?.transferID {
                        assetTransferOutcomes[transferID] = .rejected(
                            transferID: transferID,
                            code: error.code,
                            nextOffset: error.nextOffset,
                            message: error.message
                        )
                    }
                    if let transferID = activeAssetTransfer?.handle.transferID {
                        retainActiveAssetTransferForAbort(transferID: transferID)
                    } else {
                        clearAssetOperations(recordInvalidation: false)
                    }
                }
            }
        case .screen(let screen):
            guard var selection = currentScreenSelection,
                  selection.epochGeneration == context.epochGeneration,
                  selection.snapshotGeneration == context.snapshotGeneration else {
                throw USBSessionError.protocolFailure("screen event without current selection")
            }
            switch screen {
            case .state(let configured, let mode, let revision, _, _):
                guard revision >= selection.revision else {
                    throw USBSessionError.protocolFailure("screen revision moved backwards")
                }
                selection.configured = configured
                selection.mode = mode
                selection.revision = revision
                pendingScreenCommit = nil
            case .committed(_, let previousRevision, let revision, _):
                guard let pending = pendingScreenCommit,
                      pending.previousRevision == previousRevision,
                      pending.revision == revision,
                      selection.revision == previousRevision else {
                    throw USBSessionError.protocolFailure("uncorrelated screen commit")
                }
                selection.configured = true
                selection.mode = pending.mode
                selection.revision = revision
                pendingScreenCommit = nil
            }
            currentScreenSelection = selection
        case .asset(.storageFormatted), .asset(.page), .asset(.deleted):
            break
        }
    }

    private func requireReplacementContext() throws -> ReplacementSessionContext {
        guard case .ready = state, let context = replacementContext else {
            throw USBSessionError.protocolFailure("replacement capability unavailable")
        }
        return context
    }

    private func requireUnchanged(_ expected: ReplacementSessionContext) throws {
        guard replacementContext == expected else {
            throw USBSessionError.protocolFailure("stale replacement capability")
        }
    }

    private func requireAssets(in context: ReplacementSessionContext) throws -> AssetsCapability {
        guard case .available(let assets)? = context.snapshot.assets else {
            throw USBSessionError.protocolFailure("assets unavailable")
        }
        return assets
    }

    private func requireScreen(in context: ReplacementSessionContext) throws -> ScreenCapability {
        guard case .available(let screen)? = context.snapshot.screen else {
            throw USBSessionError.protocolFailure("screen unavailable")
        }
        return screen
    }

    private func authorize(
        _ command: AssetCommand,
        assets: AssetsCapability,
        context: ReplacementSessionContext
    ) throws {
        switch command {
        case .storageFormat:
            throw USBSessionError.protocolFailure("verified-erased authorization unavailable")
        case .begin(_, _, let totalBytes, _):
            guard assets.storageState == .ready, assets.uploadMaxBytes > 0,
                  totalBytes <= min(assets.uploadMaxBytes, assets.maxAssetBytes),
                  pendingAssetBegin == nil, activeAssetTransfer == nil else {
                throw USBSessionError.protocolFailure("asset begin unauthorized")
            }
        case .query(let transferID):
            if assets.storageState == .busy {
                guard activeAssetTransfer?.handle.transferID == transferID else {
                    throw USBSessionError.protocolFailure("asset query unauthorized")
                }
            } else {
                guard assets.storageState == .ready else {
                    throw USBSessionError.protocolFailure("asset query unauthorized")
                }
            }
        case .end(let transferID, let sha256, let totalBytes, let kind):
            guard assets.storageState == .ready, assets.uploadMaxBytes > 0,
                  let active = activeAssetTransfer,
                  active.handle.transferID == transferID,
                  active.handle.sha256 == sha256,
                  active.handle.totalBytes == totalBytes,
                  active.handle.kind == kind,
                  active.handle.nextOffset == totalBytes,
                  active.awaitingProgress == nil,
                  !active.endRequested, !active.abortRequested,
                  active.handle.epochGeneration == context.epochGeneration,
                  active.handle.snapshotGeneration == context.snapshotGeneration else {
                throw USBSessionError.protocolFailure("asset end unauthorized")
            }
        case .abort(let transferID):
            guard assets.storageState == .ready, assets.uploadMaxBytes > 0,
                  let active = activeAssetTransfer,
                  active.handle.transferID == transferID,
                  !active.endRequested, !active.abortRequested else {
                throw USBSessionError.protocolFailure("asset abort unauthorized")
            }
        case .list:
            guard assets.management, assets.storageState == .ready else {
                throw USBSessionError.protocolFailure("asset list unauthorized")
            }
        case .delete:
            guard assets.management, assets.storageState == .ready else {
                throw USBSessionError.protocolFailure("asset delete unauthorized")
            }
        }
    }

    private func performWrite(timeout: Duration, encoder: () throws -> Data) async throws {
        let waitDeadline = Self.deadline(
            start: clock.nowNanoseconds(),
            duration: timeout
        )
        while writeInProgress {
            try Task.checkCancellation()
            guard clock.nowNanoseconds() < waitDeadline else {
                throw USBSessionError.writeTimedOut
            }
            try await Task.sleep(for: .milliseconds(5))
        }
        writeInProgress = true
        do {
            try await write(try encoder(), timeout: timeout)
            writeInProgress = false
        } catch is CancellationError {
            writeInProgress = false
            terminate(.cancelled)
            throw USBSessionError.cancelled
        } catch let error as USBSessionError {
            writeInProgress = false
            if error.isTerminalWriteFailure { terminate(error) }
            throw error
        } catch {
            writeInProgress = false
            throw USBSessionError.protocolFailure(String(describing: error))
        }
    }

    private func write(command: ControlCommand, timeout: Duration = .seconds(2)) async throws {
        let frame: Data
        do { frame = try FrameEncoder.encode(command) }
        catch { throw USBSessionError.protocolFailure(String(describing: error)) }
        try await write(frame, timeout: timeout)
    }

    private func write(_ frame: Data, timeout: Duration) async throws {
        guard let fd = fileDescriptor else { throw USBSessionError.disconnected }
        let deadline = Self.deadline(start: clock.nowNanoseconds(), duration: timeout)
        var offset = 0
        while offset < frame.count {
            try Task.checkCancellation()
            let beforeWrite = clock.nowNanoseconds()
            guard beforeWrite < deadline else { throw USBSessionError.writeTimedOut }
            let remainder = frame.subdata(in: offset..<frame.count)
            switch operations.write(fileDescriptor: fd, data: remainder) {
            case .value(let count):
                guard count > 0 && count <= remainder.count else { throw USBSessionError.writeFailed(EIO) }
                offset += count
            case .failure(let code) where code == EINTR:
                continue
            case .failure(let code) where code == EAGAIN || code == EWOULDBLOCK:
                await Task.yield()
                try Task.checkCancellation()
                let now = clock.nowNanoseconds()
                guard now < deadline else { throw USBSessionError.writeTimedOut }
                let remaining = deadline - now
                let milliseconds = Int32(max(1, min(UInt64(Self.waitSliceMilliseconds), remaining / 1_000_000)))
                switch operations.wait(fileDescriptor: fd, events: Int16(POLLOUT), timeoutMilliseconds: milliseconds) {
                case .value:
                    continue
                case .failure(let waitCode) where waitCode == EINTR:
                    continue
                case .failure(let waitCode):
                    throw USBSessionError.waitFailed(waitCode)
                }
            case .failure(let code):
                throw USBSessionError.writeFailed(code)
            }
        }
    }

    private func disconnect(reason: USBSessionError?) {
        clearReplacementState()
        heartbeatTask?.cancel()
        heartbeatTask = nil
        readTask?.cancel()
        readTask = nil
        closeFileDescriptor()
        if let reason {
            publishTerminalState(reason)
        } else if !eventsFinished {
            _ = setState(.disconnected)
        }
    }

    private func disconnectPreservingState() {
        clearReplacementState()
        heartbeatTask?.cancel()
        heartbeatTask = nil
        readTask?.cancel()
        readTask = nil
        closeFileDescriptor()
    }

    private func terminate(_ error: USBSessionError) {
        clearReplacementState()
        heartbeatTask?.cancel()
        heartbeatTask = nil
        readTask?.cancel()
        readTask = nil
        closeFileDescriptor()
        publishTerminalState(error)
    }

    private func handleEventConsumerTermination() {
        guard !eventsFinished else { return }
        clearReplacementState()
        eventsFinished = true
        heartbeatTask?.cancel()
        heartbeatTask = nil
        readTask?.cancel()
        readTask = nil
        closeFileDescriptor()
        state = .failed(.eventConsumerTerminated)
    }

    private func publishTerminalState(_ error: USBSessionError) {
        state = .failed(error)
        guard !eventsFinished else { return }
        _ = continuation.yield(.stateChanged(.failed(error)))
        if error == .eventBufferOverflow {
            eventsFinished = true
            continuation.finish()
        }
    }

    private func startHeartbeat() {
        heartbeatTask?.cancel()
        heartbeatTask = Task {
            while !Task.isCancelled {
                do {
                    try await Task.sleep(for: self.heartbeatInterval)
                    while self.writeInProgress {
                        try await Task.sleep(for: .milliseconds(10))
                    }
                    try await self.performWrite(timeout: .seconds(2)) {
                        try FrameEncoder.encode(.ping)
                    }
                } catch is CancellationError {
                    return
                } catch let error as USBSessionError {
                    self.terminate(error)
                    return
                } catch {
                    self.terminate(.protocolFailure(String(describing: error)))
                    return
                }
            }
        }
    }

    private func clearAssetOperations(recordInvalidation: Bool = true) {
        if recordInvalidation {
            if let transferID = activeAssetTransfer?.handle.transferID ?? pendingAssetBegin?.transferID {
                assetTransferOutcomes[transferID] = .invalidated(transferID: transferID)
            }
        }
        pendingAssetBegin = nil
        pendingAssetQueries.removeAll(keepingCapacity: true)
        activeAssetTransfer = nil
    }

    private func retainActiveAssetTransferForAbort(transferID: UInt32) {
        pendingAssetBegin = nil
        pendingAssetQueries.removeAll(keepingCapacity: true)
        guard var active = activeAssetTransfer, active.handle.transferID == transferID else {
            activeAssetTransfer = nil
            return
        }
        active.awaitingProgress = nil
        active.endRequested = false
        active.abortRequested = false
        activeAssetTransfer = active
    }

    private func clearPublishedReplacementContext() {
        receivedCapabilities = nil
        replacementContext = nil
        clearAssetOperations()
        currentScreenSelection = nil
        pendingScreenCommit = nil
    }

    private func clearReplacementState() {
        clearPublishedReplacementContext()
        pendingReplacementDeviceInfo = nil
        acceptedCapabilityIdentity = nil
        sawUnpairedCapabilities = false
        snapshotGeneration = 0
    }

    private func nextGeneration(_ value: UInt64) -> UInt64 {
        value == UInt64.max ? 1 : value + 1
    }

    private func closeFileDescriptor() {
        guard let fd = fileDescriptor else { return }
        fileDescriptor = nil
        _ = operations.close(fileDescriptor: fd)
    }

    @discardableResult
    private func setState(_ value: USBSessionState) -> Bool {
        state = value
        return emit(.stateChanged(value))
    }

    private func requireState(_ value: USBSessionState) throws {
        guard setState(value) else { throw USBSessionError.eventBufferOverflow }
    }

    private func emitOrThrow(_ event: USBSessionEvent) throws {
        guard emit(event) else { throw USBSessionError.eventBufferOverflow }
    }

    @discardableResult
    private func emit(_ event: USBSessionEvent) -> Bool {
        guard !eventsFinished else { return false }
        switch continuation.yield(event) {
        case .enqueued:
            return true
        case .dropped:
            appendEntry("event buffer overflow")
            clearReplacementState()
            eventsFinished = true
            state = .failed(.eventBufferOverflow)
            heartbeatTask?.cancel()
            heartbeatTask = nil
            readTask?.cancel()
            readTask = nil
            closeFileDescriptor()
            _ = continuation.yield(.stateChanged(.failed(.eventBufferOverflow)))
            continuation.finish()
            return false
        case .terminated:
            handleEventConsumerTermination()
            return false
        @unknown default:
            handleEventConsumerTermination()
            return false
        }
    }

    private func appendDiagnosticByte(_ byte: UInt8) {
        diagnosticBytes.append(byte)
        if diagnosticBytes.count > diagnosticByteLimit {
            diagnosticBytes.removeFirst(diagnosticBytes.count - diagnosticByteLimit)
        }
    }

    private func appendEntry(_ entry: String) {
        diagnosticEntries.append(entry)
        if diagnosticEntries.count > diagnosticEntryLimit {
            diagnosticEntries.removeFirst(diagnosticEntries.count - diagnosticEntryLimit)
        }
    }

    private static func deadline(start: UInt64, duration: Duration) -> UInt64 {
        addingClamped(nanoseconds(duration), to: start)
    }

    private static func addingClamped(_ value: UInt64, to base: UInt64) -> UInt64 {
        let (sum, overflow) = base.addingReportingOverflow(value)
        return overflow ? UInt64.max : sum
    }

    private static func nanoseconds(_ duration: Duration) -> UInt64 {
        let parts = duration.components
        let seconds = max(Int64(0), parts.seconds)
        let attoseconds = max(Int64(0), parts.attoseconds)
        let secondsValue = UInt64(seconds)
        guard secondsValue <= UInt64.max / 1_000_000_000 else { return UInt64.max }
        let base = secondsValue * 1_000_000_000
        return addingClamped(UInt64(attoseconds) / 1_000_000_000, to: base)
    }
}

private extension USBSessionError {
    var isTerminalWriteFailure: Bool {
        switch self {
        case .writeFailed, .waitFailed, .writeTimedOut, .cancelled:
            true
        default:
            false
        }
    }
}
