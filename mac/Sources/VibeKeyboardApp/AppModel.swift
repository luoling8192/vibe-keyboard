import AppKit
import Combine
import Foundation
import VibeBoardKit

protocol AppDeviceSession: Sendable {
    var descriptor: USBDeviceDescriptor { get }
    var events: AsyncStream<USBSessionEvent> { get }
    func connect() async throws
    func disconnect() async
    func upload(_ asset: PreparedAsset, transferID: UInt32) async throws -> AssetTransferService.Progress
    func cancelUpload(transferID: UInt32) async throws
    func queryScreen() async throws
    func commitScreen(_ commit: ScreenCommit) async throws
    func synchronizeLED(_ context: ReplacementSessionContext?) async
    func queryLED() async throws
    func configureLED(enabled: Bool, brightness: UInt8) async throws
    func configureInput(mode: InteractionMode, voiceKey: VoiceKey) async throws
    func sendWidgetUpdate(_ command: WidgetUpdateCommand) async throws
}

protocol AppDeviceSessionFactory: Sendable {
    func makeSession(for descriptor: USBDeviceDescriptor) throws -> any AppDeviceSession
}

actor ProductionDeviceSession: AppDeviceSession {
    nonisolated let descriptor: USBDeviceDescriptor
    nonisolated let events: AsyncStream<USBSessionEvent>

    private let session: USBSession
    private let assets: AssetTransferService
    private let screen: ScreenConfigurationService
    private let led: LEDService

    init(descriptor: USBDeviceDescriptor) throws {
        let session = try USBSession(descriptor: descriptor)
        self.descriptor = descriptor
        self.events = session.events
        self.session = session
        self.assets = AssetTransferService(session: session)
        self.screen = ScreenConfigurationService(session: session)
        let led = LEDService(commandWriter: { command in try await session.sendLEDCommand(command) })
        self.led = led
        Task {
            _ = await session.addLEDServiceConsumer { event, responseTime in
                try? await led.consume(event, at: responseTime)
            }
        }
    }

    func connect() async throws {
        _ = try await session.connect()
    }

    func disconnect() async {
        await session.disconnect()
    }

    func upload(_ asset: PreparedAsset, transferID: UInt32) async throws -> AssetTransferService.Progress {
        try await assets.upload(asset, transferID: transferID)
    }

    func cancelUpload(transferID: UInt32) async throws {
        try await assets.cancel(transferID: transferID)
    }

    func queryScreen() async throws {
        try await screen.query()
    }

    func commitScreen(_ commit: ScreenCommit) async throws {
        try await screen.commit(commit)
    }

    func synchronizeLED(_ context: ReplacementSessionContext?) async {
        await led.synchronize(with: context)
    }

    func queryLED() async throws {
        _ = try await led.query()
    }

    func configureLED(enabled: Bool, brightness: UInt8) async throws {
        _ = try await led.configure(enabled: enabled, brightness: brightness)
    }

    func configureInput(mode: InteractionMode, voiceKey: VoiceKey) async throws {
        try await session.send(.interactionMode(mode))
        try await session.send(.voiceKey(voiceKey))
    }

    func sendWidgetUpdate(_ command: WidgetUpdateCommand) async throws {
        try await session.sendWidgetUpdate(command)
    }
}

struct ProductionDeviceSessionFactory: AppDeviceSessionFactory {
    func makeSession(for descriptor: USBDeviceDescriptor) throws -> any AppDeviceSession {
        try ProductionDeviceSession(descriptor: descriptor)
    }
}

enum AppPage: String, CaseIterable, Identifiable {
    case device = "Device"
    case screen = "Screen"
    case pets = "Pets"
    case keys = "Keys"
    case audio = "Audio"
    case firmware = "Firmware"

    var id: String { rawValue }
    var symbol: String {
        switch self {
        case .device: "keyboard"
        case .screen: "rectangle.on.rectangle"
        case .pets: "pawprint"
        case .keys: "command"
        case .audio: "waveform"
        case .firmware: "cpu"
        }
    }
}

