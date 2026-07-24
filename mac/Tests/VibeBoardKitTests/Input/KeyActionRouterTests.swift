import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Key action routing")
struct KeyActionRouterTests {
    @Test func everyActionRoutesThroughItsExplicitBoundary() async throws {
        let services = Services()
        let router = makeRouter(services: services)
        let shortcut = try KeyboardShortcut(modifiers: [.command], key: "v")
        let command = try CommandSpecification(executable: "/usr/bin/true", arguments: ["literal"], timeoutMilliseconds: 1000)
        let cases: [(HostAction, ServiceEvent?, ActionExecutionResult)] = [
            (.none, nil, .noAction),
            (.voiceInput, .voice, .completed),
            (.sendEnter, .enter, .completed),
            (.systemCopy, .copy, .completed),
            (.interruptControlC, .interrupt, .completed),
            (.wakeApplication, .wake, .completed),
            (.pasteText("device text stays data"), .paste("device text stays data"), .completed),
            (.customShortcut(shortcut), .shortcut(shortcut), .completed),
            (.customCommand(command), .command(command), .command(CommandResult(exitStatus: 7))),
            (.launchApplication(bundleIdentifier: "com.example.App"), .launch("com.example.App"), .completed),
            (.screenMode(.dashboard), .screen(.dashboard), .completed),
            (.dashboardNextPage, .dashboardPage, .completed),
            (.dashboardNextStocks, .dashboardStocks, .completed),
            (.petInteraction("wave"), .pet("wave"), .completed),
        ]

        for (action, event, expected) in cases {
            #expect(try await router.execute(action) == expected)
            if let event { #expect(await services.events.last == event) }
        }
        #expect(await services.permissionCount == 5)
    }

    @Test func profileBindingSelectsGestureAction() async throws {
        let services = Services()
        let router = makeRouter(services: services)
        #expect(try await router.execute(RoutedGesture(key: .k4, gesture: .double)) == .completed)
        #expect(await services.events.last == .enter)
    }

    @Test func permissionFailureStopsActionWithoutFallback() async throws {
        let services = Services(permissionError: TestFailure.denied)
        let router = makeRouter(services: services)
        await #expect(throws: (any Error).self) { try await router.execute(.pasteText("safe")) }
        #expect(await services.events.isEmpty)
    }

    @Test func updateProfileRejectsInvalidMutationAndRetainsPreviousProfile() async throws {
        let services = Services()
        let router = makeRouter(services: services)
        var invalid = KeyMappingProfile.vendorDefault()
        invalid.mappings.removeValue(forKey: .k4)

        await #expect(throws: InputConfigurationError.incompleteMapping(missing: [.k4])) {
            try await router.updateProfile(invalid)
        }
        #expect(try await router.execute(RoutedGesture(key: .k4, gesture: .double)) == .completed)
        #expect(await services.events.last == .enter)
    }

    @Test func commandArgumentsArePassedAsLiteralArray() async throws {
        let services = Services()
        let router = makeRouter(services: services)
        let command = try CommandSpecification(
            executable: "/usr/bin/printf",
            arguments: ["$(not-executed)", "; rm -rf /"],
            timeoutMilliseconds: 1000
        )
        _ = try await router.execute(.customCommand(command))
        #expect(await services.events.last == .command(command))
    }

    @Test func productionExecutorReturnsSuccessAndNonzeroStatus() async throws {
        let executor = ProcessCommandExecutor()
        let success = try CommandSpecification(executable: "/usr/bin/true", timeoutMilliseconds: 1000)
        let failure = try CommandSpecification(executable: "/usr/bin/false", timeoutMilliseconds: 1000)
        #expect(try await executor.execute(success).exitStatus == 0)
        #expect(try await executor.execute(failure).exitStatus != 0)
    }

    @Test func productionExecutorUsesEmptyEnvironmentAndLiteralArguments() async throws {
        let executor = ProcessCommandExecutor()
        let script = "import os,sys; forbidden={'PATH','HOME','SHELL','TOKEN','API_KEY'}; sys.exit(0 if not forbidden.intersection(os.environ) and sys.argv[1:] == ['$(literal)', ';'] else 9)"
        let command = try CommandSpecification(
            executable: "/usr/bin/python3",
            arguments: ["-c", script, "$(literal)", ";"],
            timeoutMilliseconds: 2000
        )
        #expect(try await executor.execute(command).exitStatus == 0)
    }

