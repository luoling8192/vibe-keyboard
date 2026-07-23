import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Audio recording session")
struct AudioRecordingSessionTests {
    @Test func orderedSessionCompletesOnEmptyEOS() throws {
        let sink = DataOggPageSink()
        let session = AudioRecordingSession(sink: sink)
        try session.consume(frame(session: 7, sequence: 0, flags: 1, payload: [1]))
        try session.consume(frame(session: 7, sequence: 1, flags: 0, payload: [2]))
        try session.consume(frame(session: 7, sequence: 2, flags: 2, payload: []))
        #expect(session.state == .completed(session: 7, packetCount: 2))
        #expect(sink.isCommitted)
    }

    @Test func nonEmptyFinalPayloadIsWrittenAndCounted() throws {
        let sink = DataOggPageSink()
        let session = AudioRecordingSession(sink: sink)
        try session.consume(frame(session: 1, sequence: 0, flags: 1, payload: [1]))
        try session.consume(frame(session: 1, sequence: 1, flags: 2, payload: [2]))
        #expect(session.state == .completed(session: 1, packetCount: 2))
        #expect(splitOggPages(sink.data).last?[5] == OggPageHeaderType.endOfStream.rawValue)
    }

    @Test func initialValidationErrorsAreTyped() {
        assertFirstError(.missingFirstFlag(flags: 0), frame: frame(session: 1, sequence: 0, flags: 0, payload: [1]))
        assertFirstError(.invalidFirstSequence(1), frame: frame(session: 1, sequence: 1, flags: 1, payload: [1]))
        assertFirstError(.emptyAudioPacket(sequence: 0), frame: frame(session: 1, sequence: 0, flags: 1, payload: []))
        assertFirstError(.unsupportedFlags(3), frame: frame(session: 1, sequence: 0, flags: 3, payload: [1]))
        assertFirstError(.unsupportedFlags(4), frame: frame(session: 1, sequence: 0, flags: 4, payload: [1]))
        assertFirstError(.noAudio(session: 1), frame: frame(session: 1, sequence: 0, flags: 2, payload: []))
    }

