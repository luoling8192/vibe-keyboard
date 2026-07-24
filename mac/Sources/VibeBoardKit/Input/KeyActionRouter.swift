import Darwin
import Foundation

public enum InputPermission: Sendable {
    case inputInjection
}

public protocol PermissionAuthorizing: Sendable {
    func require(_ permission: InputPermission) async throws
}

public protocol InputInjecting: Sendable {
    func sendEnter() async throws
    func copySelection() async throws
    func interruptControlC() async throws
    func pasteText(_ text: String) async throws
    func sendShortcut(_ shortcut: KeyboardShortcut) async throws
}

public protocol ApplicationControlling: Sendable {
    func wakeApplication() async throws
    func launchApplication(bundleIdentifier: String) async throws
}

public protocol VoiceInputControlling: Sendable {
    func toggleVoiceInput() async throws
}

public protocol ScreenControlling: Sendable {
    func activate(mode: ScreenMode) async throws
    func advanceDashboardPage() async throws
    func advanceDashboardStocks() async throws
    func interactWithPet(_ interaction: String) async throws
}

public struct CommandResult: Equatable, Sendable {
    public let exitStatus: Int32

    public init(exitStatus: Int32) {
        self.exitStatus = exitStatus
    }
}

public protocol CommandExecuting: Sendable {
    func execute(_ command: CommandSpecification) async throws -> CommandResult
}

public enum ActionExecutionResult: Equatable, Sendable {
    case noAction
    case completed
    case command(CommandResult)
}

public enum ActionRoutingError: Error, Equatable, Sendable {
    case missingBinding(CanonicalKey)
    case permissionDenied(String)
    case actionFailed(String)
    case commandTimedOut(UInt32)
    case commandLaunchFailed(String)
}

public actor KeyActionRouter {
    private var profile: KeyMappingProfile
    private let permissions: any PermissionAuthorizing
    private let input: any InputInjecting
    private let applications: any ApplicationControlling
    private let voice: any VoiceInputControlling
    private let screen: any ScreenControlling
    private let commands: any CommandExecuting

    public init(
        profile: KeyMappingProfile,
        permissions: any PermissionAuthorizing,
        input: any InputInjecting,
        applications: any ApplicationControlling,
        voice: any VoiceInputControlling,
        screen: any ScreenControlling,
        commands: any CommandExecuting
    ) throws {
        try profile.validate()
        self.profile = profile
        self.permissions = permissions
        self.input = input
        self.applications = applications
        self.voice = voice
        self.screen = screen
        self.commands = commands
    }

    public func updateProfile(_ profile: KeyMappingProfile) throws {
        try profile.validate()
        self.profile = profile
    }

    public func execute(_ routed: RoutedGesture) async throws -> ActionExecutionResult {
        guard let bindings = profile.mappings[routed.key] else {
            throw ActionRoutingError.missingBinding(routed.key)
        }
        return try await execute(bindings[routed.gesture])
    }

    public func execute(_ action: HostAction) async throws -> ActionExecutionResult {
        switch action {
        case .none:
            return .noAction
        case .voiceInput:
            try await voice.toggleVoiceInput()
        case .sendEnter:
            try await requireInputPermission()
            try await input.sendEnter()
        case .systemCopy:
            try await requireInputPermission()
            try await input.copySelection()
        case .interruptControlC:
            try await requireInputPermission()
            try await input.interruptControlC()
        case .wakeApplication:
            try await applications.wakeApplication()
        case let .pasteText(text):
            guard !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
                throw InputConfigurationError.missingAssociatedValue(action: "pasteText")
            }
            try await requireInputPermission()
            try await input.pasteText(text)
        case let .customShortcut(shortcut):
            try await requireInputPermission()
            try await input.sendShortcut(shortcut)
        case let .customCommand(command):
            return .command(try await commands.execute(command))
        case let .launchApplication(bundleIdentifier):
            guard !bundleIdentifier.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
                throw InputConfigurationError.missingAssociatedValue(action: "launchApplication")
            }
            try await applications.launchApplication(bundleIdentifier: bundleIdentifier)
        case let .screenMode(mode):
            try await screen.activate(mode: mode)
        case .dashboardNextPage:
            try await screen.advanceDashboardPage()
        case .dashboardNextStocks:
            try await screen.advanceDashboardStocks()
        case let .petInteraction(interaction):
            guard !interaction.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
                throw InputConfigurationError.missingAssociatedValue(action: "petInteraction")
            }
            try await screen.interactWithPet(interaction)
        }
        return .completed
    }

    private func requireInputPermission() async throws {
        do {
            try await permissions.require(.inputInjection)
        } catch {
            throw ActionRoutingError.permissionDenied(String(describing: error))
        }
    }
}

public struct ProcessCommandExecutor: CommandExecuting {
    private let processFactory: any CommandProcessFactory
    private let clock: any CommandExecutionClock
    private let terminationGraceMilliseconds: UInt32

