import Foundation
import VibeBoardKit

@main
struct VibeBoardDiagnostic {
    static func main() async {
        do {
            let command = try DiagnosticCLIParser.parse(Array(CommandLine.arguments.dropFirst()))
            let provider = IOKitUSBRegistryProvider()
            let descriptors = try USBDeviceDiscovery.descriptors(from: provider.snapshots())
            try await run(command, descriptors: descriptors)
        } catch {
            if error as? DiagnosticCLIError == .usage {
                FileHandle.standardError.write(Data("\(DiagnosticCLIParser.usage)\n".utf8))
            } else {
                FileHandle.standardError.write(Data("error: \(error)\n".utf8))
            }
            Foundation.exit(2)
        }
    }

    private static func run(_ command: DiagnosticCLICommand, descriptors: [USBDeviceDescriptor]) async throws {
        switch command {
        case .help:
            print(DiagnosticCLIParser.usage)
        case .list:
            for descriptor in descriptors { printDescriptor(descriptor) }
        case let .inspect(duration):
            let descriptor = try select(descriptors)
            let session = try USBSession(descriptor: descriptor)
            try await session.openForInspection()
            try await Task.sleep(for: .seconds(duration))
            let diagnostics = await session.diagnostics()
            await session.disconnect()
            printDescriptor(descriptor)
            print("diagnostic_bytes=\(diagnostics.text.utf8.count)")
            for entry in diagnostics.entries { print(entry) }
        case let .handshake(timeout):
            let descriptor = try select(descriptors)
            let session = try USBSession(descriptor: descriptor)
            let info = try await session.connect(handshakeTimeout: .seconds(timeout))
            printDescriptor(descriptor)
            print("hardware=\(info.hardware)")
            print("firmware=\(info.firmwareVersion ?? "unknown")")
            print("firmware_device_id=\(info.firmwareDeviceID ?? "unknown")")
            try await Task.sleep(for: .milliseconds(500))
            if let ctx = await session.currentReplacementContext() {
                let snap = ctx.snapshot
                print("epoch_gen=\(ctx.epochGeneration)")
                print("snapshot_gen=\(ctx.snapshotGeneration)")
                print("protocol=\(snap.protocolVersion)")
                print("display=\(snap.display.width)x\(snap.display.height) \(snap.display.format)")
                if let a = snap.assets { print("assets=\(a)") }
                if let s = snap.screen { print("screen=\(s)") }
                if let u = snap.update { print("update=\(u)") }
                if let l = snap.led { print("led=\(l)") }
            } else {
                print("capabilities=not_yet_received")
            }
            await session.disconnect()
        case let .keys(duration):
            try await captureKeys(descriptor: select(descriptors), durationSeconds: duration)
        case .screen:
            try await commitAcceptanceScreen(descriptor: select(descriptors))
        case let .record(outputURL, timeout):
            try await record(descriptor: select(descriptors), outputURL: outputURL, timeoutSeconds: timeout)
        }
    }

