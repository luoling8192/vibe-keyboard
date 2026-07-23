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
        #expect(model.updateCapability.label == "ROM flash only")
        #expect(model.ledCapability.label == "Calibration pending")
    }

    @Test func busyStorageBlocksImportWithRecoveryMessage() async throws {
        let descriptor = testDescriptor()
        let monitor = TestMonitor()
        let session = TestSession(descriptor: descriptor)
        let model = makeModel(monitor: monitor, session: session)
        model.start()
        monitor.send(.attached(descriptor))
        await eventually { model.isConnected }

        session.send(.replacementCapabilities(.init(
            epochGeneration: 2,
            snapshotGeneration: 5,
            snapshot: try ReplacementCapabilitySnapshot.decode(Data(capabilityJSON(
                assets: busyAssetsJSON,
                screen: availableScreenJSON
            ).utf8))
        )))
        await eventually { model.availableAssets?.storageState == .busy }
        #expect(!model.canUploadAssets)

        model.importAndUpload(url: URL(fileURLWithPath: "/dev/null"))
        #expect(model.upload == .failed("Busy — reconnect the device after a USB power cycle"))
    }

    @Test func failedTransferIsAbortedSoTheNextUploadIsNotLeftBusy() async throws {
        let descriptor = testDescriptor()
        let monitor = TestMonitor()
        let session = TestSession(descriptor: descriptor, failUploads: true)
        let model = makeModel(monitor: monitor, session: session)
        model.start()
        monitor.send(.attached(descriptor))
        await eventually { model.isConnected }
        session.send(.replacementCapabilities(.init(epochGeneration: 2, snapshotGeneration: 6, snapshot: try availableSnapshot())))
        await eventually { model.canUploadAssets }

        let imageURL = FileManager.default.temporaryDirectory
            .appendingPathComponent("vibe-keyboard-\(UUID().uuidString).png")
        let image = try #require(Data(base64Encoded:
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="
        ))
        try image.write(to: imageURL)
        defer { try? FileManager.default.removeItem(at: imageURL) }

        model.importAndUpload(url: imageURL)
        await eventually { await session.cancelCount() == 1 }
        #expect(model.activeTransferID == nil)
        guard case .failed = model.upload else {
            Issue.record("Expected failed upload")
            return
        }
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

    @Test func audioPageVoiceSelectionReplacesAndPersistsTheVoiceBinding() async throws {
        let descriptor = testDescriptor()
        let monitor = TestMonitor()
        let session = TestSession(descriptor: descriptor)
        let store = MemoryConfigurationStore()
        let model = AppModel(
            monitor: monitor,
            sessionFactory: TestSessionFactory(session: session),
            mappingRepository: KeyMappingRepository(store: store)
        )
        model.start()
        monitor.send(.attached(descriptor))
        await eventually { model.isConnected }

        #expect(model.configuredVoiceKey == .k4)
        model.setVoiceKey(.k2)
        await eventually { await session.lastVoiceKey() == .k2 }
        #expect(model.configuredVoiceKey == .k2)
        #expect(model.keyProfile.mappings[.k2]?.single == .voiceInput)
        #expect(model.keyProfile.mappings[.k4]?.single == HostAction.none)

        let loaded = try await KeyMappingRepository(store: store).load()
        #expect(loaded.mappings[.k2]?.single == .voiceInput)
        #expect(loaded.mappings[.k4]?.single == HostAction.none)
    }

    @Test func inputAudioErrorBecomesVisibleAudioFailure() async throws {
        let descriptor = testDescriptor()
        let monitor = TestMonitor()
        let session = TestSession(descriptor: descriptor)
        let model = makeModel(monitor: monitor, session: session)
        model.start()
        monitor.send(.attached(descriptor))
        await eventually { model.isConnected }
        session.send(.replacementCapabilities(.init(epochGeneration: 3, snapshotGeneration: 2, snapshot: try availableSnapshot())))
        await eventually { model.replacementContext != nil }

        let error = try ReplacementEventDecoder.decode(
            Data(#"{"code":"audio_start_failed","event":"vk_error","operation":"input"}"#.utf8)
        )
        session.send(.replacementEvent(error))
        await eventually {
            model.audioState == .failed(.output("Device audio: audio_start_failed"))
        }
    }

    @Test func shortcutResolverSupportsControlDigitsFunctionAndNamedKeys() throws {
        #expect(ProductionHostActionAdapter.keyCode(forShortcutKey: "1") == 18)
        #expect(ProductionHostActionAdapter.keyCode(forShortcutKey: "fn") == 63)
        #expect(ProductionHostActionAdapter.keyCode(forShortcutKey: "F12") == 111)
        #expect(ProductionHostActionAdapter.keyCode(forShortcutKey: "F20") == 90)
        #expect(ProductionHostActionAdapter.keyCode(forShortcutKey: "pageup") == 116)
        #expect(ProductionHostActionAdapter.keyCode(forShortcutKey: "unknown") == nil)

        let flags = ProductionHostActionAdapter.flags(for: [.control, .function])
        #expect(flags.contains(.maskControl))
        #expect(flags.contains(.maskSecondaryFn))
    }

    @Test func liveDashboardCommitsTwoRotatingTilesAndStartsUpdates() async throws {
        let descriptor = testDescriptor()
        let monitor = TestMonitor()
        let session = TestSession(descriptor: descriptor)
        let snapshot = LiveDashboardSnapshot(
            codex: .init(usedPercent: 45, tokensToday: 1_250_000, status: "ready", error: nil),
            claude: .init(usedPercent: nil, tokensToday: 42_000, status: "idle", error: nil),
            cpuPercent: 23,
            memoryPercent: 67,
            downloadBytesPerSecond: 1_500_000,
            uploadBytesPerSecond: 42_000,
            stocks: [
                .init(symbol: "000001", price: "3876.78", changePercent: "+0.25%"),
                .init(symbol: "AAPL", price: "220.12", changePercent: "+0.57%"),
                .init(symbol: "00700", price: "601.50", changePercent: "-0.18%"),
            ],
            sampledAt: Date()
        )
        let model = AppModel(
            monitor: monitor,
            sessionFactory: TestSessionFactory(session: session),
            mappingRepository: KeyMappingRepository(store: MemoryConfigurationStore()),
            dashboardProvider: DashboardProviderStub(value: snapshot)
        )
        model.start()
        monitor.send(.attached(descriptor))
        await eventually { model.isConnected }
        session.send(.replacementCapabilities(.init(
            epochGeneration: 7,
            snapshotGeneration: 2,
            snapshot: try availableSnapshot()
        )))
        await eventually { model.canSendScreen && model.dashboardSnapshot == snapshot }

        model.activateLiveDashboard()
        await eventually { await session.commitCount() == 1 }
        let commit = try #require(await session.lastCommit())
        guard case .dashboard(let layout) = commit.payload else {
            Issue.record("Expected live dashboard payload")
            return
        }
        #expect(layout.widgets.count == 6)
        #expect(layout.objects.count == 6)
        #expect(layout.widgets.first == .text(
            id: "left-title",
            target: "left-title-value",
            fallback: "A1 CODEX"
        ))
        #expect(try ReplacementCommandEncoder.encode(.commit(commit)).count <= 4096)

        session.send(.replacementEvent(.screen(.committed(
            assetsManifestSHA256: String(repeating: "a", count: 64),
            previousRevision: 0,
            revision: 1,
            screenManifestSHA256: String(repeating: "b", count: 64)
        ))))
        await eventually { model.liveDashboardEnabled }
        await eventually { await session.widgetCount() >= 6 }
        let widgets = await session.allWidgets()
        #expect(Array(widgets.prefix(6).map(\.widgetID)) == [
            "left-title", "left-line1", "left-line2",
            "right-title", "right-line1", "right-line2",
        ])
        #expect(widgets.prefix(6).allSatisfy { $0.revision == 1 })
    }

    @Test func dashboardPagesUseFourModulesAndRotateStockRows() {
        let snapshot = LiveDashboardSnapshot(
            codex: .init(usedPercent: 45, tokensToday: 1_250_000, status: "ready", error: nil),
            claude: .init(usedPercent: nil, tokensToday: 42_000, status: "idle", error: nil),
            cpuPercent: 23,
            memoryPercent: 67,
            downloadBytesPerSecond: 1_500_000,
            uploadBytesPerSecond: 42_000,
            stocks: [
                .init(symbol: "000001", price: "3876.8", changePercent: "+0.25%"),
                .init(symbol: "AAPL", price: "220.12", changePercent: "+0.57%"),
                .init(symbol: "00700", price: "601.50", changePercent: "-0.18%"),
            ],
            sampledAt: Date()
        )
        let modules: [DashboardModule] = [.codex, .system, .stocks, .network]

        let first = snapshot.page(modules: modules, pageIndex: 0, stockOffset: 0)
        #expect(first.left == .init(
            title: "A1 CODEX",
            line1: "LIMIT 45%",
            line2: "TODAY 1.2M"
        ))
        #expect(first.right == .init(
            title: "A2 SYSTEM",
            line1: "CPU 23%",
            line2: "MEM 67%"
        ))

        let second = snapshot.page(modules: modules, pageIndex: 1, stockOffset: 2)
        #expect(second.left == .init(
            title: "B1 STOCKS 3-1/3",
            line1: "00700 601.50 -0.18%",
            line2: "000001 3876.8 +0.25%"
        ))
        #expect(second.right == .init(
            title: "B2 NETWORK",
            line1: "DOWN 1.5M/s",
            line2: "UP 42K/s"
        ))
    }

    @Test func stockInputIsBoundedAndASCIIQuoteParsingPreservesOrder() {
        #expect(StockReader.normalizedList(" sh000001，hk700,usAapl,bad ") == [
            "sh000001", "hk00700", "usAAPL",
        ])
        let data = Data(
            #"v_usAAPL="1~Apple~AAPL~220.12~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~1.25~0.57";v_sh000001="1~Index~000001~3876.78~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~0~9.75~0.25";"#.utf8
        )
        let values = StockReader.parse(data: data, order: ["sh000001", "usAAPL"])
        #expect(values == [
            StockQuote(symbol: "000001", price: "3876.8", changePercent: "+0.25%"),
            StockQuote(symbol: "AAPL", price: "220.12", changePercent: "+0.57%"),
        ])
    }

    @Test func selectedActionCanBeTestedWithoutForgingAPhysicalKeyEvent() async throws {
        let pipeline = ActionPipelineSpy()
        let model = AppModel(
            monitor: TestMonitor(),
            sessionFactory: TestSessionFactory(session: TestSession(descriptor: testDescriptor())),
            mappingRepository: KeyMappingRepository(store: MemoryConfigurationStore()),
            actionPipeline: pipeline
        )
        model.testAction(.sendEnter)
        await eventually { await pipeline.executedCount() == 1 }
        #expect(model.lastActionResult == "Test — Completed")
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
    private var cancelledTransfers: [UInt32] = []
    private let failUploads: Bool

    init(descriptor: USBDeviceDescriptor, failUploads: Bool = false) {
        self.descriptor = descriptor
        self.failUploads = failUploads
        let pair = AsyncStream<USBSessionEvent>.makeStream()
        events = pair.stream
        continuation = pair.continuation
    }

    func connect() async throws {
        continuation.yield(.stateChanged(.ready(USBDeviceInfo.testValue(descriptor: descriptor))))
    }
    func disconnect() async { continuation.yield(.stateChanged(.disconnected)) }
    func upload(_ asset: PreparedAsset, transferID: UInt32) async throws -> AssetTransferService.Progress {
        if failUploads { throw TestSessionError.uploadFailed }
        return .init(transferID: transferID, nextOffset: UInt32(asset.data.count), totalBytes: UInt32(asset.data.count))
    }
    func cancelUpload(transferID: UInt32) async throws { cancelledTransfers.append(transferID) }
    func queryScreen() async throws {}
    func commitScreen(_ commit: ScreenCommit) async throws { commits.append(commit) }
    func synchronizeLED(_ context: ReplacementSessionContext?) async {}
    func queryLED() async throws {}
    func configureLED(enabled: Bool, brightness: UInt8) async throws {}
    func configureInput(mode: InteractionMode, voiceKey: VoiceKey) async throws {
        inputConfigurations.append((mode, voiceKey))
    }
    func sendWidgetUpdate(_ command: WidgetUpdateCommand) async throws { widgets.append(command) }
    func diagnostics() async -> USBDiagnostics { .init(text: "", entries: []) }
    func commitCount() -> Int { commits.count }
    func lastCommit() -> ScreenCommit? { commits.last }
    func widgetCount() -> Int { widgets.count }
    func lastWidget() -> WidgetUpdateCommand? { widgets.last }
    func allWidgets() -> [WidgetUpdateCommand] { widgets }
    func inputConfigurationCount() -> Int { inputConfigurations.count }
    func lastVoiceKey() -> VoiceKey? { inputConfigurations.last?.1 }
    func cancelCount() -> Int { cancelledTransfers.count }
    nonisolated func send(_ event: USBSessionEvent) { continuation.yield(event) }
}

