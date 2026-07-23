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

    func setScreenHandler(_ handler: @escaping @MainActor @Sendable (ScreenMode) -> Void) {
        screenHandler = handler
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
        guard let scalar = shortcut.key.lowercased().unicodeScalars.first,
              shortcut.key.unicodeScalars.count == 1,
              let code = Self.keyCodes[scalar] else {
            throw ProductionActionError.unsupported("shortcut key \(shortcut.key)")
        }
        var flags: CGEventFlags = []
        if shortcut.modifiers.contains(.command) { flags.insert(.maskCommand) }
        if shortcut.modifiers.contains(.control) { flags.insert(.maskControl) }
        if shortcut.modifiers.contains(.option) { flags.insert(.maskAlternate) }
        if shortcut.modifiers.contains(.shift) { flags.insert(.maskShift) }
        try postKey(code: code, flags: flags)
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

    func toggleVoiceInput() async throws {}

    func activate(mode: ScreenMode) async throws {
        guard let screenHandler else { throw ProductionActionError.unsupported("screen mode") }
        await screenHandler(mode)
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

    private static let keyCodes: [UnicodeScalar: CGKeyCode] = [
        "a": 0, "s": 1, "d": 2, "f": 3, "h": 4, "g": 5, "z": 6, "x": 7,
        "c": 8, "v": 9, "b": 11, "q": 12, "w": 13, "e": 14, "r": 15,
        "y": 16, "t": 17, "1": 18, "2": 19, "3": 20, "4": 21, "6": 22,
        "5": 23, "=": 24, "9": 25, "7": 26, "-": 27, "8": 28, "0": 29,
        "]": 30, "o": 31, "u": 32, "[": 33, "i": 34, "p": 35, "l": 37,
        "j": 38, "'": 39, "k": 40, ";": 41, "\\": 42, ",": 43, "/": 44,
        "n": 45, "m": 46, ".": 47, "`": 50,
    ]
}

protocol AppActionRouting: Sendable {
    func updateProfile(_ profile: KeyMappingProfile) async throws
    func consume(_ event: DeviceKeyEvent, at timestampMilliseconds: UInt64) async throws -> [ActionExecutionResult]
    func disconnect(at timestampMilliseconds: UInt64) async throws
}

actor AppGestureActionPipeline: AppActionRouting {
    private var gestures: GestureRouter
    private let actions: KeyActionRouter

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

    func disconnect(at timestampMilliseconds: UInt64) async throws {
        _ = try gestures.handle(.disconnect, at: timestampMilliseconds)
    }
}
