import Foundation

public enum CanonicalKey: String, Codable, CaseIterable, Sendable {
    case k1
    case k2
    case k3
    case k4

    public init(deviceValue: String) throws {
        switch deviceValue {
        case "k1", "secondary": self = .k1
        case "k2": self = .k2
        case "k3": self = .k3
        case "k4", "primary": self = .k4
        default: throw InputConfigurationError.unknownKey(deviceValue)
        }
    }
}

public enum KeyGesture: String, Codable, CaseIterable, Sendable {
    case single
    case double
    case long
}

public enum ScreenMode: String, Codable, Sendable {
    case image
    case pet
    case dashboard
    case custom
}

public struct KeyboardShortcut: Codable, Equatable, Sendable {
    public enum Modifier: String, Codable, CaseIterable, Comparable, Sendable {
        case command
        case control
        case function
        case option
        case shift

        public static func < (lhs: Self, rhs: Self) -> Bool {
            lhs.rawValue < rhs.rawValue
        }
    }

    public let modifiers: Set<Modifier>
    public let key: String

    public init(modifiers: Set<Modifier>, key: String) throws {
        let normalizedKey = key.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard !normalizedKey.isEmpty else {
            throw InputConfigurationError.missingAssociatedValue(action: "customShortcut")
        }
        guard !normalizedKey.contains(where: \Character.isWhitespace) else {
            throw InputConfigurationError.invalidShortcutKey(key)
        }
        self.modifiers = modifiers
        self.key = normalizedKey
    }

    private enum CodingKeys: String, CodingKey {
        case modifiers
        case key
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        let modifiers = try container.decode(Set<Modifier>.self, forKey: .modifiers)
        let key = try container.decode(String.self, forKey: .key)
        try self.init(modifiers: modifiers, key: key)
    }
}

public struct CommandSpecification: Codable, Equatable, Sendable {
    public let executable: String
    public let arguments: [String]
    public let timeoutMilliseconds: UInt32

    public init(executable: String, arguments: [String] = [], timeoutMilliseconds: UInt32) throws {
        let normalizedExecutable = executable.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !normalizedExecutable.isEmpty else {
            throw InputConfigurationError.missingAssociatedValue(action: "customCommand")
        }
        guard normalizedExecutable.hasPrefix("/") else {
            throw InputConfigurationError.commandExecutableMustBeAbsolute(normalizedExecutable)
        }
        guard timeoutMilliseconds > 0, timeoutMilliseconds <= 300_000 else {
            throw InputConfigurationError.invalidCommandTimeout(timeoutMilliseconds)
        }
        self.executable = normalizedExecutable
        self.arguments = arguments
        self.timeoutMilliseconds = timeoutMilliseconds
    }

    private enum CodingKeys: String, CodingKey {
        case executable
        case arguments
        case timeoutMilliseconds = "timeout_ms"
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        try self.init(
            executable: container.decode(String.self, forKey: .executable),
            arguments: container.decode([String].self, forKey: .arguments),
            timeoutMilliseconds: container.decode(UInt32.self, forKey: .timeoutMilliseconds)
        )
    }
}

public enum HostAction: Equatable, Sendable {
    case none
    case voiceInput
    case sendEnter
    case systemCopy
    case interruptControlC
    case wakeApplication
    case pasteText(String)
    case holdKey(String)
    case customShortcut(KeyboardShortcut)
    case customCommand(CommandSpecification)
    case launchApplication(bundleIdentifier: String)
    case screenMode(ScreenMode)
    case dashboardNextPage
    case dashboardNextStocks
    case petInteraction(String)
}

extension HostAction: Codable {
    private enum CodingKeys: String, CodingKey {
        case type
        case text
        case key
        case shortcut
        case command
        case bundleIdentifier = "bundle_identifier"
        case mode
        case interaction
    }