private enum TestSessionError: Error {
    case uploadFailed
}

private struct TestSessionFactory: AppDeviceSessionFactory {
    let session: TestSession
    func makeSession(for descriptor: USBDeviceDescriptor) throws -> any AppDeviceSession { session }
}

private actor ActionPipelineSpy: AppActionRouting {
    private var events: [DeviceKeyEvent] = []
    private var eventTimestamps: [UInt64] = []
    private var executedActions: [HostAction] = []
    private var resets = 0
    func updateProfile(_ profile: KeyMappingProfile) async throws {}
    func consume(_ event: DeviceKeyEvent, at timestampMilliseconds: UInt64) async throws -> [ActionExecutionResult] {
        events.append(event)
        eventTimestamps.append(timestampMilliseconds)
        return [.completed]
    }
    func execute(_ action: HostAction) async throws -> ActionExecutionResult {
        executedActions.append(action)
        return .completed
    }
    func disconnect(at timestampMilliseconds: UInt64) async throws { resets += 1 }
    func eventCount() -> Int { events.count }
    func timestamps() -> [UInt64] { eventTimestamps }
    func resetCount() -> Int { resets }
    func executedCount() -> Int { executedActions.count }
}

private actor DashboardProviderStub: LiveDashboardProviding {
    let value: LiveDashboardSnapshot
    init(value: LiveDashboardSnapshot) { self.value = value }
    func snapshot(stockSymbols: String) async -> LiveDashboardSnapshot { value }
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
private let busyAssetsJSON = #"{"available":true,"chunk_bytes":4084,"decoder_scratch_bytes":4096,"encodings":["raw","row_rle"],"free_bytes":1048576,"management":true,"max_active_decoded_bytes":131072,"max_asset_bytes":524288,"max_assets":64,"max_frame_ms":5000,"max_frames":32,"min_frame_ms":20,"reserve_bytes":65536,"revision":0,"storage_state":"busy","upload_max_bytes":0,"version":1}"#
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