enum AppConnectionState: Equatable {
    case disconnected
    case connecting(String)
    case ready(USBDeviceInfo)
    case incompatible(String)
    case failed(String)

    var title: String {
        switch self {
        case .disconnected: "Disconnected"
        case .connecting: "Connecting"
        case .ready: "Connected"
        case .incompatible: "Incompatible"
        case .failed: "Failed"
        }
    }
}

enum CapabilityPresentation: Equatable {
    case absent
    case unavailable(String)
    case available

    var label: String {
        switch self {
        case .absent: "Not advertised"
        case .unavailable(let reason): "Unavailable — \(reason.replacingOccurrences(of: "_", with: " "))"
        case .available: "Available"
        }
    }

    var isAvailable: Bool {
        if case .available = self { return true }
        return false
    }
}

struct UploadedAssetSummary: Equatable {
    let sha256: String
    let totalBytes: UInt32
    let kind: AssetKind
}

enum UploadPresentation: Equatable {
    case idle
    case validating
    case converting
    case sending
    case verifying
    case active(String)
    case cancelled
    case failed(String)

    var label: String {
        switch self {
        case .idle: "Idle"
        case .validating: "Validating"
        case .converting: "Converting"
        case .sending: "Sending"
        case .verifying: "Verifying"
        case .active(let hash): "Active — \(hash.prefix(12))…"
        case .cancelled: "Cancelled"
        case .failed(let message): "Failed — \(message)"
        }
    }
}

protocol AppMonotonicClock: Sendable {
    func nowMilliseconds() -> UInt64
}

struct SystemAppMonotonicClock: AppMonotonicClock {
    func nowMilliseconds() -> UInt64 {
        DispatchTime.now().uptimeNanoseconds / 1_000_000
    }
}

private struct CurrentScreenSelection: Equatable {
    let epochGeneration: UInt64
    let snapshotGeneration: UInt64
    var configured: Bool
    var mode: ScreenMode?
    var revision: UInt32
}

@MainActor
final class AppModel: ObservableObject {
    @Published var selectedPage: AppPage = .device
    @Published private(set) var connection: AppConnectionState = .disconnected
    @Published private(set) var replacementContext: ReplacementSessionContext?
    @Published private(set) var assetsCapability: CapabilityPresentation = .absent
    @Published private(set) var screenCapability: CapabilityPresentation = .absent
    @Published private(set) var updateCapability: CapabilityPresentation = .absent
    @Published private(set) var ledCapability: CapabilityPresentation = .absent
    @Published private(set) var ledState: LEDStateEvent?
    @Published private(set) var lastWidgetEvent: WidgetProtocolEvent?
    @Published private(set) var upload: UploadPresentation = .idle
    @Published private(set) var uploadProgress: Double = 0
    @Published private(set) var activeTransferID: UInt32?
    @Published private(set) var lastUploadedAsset: UploadedAssetSummary?
    @Published private(set) var lastScreenState: ReplacementScreenEvent?
    @Published private(set) var keyProfile: KeyMappingProfile = .vendorDefault()
    @Published private(set) var highlightedKey: CanonicalKey?
    @Published private(set) var audioState: AudioRecordingState = .ready
    @Published private(set) var lastRecording: String = "None"
    @Published private(set) var diagnosticMessage: String?
    @Published private(set) var previewPixels: [UInt16]?
    @Published private(set) var previewLayout: ScreenLayout?
    @Published private(set) var lastActionResult: String?
    @Published var selectedKey: CanonicalKey = .k1
    @Published var screenMode: ScreenMode = .pet
    @Published private(set) var interactionMode: InteractionMode = .holdToTalk
    @Published var saveRecordings = false
    @Published var layoutTitle: String = "Vibe Dashboard"
    @Published var widgetText: String = "Ready"