    private enum Kind: String, Codable {
        case none
        case voiceInput = "voice_input"
        case sendEnter = "send_enter"
        case systemCopy = "system_copy"
        case interruptControlC = "interrupt_ctrl_c"
        case wakeApplication = "wake_application"
        case pasteText = "paste_text"
        case holdKey = "hold_key"
        case customShortcut = "custom_shortcut"
        case customCommand = "custom_command"
        case launchApplication = "launch_application"
        case screenMode = "screen_mode"
        case dashboardNextPage = "dashboard_next_page"
        case dashboardNextStocks = "dashboard_next_stocks"
        case petInteraction = "pet_interaction"
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        let kind = try container.decode(Kind.self, forKey: .type)
        switch kind {
        case .none: self = .none
        case .voiceInput: self = .voiceInput
        case .sendEnter: self = .sendEnter
        case .systemCopy: self = .systemCopy
        case .interruptControlC: self = .interruptControlC
        case .wakeApplication: self = .wakeApplication
        case .pasteText:
            self = .pasteText(try Self.requireNonempty(container.decode(String.self, forKey: .text), action: kind.rawValue))
        case .holdKey:
            self = .holdKey(try Self.requireKey(container.decode(String.self, forKey: .key), action: kind.rawValue))
        case .customShortcut:
            self = .customShortcut(try container.decode(KeyboardShortcut.self, forKey: .shortcut))
        case .customCommand:
            self = .customCommand(try container.decode(CommandSpecification.self, forKey: .command))
        case .launchApplication:
            self = .launchApplication(bundleIdentifier: try Self.requireNonempty(container.decode(String.self, forKey: .bundleIdentifier), action: kind.rawValue))
        case .screenMode:
            self = .screenMode(try container.decode(ScreenMode.self, forKey: .mode))
        case .dashboardNextPage:
            self = .dashboardNextPage
        case .dashboardNextStocks:
            self = .dashboardNextStocks
        case .petInteraction:
            self = .petInteraction(try Self.requireNonempty(container.decode(String.self, forKey: .interaction), action: kind.rawValue))
        }
    }

    public func encode(to encoder: Encoder) throws {
        var container = encoder.container(keyedBy: CodingKeys.self)
        switch self {
        case .none: try container.encode(Kind.none, forKey: .type)
        case .voiceInput: try container.encode(Kind.voiceInput, forKey: .type)
        case .sendEnter: try container.encode(Kind.sendEnter, forKey: .type)
        case .systemCopy: try container.encode(Kind.systemCopy, forKey: .type)
        case .interruptControlC: try container.encode(Kind.interruptControlC, forKey: .type)
        case .wakeApplication: try container.encode(Kind.wakeApplication, forKey: .type)
        case let .pasteText(text):
            try container.encode(Kind.pasteText, forKey: .type)
            try container.encode(Self.requireNonempty(text, action: Kind.pasteText.rawValue), forKey: .text)
        case let .holdKey(key):
            try container.encode(Kind.holdKey, forKey: .type)
            try container.encode(Self.requireKey(key, action: Kind.holdKey.rawValue), forKey: .key)
        case let .customShortcut(shortcut):
            try container.encode(Kind.customShortcut, forKey: .type)
            try container.encode(shortcut, forKey: .shortcut)
        case let .customCommand(command):
            try container.encode(Kind.customCommand, forKey: .type)
            try container.encode(command, forKey: .command)
        case let .launchApplication(bundleIdentifier):
            try container.encode(Kind.launchApplication, forKey: .type)
            try container.encode(Self.requireNonempty(bundleIdentifier, action: Kind.launchApplication.rawValue), forKey: .bundleIdentifier)
        case let .screenMode(mode):
            try container.encode(Kind.screenMode, forKey: .type)
            try container.encode(mode, forKey: .mode)
        case .dashboardNextPage:
            try container.encode(Kind.dashboardNextPage, forKey: .type)
        case .dashboardNextStocks:
            try container.encode(Kind.dashboardNextStocks, forKey: .type)
        case let .petInteraction(interaction):
            try container.encode(Kind.petInteraction, forKey: .type)
            try container.encode(Self.requireNonempty(interaction, action: Kind.petInteraction.rawValue), forKey: .interaction)
        }
    }