    @Test func sequenceGapDuplicateRegressionAndSessionReplacementAreTyped() throws {
        let cases: [(AudioFrame, AudioRecordingError)] = [
            (frame(session: 1, sequence: 2, flags: 0, payload: [2]), .sequenceGap(expected: 1, actual: 2)),
            (frame(session: 1, sequence: 0, flags: 0, payload: [2]), .duplicateSequence(0)),
            (frame(session: 2, sequence: 1, flags: 0, payload: [2]), .sessionChanged(expected: 1, actual: 2)),
        ]
        for (badFrame, expected) in cases {
            let session = AudioRecordingSession(sink: DataOggPageSink())
            try session.consume(frame(session: 1, sequence: 0, flags: 1, payload: [1]))
            #expect(throws: expected) { try session.consume(badFrame) }
        }

        let regression = AudioRecordingSession(sink: DataOggPageSink())
        try regression.consume(frame(session: 1, sequence: 0, flags: 1, payload: [1]))
        try regression.consume(frame(session: 1, sequence: 1, flags: 0, payload: [2]))
        #expect(throws: AudioRecordingError.sequenceRegression(expected: 2, actual: 0)) {
            try regression.consume(frame(session: 1, sequence: 0, flags: 0, payload: [3]))
        }
    }

    @Test func emptyOrdinaryPacketAndRepeatedFirstAreRejected() throws {
        let empty = AudioRecordingSession(sink: DataOggPageSink())
        try empty.consume(frame(session: 1, sequence: 0, flags: 1, payload: [1]))
        #expect(throws: AudioRecordingError.emptyAudioPacket(sequence: 1)) {
            try empty.consume(frame(session: 1, sequence: 1, flags: 0, payload: []))
        }

        let repeated = AudioRecordingSession(sink: DataOggPageSink())
        try repeated.consume(frame(session: 1, sequence: 0, flags: 1, payload: [1]))
        #expect(throws: AudioRecordingError.unsupportedFlags(1)) {
            try repeated.consume(frame(session: 1, sequence: 1, flags: 1, payload: [2]))
        }
    }

    @Test func completedAndCancelledSessionsRejectFurtherFrames() throws {
        let completed = AudioRecordingSession(sink: DataOggPageSink())
        try completed.consume(frame(session: 1, sequence: 0, flags: 1, payload: [1]))
        try completed.consume(frame(session: 1, sequence: 1, flags: 2, payload: []))
        #expect(throws: AudioRecordingError.alreadyCompleted) {
            try completed.consume(frame(session: 1, sequence: 2, flags: 0, payload: [2]))
        }

        let cancelled = AudioRecordingSession(sink: DataOggPageSink())
        cancelled.cancel()
        #expect(cancelled.state == .cancelled)
        #expect(throws: AudioRecordingError.cancelled) {
            try cancelled.consume(frame(session: 1, sequence: 0, flags: 1, payload: [1]))
        }
    }

    @Test func maximumPayloadAcceptedForFirstOrdinaryAndFinal() throws {
        let firstSink = CountingSink()
        let first = AudioRecordingSession(sink: firstSink)
        try first.consume(frame(session: 1, sequence: 0, flags: 1, payload: bytes(220)))
        #expect(firstSink.writeCount == 3)

        let ordinarySink = CountingSink()
        let ordinary = AudioRecordingSession(sink: ordinarySink)
        try ordinary.consume(frame(session: 1, sequence: 0, flags: 1, payload: [1]))
        try ordinary.consume(frame(session: 1, sequence: 1, flags: 0, payload: bytes(220)))
        #expect(ordinarySink.writeCount == 4)

        let finalSink = CountingSink()
        let final = AudioRecordingSession(sink: finalSink)
        try final.consume(frame(session: 1, sequence: 0, flags: 1, payload: [1]))
        try final.consume(frame(session: 1, sequence: 1, flags: 2, payload: bytes(220)))
        #expect(final.state == .completed(session: 1, packetCount: 2))
    }

    @Test func oversizedPayloadRejectedBeforeAnyWriteForFirstOrdinaryAndFinal() throws {
        let firstSink = CountingSink()
        let first = AudioRecordingSession(sink: firstSink)
        #expect(throws: AudioRecordingError.audioPacketTooLarge(sequence: 0, actual: 221, maximum: 220)) {
            try first.consume(frame(session: 1, sequence: 0, flags: 1, payload: bytes(221)))
        }
        #expect(firstSink.writeCount == 0)

        for flags: UInt8 in [0, 2] {
            let sink = CountingSink()
            let session = AudioRecordingSession(sink: sink)
            try session.consume(frame(session: 1, sequence: 0, flags: 1, payload: [1]))
            let writesBefore = sink.writeCount
            #expect(throws: AudioRecordingError.audioPacketTooLarge(sequence: 1, actual: 221, maximum: 220)) {
                try session.consume(frame(session: 1, sequence: 1, flags: flags, payload: bytes(221)))
            }
            #expect(sink.writeCount == writesBefore)
        }
    }
}

private final class CountingSink: OggPageSink {
    var writeCount = 0
    func write(_ data: Data) throws { writeCount += 1 }
    func commit() throws {}
    func cancel() {}
}

private func frame(session: UInt32, sequence: UInt32, flags: UInt8, payload: [UInt8]) -> AudioFrame {
    AudioFrame(session: session, sequence: sequence, flags: flags, payload: Data(payload))
}

private func bytes(_ count: Int) -> [UInt8] { Array(repeating: 0xa5, count: count) }

private func assertFirstError(_ expected: AudioRecordingError, frame: AudioFrame) {
    let sink = DataOggPageSink()
    let session = AudioRecordingSession(sink: sink)
    #expect(throws: expected) { try session.consume(frame) }
    #expect(session.state == .failed(expected))
    #expect(sink.isCancelled)
}

private func splitOggPages(_ data: Data) -> [Data] {
    var pages: [Data] = []
    var offset = 0
    while offset < data.count {
        let segmentCount = Int(data[offset + 26])
        let bodyCount = data[(offset + 27)..<(offset + 27 + segmentCount)].reduce(0) { $0 + Int($1) }
        let size = 27 + segmentCount + bodyCount
        pages.append(Data(data[offset..<(offset + size)]))
        offset += size
    }
    return pages
}