    private let monitor: any USBDeviceMonitoring
    private let sessionFactory: any AppDeviceSessionFactory
    private let mappingRepository: KeyMappingRepository
    private let recordingFactory: any RecordingSinkCreating
    private let actionPipeline: (any AppActionRouting)?
    private let monotonicClock: any AppMonotonicClock
    private var monitorTask: Task<Void, Never>?
    private var sessionTask: Task<Void, Never>?
    private var operationTask: Task<Void, Never>?
    private var session: (any AppDeviceSession)?
    private var audioRecorder: AudioRecordingSession?
    private var recordingSink: DataOggPageSink?
    private var recordingDestination: URL?
    private var currentScreenSelection: CurrentScreenSelection?
    private var pendingScreenCommit: (epochGeneration: UInt64, snapshotGeneration: UInt64, previousRevision: UInt32, revision: UInt32, mode: ScreenMode)?
    private var widgetSequence: UInt32 = 0

    init(
        monitor: any USBDeviceMonitoring,
        sessionFactory: any AppDeviceSessionFactory,
        mappingRepository: KeyMappingRepository,
        recordingFactory: any RecordingSinkCreating = ApplicationSupportRecordingSinkFactory(),
        actionPipeline: (any AppActionRouting)? = nil,
        monotonicClock: any AppMonotonicClock = SystemAppMonotonicClock()
    ) {
        self.monitor = monitor
        self.sessionFactory = sessionFactory
        self.mappingRepository = mappingRepository
        self.recordingFactory = recordingFactory
        self.actionPipeline = actionPipeline
        self.monotonicClock = monotonicClock
    }

    convenience init() {
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
            .appendingPathComponent("VibeKeyboard", isDirectory: true)
            .appendingPathComponent("key-mappings.json", isDirectory: false)
        let profile = KeyMappingProfile.vendorDefault()
        let adapter = ProductionHostActionAdapter()
        let pipeline = try? AppGestureActionPipeline(profile: profile, adapter: adapter)
        self.init(
            monitor: USBDeviceMonitor(),
            sessionFactory: ProductionDeviceSessionFactory(),
            mappingRepository: KeyMappingRepository(store: FileConfigurationDataStore(url: support)),
            actionPipeline: pipeline
        )
        Task { [weak self, adapter] in
            await adapter.setScreenHandler { [weak self] mode in self?.screenMode = mode }
        }
    }

    deinit {
        monitorTask?.cancel()
        sessionTask?.cancel()
        operationTask?.cancel()
    }

    func start() {
        guard monitorTask == nil else { return }
        monitorTask = Task { [weak self, monitor] in
            guard let self else { return }
            do {
                self.keyProfile = try await self.mappingRepository.load()
                try await self.actionPipeline?.updateProfile(self.keyProfile)
            } catch {
                self.diagnosticMessage = "Key mapping load failed: \(error)"
            }
            for await event in monitor.events() {
                guard !Task.isCancelled else { return }
                await self.consumeMonitor(event)
            }
        }
    }

    func reconnect() {
        guard let descriptor = session?.descriptor else {
            connection = .disconnected
            return
        }
        Task { [weak self] in await self?.attach(descriptor) }
    }

    func disconnect() {
        let old = session
        clearSessionPresentation()
        sessionTask?.cancel()
        sessionTask = nil
        session = nil
        Task { await old?.disconnect() }
    }

