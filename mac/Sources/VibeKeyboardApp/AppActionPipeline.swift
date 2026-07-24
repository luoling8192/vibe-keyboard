import AppKit
import ApplicationServices
import Foundation
import VibeBoardKit

enum ProductionActionError: Error, CustomStringConvertible {
    case unsupported(String)
    case eventCreationFailed

    var description: String {
        switch self {
        case .unsupported(let action): "Unsupported action: \(action)"
        case .eventCreationFailed: "Unable to create keyboard event"
        }
    }
}

actor ProductionHostActionAdapter: PermissionAuthorizing, InputInjecting, ApplicationControlling, VoiceInputControlling, ScreenControlling {
    private var screenHandler: (@MainActor @Sendable (ScreenMode) -> Void)?
    private var dashboardPageHandler: (@MainActor @Sendable () -> Void)?
    private var dashboardStocksHandler: (@MainActor @Sendable () -> Void)?
    private var voiceHotkey: KeyboardShortcut?

    func setScreenHandler(_ handler: @escaping @MainActor @Sendable (ScreenMode) -> Void) {
        screenHandler = handler
    }

    func setDashboardHandlers(
        nextPage: @escaping @MainActor @Sendable () -> Void,
        nextStocks: @escaping @MainActor @Sendable () -> Void
    ) {
        dashboardPageHandler = nextPage
        dashboardStocksHandler = nextStocks
    }

    func require(_ permission: InputPermission) async throws {
        guard permission == .inputInjection else { return }
        let options = ["AXTrustedCheckOptionPrompt": true] as CFDictionary
        guard AXIsProcessTrustedWithOptions(options) else {
            throw ActionRoutingError.permissionDenied("Accessibility permission is required")
        }
    }

    func sendEnter() async throws { try postKey(code: 36, flags: []) }
    func copySelection() async throws { try postKey(code: 8, flags: .maskCommand) }
    func interruptControlC() async throws { try postKey(code: 8, flags: .maskControl) }

    func pasteText(_ text: String) async throws {
        let pasteboard = NSPasteboard.general
        pasteboard.clearContents()
        guard pasteboard.setString(text, forType: .string) else { throw ProductionActionError.unsupported("paste") }
        try postKey(code: 9, flags: .maskCommand)
    }

    func sendShortcut(_ shortcut: KeyboardShortcut) async throws {
        guard let code = Self.keyCode(forShortcutKey: shortcut.key) else {
            throw ProductionActionError.unsupported("shortcut key \(shortcut.key)")
        }
        try postKey(code: code, flags: Self.flags(for: shortcut.modifiers))
    }

    func wakeApplication() async throws {
        await MainActor.run { NSApp.activate(ignoringOtherApps: true) }
    }

    func launchApplication(bundleIdentifier: String) async throws {
        guard let url = NSWorkspace.shared.urlForApplication(withBundleIdentifier: bundleIdentifier) else {
            throw ProductionActionError.unsupported("application \(bundleIdentifier)")
        }
        _ = try await NSWorkspace.shared.openApplication(at: url, configuration: .init())
    }

    func configureVoiceHotkey(_ shortcut: KeyboardShortcut?) {
        voiceHotkey = shortcut
    }

    func toggleVoiceInput() async throws {
        guard let shortcut = voiceHotkey else { return }
        try await sendShortcut(shortcut)
    }

    /// Posts the configured hotkey. Called by AppModel from consumeAudio()
    /// when BlackHole audio starts/stops, so the dictation app and the
    /// BlackHole audio stream are synchronized.
    func postVoiceHotkey() async throws {
        guard let shortcut = voiceHotkey else { return }
        try await sendShortcut(shortcut)
    }

    func activate(mode: ScreenMode) async throws {
        guard let screenHandler else { throw ProductionActionError.unsupported("screen mode") }
        await screenHandler(mode)
    }

    func advanceDashboardPage() async throws {
        guard let dashboardPageHandler else {
            throw ProductionActionError.unsupported("dashboard page")
        }
        await dashboardPageHandler()
    }

    func advanceDashboardStocks() async throws {
        guard let dashboardStocksHandler else {
            throw ProductionActionError.unsupported("dashboard stocks")
        }
        await dashboardStocksHandler()
    }

    func interactWithPet(_ interaction: String) async throws {
        throw ProductionActionError.unsupported("pet interaction \(interaction)")
    }

    private func postKey(code: CGKeyCode, flags: CGEventFlags) throws {
        guard let down = CGEvent(keyboardEventSource: nil, virtualKey: code, keyDown: true),
              let up = CGEvent(keyboardEventSource: nil, virtualKey: code, keyDown: false) else {
            throw ProductionActionError.eventCreationFailed
        }
        down.flags = flags
        up.flags = flags
        down.post(tap: .cghidEventTap)
        up.post(tap: .cghidEventTap)
    }

    static func keyCode(forShortcutKey key: String) -> CGKeyCode? {
        let normalized = key.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        if normalized.unicodeScalars.count == 1, let scalar = normalized.unicodeScalars.first {
            return characterKeyCodes[scalar]
        }
        return namedKeyCodes[normalized]
    }

    static func flags(for modifiers: Set<KeyboardShortcut.Modifier>) -> CGEventFlags {
        var flags: CGEventFlags = []
        if modifiers.contains(.command) { flags.insert(.maskCommand) }
        if modifiers.contains(.control) { flags.insert(.maskControl) }
        if modifiers.contains(.function) { flags.insert(.maskSecondaryFn) }
        if modifiers.contains(.option) { flags.insert(.maskAlternate) }
        if modifiers.contains(.shift) { flags.insert(.maskShift) }
        return flags
    }

    private static let characterKeyCodes: [UnicodeScalar: CGKeyCode] = [
        "a": 0, "s": 1, "d": 2, "f": 3, "h": 4, "g": 5, "z": 6, "x": 7,
        "c": 8, "v": 9, "b": 11, "q": 12, "w": 13, "e": 14, "r": 15,
        "y": 16, "t": 17, "1": 18, "2": 19, "3": 20, "4": 21, "6": 22,
        "5": 23, "=": 24, "9": 25, "7": 26, "-": 27, "8": 28, "0": 29,
        "]": 30, "o": 31, "u": 32, "[": 33, "i": 34, "p": 35, "l": 37,
        "j": 38, "'": 39, "k": 40, ";": 41, "\\": 42, ",": 43, "/": 44,
        "n": 45, "m": 46, ".": 47, "`": 50,
    ]

    private static let namedKeyCodes: [String: CGKeyCode] = [
        "return": 36, "enter": 36, "tab": 48, "space": 49,
        "delete": 51, "backspace": 51, "escape": 53, "esc": 53, "fn": 63,
        "help": 114, "home": 115, "pageup": 116, "forwarddelete": 117,
        "end": 119, "pagedown": 121,
        "left": 123, "right": 124, "down": 125, "up": 126,
        "f1": 122, "f2": 120, "f3": 99, "f4": 118, "f5": 96, "f6": 97,
        "f7": 98, "f8": 100, "f9": 101, "f10": 109, "f11": 103, "f12": 111,
        "f13": 105, "f14": 107, "f15": 113, "f16": 106,
        "f17": 64, "f18": 79, "f19": 80, "f20": 90,
    ]
}

