import Foundation
import VibeBoardKit

protocol RecordingSinkCreating: Sendable {
    func makeSink(session: UInt32) throws -> (sink: any OggPageSink, destination: URL)
}

struct ApplicationSupportRecordingSinkFactory: RecordingSinkCreating {
    let baseDirectory: URL

    init(fileManager: FileManager = .default) {
        baseDirectory = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("VibeKeyboard", isDirectory: true)
            .appendingPathComponent("Recordings", isDirectory: true)
    }

    func makeSink(session: UInt32) throws -> (sink: any OggPageSink, destination: URL) {
        try FileManager.default.createDirectory(at: baseDirectory, withIntermediateDirectories: true, attributes: [
            .posixPermissions: NSNumber(value: Int16(0o700)),
        ])
        let timestamp = ISO8601DateFormatter().string(from: Date())
            .replacingOccurrences(of: ":", with: "-")
        let filename = "VibeBoard-\(timestamp)-session-\(session)-\(UUID().uuidString).ogg"
        let destination = baseDirectory.appendingPathComponent(filename, isDirectory: false)
        guard destination.deletingLastPathComponent().standardizedFileURL == baseDirectory.standardizedFileURL else {
            throw OggSinkError.createFailed(path: destination.path, errno: EINVAL)
        }
        return (try AtomicFileOggPageSink(destinationURL: destination), destination)
    }
}