    private static func commitAcceptanceScreen(descriptor: USBDeviceDescriptor) async throws {
        let session = try USBSession(descriptor: descriptor)
        let committed = Task { () throws -> ReplacementScreenEvent in
            for await event in session.events {
                switch event {
                case let .replacementEvent(.screen(screen)):
                    return screen
                case let .replacementEvent(.error(error)) where error.operation == "screen":
                    throw USBSessionError.protocolFailure(
                        "screen \(error.code)\(error.message.map { ": \($0)" } ?? "")"
                    )
                case let .stateChanged(.failed(error)):
                    throw error
                default:
                    continue
                }
            }
            throw USBSessionError.cancelled
        }
        do {
            _ = try await session.connect()
            guard let context = await session.currentReplacementContext(),
                  case let .available(screen)? = context.snapshot.screen,
                  let font = screen.fonts.first else {
                throw USBSessionError.protocolFailure("screen capability unavailable")
            }
            let previousRevision = screen.revision
            let revision = previousRevision &+ 1
            guard revision != 0 else {
                throw USBSessionError.protocolFailure("screen revision exhausted")
            }
            let fontReference = ScreenFontReference(id: font.id, version: font.version)
            let layout = ScreenLayout(
                backgroundRGB888: 0x101820,
                mode: .dashboard,
                revision: revision,
                objects: [
                    .init(
                        x: 16,
                        y: 28,
                        node: .staticLabel(
                            base: .init(id: "title", width: 396, height: 28, z: 0, clip: true, visible: true),
                            align: .left,
                            colorRGB888: 0xffffff,
                            font: fontReference,
                            text: "VIBEBOARD CUSTOM"
                        )
                    ),
                    .init(
                        x: 16,
                        y: 72,
                        node: .dynamicLabel(
                            base: .init(id: "status-value", width: 396, height: 28, z: 1, clip: true, visible: true),
                            align: .left,
                            colorRGB888: 0x6ed0ff,
                            font: fontReference,
                            widgetID: "status"
                        )
                    ),
                ],
                widgets: [.text(id: "status", target: "status-value", fallback: "READY - K1 K2 K3 K4")]
            )
            let commit = ScreenCommit(
                expectedRevision: previousRevision,
                revision: revision,
                assets: [],
                payload: .dashboard(layout),
                limits: .init(
                    displayWidth: context.snapshot.display.width,
                    displayHeight: context.snapshot.display.height,
                    screen: screen
                )
            )
            try await session.sendScreenCommand(.commit(commit))
            let event = try await withThrowingTaskGroup(of: ReplacementScreenEvent.self) { group in
                group.addTask { try await committed.value }
                group.addTask {
                    try await Task.sleep(for: .seconds(5))
                    committed.cancel()
                    throw ConnectedDiagnosticError.screenTimedOut
                }
                defer { group.cancelAll() }
                guard let value = try await group.next() else {
                    throw ConnectedDiagnosticError.screenTimedOut
                }
                return value
            }
            guard case let .committed(_, acceptedPreviousRevision, acceptedRevision, _) = event,
                  acceptedPreviousRevision == previousRevision,
                  acceptedRevision == revision else {
                throw USBSessionError.protocolFailure("unexpected screen response")
            }
            await session.disconnect()
            print("screen_committed previous_revision=\(previousRevision) revision=\(revision) mode=dashboard")
        } catch {
            committed.cancel()
            await session.disconnect()
            throw error
        }
    }

    private static func captureKeys(descriptor: USBDeviceDescriptor, durationSeconds: Int) async throws {
        let session = try USBSession(descriptor: descriptor)
        let workflow = try KeyDiagnosticWorkflow()
        let start = DispatchTime.now().uptimeNanoseconds
        let consumer = Task {
            for await event in session.events {
                switch event {
                case let .stateEvent(state):
                    let elapsed = (DispatchTime.now().uptimeNanoseconds - start) / 1_000_000
                    try await workflow.consume(state, at: elapsed)
                case let .stateChanged(state):
                    switch state {
                    case .disconnected:
                        try await workflow.disconnect(at: (DispatchTime.now().uptimeNanoseconds - start) / 1_000_000)
                        return
                    case let .failed(error):
                        throw error
                    default:
                        break
                    }
                default:
                    break
                }
            }
        }
        do {
            _ = try await session.connect(handshakeTimeout: .seconds(min(durationSeconds, 20)))
            try await Task.sleep(for: .seconds(durationSeconds))
            await session.disconnect()
            try await consumer.value
        } catch {
            consumer.cancel()
            await session.disconnect()
            throw error
        }
        print(try keySummaryJSON(await workflow.summary()))
    }