    public func validate() throws {
        switch self {
        case let .pasteText(text):
            _ = try Self.requireNonempty(text, action: Kind.pasteText.rawValue)
        case let .holdKey(key):
            _ = try Self.requireKey(key, action: Kind.holdKey.rawValue)
        case let .launchApplication(bundleIdentifier):
            _ = try Self.requireNonempty(bundleIdentifier, action: Kind.launchApplication.rawValue)
        case let .petInteraction(interaction):
            _ = try Self.requireNonempty(interaction, action: Kind.petInteraction.rawValue)
        default:
            break
        }
    }

    private static func requireNonempty(_ value: String, action: String) throws -> String {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else {
            throw InputConfigurationError.missingAssociatedValue(action: action)
        }
        return value
    }

    private static func requireKey(_ value: String, action: String) throws -> String {
        let normalized = value.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard !normalized.isEmpty else {
            throw InputConfigurationError.missingAssociatedValue(action: action)
        }
        guard !normalized.contains(where: \.isWhitespace) else {
            throw InputConfigurationError.invalidShortcutKey(value)
        }
        return normalized
    }
}

public struct KeyBindings: Codable, Equatable, Sendable {
    public var single: HostAction
    public var double: HostAction
    public var long: HostAction

    public init(single: HostAction = .none, double: HostAction = .none, long: HostAction = .none) {
        self.single = single
        self.double = double
        self.long = long
    }

    public subscript(gesture: KeyGesture) -> HostAction {
        get {
            switch gesture {
            case .single: single
            case .double: double
            case .long: long
            }
        }
        set {
            switch gesture {
            case .single: single = newValue
            case .double: double = newValue
            case .long: long = newValue
            }
        }
    }

    public func validate() throws {
        try single.validate()
        try double.validate()
        try long.validate()
    }
}

public struct KeyMappingProfile: Codable, Equatable, Sendable {
    public static let currentSchemaVersion = 1

    public let schemaVersion: Int
    public var mappings: [CanonicalKey: KeyBindings]

    public init(schemaVersion: Int = currentSchemaVersion, mappings: [CanonicalKey: KeyBindings]) throws {
        guard schemaVersion == Self.currentSchemaVersion else {
            throw InputConfigurationError.unsupportedSchemaVersion(schemaVersion)
        }
        let expected = Set(CanonicalKey.allCases)
        let actual = Set(mappings.keys)
        guard actual == expected else {
            throw InputConfigurationError.incompleteMapping(missing: expected.subtracting(actual).sorted { $0.rawValue < $1.rawValue })
        }
        for bindings in mappings.values {
            try bindings.validate()
        }
        self.schemaVersion = schemaVersion
        self.mappings = mappings
    }

    public func validate() throws {
        _ = try KeyMappingProfile(schemaVersion: schemaVersion, mappings: mappings)
    }

    public static func vendorDefault() -> KeyMappingProfile {
        KeyMappingProfile(
            validatedMappings: [
                .k1: KeyBindings(single: .wakeApplication),
                .k2: KeyBindings(single: .pasteText("继续")),
                .k3: KeyBindings(single: .interruptControlC),
                .k4: KeyBindings(single: .voiceInput, double: .sendEnter),
            ]
        )
    }

    private init(validatedMappings mappings: [CanonicalKey: KeyBindings]) {
        self.schemaVersion = Self.currentSchemaVersion
        self.mappings = mappings
    }

    private enum CodingKeys: String, CodingKey {
        case schemaVersion = "schema_version"
        case mappings
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        try self.init(
            schemaVersion: container.decode(Int.self, forKey: .schemaVersion),
            mappings: container.decode([CanonicalKey: KeyBindings].self, forKey: .mappings)
        )
    }
}

public enum InputConfigurationError: Error, Equatable, Sendable {
    case unknownKey(String)
    case unsupportedSchemaVersion(Int)
    case incompleteMapping(missing: [CanonicalKey])
    case missingAssociatedValue(action: String)
    case invalidShortcutKey(String)
    case commandExecutableMustBeAbsolute(String)
    case invalidCommandTimeout(UInt32)
    case persistenceRead(String)
    case persistenceWrite(String)
    case invalidStoredConfiguration(String)
}