    func importAndUpload(url: URL, pet: Bool = false) {
        guard operationTask == nil, assetsCapability.isAvailable, let session,
              let assets = availableAssets else { return }
        operationTask = Task { [weak self] in
            guard let self else { return }
            defer { self.operationTask = nil }
            do {
                self.upload = .validating
                let data = try Data(contentsOf: url, options: .mappedIfSafe)
                let decoded = try AssetSourceDecoder.decode(
                    data,
                    minimumFrameMS: assets.minFrameMS,
                    maximumFrameMS: assets.maxFrameMS
                )
                self.upload = .converting
                let limits = VKA1Limits(
                    maxFrames: assets.maxFrames,
                    minFrameDurationMS: assets.minFrameMS,
                    maxFrameDurationMS: assets.maxFrameMS,
                    maxContainerBytes: assets.maxAssetBytes,
                    maxDecodedBytes: assets.maxActiveDecodedBytes
                )
                let container = try ConvertedAssetFactory.makeVKA1(
                    source: decoded,
                    fit: .contain,
                    background: AssetRGB888(red: 0, green: 0, blue: 0),
                    limits: limits
                )
                let prepared = try PreparedAsset(data: container, limits: limits)
                guard !pet || prepared.kind == .animation else {
                    throw AssetServiceError.invalidAsset
                }
                let transferID = Self.nonzeroTransferID()
                self.activeTransferID = transferID
                self.upload = .sending
                let result = try await session.upload(prepared, transferID: transferID)
                self.uploadProgress = Double(result.nextOffset) / Double(result.totalBytes)
                self.lastUploadedAsset = UploadedAssetSummary(sha256: prepared.sha256, totalBytes: result.totalBytes, kind: prepared.kind)
                self.previewPixels = try VKA1Codec.decode(container, limits: limits).frames.first?.pixels
                self.previewLayout = nil
                self.upload = .active(prepared.sha256)
                self.activeTransferID = nil
            } catch is CancellationError {
                self.upload = .cancelled
                self.activeTransferID = nil
            } catch {
                self.upload = .failed(String(describing: error))
                self.activeTransferID = nil
            }
        }
    }

    func cancelUpload() {
        operationTask?.cancel()
        operationTask = nil
        guard let transferID = activeTransferID, let session else {
            upload = .cancelled
            return
        }
        Task {
            try? await session.cancelUpload(transferID: transferID)
            self.activeTransferID = nil
            self.upload = .cancelled
        }
    }

    func queryScreen() {
        guard screenCapability.isAvailable, let session else { return }
        operationTask = Task { [weak self] in
            defer { self?.operationTask = nil }
            do { try await session.queryScreen() }
            catch { self?.diagnosticMessage = "Screen query failed: \(error)" }
        }
    }

    func activateUploadedImage() {
        guard canSendScreen, let session, let context = replacementContext,
              let screen = availableScreen, let asset = lastUploadedAsset,
              asset.kind == .image else { return }
        let limits = ScreenCommitLimits(
            displayWidth: context.snapshot.display.width,
            displayHeight: context.snapshot.display.height,
            screen: screen
        )
        guard let previousRevision = currentScreenRevision else { return }
        let revision = previousRevision &+ 1
        guard revision != 0 else { return }
        let commit = ScreenCommit(
            expectedRevision: previousRevision,
            revision: revision,
            assets: [ScreenAssetReference(bytes: asset.totalBytes, kind: asset.kind, sha256: asset.sha256)],
            payload: .image(ScreenImage(backgroundRGB888: 0, fit: .contain, sha256: asset.sha256)),
            limits: .init(displayWidth: limits.displayWidth, displayHeight: limits.displayHeight, screen: screen.selecting(revision: previousRevision, configured: isCurrentScreenConfigured))
        )
        pendingScreenCommit = (context.epochGeneration, context.snapshotGeneration, previousRevision, revision, .image)
        operationTask = Task { [weak self] in
            defer { self?.operationTask = nil }
            do { try await session.commitScreen(commit) }
            catch {
                self?.pendingScreenCommit = nil
                self?.diagnosticMessage = "Screen commit failed: \(error)"
            }
        }
    }

    func activateUploadedPet() {
        guard let asset = lastUploadedAsset, asset.kind == .animation else {
            diagnosticMessage = "A bounded animation must be uploaded first"
            return
        }
        let manifest = ScreenPetManifest(id: "primary-pet", states: [
            .idle: .asset(sha256: asset.sha256),
            .active: .idleFallback,
            .recording: .idleFallback,
            .thinking: .idleFallback,
            .success: .idleFallback,
            .error: .idleFallback,
        ])
        commit(payload: .pet(manifest), assets: [ScreenAssetReference(bytes: asset.totalBytes, kind: asset.kind, sha256: asset.sha256)])
    }