    private static func record(
        descriptor: USBDeviceDescriptor,
        outputURL: URL,
        timeoutSeconds: Int
    ) async throws {
        let session = try USBSession(descriptor: descriptor)
        let workflow = try AudioRecordingWorkflow(outputURL: outputURL)
        let consumer = Task {
            for await event in session.events {
                switch event {
                case let .audioFrame(frame):
                    try await workflow.consume(frame)
                case let .stateChanged(state):
                    switch state {
                    case .disconnected:
                        return
                    case let .failed(error):
                        throw error
                    default:
                        break
                    }
                default:
                    break
                }
            }
        }

        do {
            _ = try await session.connect(handshakeTimeout: .seconds(min(timeoutSeconds, 20)))
            try await session.send(.uiState(.listening, text: ""))
            let deadline = ContinuousClock.now.advanced(by: .seconds(timeoutSeconds))
            while ContinuousClock.now < deadline {
                switch await workflow.state() {
                case .completed:
                    try? await session.send(.uiState(.ready, text: ""))
                    await session.disconnect()
                    try await consumer.value
                    let acceptance = try AudioAcceptanceAnalyzer.analyze(fileURL: outputURL)
                    print(try audioSummaryJSON(acceptance, outputURL: outputURL))
                    return
                case let .failed(error):
                    throw error
                case .cancelled:
                    throw AudioRecordingError.cancelled
                default:
                    try await Task.sleep(for: .milliseconds(25))
                }
            }
            throw ConnectedDiagnosticError.recordingTimedOut
        } catch {
            try? await session.send(.uiState(.ready, text: ""))
            await session.disconnect()
            consumer.cancel()
            _ = try? await consumer.value
            await workflow.cancel()
            try? FileManager.default.removeItem(at: outputURL)
            throw error
        }
    }

    private static func select(_ descriptors: [USBDeviceDescriptor]) throws -> USBDeviceDescriptor {
        guard !descriptors.isEmpty else { throw USBDiscoveryError.deviceNotFound }
        guard descriptors.count == 1 else { throw USBDiscoveryError.ambiguousDevices(descriptors) }
        return descriptors[0]
    }

    private static func printDescriptor(_ descriptor: USBDeviceDescriptor) {
        print(String(
            format: "vid=0x%04x pid=0x%04x serial=%@ device_id=%@ path=%@",
            descriptor.vendorID,
            descriptor.productID,
            descriptor.serialNumber,
            descriptor.normalizedDeviceID,
            descriptor.calloutPath
        ))
    }

    private static func keySummaryJSON(_ summary: KeyDiagnosticSummary) throws -> String {
        let rawEvents: [[String: Any]] = summary.rawEvents.map {
            var value: [String: Any] = [
                "event": $0.event,
                "key": $0.key.rawValue,
                "timestamp_ms": $0.timestampMilliseconds,
            ]
            if let duration = $0.durationMilliseconds { value["duration_ms"] = duration }
            if let session = $0.sessionID { value["session_id"] = session }
            return value
        }
        var gestureCounts: [String: [String: Int]] = [:]
        for key in CanonicalKey.allCases {
            gestureCounts[key.rawValue] = Dictionary(uniqueKeysWithValues: KeyGesture.allCases.map {
                ($0.rawValue, summary.gestureCounts[key]?[$0] ?? 0)
            })
        }
        let markers = Dictionary(uniqueKeysWithValues: CanonicalKey.allCases.map {
            ($0.rawValue, summary.inertMarkers[$0] ?? 0)
        })
        return try jsonString([
            "raw_events": rawEvents,
            "gesture_counts": gestureCounts,
            "inert_markers": markers,
        ])
    }

    private static func audioSummaryJSON(_ result: AudioAcceptanceResult, outputURL: URL) throws -> String {
        try jsonString([
            "output": outputURL.path,
            "codec": result.codecName,
            "decoder_sample_rate": result.decoderSampleRate,
            "original_input_rate": result.originalInputRate,
            "channels": result.channels,
            "duration_seconds": result.durationSeconds,
            "rms_db": result.rmsDecibels,
            "peak_db": result.peakDecibels,
            "nonzero_audio": result.hasNonzeroAudio,
        ])
    }

    private static func jsonString(_ object: Any) throws -> String {
        let data = try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
        return String(decoding: data, as: UTF8.self)
    }
}
