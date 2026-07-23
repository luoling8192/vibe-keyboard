import Foundation
import Testing
import VibeBoardKit
@testable import VibeKeyboardApp

@Suite("Production app model")
@MainActor
struct AppModelTests {
    @Test func publishesCurrentEpochCapabilitiesAndClearsOnDetach() async throws {
        let descriptor = testDescriptor()
        let monitor = TestMonitor()
        let session = TestSession(descriptor: descriptor)
        let model = makeModel(monitor: monitor, session: session)

        model.start()
        monitor.send(.attached(descriptor))
        await eventually { model.isConnected }

        session.send(.replacementCapabilities(ReplacementSessionContext(
            epochGeneration: 1,
            snapshotGeneration: 1,
            snapshot: try availableSnapshot()
        )))
        await eventually { model.assetsCapability == .available && model.screenCapability == .available }
        #expect(!model.canMutateFirmware)
        #expect(!model.canConfigureLED)
        #expect(model.canSendScreen)

        monitor.send(.detached(registryEntryID: descriptor.registryEntryID))
        await eventually { model.connection == .disconnected }
        #expect(model.replacementContext == nil)
        #expect(model.assetsCapability == .absent)
        #expect(model.screenCapability == .absent)
    }

    @Test func unavailableCapabilitiesNeverExposeMutations() async throws {
        let descriptor = testDescriptor()
        let monitor = TestMonitor()
        let session = TestSession(descriptor: descriptor)
        let model = makeModel(monitor: monitor, session: session)
        model.start()
        monitor.send(.attached(descriptor))
        await eventually { model.isConnected }

        session.send(.replacementCapabilities(ReplacementSessionContext(
            epochGeneration: 2,
            snapshotGeneration: 4,
            snapshot: try unavailableSnapshot()
        )))
        await eventually { model.assetsCapability == .unavailable("display_acceptance_required") }
        #expect(!model.canSendScreen)
        #expect(!model.canMutateFirmware)
        #expect(!model.canConfigureLED)
        #expect(model.ledCapability == .unavailable("calibration_required"))
    }

    @Test func sessionFailureInvalidatesStaleEpoch() async throws {
        let descriptor = testDescriptor()
        let monitor = TestMonitor()
        let session = TestSession(descriptor: descriptor)
        let model = makeModel(monitor: monitor, session: session)
        model.start()
        monitor.send(.attached(descriptor))
        await eventually { model.isConnected }
        session.send(.replacementCapabilities(ReplacementSessionContext(epochGeneration: 3, snapshotGeneration: 1, snapshot: try availableSnapshot())))
        await eventually { model.replacementContext != nil }

        session.send(.stateChanged(.failed(.endOfFile)))
        await eventually { model.replacementContext == nil }
        #expect(model.assetsCapability == .absent)
        #expect(!model.canSendScreen)
    }