    func activateLayout(mode: ScreenLayout.Mode) {
        guard let screen = availableScreen, let context = replacementContext,
              let font = screen.fonts.first, let previousRevision = currentScreenRevision else {
            diagnosticMessage = "A current screen capability and reviewed font are required"
            return
        }
        guard Self.isPrintableASCII(layoutTitle), Self.isPrintableASCII(widgetText) else {
            diagnosticMessage = "Layout text supports printable ASCII; use Image mode for other text"
            return
        }
        let revision = previousRevision &+ 1
        guard revision != 0 else { diagnosticMessage = "Revision wrapped to zero"; return }
        let title = ScreenObjectNode.staticLabel(
            base: .init(id: "title", width: 300, height: 28, z: 0, clip: true, visible: true),
            align: .left,
            colorRGB888: 0xFFFFFF,
            font: .init(id: font.id, version: font.version),
            text: layoutTitle
        )
        let value = ScreenObjectNode.dynamicLabel(
            base: .init(id: "status-value", width: 300, height: 28, z: 1, clip: true, visible: true),
            align: .left,
            colorRGB888: 0x6ED0FF,
            font: .init(id: font.id, version: font.version),
            widgetID: "status"
        )
        let layout = ScreenLayout(
            backgroundRGB888: 0x101820,
            mode: mode,
            revision: revision,
            objects: [
                .init(x: 16, y: 28, node: title),
                .init(x: 16, y: 72, node: value),
            ],
            widgets: [.text(id: "status", target: "status-value", fallback: widgetText)]
        )
        previewLayout = layout
        previewPixels = nil
        commit(payload: mode == .dashboard ? .dashboard(layout) : .custom(layout), assets: [])
        _ = context
    }

    func sendStatusWidget() {
        guard let session, let revision = currentScreenRevision, isCurrentScreenConfigured else { return }
        guard Self.isPrintableASCII(widgetText) else {
            diagnosticMessage = "Widget text supports printable ASCII; use Image mode for other text"
            return
        }
        let next = widgetSequence &+ 1
        guard next != 0 else { diagnosticMessage = "Widget sequence exhausted"; return }
        widgetSequence = next
        operationTask = Task { [weak self] in
            defer { self?.operationTask = nil }
            do {
                try await session.sendWidgetUpdate(.init(revision: revision, widgetID: "status", sequence: next, state: .freshText(self?.widgetText ?? "")))
            } catch { self?.diagnosticMessage = "Widget update failed: \(error)" }
        }
    }

    private func commit(payload: ScreenConfiguredPayload, assets: [ScreenAssetReference]) {
        guard canSendScreen, let session, let context = replacementContext, let screen = availableScreen,
              let previousRevision = currentScreenRevision else { return }
        let revision = previousRevision &+ 1
        guard revision != 0 else { diagnosticMessage = "Revision wrapped to zero"; return }
        let commit = ScreenCommit(
            expectedRevision: previousRevision,
            revision: revision,
            assets: assets.sorted { $0.sha256 < $1.sha256 },
            payload: payload,
            limits: .init(displayWidth: context.snapshot.display.width, displayHeight: context.snapshot.display.height, screen: screen.selecting(revision: previousRevision, configured: isCurrentScreenConfigured))
        )
        let mode: ScreenMode
        switch payload {
        case .image: mode = .image
        case .pet: mode = .pet
        case .dashboard: mode = .dashboard
        case .custom: mode = .custom
        }
        pendingScreenCommit = (context.epochGeneration, context.snapshotGeneration, previousRevision, revision, mode)
        operationTask = Task { [weak self] in
            defer { self?.operationTask = nil }
            do { try await session.commitScreen(commit) }
            catch {
                self?.pendingScreenCommit = nil
                self?.diagnosticMessage = "Screen commit failed: \(error)"
            }
        }
    }

