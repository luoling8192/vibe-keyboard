import Foundation
import COpus

/// Decodes raw Opus packets into 48 kHz / 16-bit / mono PCM samples.
///
/// Wraps libopus. The device streams Opus packets (≤220 bytes each) over USB;
/// this decoder turns them into linear PCM that can be pushed into the
/// BlackHole virtual audio device.
public final class OpusStreamDecoder: @unchecked Sendable {
    public static let sampleRate: Int = 48_000
    public static let channels: Int = 1
    public static let maxFrameSamples: Int = 2_880  // 60 ms at 48 kHz

    private var decoder: OpaquePointer?
    private let lock = NSLock()

    public init() throws {
        var error: Int32 = 0
        guard let raw = opus_decoder_create(
            Int32(Self.sampleRate),
            Int32(Self.channels),
            &error
        ) else {
            throw OpusStreamDecoderError.decoderCreateFailed(error)
        }
        decoder = raw
    }

    deinit {
        if let decoder { opus_decoder_destroy(decoder) }
    }

    /// Decodes one Opus packet into 16-bit mono PCM samples.
    /// - Parameter packet: Raw Opus packet bytes.
    /// - Returns: Array of Int16 PCM samples (mono, 48 kHz).
    public func decode(packet: Data) throws -> [Int16] {
        lock.lock()
        defer { lock.unlock() }
        guard let decoder else { throw OpusStreamDecoderError.decoderNotInitialized }

        let maxSamples = Self.maxFrameSamples
        var outputBuffer = [Int16](repeating: 0, count: maxSamples)

        let decodedCount: Int32
        if packet.isEmpty {
            // Packet loss concealment — pass NULL data to opus_decode.
            decodedCount = opus_decode(
                decoder,
                nil,
                0,
                &outputBuffer,
                Int32(maxSamples),
                0
            )
        } else {
            decodedCount = packet.withUnsafeBytes { (ptr: UnsafeRawBufferPointer) -> Int32 in
                guard let base = ptr.bindMemory(to: UInt8.self).baseAddress else {
                    return -1
                }
                return opus_decode(
                    decoder,
                    base,
                    Int32(packet.count),
                    &outputBuffer,
                    Int32(maxSamples),
                    0
                )
            }
        }

        guard decodedCount > 0 else {
            throw OpusStreamDecoderError.decodeFailed(Int(decodedCount))
        }

        return Array(outputBuffer.prefix(Int(decodedCount)))
    }
}

public enum OpusStreamDecoderError: Error, CustomStringConvertible {
    case decoderCreateFailed(Int32)
    case decoderNotInitialized
    case decodeFailed(Int)

    public var description: String {
        switch self {
        case .decoderCreateFailed(let code):
            "opus_decoder_create failed (error \(code))"
        case .decoderNotInitialized:
            "Opus decoder was not initialized"
        case .decodeFailed(let code):
            "opus_decode failed (error \(code))"
        }
    }
}