protocol AppActionRouting: Sendable {
    func updateProfile(_ profile: KeyMappingProfile) async throws
    func consume(_ event: DeviceKeyEvent, at timestampMilliseconds: UInt64) async throws -> [ActionExecutionResult]
    func execute(_ action: HostAction) async throws -> ActionExecutionResult
    func disconnect(at timestampMilliseconds: UInt64) async throws
    func configureVoiceHotkey(_ shortcut: KeyboardShortcut?) async
    func postVoiceHotkey() async throws
}

actor AppGestureActionPipeline: AppActionRouting {
    private var gestures: GestureRouter
    private let actions: KeyActionRouter
    private let adapter: ProductionHostActionAdapter

    init(profile: KeyMappingProfile, adapter: ProductionHostActionAdapter) throws {
        gestures = GestureRouter(policy: try GesturePolicy(
            doubleClickWindowMilliseconds: 300,
            longPressThresholdMilliseconds: 1_500,
            derivesDoubleClick: false,
            derivesLongPress: true
        ))
        actions = try KeyActionRouter(
            profile: profile,
            permissions: adapter,
            input: adapter,
            applications: adapter,
            voice: adapter,
            screen: adapter,
            commands: ProcessCommandExecutor()
        )
        self.adapter = adapter
    }

    func updateProfile(_ profile: KeyMappingProfile) async throws {
        try await actions.updateProfile(profile)
    }

    func consume(_ event: DeviceKeyEvent, at timestampMilliseconds: UInt64) async throws -> [ActionExecutionResult] {
        let routed = try gestures.handle(event, at: timestampMilliseconds)
        var results: [ActionExecutionResult] = []
        for gesture in routed { results.append(try await actions.execute(gesture)) }
        return results
    }

    func execute(_ action: HostAction) async throws -> ActionExecutionResult {
        try await actions.execute(action)
    }

    func disconnect(at timestampMilliseconds: UInt64) async throws {
        _ = try gestures.handle(.disconnect, at: timestampMilliseconds)
    }

    func postVoiceHotkey() async throws {
        try await adapter.postVoiceHotkey()
    }

    func configureVoiceHotkey(_ shortcut: KeyboardShortcut?) async {
        await adapter.configureVoiceHotkey(shortcut)
    }
}