    func saveMappings() {
        let profile = keyProfile
        let mode = interactionMode
        let voiceKey = Self.deviceVoiceKey(for: profile)
        let session = session
        Task { [weak self] in
            do {
                try await self?.mappingRepository.save(profile)
            } catch {
                self?.diagnosticMessage = "Key mapping save failed: \(error)"
                return
            }
            do { try await session?.configureInput(mode: mode, voiceKey: voiceKey) }
            catch { self?.diagnosticMessage = "Input configuration failed: \(error)" }
        }
    }

    func setAction(_ action: HostAction, for key: CanonicalKey, gesture: KeyGesture) {
        var mappings = keyProfile.mappings
        if action == .voiceInput {
            for candidate in CanonicalKey.allCases {
                guard var bindings = mappings[candidate] else { continue }
                if bindings.single == .voiceInput { bindings.single = .none }
                if bindings.double == .voiceInput { bindings.double = .none }
                if bindings.long == .voiceInput { bindings.long = .none }
                mappings[candidate] = bindings
            }
        }
        guard var bindings = mappings[key] else { return }
        bindings[gesture] = action
        mappings[key] = bindings
        do {
            keyProfile = try KeyMappingProfile(mappings: mappings)
            let profile = keyProfile
            Task { [weak self] in
                do { try await self?.actionPipeline?.updateProfile(profile) }
                catch { self?.diagnosticMessage = "Action routing update failed: \(error)" }
            }
        } catch { diagnosticMessage = "Invalid mapping: \(error)" }
    }

    func setSingleAction(_ action: HostAction, for key: CanonicalKey) {
        setAction(action, for: key, gesture: .single)
    }

    func setInteractionMode(_ mode: InteractionMode) {
        interactionMode = mode
        guard let session else { return }
        let voiceKey = Self.deviceVoiceKey(for: keyProfile)
        Task { [weak self] in
            do { try await session.configureInput(mode: mode, voiceKey: voiceKey) }
            catch { self?.diagnosticMessage = "Input configuration failed: \(error)" }
        }
    }

    var availableAssets: AssetsCapability? {
        guard case .available(let value)? = replacementContext?.snapshot.assets else { return nil }
        return value
    }

    var availableScreen: ScreenCapability? {
        guard case .available(let value)? = replacementContext?.snapshot.screen else { return nil }
        return value
    }

    private var currentScreenRevision: UInt32? {
        guard let context = replacementContext, let selection = currentScreenSelection,
              selection.epochGeneration == context.epochGeneration,
              selection.snapshotGeneration == context.snapshotGeneration else {
            return availableScreen?.revision
        }
        return selection.revision
    }

    private var isCurrentScreenConfigured: Bool {
        guard let context = replacementContext, let selection = currentScreenSelection,
              selection.epochGeneration == context.epochGeneration,
              selection.snapshotGeneration == context.snapshotGeneration else {
            return availableScreen?.configured ?? false
        }
        return selection.configured
    }

    var canMutateFirmware: Bool { false }
    var canConfigureLED: Bool { ledCapability.isAvailable }
    var canSendScreen: Bool { screenCapability.isAvailable && assetsCapability.isAvailable }
    var isConnected: Bool { if case .ready = connection { true } else { false } }

    private func consumeMonitor(_ event: USBDeviceMonitorEvent) async {
        switch event {
        case .attached(let descriptor):
            guard session == nil else { return }
            await attach(descriptor)
        case .detached(let registryEntryID):
            guard session?.descriptor.registryEntryID == registryEntryID else { return }
            disconnect()
        case .failed(let error):
            diagnosticMessage = "USB discovery failed: \(error)"
            connection = .failed("USB discovery failed")
        }
    }

    private func attach(_ descriptor: USBDeviceDescriptor) async {
        operationTask?.cancel()
        sessionTask?.cancel()
        if let existing = session { await existing.disconnect() }
        clearSessionPresentation()
        do {
            let next = try sessionFactory.makeSession(for: descriptor)
            session = next
            connection = .connecting("Opening USB Serial/JTAG")
            sessionTask = Task { [weak self, events = next.events] in
                for await event in events {
                    guard !Task.isCancelled else { return }
                    self?.consumeSession(event)
                }
            }
            try await next.connect()
            try await next.configureInput(
                mode: interactionMode,
                voiceKey: Self.deviceVoiceKey(for: keyProfile)
            )
        } catch {
            connection = .failed(String(describing: error))
            diagnosticMessage = "Connection failed: \(error)"
            replacementContext = nil
            updateCapabilities()
        }
    }