    @Test func productionExecutorReportsLaunchFailure() async throws {
        let executor = ProcessCommandExecutor()
        let command = try CommandSpecification(executable: "/definitely/missing/vibe-keyboard", timeoutMilliseconds: 100)
        await #expect(throws: (any Error).self) {
            try await executor.execute(command)
        }
    }

    @Test func productionExecutorTimesOutNormally() async throws {
        let executor = ProcessCommandExecutor()
        let command = try CommandSpecification(executable: "/bin/sleep", arguments: ["5"], timeoutMilliseconds: 50)
        await #expect(throws: ActionRoutingError.commandTimedOut(50)) {
            try await executor.execute(command)
        }
    }

    @Test func productionExecutorEscalatesIgnoredTermToKill() async throws {
        let executor = ProcessCommandExecutor()
        let script = "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(30)"
        let command = try CommandSpecification(
            executable: "/usr/bin/python3",
            arguments: ["-c", script],
            timeoutMilliseconds: 100
        )
        let clock = ContinuousClock()
        let start = clock.now
        await #expect(throws: ActionRoutingError.commandTimedOut(100)) {
            try await executor.execute(command)
        }
        #expect(start.duration(to: clock.now) < .seconds(3))
    }

    @Test func productionExecutorCancellationTerminatesChildPromptly() async throws {
        let executor = ProcessCommandExecutor()
        let script = "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); time.sleep(30)"
        let command = try CommandSpecification(
            executable: "/usr/bin/python3",
            arguments: ["-c", script],
            timeoutMilliseconds: 30_000
        )
        let task = Task { try await executor.execute(command) }
        try await Task.sleep(for: .milliseconds(100))
        let clock = ContinuousClock()
        let start = clock.now
        task.cancel()
        await #expect(throws: CancellationError.self) {
            try await task.value
        }
        #expect(start.duration(to: clock.now) < .seconds(3))
    }

    private func makeRouter(services: Services) -> KeyActionRouter {
        try! KeyActionRouter(
            profile: .vendorDefault(), permissions: services, input: services,
            applications: services, voice: services, screen: services, commands: services
        )
    }
}

private enum TestFailure: Error { case denied }

private enum ServiceEvent: Equatable, Sendable {
    case enter, copy, interrupt, wake, voice
    case paste(String)
    case shortcut(KeyboardShortcut)
    case command(CommandSpecification)
    case launch(String)
    case screen(ScreenMode)
    case dashboardPage
    case dashboardStocks
    case pet(String)
}

private actor Services: PermissionAuthorizing, InputInjecting, ApplicationControlling, VoiceInputControlling, ScreenControlling, CommandExecuting {
    var events: [ServiceEvent] = []
    var permissionCount = 0
    let permissionError: (any Error)?

    init(permissionError: (any Error)? = nil) {
        self.permissionError = permissionError
    }

    func require(_ permission: InputPermission) throws {
        permissionCount += 1
        if let permissionError { throw permissionError }
    }
    func sendEnter() { events.append(.enter) }
    func copySelection() { events.append(.copy) }
    func interruptControlC() { events.append(.interrupt) }
    func pasteText(_ text: String) { events.append(.paste(text)) }
    func sendShortcut(_ shortcut: KeyboardShortcut) { events.append(.shortcut(shortcut)) }
    func wakeApplication() { events.append(.wake) }
    func launchApplication(bundleIdentifier: String) { events.append(.launch(bundleIdentifier)) }
    func toggleVoiceInput() { events.append(.voice) }
    func activate(mode: ScreenMode) { events.append(.screen(mode)) }
    func advanceDashboardPage() { events.append(.dashboardPage) }
    func advanceDashboardStocks() { events.append(.dashboardStocks) }
    func interactWithPet(_ interaction: String) { events.append(.pet(interaction)) }
    func execute(_ command: CommandSpecification) -> CommandResult {
        events.append(.command(command))
        return CommandResult(exitStatus: 7)
    }
}
