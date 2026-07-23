import Foundation

public enum ConnectedDiagnosticError: Error, Equatable, Sendable {
    case invalidButtonEvent(String)
    case unexpectedAction(String)
    case recordingTimedOut
    case recordingFailed(String)
    case screenTimedOut
}

public struct KeyDiagnosticRawEvent: Equatable, Sendable {
    public let event: String
    public let key: CanonicalKey
    public let durationMilliseconds: UInt32?
    public let sessionID: UInt32?
    public let timestampMilliseconds: UInt64

    public init(
        event: String,
        key: CanonicalKey,
        durationMilliseconds: UInt32?,
        sessionID: UInt32?,
        timestampMilliseconds: UInt64
    ) {
        self.event = event
        self.key = key
        self.durationMilliseconds = durationMilliseconds
        self.sessionID = sessionID
        self.timestampMilliseconds = timestampMilliseconds
    }
}

public struct KeyDiagnosticSummary: Equatable, Sendable {
    public let rawEvents: [KeyDiagnosticRawEvent]
    public let gestureCounts: [CanonicalKey: [KeyGesture: Int]]
    public let inertMarkers: [CanonicalKey: Int]

    public init(
        rawEvents: [KeyDiagnosticRawEvent],
        gestureCounts: [CanonicalKey: [KeyGesture: Int]],
        inertMarkers: [CanonicalKey: Int]
    ) {
        self.rawEvents = rawEvents
        self.gestureCounts = gestureCounts
        self.inertMarkers = inertMarkers
    }
}

public actor InertDiagnosticActionAdapter: PermissionAuthorizing, InputInjecting,
    ApplicationControlling, VoiceInputControlling, ScreenControlling, CommandExecuting {
    private var screenModeCounts: [ScreenMode: Int] = [:]

    public init() {}

    public func require(_ permission: InputPermission) async throws {
        throw ConnectedDiagnosticError.unexpectedAction("permission")
    }

    public func sendEnter() async throws { throw ConnectedDiagnosticError.unexpectedAction("sendEnter") }
    public func copySelection() async throws { throw ConnectedDiagnosticError.unexpectedAction("copySelection") }
    public func interruptControlC() async throws { throw ConnectedDiagnosticError.unexpectedAction("interruptControlC") }
    public func pasteText(_ text: String) async throws { throw ConnectedDiagnosticError.unexpectedAction("pasteText") }
    public func sendShortcut(_ shortcut: KeyboardShortcut) async throws { throw ConnectedDiagnosticError.unexpectedAction("shortcut") }
    public func wakeApplication() async throws { throw ConnectedDiagnosticError.unexpectedAction("wakeApplication") }
    public func launchApplication(bundleIdentifier: String) async throws { throw ConnectedDiagnosticError.unexpectedAction("launchApplication") }
    public func toggleVoiceInput() async throws { throw ConnectedDiagnosticError.unexpectedAction("voiceInput") }
    public func interactWithPet(_ interaction: String) async throws { throw ConnectedDiagnosticError.unexpectedAction("petInteraction") }
    public func execute(_ command: CommandSpecification) async throws -> CommandResult {
        throw ConnectedDiagnosticError.unexpectedAction("command")
    }

    public func activate(mode: ScreenMode) async throws {
        screenModeCounts[mode, default: 0] += 1
    }

    public func markerCounts() -> [CanonicalKey: Int] {
        [
            .k1: screenModeCounts[.image, default: 0],
            .k2: screenModeCounts[.pet, default: 0],
            .k3: screenModeCounts[.dashboard, default: 0],
            .k4: screenModeCounts[.custom, default: 0],
        ]
    }
}

