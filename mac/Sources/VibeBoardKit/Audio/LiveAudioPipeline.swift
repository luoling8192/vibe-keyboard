import Foundation

/// Ties Opus decoding and BlackHole device IO together for the lifetime of a
/// single device audio session (from first-flag frame to last-flag frame).
///
/// Lifecycle mirrors `AudioRecordingSession`:
/// - `start()` creates an Opus decoder and opens the BlackHole device.
/// - `consume(opusPacket:)` decodes one packet and pushes PCM to BlackHole.
/// - `stop()` tears down the decoder and closes the device.
public final class LiveAudioPipeline: @unchecked Sendable {

    private var decoder: OpusStreamDecoder?
    private var writer: BlackHoleAudioWriter?
    private let lock = NSLock()
    private let deviceName: String

    public init(deviceName: String = "BlackHole 2ch") {
        self.deviceName = deviceName
    }

    /// Creates the Opus decoder and starts the BlackHole IO.
    public func start() throws {
        lock.lock()
        defer { lock.unlock() }
        decoder = try OpusStreamDecoder()
        writer = BlackHoleAudioWriter(deviceName: deviceName)
        try writer?.start()
    }

    /// Decodes an Opus packet and writes the resulting PCM into BlackHole.
    public func consume(opusPacket: Data) {
        lock.lock()
        defer { lock.unlock() }
        guard let decoder else { return }
        guard let pcm = try? decoder.decode(packet: opusPacket) else { return }
        writer?.write(pcm: pcm)
    }

    /// Drains remaining buffered audio and tears down the pipeline.
    public func stop() {
        lock.lock()
        defer { lock.unlock() }
        writer?.stop()
        writer = nil
        decoder = nil
    }

    /// Whether the pipeline currently has an active decoder + writer.
    public var isActive: Bool {
        lock.lock()
        defer { lock.unlock() }
        return decoder != nil && writer != nil
    }
}
