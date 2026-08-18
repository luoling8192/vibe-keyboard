import Foundation

public enum DiagnosticCLICommand: Equatable, Sendable {
    case help
    case list
    case inspect(durationSeconds: Int)
    case handshake(timeoutSeconds: Int)
    case inputConfiguration
    case keys(durationSeconds: Int)
    case screen
    case image(inputURL: URL)
    case record(outputURL: URL, timeoutSeconds: Int)
}

public enum DiagnosticCLIError: Error, Equatable, Sendable {
    case usage
    case safeFlagRequired
    case invalidDuration
    case missingInput
    case invalidInput(String)
    case missingOutput
    case invalidOutput(String)
    case unsupportedOption(String)
}

public enum DiagnosticCLIParser {
    public static let usage = """
    usage:
      VibeBoardDiagnostic list
      VibeBoardDiagnostic inspect [--duration 1...60]
      VibeBoardDiagnostic handshake --allow-safe-commands [--timeout 1...60]
      VibeBoardDiagnostic input --allow-safe-commands
      VibeBoardDiagnostic keys --allow-safe-commands [--duration 1...60]
      VibeBoardDiagnostic screen --allow-safe-commands
      VibeBoardDiagnostic image --allow-safe-commands --input /absolute/image.jpg
      VibeBoardDiagnostic record --allow-safe-commands --output /absolute/private/file.ogg [--timeout 1...60]
    """

    public static func parse(_ arguments: [String]) throws -> DiagnosticCLICommand {
        guard let command = arguments.first else { throw DiagnosticCLIError.usage }
        let options = Array(arguments.dropFirst())

        switch command {
        case "help", "--help", "-h":
            try rejectOptions(options, allowed: [])
            return .help
        case "list":
            try rejectOptions(options, allowed: [])
            return .list
        case "inspect":
            try rejectOptions(options, allowed: ["--duration"])
            return .inspect(durationSeconds: try boundedInteger(options, name: "--duration", defaultValue: 5))
        case "handshake":
            try requireSafeFlag(options)
            try rejectOptions(options, allowed: ["--allow-safe-commands", "--timeout"])
            return .handshake(timeoutSeconds: try boundedInteger(options, name: "--timeout", defaultValue: 20))
        case "input":
            try requireSafeFlag(options)
            try rejectOptions(options, allowed: ["--allow-safe-commands"])
            return .inputConfiguration
        case "keys":
            try requireSafeFlag(options)
            try rejectOptions(options, allowed: ["--allow-safe-commands", "--duration"])
            return .keys(durationSeconds: try boundedInteger(options, name: "--duration", defaultValue: 20))
        case "screen":
            try requireSafeFlag(options)
            try rejectOptions(options, allowed: ["--allow-safe-commands"])
            return .screen
        case "image":
            try requireSafeFlag(options)
            try rejectOptions(options, allowed: ["--allow-safe-commands", "--input"])
            let input = try requiredValue(options, name: "--input", missing: .missingInput)
            return .image(inputURL: try validateInput(input))
        case "record":
            try requireSafeFlag(options)
            try rejectOptions(options, allowed: ["--allow-safe-commands", "--output", "--timeout"])
            let output = try requiredValue(options, name: "--output", missing: .missingOutput)
            let outputURL = try validateOutput(output)
            return .record(
                outputURL: outputURL,
                timeoutSeconds: try boundedInteger(options, name: "--timeout", defaultValue: 30)
            )
        default:
            throw DiagnosticCLIError.usage
        }
    }

    private static func requireSafeFlag(_ arguments: [String]) throws {
        guard arguments.contains("--allow-safe-commands") else {
            throw DiagnosticCLIError.safeFlagRequired
        }
    }

    private static func boundedInteger(_ arguments: [String], name: String, defaultValue: Int) throws -> Int {
        guard arguments.contains(name) else { return defaultValue }
        let raw = try requiredValue(arguments, name: name, missing: .invalidDuration)
        guard let value = Int(raw), (1...60).contains(value) else {
            throw DiagnosticCLIError.invalidDuration
        }
        return value
    }

    private static func requiredValue(
        _ arguments: [String],
        name: String,
        missing: DiagnosticCLIError
    ) throws -> String {
        guard let index = arguments.firstIndex(of: name),
              arguments.indices.contains(index + 1),
              !arguments[index + 1].hasPrefix("--") else {
            throw missing
        }
        return arguments[index + 1]
    }

    private static func rejectOptions(_ arguments: [String], allowed: Set<String>) throws {
        var index = 0
        while index < arguments.count {
            let argument = arguments[index]
            guard argument.hasPrefix("--") else {
                throw DiagnosticCLIError.unsupportedOption(argument)
            }
            guard allowed.contains(argument) else {
                throw DiagnosticCLIError.unsupportedOption(argument)
            }
            if argument == "--allow-safe-commands" {
                index += 1
            } else {
                guard arguments.indices.contains(index + 1), !arguments[index + 1].hasPrefix("--") else {
                    if argument == "--output" { throw DiagnosticCLIError.missingOutput }
                    if argument == "--input" { throw DiagnosticCLIError.missingInput }
                    throw DiagnosticCLIError.invalidDuration
                }
                index += 2
            }
        }
    }

    private static func validateInput(_ path: String) throws -> URL {
        let url = URL(fileURLWithPath: path).standardizedFileURL
        var isDirectory: ObjCBool = false
        guard path.hasPrefix("/"),
              FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory),
              !isDirectory.boolValue,
              FileManager.default.isReadableFile(atPath: url.path) else {
            throw DiagnosticCLIError.invalidInput(path)
        }
        return url
    }

    private static func validateOutput(_ path: String) throws -> URL {
        let url = URL(fileURLWithPath: path).standardizedFileURL
        guard path.hasPrefix("/"), url.pathExtension.lowercased() == "ogg" else {
            throw DiagnosticCLIError.invalidOutput(path)
        }
        let manager = FileManager.default
        var isDirectory: ObjCBool = false
        let parent = url.deletingLastPathComponent()
        guard manager.fileExists(atPath: parent.path, isDirectory: &isDirectory), isDirectory.boolValue,
              !manager.fileExists(atPath: url.path) else {
            throw DiagnosticCLIError.invalidOutput(path)
        }
        return url
    }
}