    private static func deviceVoiceKey(for profile: KeyMappingProfile) -> VoiceKey {
        for key in CanonicalKey.allCases {
            guard let bindings = profile.mappings[key],
                  bindings.single == .voiceInput ||
                  bindings.double == .voiceInput ||
                  bindings.long == .voiceInput else { continue }
            return VoiceKey(rawValue: key.rawValue) ?? .none
        }
        return .none
    }

    private static func isPrintableASCII(_ value: String) -> Bool {
        !value.isEmpty && value.utf8.allSatisfy { (0x20...0x7e).contains($0) }
    }

    private func consumeSession(_ event: USBSessionEvent) {
        switch event {
        case .stateChanged(let state):
            switch state {
            case .disconnected:
                clearSessionPresentation()
            case .opening, .inspecting, .announcingUSBTransport, .requestingDeviceInfo, .synchronizingConfiguration:
                connection = .connecting(String(describing: state))
            case .ready(let info): connection = .ready(info)
            case .incompatible(let reason): connection = .incompatible(reason)
            case .failed(let error):
                replacementContext = nil
                currentScreenSelection = nil
                pendingScreenCommit = nil
                resetGestures()
                updateCapabilities()
                connection = .failed(String(describing: error))
            }
        case .replacementCapabilities(let context):
            replacementContext = context
            pendingScreenCommit = nil
            if case .available(let screen)? = context.snapshot.screen {
                currentScreenSelection = .init(
                    epochGeneration: context.epochGeneration,
                    snapshotGeneration: context.snapshotGeneration,
                    configured: screen.configured,
                    mode: nil,
                    revision: screen.revision
                )
            } else {
                currentScreenSelection = nil
            }
            resetGestures()
            updateCapabilities()
            if let session { Task { await session.synchronizeLED(context) } }
        case .replacementEvent(let event):
            if case .screen(let screen) = event {
                consumeScreenEvent(screen)
            }
            if case .widget(let widget) = event { lastWidgetEvent = widget }
            if case .led(.state(let state)) = event { ledState = state }
            if case .led(.error(let error)) = event { diagnosticMessage = "LED: \(error.code)" }
            if case .error(let error) = event { diagnosticMessage = "\(error.operation): \(error.code)" }
        case .audioFrame(let frame): consumeAudio(frame)
        case .stateEvent(let state): consumeKeyEvent(state)
        case .protocolDiagnostic, .discardedByte: break
        }
    }

    private func consumeKeyEvent(_ event: StateEvent) {
        guard let button = event.button, let key = try? CanonicalKey(deviceValue: button) else { return }
        let deviceEvent: DeviceKeyEvent
        switch event.event {
        case "button_down":
            highlightedKey = key
            deviceEvent = .down(key)
        case "button_up":
            if highlightedKey == key { highlightedKey = nil }
            deviceEvent = .up(key, durationMilliseconds: event.durationMS)
        case "button_click":
            if highlightedKey == key { highlightedKey = nil }
            deviceEvent = .click(key, durationMilliseconds: event.durationMS)
        default:
            return
        }
        let timestamp = monotonicClock.nowMilliseconds()
        Task { [weak self] in
            guard let self, let actionPipeline else { return }
            do {
                let results = try await actionPipeline.consume(deviceEvent, at: timestamp)
                if let result = results.last { self.lastActionResult = String(describing: result) }
            } catch {
                self.diagnosticMessage = "Key action failed: \(error)"
            }
        }
    }

