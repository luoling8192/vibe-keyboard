import AppKit
import ApplicationServices
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
    func diagnostics() async -> USBDiagnostics
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

    func diagnostics() async -> USBDiagnostics {
        await session.diagnostics()
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
    case keys = "Keys"
    case audio = "Audio"
    case firmware = "Firmware"

    var id: String { rawValue }
    var symbol: String {
        switch self {
        case .device: "keyboard"
        case .screen: "rectangle.on.rectangle"
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
        case .unavailable("bootloader_migration_required"): "ROM flash only"
        case .unavailable("calibration_required"): "Calibration pending"
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
    @Published private(set) var inputPermissionGranted = AXIsProcessTrusted()
    @Published private(set) var dashboardSnapshot = LiveDashboardSnapshot.empty
    @Published private(set) var liveDashboardEnabled = false
    @Published private(set) var pets: [PetCatalogItem] = []
    @Published private(set) var petCatalogStatus = "Loading local pets"
    @Published private(set) var uploadedPetID: String?
    @Published var selectedKey: CanonicalKey = .k1
    @Published var screenMode: ScreenMode = .image
    @Published private(set) var interactionMode: InteractionMode = .holdToTalk
    @Published var saveRecordings = false
    @Published var layoutTitle: String = "Vibe Dashboard"
    @Published var widgetText: String = "Ready"
    @Published var stockSymbols: String = "sh000001"
    @Published var dashboardModules: [DashboardModule] = [
        .codex, .claude, .system, .stocks,
    ]
    @Published var dashboardPageDurationSeconds = 6
    @Published private(set) var dashboardPageIndex = 0
    @Published var petSearch: String = ""
    @Published var selectedPetID: String?
    @Published var petAnimationChoice: PetAnimationChoice = .idle
    @Published private(set) var petPreviewFrames: [CGImage] = []
    @Published private(set) var petPreviewFrameDurationsMS: [Int] = []
    @Published private(set) var petPreviewLoading = false
    @Published private(set) var petPreviewFailed = false
    @Published private(set) var petPreviewItemID: String?

    private let monitor: any USBDeviceMonitoring
    private let sessionFactory: any AppDeviceSessionFactory
    private let mappingRepository: KeyMappingRepository
    private let recordingFactory: any RecordingSinkCreating
    private let actionPipeline: (any AppActionRouting)?
    private let monotonicClock: any AppMonotonicClock
    private let dashboardProvider: any LiveDashboardProviding
    private let petCatalog: any PetCatalogProviding
    private var monitorTask: Task<Void, Never>?
    private var sessionTask: Task<Void, Never>?
    private var operationTask: Task<Void, Never>?
    private var dashboardTask: Task<Void, Never>?
    private var petCatalogTask: Task<Void, Never>?
    private var petPreviewTask: Task<Void, Never>?
    private var session: (any AppDeviceSession)?
    private var audioRecorder: AudioRecordingSession?
    private var recordingSink: DataOggPageSink?
    private var recordingDestination: URL?
    private var currentScreenSelection: CurrentScreenSelection?
    private var pendingScreenCommit: (epochGeneration: UInt64, snapshotGeneration: UInt64, previousRevision: UInt32, revision: UInt32, mode: ScreenMode)?
    private var widgetSequence: UInt32 = 0
    private var pendingLiveDashboardCommit = false
    private var dashboardRotationElapsedSeconds = 0
    private var dashboardStockElapsedSeconds = 0
    private var dashboardStockOffset = 0
    private var activeDashboardPetAsset: UploadedAssetSummary?

    init(
        monitor: any USBDeviceMonitoring,
        sessionFactory: any AppDeviceSessionFactory,
        mappingRepository: KeyMappingRepository,
        recordingFactory: any RecordingSinkCreating = ApplicationSupportRecordingSinkFactory(),
        actionPipeline: (any AppActionRouting)? = nil,
        monotonicClock: any AppMonotonicClock = SystemAppMonotonicClock(),
        dashboardProvider: any LiveDashboardProviding = EmptyLiveDashboardProvider(),
        petCatalog: any PetCatalogProviding = EmptyPetCatalog()
    ) {
        self.monitor = monitor
        self.sessionFactory = sessionFactory
        self.mappingRepository = mappingRepository
        self.recordingFactory = recordingFactory
        self.actionPipeline = actionPipeline
        self.monotonicClock = monotonicClock
        self.dashboardProvider = dashboardProvider
        self.petCatalog = petCatalog
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
            actionPipeline: pipeline,
            dashboardProvider: ProductionLiveDashboardProvider(),
            petCatalog: ProductionPetCatalog()
        )
        let defaults = UserDefaults.standard
        stockSymbols = defaults.string(forKey: "dashboard.stockSymbols") ?? "sh000001"
        if let stored = defaults.string(forKey: "dashboard.modules") {
            let modules = stored.split(separator: ",")
                .compactMap { DashboardModule(rawValue: String($0)) }
            dashboardModules = Self.normalizedDashboardModules(modules)
        } else {
            let fallback: [DashboardModule] = [.codex, .claude, .system, .stocks]
            let legacy = (0..<4).map {
                defaults.string(forKey: "dashboard.module.\($0)") ?? fallback[$0].rawValue
            }
            dashboardModules = legacy.enumerated().map {
                DashboardModule(rawValue: $0.element) ?? fallback[$0.offset]
            }
        }
        let storedDuration = defaults.integer(
            forKey: "dashboard.pageDurationSeconds"
        )
        if [4, 6, 8, 10, 12].contains(storedDuration) {
            dashboardPageDurationSeconds = storedDuration
        }
        Task { [weak self, adapter] in
            await adapter.setScreenHandler { [weak self] mode in
                self?.screenMode = mode == .image ? .image : .dashboard
                self?.selectedPage = .screen
            }
            await adapter.setDashboardHandlers(
                nextPage: { [weak self] in self?.nextDashboardPage() },
                nextStocks: { [weak self] in self?.nextDashboardStockPage() }
            )
        }
    }

    deinit {
        monitorTask?.cancel()
        sessionTask?.cancel()
        operationTask?.cancel()
        dashboardTask?.cancel()
        petCatalogTask?.cancel()
    }

    func start() {
        guard monitorTask == nil else { return }
        startDashboardSampling()
        monitorTask = Task { [weak self, monitor] in
            guard let self else { return }
            do {
                self.keyProfile = try await self.mappingRepository.load()
                try await self.actionPipeline?.updateProfile(self.keyProfile)
            } catch {
                self.diagnosticMessage = "Key mapping load failed: \(error)"
            }
            let localPets = await self.petCatalog.localPets()
            self.pets = localPets
            self.selectedPetID = self.selectedPetID ?? localPets.first?.id
            self.petCatalogStatus = localPets.isEmpty
                ? "No local Codex pets found"
                : "\(localPets.count) local Codex pets"
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
        guard operationTask == nil else { return }
        guard assetsCapability.isAvailable, let session, let assets = availableAssets else {
            upload = .failed("Asset upload is unavailable")
            return
        }
        guard canUploadAssets else {
            upload = .failed(assetStorageLabel)
            return
        }
        operationTask = Task { [weak self] in
            guard let self else { return }
            defer { self.operationTask = nil }
            do {
                self.upload = .validating
                let accessed = url.startAccessingSecurityScopedResource()
                defer {
                    if accessed { url.stopAccessingSecurityScopedResource() }
                }
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
                    width: pet ? 119 : 428,
                    height: pet ? 129 : 142,
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
                self.activeTransferID = nil
                self.lastUploadedAsset = UploadedAssetSummary(sha256: prepared.sha256, totalBytes: result.totalBytes, kind: prepared.kind)
                self.uploadedPetID = nil
                self.previewPixels = try VKA1Codec.decode(container, limits: limits).frames.first?.pixels
                self.previewLayout = nil
                self.screenMode = pet ? .dashboard : .image
                self.upload = .active(prepared.sha256)
            } catch is CancellationError {
                await self.abortActiveUpload(using: session)
                self.upload = .cancelled
            } catch {
                await self.abortActiveUpload(using: session)
                self.upload = .failed(String(describing: error))
            }
        }
    }

    func cancelUpload() {
        guard operationTask != nil else {
            upload = .cancelled
            return
        }
        operationTask?.cancel()
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
        pendingLiveDashboardCommit = false
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
                self?.pendingLiveDashboardCommit = false
                self?.diagnosticMessage = "Screen commit failed: \(error)"
            }
        }
    }

    func activateLayout(mode: ScreenLayout.Mode) {
        pendingLiveDashboardCommit = false
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

    func activateLiveDashboard() {
        guard operationTask == nil, let screen = availableScreen,
              let font = screen.fonts.first, let previousRevision = currentScreenRevision else {
            diagnosticMessage = "A current screen capability and reviewed font are required"
            return
        }
        dashboardModules = Self.normalizedDashboardModules(dashboardModules)
        let petAsset: UploadedAssetSummary?
        if dashboardModules.contains(.pet) {
            guard let asset = lastUploadedAsset, asset.kind == .animation else {
                diagnosticMessage = "Upload a pet before installing this dashboard"
                return
            }
            petAsset = asset
        } else {
            petAsset = nil
        }
        let revision = previousRevision &+ 1
        guard revision != 0 else {
            diagnosticMessage = "Revision wrapped to zero"
            return
        }
        let fontReference = ScreenFontReference(id: font.id, version: font.version)
        dashboardPageIndex = 0
        dashboardStockOffset = 0
        dashboardRotationElapsedSeconds = 0
        dashboardStockElapsedSeconds = 0
        let layout = makeLiveDashboardLayout(
            font: fontReference,
            revision: revision,
            page: dashboardPageContent(snapshot: dashboardSnapshot),
            petAsset: petAsset
        )
        previewLayout = layout
        previewPixels = nil
        screenMode = .dashboard
        activeDashboardPetAsset = petAsset
        pendingLiveDashboardCommit = true
        let assets = petAsset.map {
            [ScreenAssetReference(bytes: $0.totalBytes, kind: $0.kind, sha256: $0.sha256)]
        } ?? []
        commit(payload: .dashboard(layout), assets: assets)
    }

    func stopLiveDashboard() {
        liveDashboardEnabled = false
    }

    func saveDashboardSettings() {
        let normalized = StockReader.normalizedList(stockSymbols)
        if !stockSymbols.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
           normalized.isEmpty {
            diagnosticMessage = "Use stock symbols such as sh000001, hk00700, or usAAPL"
            return
        }
        stockSymbols = normalized.joined(separator: ",")
        UserDefaults.standard.set(stockSymbols, forKey: "dashboard.stockSymbols")
        dashboardModules = Self.normalizedDashboardModules(dashboardModules)
        UserDefaults.standard.set(
            dashboardModules.map(\.rawValue).joined(separator: ","),
            forKey: "dashboard.modules"
        )
        UserDefaults.standard.set(
            dashboardPageDurationSeconds,
            forKey: "dashboard.pageDurationSeconds"
        )
    }

    func setDashboardModule(_ module: DashboardModule, at index: Int) {
        guard dashboardModules.indices.contains(index) else { return }
        var modules = Self.normalizedDashboardModules(dashboardModules)
        modules[index] = module
        dashboardModules = modules
    }

    func addDashboardPage() {
        dashboardModules = Self.normalizedDashboardModules(
            dashboardModules + [.network, .stocks]
        )
    }

    func removeDashboardPage(at pageIndex: Int) {
        let modules = Self.normalizedDashboardModules(dashboardModules)
        guard modules.count > 2, (0..<(modules.count / 2)).contains(pageIndex) else {
            return
        }
        var updated = modules
        updated.removeSubrange((pageIndex * 2)..<(pageIndex * 2 + 2))
        dashboardModules = updated
        dashboardPageIndex %= updated.count / 2
    }

    var dashboardPages: [DashboardPageContent] {
        let modules = Self.normalizedDashboardModules(dashboardModules)
        return (0..<(modules.count / 2)).map {
            dashboardSnapshot.page(
                modules: modules,
                pageIndex: $0,
                stockOffset: dashboardStockOffset
            )
        }
    }

    func nextDashboardPage() {
        let pageCount = Self.normalizedDashboardModules(dashboardModules).count / 2
        dashboardPageIndex = (dashboardPageIndex + 1) % pageCount
        dashboardRotationElapsedSeconds = 0
        pushCurrentDashboardPage()
    }

    func nextDashboardStockPage() {
        dashboardStockOffset += 4
        dashboardStockElapsedSeconds = 0
        pushCurrentDashboardPage()
    }

    var filteredPets: [PetCatalogItem] {
        let query = petSearch.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !query.isEmpty else { return pets }
        return pets.filter {
            $0.displayName.localizedCaseInsensitiveContains(query) ||
                $0.slug.localizedCaseInsensitiveContains(query)
        }
    }

    var selectedPet: PetCatalogItem? {
        guard let selectedPetID else { return nil }
        return pets.first { $0.id == selectedPetID }
    }

    func refreshPetdex() {
        guard petCatalogTask == nil else { return }
        petCatalogStatus = "Loading Petdex"
        petCatalogTask = Task { [weak self] in
            guard let self else { return }
            defer { self.petCatalogTask = nil }
            do {
                let remote = try await self.petCatalog.petdexPets()
                let local = await self.petCatalog.localPets()
                self.pets = local + remote.sorted {
                    $0.displayName.localizedCaseInsensitiveCompare($1.displayName) == .orderedAscending
                }
                self.selectedPetID = self.selectedPetID ?? self.pets.first?.id
                self.petCatalogStatus = "\(local.count) local · \(remote.count) Petdex"
            } catch {
                self.petCatalogStatus = "Petdex failed · \(error)"
                self.diagnosticMessage = "Petdex load failed: \(error)"
            }
        }
    }

    /// Asynchronously decodes the idle animation of the given pet so the picker
    /// sheet can render a looping preview. Cheap to cancel and re-issue when a
    /// different pet is tapped.
    func loadPetPreview(for item: PetCatalogItem?) {
        petPreviewTask?.cancel()
        petPreviewFrames = []
        petPreviewFrameDurationsMS = []
        petPreviewFailed = false
        petPreviewItemID = item?.id
        guard let item else {
            petPreviewLoading = false
            return
        }
        petPreviewLoading = true
        petPreviewTask = Task { [weak self] in
            guard let self else { return }
            do {
                let decoded = try await self.petCatalog.animation(
                    for: item,
                    choice: .idle,
                    minimumFrameMS: 1,
                    maximumFrameMS: 2_000
                )
                if Task.isCancelled { return }
                let images = decoded.frames.compactMap { Self.cgImage(from: $0.raster) }
                guard images.count == decoded.frames.count, !Task.isCancelled else {
                    self.petPreviewFailed = true
                    self.petPreviewLoading = false
                    return
                }
                self.petPreviewFrames = images
                self.petPreviewFrameDurationsMS = decoded.frames.map { Int($0.durationMS) }
                self.petPreviewLoading = false
            } catch is CancellationError {
                // Superseded by a newer selection — leave state to the new task.
            } catch {
                self.petPreviewFailed = true
                self.petPreviewLoading = false
            }
        }
    }

    private static func cgImage(from raster: AssetRaster) -> CGImage? {
        var rgba = Data(capacity: raster.pixels.count * 4)
        for pixel in raster.pixels {
            rgba.append(pixel.red)
            rgba.append(pixel.green)
            rgba.append(pixel.blue)
            rgba.append(pixel.alpha)
        }
        guard let provider = CGDataProvider(data: rgba as CFData) else { return nil }
        return CGImage(
            width: raster.width,
            height: raster.height,
            bitsPerComponent: 8,
            bitsPerPixel: 32,
            bytesPerRow: raster.width * 4,
            space: CGColorSpace(name: CGColorSpace.sRGB)!,
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
            provider: provider,
            decode: nil,
            shouldInterpolate: false,
            intent: .defaultIntent
        )
    }

    func downloadSelectedPet() {
        guard operationTask == nil else { return }
        guard let item = selectedPet else {
            diagnosticMessage = "Select a pet first"
            return
        }
        guard assetsCapability.isAvailable, let session, let assets = availableAssets else {
            upload = .failed("Asset upload is unavailable")
            return
        }
        guard canUploadAssets else {
            upload = .failed(assetStorageLabel)
            return
        }
        let choice = petAnimationChoice
        operationTask = Task { [weak self] in
            guard let self else { return }
            defer { self.operationTask = nil }
            do {
                self.upload = .validating
                self.petCatalogStatus = "Preparing \(item.displayName)"
                let decoded = try await self.petCatalog.animation(
                    for: item,
                    choice: choice,
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
                    width: 119,
                    height: 129,
                    limits: limits
                )
                let prepared = try PreparedAsset(data: container, limits: limits)
                guard prepared.kind == .animation else {
                    throw AssetServiceError.invalidAsset
                }
                let transferID = Self.nonzeroTransferID()
                self.activeTransferID = transferID
                self.upload = .sending
                let result = try await session.upload(prepared, transferID: transferID)
                self.uploadProgress = Double(result.nextOffset) / Double(result.totalBytes)
                self.activeTransferID = nil
                self.lastUploadedAsset = UploadedAssetSummary(
                    sha256: prepared.sha256,
                    totalBytes: result.totalBytes,
                    kind: prepared.kind
                )
                self.uploadedPetID = item.id
                self.previewPixels = try decoded.frames.first.map {
                    try AssetPixelConverter.convert(
                        $0.raster,
                        width: 428,
                        height: 142,
                        fit: .contain,
                        background: AssetRGB888(red: 0, green: 0, blue: 0)
                    )
                }
                self.previewLayout = nil
                self.screenMode = .dashboard
                self.upload = .active(prepared.sha256)
                self.petCatalogStatus = "\(item.displayName) uploaded · install the dashboard"
            } catch is CancellationError {
                await self.abortActiveUpload(using: session)
                self.upload = .cancelled
                self.petCatalogStatus = "Pet upload cancelled"
            } catch {
                await self.abortActiveUpload(using: session)
                self.upload = .failed(String(describing: error))
                self.petCatalogStatus = "Pet upload failed · \(error)"
                self.diagnosticMessage = "Pet upload failed: \(error)"
            }
        }
    }

    private func startDashboardSampling() {
        guard dashboardTask == nil else { return }
        dashboardTask = Task { [weak self] in
            guard let self else { return }
            while !Task.isCancelled {
                let snapshot = await self.dashboardProvider.snapshot(
                    stockSymbols: self.stockSymbols
                )
                self.dashboardSnapshot = snapshot
                if self.liveDashboardEnabled {
                    self.dashboardRotationElapsedSeconds += 2
                    self.dashboardStockElapsedSeconds += 2
                    if self.dashboardRotationElapsedSeconds >= self.dashboardPageDurationSeconds {
                        let pageCount = Self.normalizedDashboardModules(
                            self.dashboardModules
                        ).count / 2
                        self.dashboardPageIndex = (self.dashboardPageIndex + 1) % pageCount
                        self.dashboardRotationElapsedSeconds = 0
                    }
                    if self.dashboardStockElapsedSeconds >= 4 {
                        self.dashboardStockOffset += 3
                        self.dashboardStockElapsedSeconds = 0
                    }
                    let page = self.dashboardPageContent(snapshot: snapshot)
                    await self.pushLiveDashboard(page)
                }
                do {
                    try await Task.sleep(for: .seconds(2))
                } catch {
                    return
                }
            }
        }
    }

    private func pushLiveDashboard(_ page: DashboardPageContent) async {
        guard liveDashboardEnabled, let session, let revision = currentScreenRevision,
              isCurrentScreenConfigured, currentScreenSelection?.mode == .dashboard else {
            return
        }
        var values: [(String, WidgetUpdateState)] = []
        let tiles = [("left", page.left), ("right", page.right)]
        for (side, tile) in tiles {
            values.append((
                "\(side)-content",
                tile.module == .pet ? .stale : .freshText(tile.screenText)
            ))
            if tile.module == .system {
                values.append((
                    "\(side)-cpu",
                    .freshNumber(.init(coefficient: Int64(dashboardSnapshot.cpuPercent), scale: 0))
                ))
                values.append((
                    "\(side)-memory",
                    .freshNumber(.init(coefficient: Int64(dashboardSnapshot.memoryPercent), scale: 0))
                ))
            } else {
                values.append(("\(side)-cpu", .stale))
                values.append(("\(side)-memory", .stale))
            }
        }
        if let font = availableScreen?.fonts.first {
            previewLayout = makeLiveDashboardLayout(
                font: .init(id: font.id, version: font.version),
                revision: revision,
                page: page,
                petAsset: activeDashboardPetAsset
            )
        }
        do {
            for (widgetID, state) in values {
                let next = widgetSequence &+ 1
                guard next != 0 else {
                    liveDashboardEnabled = false
                    diagnosticMessage = "Widget sequence exhausted"
                    return
                }
                widgetSequence = next
                try await session.sendWidgetUpdate(.init(
                    revision: revision,
                    widgetID: widgetID,
                    sequence: next,
                    state: state
                ))
            }
        } catch {
            liveDashboardEnabled = false
            diagnosticMessage = "Live dashboard paused: \(error)"
        }
    }

    private func dashboardPageContent(
        snapshot: LiveDashboardSnapshot
    ) -> DashboardPageContent {
        return snapshot.page(
            modules: dashboardModules,
            pageIndex: dashboardPageIndex,
            stockOffset: dashboardStockOffset
        )
    }

    private func pushCurrentDashboardPage() {
        guard liveDashboardEnabled else { return }
        let page = dashboardPageContent(snapshot: dashboardSnapshot)
        Task { [weak self] in
            await self?.pushLiveDashboard(page)
        }
    }

    private func makeLiveDashboardLayout(
        font: ScreenFontReference,
        revision: UInt32,
        page: DashboardPageContent,
        petAsset: UploadedAssetSummary?
    ) -> ScreenLayout {
        let tiles: [(
            side: String,
            x: Int16,
            titleColor: UInt32,
            content: DashboardTileContent
        )] = [
            ("left", 8, 0x6ED0FF, page.left),
            ("right", 222, 0xFFD166, page.right),
        ]
        var objects: [ScreenRootObject] = []
        var widgets: [ScreenWidgetDeclaration] = []
        for (tileIndex, tile) in tiles.enumerated() {
            let contentWidgetID = "\(tile.side)-content"
            let contentObjectID = "\(contentWidgetID)-value"
            objects.append(.init(
                x: tile.x,
                y: 9,
                node: .dynamicLabel(
                    base: .init(
                        id: contentObjectID,
                        width: 198,
                        height: 124,
                        z: Int16(tileIndex * 4),
                        clip: true,
                        visible: tile.content.module != .pet
                    ),
                    align: .left,
                    colorRGB888: tile.titleColor,
                    font: font,
                    widgetID: contentWidgetID
                )
            ))
            widgets.append(.text(
                id: contentWidgetID,
                target: contentObjectID,
                fallback: tile.content.screenText
            ))

            let gauges: [(name: String, x: Int16, value: Int)] = [
                ("cpu", tile.x + 13, dashboardSnapshot.cpuPercent),
                ("memory", tile.x + 105, dashboardSnapshot.memoryPercent),
            ]
            for (gaugeIndex, gauge) in gauges.enumerated() {
                let widgetID = "\(tile.side)-\(gauge.name)"
                let objectID = "\(widgetID)-gauge"
                objects.append(.init(
                    x: gauge.x,
                    y: 42,
                    node: .progress(
                        base: .init(
                            id: objectID,
                            width: 80,
                            height: 80,
                            z: Int16(tileIndex * 4 + gaugeIndex + 1),
                            clip: true,
                            visible: tile.content.module == .system
                        ),
                        backgroundRGB888: 0x26343F,
                        fillRGB888: Self.dashboardGaugeColor(gauge.value),
                        widgetID: widgetID
                    )
                ))
                widgets.append(.progress(
                    id: widgetID,
                    target: objectID,
                    fallback: .init(coefficient: Int64(gauge.value), scale: 0),
                    min: .init(coefficient: 0, scale: 0),
                    max: .init(coefficient: 100, scale: 0),
                    decimals: 0
                ))
            }

            let sideOffset = tile.side == "left" ? 0 : 1
            let sideUsesPet = stride(
                from: sideOffset,
                to: dashboardModules.count,
                by: 2
            ).contains { dashboardModules[$0] == .pet }
            if sideUsesPet, let petAsset {
                let manifest = ScreenPetManifest(
                    id: "\(tile.side)-pet",
                    states: [.idle: .asset(sha256: petAsset.sha256)]
                )
                objects.append(.init(
                    x: tile.x,
                    y: 6,
                    node: .pet(
                        base: .init(
                            id: "\(tile.side)-pet-view",
                            width: 198,
                            height: 129,
                            z: Int16(tileIndex * 4 + 3),
                            clip: true,
                            visible: tile.content.module == .pet
                        ),
                        backgroundRGB888: 0x081018,
                        fit: .contain,
                        manifest: manifest
                    )
                ))
            }
        }
        return ScreenLayout(
            backgroundRGB888: 0x081018,
            mode: .dashboard,
            revision: revision,
            objects: objects,
            widgets: widgets
        )
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
                self?.pendingLiveDashboardCommit = false
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

    func testAction(_ action: HostAction) {
        guard let actionPipeline else {
            lastActionResult = "Failed — action routing is unavailable"
            return
        }
        Task { [weak self] in
            do {
                let result = try await actionPipeline.execute(action)
                self?.lastActionResult = "Test — \(Self.actionResultLabel(result))"
            } catch {
                let message = Self.actionFailureLabel(error)
                self?.lastActionResult = "Failed — \(message)"
                self?.diagnosticMessage = "Action test failed: \(message)"
            }
        }
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

    var configuredVoiceKey: CanonicalKey? {
        let voiceKey = Self.deviceVoiceKey(for: keyProfile)
        return CanonicalKey(rawValue: voiceKey.rawValue)
    }

    func setVoiceKey(_ key: CanonicalKey?) {
        var mappings = keyProfile.mappings
        for candidate in CanonicalKey.allCases {
            guard var bindings = mappings[candidate] else { continue }
            if bindings.single == .voiceInput { bindings.single = .none }
            if bindings.double == .voiceInput { bindings.double = .none }
            if bindings.long == .voiceInput { bindings.long = .none }
            mappings[candidate] = bindings
        }
        if let key, var bindings = mappings[key] {
            bindings.single = .voiceInput
            mappings[key] = bindings
        }
        do {
            keyProfile = try KeyMappingProfile(mappings: mappings)
            let profile = keyProfile
            Task { [weak self] in
                do { try await self?.actionPipeline?.updateProfile(profile) }
                catch { self?.diagnosticMessage = "Action routing update failed: \(error)" }
            }
            saveMappings()
        } catch {
            diagnosticMessage = "Invalid mapping: \(error)"
        }
    }

    func refreshInputPermission() {
        inputPermissionGranted = AXIsProcessTrusted()
    }

    func requestInputPermission() {
        let options = ["AXTrustedCheckOptionPrompt": true] as CFDictionary
        inputPermissionGranted = AXIsProcessTrustedWithOptions(options)
        guard !inputPermissionGranted,
              let settings = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility")
        else { return }
        NSWorkspace.shared.open(settings)
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
    var canUploadAssets: Bool {
        guard assetsCapability.isAvailable, let assets = availableAssets else { return false }
        return assets.storageState == .ready && assets.uploadMaxBytes > 0
    }
    var assetStorageLabel: String {
        guard let assets = availableAssets else { return assetsCapability.label }
        switch assets.storageState {
        case .ready where assets.uploadMaxBytes > 0:
            return "Ready — \(assets.uploadMaxBytes) bytes available per upload"
        case .ready:
            return "Read-only — no upload capacity"
        case .busy:
            return "Busy — reconnect the device after a USB power cycle"
        case .unformatted:
            return "Unformatted"
        case .corrupt:
            return "Corrupt"
        case .mountFailed:
            return "Mount failed"
        }
    }
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

    private static func normalizedDashboardModules(
        _ modules: [DashboardModule]
    ) -> [DashboardModule] {
        LiveDashboardSnapshot.normalizedModules(modules)
    }

    private static func dashboardGaugeColor(_ percent: Int) -> UInt32 {
        let colors: [UInt32] = [
            0x32D74B, 0x73C944, 0xA8C83A, 0xD7B83B,
            0xF39A3D, 0xFF6B3D, 0xFF453A,
        ]
        return colors[min(max(percent, 0) / 15, colors.count - 1)]
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
                let failure = String(describing: error)
                diagnosticMessage = "USB session failed: \(failure)"
                connection = .failed(failure)
                if let session {
                    Task { [weak self] in
                        let diagnostics = await session.diagnostics()
                        guard let detail = diagnostics.entries.last(where: {
                            $0.hasPrefix("replacement decode failed")
                        }) else { return }
                        self?.diagnosticMessage = "USB session failed: \(failure) · \(detail)"
                    }
                }
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
            if case .error(let error) = event {
                diagnosticMessage = "\(error.operation): \(error.code)"
                if error.operation == "input",
                   ["audio_start_failed", "audio_stop_failed", "audio_runtime_failed", "input_queue_overflow", "tainted"].contains(error.code) {
                    audioRecorder?.cancel()
                    audioRecorder = nil
                    recordingSink = nil
                    recordingDestination = nil
                    audioState = .failed(.output("Device audio: \(error.code)"))
                }
            }
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
                if let result = results.last { self.lastActionResult = Self.actionResultLabel(result) }
            } catch {
                let message = Self.actionFailureLabel(error)
                self.lastActionResult = "Failed — \(message)"
                self.diagnosticMessage = "Key action failed: \(message)"
            }
        }
    }

    private func abortActiveUpload(using session: any AppDeviceSession) async {
        guard let transferID = activeTransferID else { return }
        _ = await Task.detached {
            try? await session.cancelUpload(transferID: transferID)
        }.value
        activeTransferID = nil
    }

    private static func actionResultLabel(_ result: ActionExecutionResult) -> String {
        switch result {
        case .noAction:
            return "No action"
        case .completed:
            return "Completed"
        case .command(let command):
            return "Command exited with status \(command.exitStatus)"
        }
    }

    private static func actionFailureLabel(_ error: Error) -> String {
        guard let error = error as? ActionRoutingError else { return String(describing: error) }
        switch error {
        case .missingBinding(let key):
            return "No binding for \(key.rawValue)"
        case .permissionDenied:
            return "Accessibility permission is required"
        case .actionFailed(let message), .commandLaunchFailed(let message):
            return message
        case .commandTimedOut(let milliseconds):
            return "Command timed out after \(milliseconds) ms"
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
        liveDashboardEnabled = false
        pendingLiveDashboardCommit = false
        activeDashboardPetAsset = nil
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
            if mode != .dashboard {
                liveDashboardEnabled = false
            }
            pendingLiveDashboardCommit = false
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
            liveDashboardEnabled = pendingLiveDashboardCommit && pending.mode == .dashboard
            pendingLiveDashboardCommit = false
            if liveDashboardEnabled {
                let page = dashboardPageContent(snapshot: dashboardSnapshot)
                Task { [weak self] in
                    await self?.pushLiveDashboard(page)
                }
            }
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
