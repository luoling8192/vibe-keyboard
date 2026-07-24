import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Input types")
struct InputTypesTests {
    @Test func canonicalKeyNormalization() throws {
        #expect(try CanonicalKey(deviceValue: "primary") == .k4)
        #expect(try CanonicalKey(deviceValue: "secondary") == .k1)
        #expect(try CanonicalKey(deviceValue: "k2") == .k2)
        #expect(throws: InputConfigurationError.unknownKey("left")) {
            try CanonicalKey(deviceValue: "left")
        }
    }

    @Test func vendorDefaultIsComplete() {
        let profile = KeyMappingProfile.vendorDefault()
        #expect(profile.mappings[.k1]?.single == .wakeApplication)
        #expect(profile.mappings[.k2]?.single == .pasteText("继续"))
        #expect(profile.mappings[.k3]?.single == .interruptControlC)
        #expect(profile.mappings[.k4]?.single == .voiceInput)
        #expect(profile.mappings[.k4]?.double == .sendEnter)
        #expect(profile.mappings.values.allSatisfy { $0.long == .none })
    }

    @Test func gestureSubscriptMutatesEveryBinding() {
        var bindings = KeyBindings()
        bindings[.single] = .sendEnter
        bindings[.double] = .systemCopy
        bindings[.long] = .interruptControlC

        #expect(bindings.single == .sendEnter)
        #expect(bindings.double == .systemCopy)
        #expect(bindings.long == .interruptControlC)
    }

    @Test func profileRejectsMissingKeyAndUnsupportedVersion() {
        #expect(throws: InputConfigurationError.incompleteMapping(missing: [.k4])) {
            try KeyMappingProfile(mappings: [
                .k1: KeyBindings(), .k2: KeyBindings(), .k3: KeyBindings(),
            ])
        }
        #expect(throws: InputConfigurationError.unsupportedSchemaVersion(2)) {
            try KeyMappingProfile(schemaVersion: 2, mappings: Dictionary(uniqueKeysWithValues: CanonicalKey.allCases.map { ($0, KeyBindings()) }))
        }
    }

    @Test func actionRoundTripsEveryVariant() throws {
        let actions: [HostAction] = [
            .none, .voiceInput, .sendEnter, .systemCopy, .interruptControlC,
            .wakeApplication, .pasteText("hello"),
            .customShortcut(try KeyboardShortcut(modifiers: [.command, .function, .shift], key: "K")),
            .customCommand(try CommandSpecification(executable: "/usr/bin/true", timeoutMilliseconds: 1000)),
            .launchApplication(bundleIdentifier: "com.apple.TextEdit"),
            .screenMode(.pet), .dashboardNextPage, .dashboardNextStocks,
            .petInteraction("wave"),
        ]
        let encoder = JSONEncoder()
        let decoder = JSONDecoder()
        for action in actions {
            #expect(try decoder.decode(HostAction.self, from: encoder.encode(action)) == action)
        }
    }

    @Test func actionValidationRejectsMissingValues() throws {
        #expect(throws: InputConfigurationError.invalidShortcutKey("two keys")) {
            try KeyboardShortcut(modifiers: [.command], key: "two keys")
        }
        #expect(throws: InputConfigurationError.commandExecutableMustBeAbsolute("true")) {
            try CommandSpecification(executable: "true", timeoutMilliseconds: 100)
        }
        #expect(throws: InputConfigurationError.invalidCommandTimeout(0)) {
            try CommandSpecification(executable: "/usr/bin/true", timeoutMilliseconds: 0)
        }
        let invalidPaste = Data(#"{"type":"paste_text","text":"  "}"#.utf8)
        #expect(throws: (any Error).self) { try JSONDecoder().decode(HostAction.self, from: invalidPaste) }
        let unknown = Data(#"{"type":"do_everything"}"#.utf8)
        #expect(throws: (any Error).self) { try JSONDecoder().decode(HostAction.self, from: unknown) }
    }
}
