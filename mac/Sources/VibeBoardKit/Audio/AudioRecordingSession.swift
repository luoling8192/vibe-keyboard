import Foundation

public enum AudioRecordingState: Equatable, Sendable {
    case ready
    case recording(session: UInt32, nextSequence: UInt32, packetCount: UInt32)
    case finalizing(session: UInt32)
    case completed(session: UInt32, packetCount: UInt32)
    case cancelled
    case failed(AudioRecordingError)
}

public enum AudioRecordingError: Error, Equatable, Sendable {
    case alreadyCompleted
    case cancelled
    case missingFirstFlag(flags: UInt8)
    case invalidFirstSequence(UInt32)
    case unsupportedFlags(UInt8)
    case emptyAudioPacket(sequence: UInt32)
    case audioPacketTooLarge(sequence: UInt32, actual: Int, maximum: Int)
    case noAudio(session: UInt32)
    case sessionChanged(expected: UInt32, actual: UInt32)
    case sequenceGap(expected: UInt32, actual: UInt32)
    case duplicateSequence(UInt32)
    case sequenceRegression(expected: UInt32, actual: UInt32)
    case sequenceOverflow
    case output(String)
}

public final class AudioRecordingSession {
    public static let maximumOpusPacketSize = 220

    public private(set) var state: AudioRecordingState = .ready

    private let muxer: OggOpusMuxer
    private var session: UInt32?
    private var nextSequence: UInt32 = 0
    private var packetCount: UInt32 = 0

    public init(sink: any OggPageSink) {
        self.muxer = OggOpusMuxer(sink: sink)
    }

    public func consume(_ frame: AudioFrame) throws {
        switch state {
        case .completed:
            throw AudioRecordingError.alreadyCompleted
        case .cancelled:
            throw AudioRecordingError.cancelled
        case .failed(let error):
            throw error
        case .finalizing:
            throw AudioRecordingError.alreadyCompleted
        case .ready, .recording:
            break
        }

        do {
            try validateKnownFlags(frame.flags)
            try validatePayload(frame)
            if session == nil {
                try begin(with: frame)
            } else {
                try consumeInSession(frame)
            }
        } catch let error as AudioRecordingError {
            state = .failed(error)
            muxer.cancel()
            throw error
        } catch {
            let wrapped = AudioRecordingError.output(String(describing: error))
            state = .failed(wrapped)
            muxer.cancel()
            throw wrapped
        }
    }

    public func cancel() {
        switch state {
        case .completed, .cancelled:
            return
        default:
            muxer.cancel()
            state = .cancelled
        }
    }

    private func validateKnownFlags(_ flags: UInt8) throws {
        guard flags & ~UInt8(0x03) == 0, flags != 0x03 else {
            throw AudioRecordingError.unsupportedFlags(flags)
        }
    }

    private func validatePayload(_ frame: AudioFrame) throws {
        guard frame.payload.count <= Self.maximumOpusPacketSize else {
            throw AudioRecordingError.audioPacketTooLarge(
                sequence: frame.sequence,
                actual: frame.payload.count,
                maximum: Self.maximumOpusPacketSize
            )
        }
    }

    private func begin(with frame: AudioFrame) throws {
        guard frame.flags & 0x01 != 0 else {
            if frame.flags & 0x02 != 0, frame.payload.isEmpty {
                throw AudioRecordingError.noAudio(session: frame.session)
            }
            throw AudioRecordingError.missingFirstFlag(flags: frame.flags)
        }
        guard frame.sequence == 0 else {
            throw AudioRecordingError.invalidFirstSequence(frame.sequence)
        }
        guard !frame.payload.isEmpty else {
            throw AudioRecordingError.emptyAudioPacket(sequence: frame.sequence)
        }

        session = frame.session
        try muxer.append(opusPacket: frame.payload)
        packetCount = 1
        nextSequence = 1
        state = .recording(session: frame.session, nextSequence: nextSequence, packetCount: packetCount)
    }

    private func consumeInSession(_ frame: AudioFrame) throws {
        guard let session else { return }
        guard frame.session == session else {
            throw AudioRecordingError.sessionChanged(expected: session, actual: frame.session)
        }
        try validateSequence(frame.sequence)

        if frame.flags & 0x02 != 0 {
            state = .finalizing(session: session)
            if frame.payload.isEmpty {
                try muxer.finish()
            } else {
                try muxer.append(opusPacket: frame.payload, isLast: true)
                try incrementPacketCount()
            }
            try muxer.commit()
            state = .completed(session: session, packetCount: packetCount)
            return
        }

        guard frame.flags & 0x01 == 0 else {
            throw AudioRecordingError.unsupportedFlags(frame.flags)
        }
        guard !frame.payload.isEmpty else {
            throw AudioRecordingError.emptyAudioPacket(sequence: frame.sequence)
        }

        try muxer.append(opusPacket: frame.payload)
        try incrementPacketCount()
        try incrementSequence()
        state = .recording(session: session, nextSequence: nextSequence, packetCount: packetCount)
    }

    private func validateSequence(_ actual: UInt32) throws {
        if actual == nextSequence { return }
        if nextSequence > 0, actual == nextSequence - 1 {
            throw AudioRecordingError.duplicateSequence(actual)
        }
        if actual < nextSequence {
            throw AudioRecordingError.sequenceRegression(expected: nextSequence, actual: actual)
        }
        throw AudioRecordingError.sequenceGap(expected: nextSequence, actual: actual)
    }

    private func incrementSequence() throws {
        let (next, overflow) = nextSequence.addingReportingOverflow(1)
        guard !overflow else { throw AudioRecordingError.sequenceOverflow }
        nextSequence = next
    }

    private func incrementPacketCount() throws {
        let (next, overflow) = packetCount.addingReportingOverflow(1)
        guard !overflow else { throw AudioRecordingError.sequenceOverflow }
        packetCount = next
    }
}
