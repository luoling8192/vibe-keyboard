import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Connected diagnostic boundaries")
struct ConnectedDiagnosticTests {
    @Test func parserRequiresSafetyFlagAndRejectsRawOrUnsafeOptions() throws {
        #expect(try DiagnosticCLIParser.parse(["--help"]) == .help)
        #expect(throws: DiagnosticCLIError.safeFlagRequired) {
            _ = try DiagnosticCLIParser.parse(["keys", "--duration", "5"])
        }
        #expect(throws: DiagnosticCLIError.safeFlagRequired) {
            _ = try DiagnosticCLIParser.parse(["record", "--output", "/tmp/test.ogg"])
        }
        #expect(throws: DiagnosticCLIError.safeFlagRequired) {
            _ = try DiagnosticCLIParser.parse(["screen"])
        }
        #expect(throws: DiagnosticCLIError.safeFlagRequired) {
            _ = try DiagnosticCLIParser.parse(["image", "--input", "/tmp/test.jpg"])
        }
        #expect(try DiagnosticCLIParser.parse(["screen", "--allow-safe-commands"]) == .screen)
        #expect(throws: DiagnosticCLIError.unsupportedOption("--raw-json")) {
            _ = try DiagnosticCLIParser.parse(["handshake", "--allow-safe-commands", "--raw-json", "{}"])
        }
        #expect(throws: DiagnosticCLIError.unsupportedOption("--voice-gain")) {
            _ = try DiagnosticCLIParser.parse(["record", "--allow-safe-commands", "--voice-gain", "4", "--output", "/tmp/test.ogg"])
        }
    }

    @Test func parserValidatesDurationsAndPrivateOutputBoundary() throws {
        #expect(try DiagnosticCLIParser.parse(["keys", "--allow-safe-commands", "--duration", "9"]) == .keys(durationSeconds: 9))
        #expect(throws: DiagnosticCLIError.invalidDuration) {
            _ = try DiagnosticCLIParser.parse(["keys", "--allow-safe-commands", "--duration", "0"])
        }
        #expect(throws: DiagnosticCLIError.missingOutput) {
            _ = try DiagnosticCLIParser.parse(["record", "--allow-safe-commands"])
        }
        #expect(throws: DiagnosticCLIError.invalidOutput("relative.ogg")) {
            _ = try DiagnosticCLIParser.parse(["record", "--allow-safe-commands", "--output", "relative.ogg"])
        }
        #expect(throws: DiagnosticCLIError.missingInput) {
            _ = try DiagnosticCLIParser.parse(["image", "--allow-safe-commands"])
        }
        #expect(throws: DiagnosticCLIError.invalidInput("relative.jpg")) {
            _ = try DiagnosticCLIParser.parse(["image", "--allow-safe-commands", "--input", "relative.jpg"])
        }

        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let input = directory.appendingPathComponent("image.jpg")
        try Data([0xff, 0xd8, 0xff, 0xd9]).write(to: input)
        #expect(try DiagnosticCLIParser.parse([
            "image", "--allow-safe-commands", "--input", input.path,
        ]) == .image(inputURL: input.standardizedFileURL))
        let output = directory.appendingPathComponent("speech.ogg")
        #expect(try DiagnosticCLIParser.parse([
            "record", "--allow-safe-commands", "--output", output.path, "--timeout", "12",
        ]) == .record(outputURL: output.standardizedFileURL, timeoutSeconds: 12))
    }

    @Test func keyWorkflowUsesOnlyInertMarkersAndRoutesEachFirmwareClickOnce() async throws {
        let workflow = try KeyDiagnosticWorkflow()
        var timestamp: UInt64 = 0
        for key in CanonicalKey.allCases {
            try await workflow.consume(try stateEvent(event: "button_down", button: key.rawValue), at: timestamp)
            timestamp += 20
            try await workflow.consume(
                try stateEvent(event: "button_up", button: key.rawValue, duration: 20),
                at: timestamp
            )
            timestamp += 1
            try await workflow.consume(
                try stateEvent(event: "button_click", button: key.rawValue, duration: 20),
                at: timestamp
            )
            timestamp += 20
        }

        let summary = await workflow.summary()
        #expect(summary.rawEvents.count == 12)
        for key in CanonicalKey.allCases {
            #expect(summary.gestureCounts[key]?[.single] == 1)
            #expect(summary.inertMarkers[key] == 1)
        }
    }

    @Test func disconnectResetsPendingGestureAndReconnectStartsFreshEpoch() async throws {
        let workflow = try KeyDiagnosticWorkflow()
        try await workflow.consume(try stateEvent(event: "button_down", button: "k1"), at: 5_000)
        try await workflow.disconnect(at: 1)
        try await workflow.consume(try stateEvent(event: "button_down", button: "k2"), at: 0)
        try await workflow.consume(try stateEvent(event: "button_up", button: "k2", duration: 10), at: 10)
        try await workflow.consume(try stateEvent(event: "button_click", button: "k2", duration: 10), at: 11)

        let summary = await workflow.summary()
        #expect(summary.gestureCounts[.k1]?[.single] == nil)
        #expect(summary.inertMarkers[.k1] == 0)
        #expect(summary.gestureCounts[.k2]?[.single] == 1)
        #expect(summary.inertMarkers[.k2] == 1)
    }

    @Test func recordingWorkflowFinalizesAtomicallyAndRejectsOldSessionContinuation() async throws {
        let directory = try temporaryDirectory()
        defer { try? FileManager.default.removeItem(at: directory) }
        let firstURL = directory.appendingPathComponent("first.ogg")
        let first = try AudioRecordingWorkflow(outputURL: firstURL)
        try await first.consume(AudioFrame(session: 7, sequence: 0, flags: 0x01, payload: Data([0xf8, 0xff, 0xfe])))
        try await first.consume(AudioFrame(session: 7, sequence: 1, flags: 0x02, payload: Data()))
        #expect(await first.state() == .completed(session: 7, packetCount: 1))
        #expect(FileManager.default.fileExists(atPath: firstURL.path))
        let attributes = try FileManager.default.attributesOfItem(atPath: firstURL.path)
        #expect((attributes[.posixPermissions] as? NSNumber)?.intValue == 0o600)

        let secondURL = directory.appendingPathComponent("second.ogg")
        let second = try AudioRecordingWorkflow(outputURL: secondURL)
        await #expect(throws: AudioRecordingError.missingFirstFlag(flags: 0)) {
            try await second.consume(AudioFrame(session: 7, sequence: 1, flags: 0, payload: Data([1])))
        }
        #expect(!FileManager.default.fileExists(atPath: secondURL.path))
        #expect(try Data(contentsOf: firstURL).count > 0)
    }

    @Test func opusHeadAndStatisticsParsingDoNotRetainDecodedPCM() throws {
        var data = Data(repeating: 0, count: 12)
        data.append(Data("OpusHead".utf8))
        data.append(contentsOf: [1, 1, 0x38, 0x01, 0x80, 0x3e, 0x00, 0x00])
        #expect(OpusHeadInspector.originalInputRate(in: data) == 16_000)

        let statistics = try AudioAcceptanceAnalyzer.parseStatistics("""
        [Parsed_astats_0] Peak level dB: -3.250000
        [Parsed_astats_0] RMS level dB: -19.750000
        """)
        #expect(statistics.rms == -19.75)
        #expect(statistics.peak == -3.25)
        #expect(throws: AudioAcceptanceError.malformedStatistics) {
            _ = try AudioAcceptanceAnalyzer.parseStatistics("RMS level dB: -inf")
        }
    }
}

private func stateEvent(
    event: String,
    button: String,
    duration: UInt32? = nil
) throws -> StateEvent {
    var object: [String: Any] = ["event": event, "button": button]
    if let duration { object["duration_ms"] = duration }
    return try JSONDecoder().decode(StateEvent.self, from: JSONSerialization.data(withJSONObject: object))
}

private func temporaryDirectory() throws -> URL {
    let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
    try FileManager.default.createDirectory(at: url, withIntermediateDirectories: false, attributes: [.posixPermissions: 0o700])
    return url
}