    public init() {
        self.init(
            processFactory: FoundationCommandProcessFactory(),
            clock: DispatchCommandExecutionClock(),
            terminationGraceMilliseconds: 250
        )
    }

    init(
        processFactory: any CommandProcessFactory,
        clock: any CommandExecutionClock,
        terminationGraceMilliseconds: UInt32
    ) {
        self.processFactory = processFactory
        self.clock = clock
        self.terminationGraceMilliseconds = terminationGraceMilliseconds
    }

    public func execute(_ command: CommandSpecification) async throws -> CommandResult {
        let process = processFactory.makeProcess(
            configuration: CommandProcessConfiguration(
                executable: command.executable,
                arguments: command.arguments,
                environment: [:]
            )
        )
        let state = ProcessExecutionState(process: process)

        do {
            try Task.checkCancellation()
            try process.run()
        } catch is CancellationError {
            throw CancellationError()
        } catch {
            throw ActionRoutingError.commandLaunchFailed(String(describing: error))
        }

        return try await withTaskCancellationHandler {
            do {
                if try await clock.waitUntilExit(process, timeoutMilliseconds: command.timeoutMilliseconds) {
                    process.reap()
                    return CommandResult(exitStatus: process.terminationStatus)
                }
                await stopAndReap(process, graceMilliseconds: terminationGraceMilliseconds)
                throw ActionRoutingError.commandTimedOut(command.timeoutMilliseconds)
            } catch is CancellationError {
                await stopAndReap(process, graceMilliseconds: terminationGraceMilliseconds)
                throw CancellationError()
            }
        } onCancel: {
            state.requestCancellation()
        }
    }

    private func stopAndReap(_ process: any CommandProcess, graceMilliseconds: UInt32) async {
        process.sendSignal(SIGTERM)
        let terminated = (try? await clock.waitUntilExit(process, timeoutMilliseconds: graceMilliseconds)) ?? false
        if !terminated {
            process.sendSignal(SIGKILL)
            await clock.waitUntilExitWithoutDeadline(process)
        }
        process.reap()
    }
}

struct CommandProcessConfiguration: Equatable, Sendable {
    let executable: String
    let arguments: [String]
    let environment: [String: String]
}

protocol CommandProcessFactory: Sendable {
    func makeProcess(configuration: CommandProcessConfiguration) -> any CommandProcess
}

protocol CommandProcess: Sendable {
    func run() throws
    func sendSignal(_ signal: Int32)
    func waitUntilExit()
    var isRunning: Bool { get }
    var terminationStatus: Int32 { get }
}

extension CommandProcess {
    func reap() {
        waitUntilExit()
    }
}

protocol CommandExecutionClock: Sendable {
    func waitUntilExit(_ process: any CommandProcess, timeoutMilliseconds: UInt32) async throws -> Bool
    func waitUntilExitWithoutDeadline(_ process: any CommandProcess) async
}

private struct FoundationCommandProcessFactory: CommandProcessFactory {
    func makeProcess(configuration: CommandProcessConfiguration) -> any CommandProcess {
        FoundationCommandProcess(configuration: configuration)
    }
}

private final class FoundationCommandProcess: CommandProcess, @unchecked Sendable {
    private let process: Process

    init(configuration: CommandProcessConfiguration) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: configuration.executable)
        process.arguments = configuration.arguments
        process.environment = configuration.environment
        process.standardInput = FileHandle.nullDevice
        process.standardOutput = FileHandle.nullDevice
        process.standardError = FileHandle.nullDevice
        self.process = process
    }

    func run() throws {
        try process.run()
    }

    func sendSignal(_ signal: Int32) {
        guard process.isRunning else { return }
        _ = Darwin.kill(process.processIdentifier, signal)
    }

    func waitUntilExit() {
        process.waitUntilExit()
    }

    var isRunning: Bool { process.isRunning }
    var terminationStatus: Int32 { process.terminationStatus }
}

private struct DispatchCommandExecutionClock: CommandExecutionClock {
    private let pollingNanoseconds: UInt64 = 10_000_000

    func waitUntilExit(_ process: any CommandProcess, timeoutMilliseconds: UInt32) async throws -> Bool {
        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: .milliseconds(Int64(timeoutMilliseconds)))
        while process.isRunning {
            try Task.checkCancellation()
            guard clock.now < deadline else { return false }
            try await Task.sleep(nanoseconds: pollingNanoseconds)
        }
        return true
    }

    func waitUntilExitWithoutDeadline(_ process: any CommandProcess) async {
        while process.isRunning {
            try? await Task.sleep(nanoseconds: pollingNanoseconds)
        }
    }
}

private final class ProcessExecutionState: @unchecked Sendable {
    private let lock = NSLock()
    private let process: any CommandProcess
    private var cancellationRequested = false

    init(process: any CommandProcess) {
        self.process = process
    }

    func requestCancellation() {
        let shouldSignal = lock.withLock {
            guard !cancellationRequested else { return false }
            cancellationRequested = true
            return true
        }
        if shouldSignal {
            process.sendSignal(SIGTERM)
        }
    }
}