    @Test func canonicalEventsReachActionPipelineExactlyOnceAndReset() async throws {
        let descriptor = testDescriptor()
        let monitor = TestMonitor()
        let session = TestSession(descriptor: descriptor)
        let pipeline = ActionPipelineSpy()
        let clock = TestAppClock(values: [10_000, 11_500, 11_501, 12_000])
        let model = AppModel(
            monitor: monitor,
            sessionFactory: TestSessionFactory(session: session),
            mappingRepository: KeyMappingRepository(store: MemoryConfigurationStore()),
            actionPipeline: pipeline,
            monotonicClock: clock
        )
        model.start()
        monitor.send(.attached(descriptor))
        await eventually { model.isConnected }

        clock.replace(with: [10_000, 11_500, 11_501, 12_000])
        session.send(.stateEvent(try state(#"{"event":"button_down","button":"k1","duration_ms":0}"#)))
        session.send(.stateEvent(try state(#"{"event":"button_up","button":"k1","duration_ms":20}"#)))
        session.send(.stateEvent(try state(#"{"event":"button_click","button":"k1","duration_ms":20}"#)))
        await eventually { await pipeline.eventCount() == 3 }
        #expect(await pipeline.eventCount() == 3)
        #expect(await pipeline.timestamps() == [10_000, 11_500, 11_501])

        session.send(.stateChanged(.disconnected))
        await eventually { await pipeline.resetCount() == 1 }
    }

    @Test func committedScreenAdvancesWidgetAndNextCommitRevision() async throws {
        let descriptor = testDescriptor()
        let monitor = TestMonitor()
        let session = TestSession(descriptor: descriptor)
        let model = makeModel(monitor: monitor, session: session)
        model.start()
        monitor.send(.attached(descriptor))
        await eventually { model.isConnected }
        session.send(.replacementCapabilities(.init(epochGeneration: 4, snapshotGeneration: 8, snapshot: try availableSnapshot())))
        await eventually { model.canSendScreen }

        model.layoutTitle = "Build 42"
        model.widgetText = "Ready 7"
        model.activateLayout(mode: .dashboard)
        await eventually { await session.commitCount() == 1 }
        let first = try #require(await session.lastCommit())
        #expect(first.expectedRevision == 0)
        #expect(first.revision == 1)
        guard case .dashboard(let layout) = first.payload else {
            Issue.record("Expected dashboard payload")
            return
        }
        guard case .staticLabel(_, _, _, _, let title) = layout.objects.first?.node else {
            Issue.record("Expected static title")
            return
        }
        #expect(title == "Build 42")
        #expect(layout.widgets == [.text(id: "status", target: "status-value", fallback: "Ready 7")])
        session.send(.replacementEvent(.screen(.committed(
            assetsManifestSHA256: String(repeating: "a", count: 64),
            previousRevision: 0,
            revision: 1,
            screenManifestSHA256: String(repeating: "b", count: 64)
        ))))
        await eventually { model.lastScreenState != nil }

        model.sendStatusWidget()
        await eventually { await session.widgetCount() == 1 }
        #expect(await session.lastWidget()?.revision == 1)

        model.activateLayout(mode: .custom)
        await eventually { await session.commitCount() == 2 }
        let second = try #require(await session.lastCommit())
        #expect(second.expectedRevision == 1)
        #expect(second.revision == 2)

        session.send(.replacementCapabilities(.init(epochGeneration: 4, snapshotGeneration: 9, snapshot: try availableSnapshot())))
        await eventually { model.replacementContext?.snapshotGeneration == 9 }
        model.sendStatusWidget()
        try await Task.sleep(for: .milliseconds(30))
        #expect(await session.widgetCount() == 1)
    }

    @Test func mappingsPersistOnlyThroughRepository() async throws {
        let descriptor = testDescriptor()
        let monitor = TestMonitor()
        let session = TestSession(descriptor: descriptor)
        let store = MemoryConfigurationStore()
        let model = AppModel(monitor: monitor, sessionFactory: TestSessionFactory(session: session), mappingRepository: KeyMappingRepository(store: store))
        model.start()
        monitor.send(.attached(descriptor))
        await eventually { model.isConnected }
        await eventually { await session.inputConfigurationCount() == 1 }
        model.setSingleAction(.voiceInput, for: .k1)
        model.setAction(.systemCopy, for: .k2, gesture: .long)
        #expect(model.keyProfile.mappings[.k4]?.single == HostAction.none)
        #expect(model.keyProfile.mappings[.k2]?.long == .systemCopy)
        model.saveMappings()
        await eventually { await store.hasData() }
        await eventually { await session.inputConfigurationCount() == 2 }
        let loaded = try await KeyMappingRepository(store: store).load()
        #expect(loaded.mappings[.k1]?.single == .voiceInput)
        #expect(loaded.mappings[.k2]?.long == .systemCopy)
        #expect(await session.lastVoiceKey() == .k1)
    }

    private func makeModel(monitor: TestMonitor, session: TestSession) -> AppModel {
        AppModel(
            monitor: monitor,
            sessionFactory: TestSessionFactory(session: session),
            mappingRepository: KeyMappingRepository(store: MemoryConfigurationStore())
        )
    }
}

private final class TestMonitor: USBDeviceMonitoring, @unchecked Sendable {
    private let lock = NSLock()
    private var continuation: AsyncStream<USBDeviceMonitorEvent>.Continuation?
    private var pending: [USBDeviceMonitorEvent] = []
    private lazy var stream = AsyncStream<USBDeviceMonitorEvent> { continuation in
        lock.withLock {
            self.continuation = continuation
            for event in pending { continuation.yield(event) }
            pending.removeAll()
        }
    }
    func events() -> AsyncStream<USBDeviceMonitorEvent> { stream }
    func send(_ event: USBDeviceMonitorEvent) {
        lock.withLock {
            if let continuation { continuation.yield(event) }
            else { pending.append(event) }
        }
    }
}

private actor TestSession: AppDeviceSession {
    nonisolated let descriptor: USBDeviceDescriptor
    nonisolated let events: AsyncStream<USBSessionEvent>
    private let continuation: AsyncStream<USBSessionEvent>.Continuation
    private var commits: [ScreenCommit] = []
    private var widgets: [WidgetUpdateCommand] = []
    private var inputConfigurations: [(InteractionMode, VoiceKey)] = []

    init(descriptor: USBDeviceDescriptor) {
        self.descriptor = descriptor
        let pair = AsyncStream<USBSessionEvent>.makeStream()
        events = pair.stream
        continuation = pair.continuation
    }

    func connect() async throws {
        continuation.yield(.stateChanged(.ready(USBDeviceInfo.testValue(descriptor: descriptor))))
    }
    func disconnect() async { continuation.yield(.stateChanged(.disconnected)) }
    func upload(_ asset: PreparedAsset, transferID: UInt32) async throws -> AssetTransferService.Progress {
        .init(transferID: transferID, nextOffset: UInt32(asset.data.count), totalBytes: UInt32(asset.data.count))
    }
    func cancelUpload(transferID: UInt32) async throws {}
    func queryScreen() async throws {}
    func commitScreen(_ commit: ScreenCommit) async throws { commits.append(commit) }
    func synchronizeLED(_ context: ReplacementSessionContext?) async {}
    func queryLED() async throws {}
    func configureLED(enabled: Bool, brightness: UInt8) async throws {}
    func configureInput(mode: InteractionMode, voiceKey: VoiceKey) async throws {
        inputConfigurations.append((mode, voiceKey))
    }
    func sendWidgetUpdate(_ command: WidgetUpdateCommand) async throws { widgets.append(command) }
    func commitCount() -> Int { commits.count }
    func lastCommit() -> ScreenCommit? { commits.last }
    func widgetCount() -> Int { widgets.count }
    func lastWidget() -> WidgetUpdateCommand? { widgets.last }
    func inputConfigurationCount() -> Int { inputConfigurations.count }
    func lastVoiceKey() -> VoiceKey? { inputConfigurations.last?.1 }
    nonisolated func send(_ event: USBSessionEvent) { continuation.yield(event) }
}

private struct TestSessionFactory: AppDeviceSessionFactory {
    let session: TestSession
    func makeSession(for descriptor: USBDeviceDescriptor) throws -> any AppDeviceSession { session }
}

private actor ActionPipelineSpy: AppActionRouting {
    private var events: [DeviceKeyEvent] = []
    private var eventTimestamps: [UInt64] = []
    private var resets = 0
    func updateProfile(_ profile: KeyMappingProfile) async throws {}
    func consume(_ event: DeviceKeyEvent, at timestampMilliseconds: UInt64) async throws -> [ActionExecutionResult] {
        events.append(event)
        eventTimestamps.append(timestampMilliseconds)
        return [.completed]
    }
    func disconnect(at timestampMilliseconds: UInt64) async throws { resets += 1 }
    func eventCount() -> Int { events.count }
    func timestamps() -> [UInt64] { eventTimestamps }
    func resetCount() -> Int { resets }
}

private final class TestAppClock: AppMonotonicClock, @unchecked Sendable {
    private let lock = NSLock()
    private var values: [UInt64]
    private var last: UInt64

    init(values: [UInt64]) {
        self.values = values
        last = values.last ?? 0
    }

    func replace(with values: [UInt64]) {
        lock.withLock {
            self.values = values
            last = values.last ?? last
        }
    }

    func nowMilliseconds() -> UInt64 {
        lock.withLock {
            guard !values.isEmpty else { return last }
            last = values.removeFirst()
            return last
        }
    }
}

private func state(_ json: String) throws -> StateEvent {
    try JSONDecoder().decode(StateEvent.self, from: Data(json.utf8))
}

private actor MemoryConfigurationStore: ConfigurationDataStore {
    private var data: Data?
    func read() -> Data? { data }
    func replaceAtomically(with data: Data) { self.data = data }
    func hasData() -> Bool { data != nil }
}

private func testDescriptor() -> USBDeviceDescriptor {
    USBDeviceDescriptor(registryEntryID: 7, vendorID: 0x303a, productID: 0x1001, serialNumber: "02:00:00:00:00:01", normalizedDeviceID: "020000000001", calloutPath: "/dev/null")
}

private extension USBDeviceInfo {
    static func testValue(descriptor: USBDeviceDescriptor) -> USBDeviceInfo {
        USBDeviceInfo(
            registryDeviceID: descriptor.normalizedDeviceID,
            firmwareDeviceID: nil,
            hardware: "vibe_keyboard",
            firmwareVersion: "test",
            buttons: nil,
            interactionModes: nil,
            uiStates: nil
        )
    }
}

private func availableSnapshot() throws -> ReplacementCapabilitySnapshot {
    try ReplacementCapabilitySnapshot.decode(Data(capabilityJSON(assets: availableAssetsJSON, screen: availableScreenJSON).utf8))
}

private func unavailableSnapshot() throws -> ReplacementCapabilitySnapshot {
    try ReplacementCapabilitySnapshot.decode(Data(capabilityJSON(
        assets: #"{"available":false,"reason":"display_acceptance_required","version":1}"#,
        screen: #"{"available":false,"reason":"display_acceptance_required","version":1}"#
    ).utf8))
}

private func capabilityJSON(assets: String, screen: String) -> String {
    #"{"display":{"format":"rgb565","height":142,"width":428},"event":"vk_capabilities","features":{"assets":\#(assets),"led":{"available":false,"reason":"calibration_required","version":1},"screen":\#(screen),"update":{"available":false,"reason":"bootloader_migration_required","version":1}},"protocol":1}"#
}

private let availableAssetsJSON = #"{"available":true,"chunk_bytes":4084,"decoder_scratch_bytes":4096,"encodings":["raw","row_rle"],"free_bytes":1048576,"management":true,"max_active_decoded_bytes":131072,"max_asset_bytes":524288,"max_assets":64,"max_frame_ms":5000,"max_frames":32,"min_frame_ms":20,"reserve_bytes":65536,"revision":0,"storage_state":"ready","upload_max_bytes":524288,"version":1}"#
private let availableScreenJSON = #"{"available":true,"configured":false,"fonts":[{"id":"vk-sans","metrics_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","version":1}],"max_assets":64,"max_commit_bytes":4092,"max_depth":4,"max_fonts":4,"max_json_tokens":512,"max_layout_bytes":3072,"max_objects":32,"max_pet_states":6,"max_string_bytes":256,"max_widget_value_bytes":256,"max_widgets":16,"modes":["image","pet","dashboard","custom"],"revision":0,"version":1}"#

@MainActor
private func eventually(timeout: Duration = .seconds(2), _ predicate: @escaping @MainActor () async -> Bool) async {
    let clock = ContinuousClock()
    let deadline = clock.now.advanced(by: timeout)
    while clock.now < deadline {
        if await predicate() { return }
        try? await Task.sleep(for: .milliseconds(5))
    }
    Issue.record("Condition did not become true before timeout")
}