public actor KeyDiagnosticWorkflow {
    private var gestureRouter: GestureRouter
    private let actionRouter: KeyActionRouter
    private let actions: InertDiagnosticActionAdapter
    private var rawEvents: [KeyDiagnosticRawEvent] = []
    private var gestureCounts: [CanonicalKey: [KeyGesture: Int]] = [:]

    public init(
        doubleClickWindowMilliseconds: UInt32 = 300,
        longPressThresholdMilliseconds: UInt32 = 1_500
    ) throws {
        let policy = try GesturePolicy(
            doubleClickWindowMilliseconds: doubleClickWindowMilliseconds,
            longPressThresholdMilliseconds: longPressThresholdMilliseconds,
            derivesDoubleClick: false,
            derivesLongPress: true
        )
        gestureRouter = GestureRouter(policy: policy)
        let adapter = InertDiagnosticActionAdapter()
        actions = adapter
        let profile = try KeyMappingProfile(mappings: [
            .k1: KeyBindings(single: .screenMode(.image)),
            .k2: KeyBindings(single: .screenMode(.pet)),
            .k3: KeyBindings(single: .screenMode(.dashboard)),
            .k4: KeyBindings(single: .screenMode(.custom)),
        ])
        actionRouter = try KeyActionRouter(
            profile: profile,
            permissions: adapter,
            input: adapter,
            applications: adapter,
            voice: adapter,
            screen: adapter,
            commands: adapter
        )
    }

    public func consume(_ state: StateEvent, at timestampMilliseconds: UInt64) async throws {
        guard state.event == "button_down" || state.event == "button_up" || state.event == "button_click" else {
            return
        }
        guard let rawButton = state.button else {
            throw ConnectedDiagnosticError.invalidButtonEvent("missing button")
        }
        let key: CanonicalKey
        do {
            key = try CanonicalKey(deviceValue: rawButton)
        } catch {
            throw ConnectedDiagnosticError.invalidButtonEvent(rawButton)
        }
        rawEvents.append(KeyDiagnosticRawEvent(
            event: state.event,
            key: key,
            durationMilliseconds: state.durationMS,
            sessionID: state.sessionID,
            timestampMilliseconds: timestampMilliseconds
        ))

        let deviceEvent: DeviceKeyEvent
        switch state.event {
        case "button_down": deviceEvent = .down(key)
        case "button_up": deviceEvent = .up(key, durationMilliseconds: state.durationMS)
        case "button_click": deviceEvent = .click(key, durationMilliseconds: state.durationMS)
        default: return
        }
        let routed = try gestureRouter.handle(deviceEvent, at: timestampMilliseconds)
        for gesture in routed {
            gestureCounts[gesture.key, default: [:]][gesture.gesture, default: 0] += 1
            _ = try await actionRouter.execute(gesture)
        }
    }

    public func disconnect(at timestampMilliseconds: UInt64) throws {
        _ = try gestureRouter.handle(.disconnect, at: timestampMilliseconds)
    }

    public func summary() async -> KeyDiagnosticSummary {
        KeyDiagnosticSummary(
            rawEvents: rawEvents,
            gestureCounts: gestureCounts,
            inertMarkers: await actions.markerCounts()
        )
    }
}

public actor AudioRecordingWorkflow {
    private let recording: AudioRecordingSession

    public init(outputURL: URL) throws {
        recording = AudioRecordingSession(sink: try AtomicFileOggPageSink(destinationURL: outputURL))
    }

    public func consume(_ frame: AudioFrame) throws {
        try recording.consume(frame)
    }

    public func state() -> AudioRecordingState { recording.state }

    public func cancel() { recording.cancel() }
}

public enum OpusHeadInspector {
    public static func originalInputRate(in data: Data) -> UInt32? {
        let signature = Data("OpusHead".utf8)
        guard let range = data.range(of: signature), range.lowerBound + 16 <= data.endIndex else {
            return nil
        }
        let offset = range.lowerBound + 12
        return UInt32(data[offset])
            | UInt32(data[offset + 1]) << 8
            | UInt32(data[offset + 2]) << 16
            | UInt32(data[offset + 3]) << 24
    }
}