    private func consumeAudio(_ frame: AudioFrame) {
        if audioRecorder == nil {
            do {
                if saveRecordings {
                    let target = try recordingFactory.makeSink(session: frame.session)
                    recordingDestination = target.destination
                    recordingSink = nil
                    audioRecorder = AudioRecordingSession(sink: target.sink)
                } else {
                    let sink = DataOggPageSink()
                    recordingSink = sink
                    recordingDestination = nil
                    audioRecorder = AudioRecordingSession(sink: sink)
                }
            } catch {
                audioState = .failed(.output(String(describing: error)))
                diagnosticMessage = "Recording file creation failed: \(error)"
                return
            }
        }
        do {
            try audioRecorder?.consume(frame)
            if let state = audioRecorder?.state {
                audioState = state
                if case .completed(let session, let packets) = state {
                    if let destination = recordingDestination {
                        lastRecording = destination.path
                    } else {
                        let bytes = recordingSink?.data.count ?? 0
                        lastRecording = "Session \(session), \(packets) packets, \(bytes) Ogg bytes (not saved)"
                    }
                    audioRecorder = nil
                    recordingSink = nil
                    recordingDestination = nil
                }
            }
        } catch {
            audioState = .failed(error as? AudioRecordingError ?? .output(String(describing: error)))
            diagnosticMessage = "Audio recording failed: \(error)"
            audioRecorder = nil
            recordingSink = nil
            recordingDestination = nil
        }
    }

    private func clearSessionPresentation() {
        operationTask?.cancel()
        operationTask = nil
        replacementContext = nil
        ledState = nil
        lastWidgetEvent = nil
        activeTransferID = nil
        upload = .idle
        uploadProgress = 0
        highlightedKey = nil
        audioRecorder?.cancel()
        audioRecorder = nil
        recordingSink = nil
        recordingDestination = nil
        previewPixels = nil
        previewLayout = nil
        currentScreenSelection = nil
        pendingScreenCommit = nil
        resetGestures()
        audioState = .ready
        connection = .disconnected
        updateCapabilities()
    }

    private func consumeScreenEvent(_ event: ReplacementScreenEvent) {
        lastScreenState = event
        guard let context = replacementContext, var selection = currentScreenSelection,
              selection.epochGeneration == context.epochGeneration,
              selection.snapshotGeneration == context.snapshotGeneration else { return }
        switch event {
        case .state(let configured, let mode, let revision, _, _):
            guard revision >= selection.revision else {
                diagnosticMessage = "Screen state revision moved backwards"
                return
            }
            selection.configured = configured
            selection.mode = mode
            selection.revision = revision
            pendingScreenCommit = nil
        case .committed(_, let previousRevision, let revision, _):
            guard let pending = pendingScreenCommit,
                  pending.epochGeneration == context.epochGeneration,
                  pending.snapshotGeneration == context.snapshotGeneration,
                  pending.previousRevision == previousRevision,
                  pending.revision == revision,
                  selection.revision == previousRevision else {
                diagnosticMessage = "Uncorrelated screen commit"
                return
            }
            selection.configured = true
            selection.mode = pending.mode
            selection.revision = revision
            pendingScreenCommit = nil
        }
        currentScreenSelection = selection
    }

    private func resetGestures() {
        let timestamp = monotonicClock.nowMilliseconds()
        if let actionPipeline { Task { try? await actionPipeline.disconnect(at: timestamp) } }
    }

    private func updateCapabilities() {
        assetsCapability = Self.presentation(replacementContext?.snapshot.assets)
        screenCapability = Self.presentation(replacementContext?.snapshot.screen)
        updateCapability = Self.presentation(replacementContext?.snapshot.update)
        ledCapability = Self.presentation(replacementContext?.snapshot.led)
    }

    private static func presentation<T>(_ feature: FeatureAvailability<T>?) -> CapabilityPresentation {
        guard let feature else { return .absent }
        switch feature {
        case .available: return .available
        case .unavailable(let unavailable): return .unavailable(unavailable.reason)
        }
    }

    private static func nonzeroTransferID() -> UInt32 {
        var value = UInt32.random(in: 1...UInt32.max)
        if value == 0 { value = 1 }
        return value
    }
}
