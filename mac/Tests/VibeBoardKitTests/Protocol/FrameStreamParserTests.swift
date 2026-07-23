import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Incremental frame stream parsing")
struct FrameStreamParserTests {
    @Test func acceptsEverySingleSplitBoundary() throws {
        let frame = manualStateFrame(#"{"event":"button_click","button":"k2"}"#)
        for split in 0...frame.count {
            var parser = try FrameStreamParser()
            var events: [FrameStreamEvent] = []
            events += try parser.append(frame.prefix(split))
            events += try parser.append(frame.dropFirst(split))
            #expect(events == [.frame(RawFrame(type: .state, bytes: frame))])
            #expect(parser.bufferedByteCount == 0)
        }
    }

    @Test func acceptsOneByteReadsAndMultipleFramesInOneRead() throws {
        let state = manualStateFrame(#"{"event":"ping"}"#)
        let audio = manualAudioFrame(payload: [0x11, 0x22])
        var byteParser = try FrameStreamParser()
        var byteEvents: [FrameStreamEvent] = []
        for byte in state + audio {
            byteEvents += try byteParser.append([byte])
        }
        #expect(byteEvents == [
            .frame(RawFrame(type: .state, bytes: state)),
            .frame(RawFrame(type: .audio, bytes: audio)),
        ])

        var combinedParser = try FrameStreamParser()
        #expect(try combinedParser.append(state + audio) == byteEvents)
    }

    @Test func supportsIrregularReadChunks() throws {
        let expected = [
            manualStateFrame(#"{"event":"one"}"#),
            manualAudioFrame(payload: [1, 2, 3, 4, 5]),
            manualStateFrame(#"{"event":"three"}"#),
        ]
        let stream = expected.reduce(into: Data(), { $0.append($1) })
        let sizes = [1, 7, 2, 13, 3, 29, 5, 11]
        var parser = try FrameStreamParser()
        var events: [FrameStreamEvent] = []
        var offset = 0
        var index = 0
        while offset < stream.count {
            let end = min(stream.count, offset + sizes[index % sizes.count])
            events += try parser.append(stream.subdata(in: offset..<end))
            offset = end
            index += 1
        }
        #expect(events == expected.map { bytes in
            let type = bytes[1] == FrameType.audio.rawValue ? FrameType.audio : .state
            return .frame(RawFrame(type: type, bytes: bytes))
        })
    }

    @Test func discardsExactlyOneByteForInvalidVersionThenResynchronizes() throws {
        let frame = manualStateFrame(#"{"event":"ok"}"#)
        var parser = try FrameStreamParser()
        let events = try parser.append(Data([0xff]) + frame)
        #expect(events == [
            .discardedByte(0xff, reason: .invalidVersion(0xff)),
            .frame(RawFrame(type: .state, bytes: frame)),
        ])
    }

    @Test func discardsExactlyOneByteForUnknownTypeThenResynchronizes() throws {
        let frame = manualStateFrame(#"{"event":"ok"}"#)
        var parser = try FrameStreamParser()
        let events = try parser.append(Data([0x01, 0x99, 0x00, 0x00]) + frame)
        #expect(Array(events.prefix(4)) == [
            .discardedByte(0x01, reason: .unknownType(0x99)),
            .discardedByte(0x99, reason: .invalidVersion(0x99)),
            .discardedByte(0x00, reason: .invalidVersion(0x00)),
            .discardedByte(0x00, reason: .invalidVersion(0x00)),
        ])
        #expect(events.last == .frame(RawFrame(type: .state, bytes: frame)))
    }

    @Test func oversizedLengthDiscardsOneByteThenFindsFollowingFrame() throws {
        let valid = manualStateFrame(#"{"event":"ok"}"#)
        var parser = try FrameStreamParser()
        let events = try parser.append(Data([0x01, 0x10, 0xfd, 0x0f]) + valid)
        #expect(events.first == .discardedByte(0x01, reason: .frameTooLarge(4097)))
        #expect(events.last == .frame(RawFrame(type: .state, bytes: valid)))
    }

    @Test func retainsIncompleteOrdinaryFrame() throws {
        let frame = manualStateFrame(#"{"event":"waiting"}"#)
        var parser = try FrameStreamParser()
        #expect(try parser.append(frame.dropLast()).isEmpty)
        #expect(parser.bufferedByteCount == frame.count - 1)
        #expect(try parser.append([frame.last!]) == [.frame(RawFrame(type: .state, bytes: frame))])
    }

    @Test func retainsAudioUntilFixedHeaderAndPayloadAreComplete() throws {
        let frame = manualAudioFrame(payload: [0xaa, 0xbb, 0xcc])
        var parser = try FrameStreamParser()
        #expect(try parser.append(frame.prefix(15)).isEmpty)
        #expect(parser.bufferedByteCount == 15)
        #expect(try parser.append(frame.subdata(in: 15..<18)).isEmpty)
        #expect(parser.bufferedByteCount == 18)
        #expect(try parser.append(frame.suffix(1)) == [.frame(RawFrame(type: .audio, bytes: frame))])
    }

    @Test func acceptsMaximumOrdinaryAndAudioFrameLengths() throws {
        let ordinary = Data([1, 16, 0xfc, 0x0f]) + Data(repeating: 0x61, count: 4092)
        var ordinaryParser = try FrameStreamParser()
        #expect(try ordinaryParser.append(ordinary) == [.frame(RawFrame(type: .state, bytes: ordinary))])

        var audio = Data([
            1, 1, 16, 0,
            0, 0, 0, 0,
            0, 0, 0, 0,
            1, 0, 0xf0, 0x0f,
        ])
        audio.append(Data(repeating: 0x7f, count: 4080))
        var audioParser = try FrameStreamParser()
        #expect(try audioParser.append(audio) == [.frame(RawFrame(type: .audio, bytes: audio))])
    }

    @Test func rejectsInvalidReceiveLimitAtInitialization() {
        #expect(throws: ProtocolError.receiveBufferLimitExceeded(limit: 15, attempted: 16)) {
            try FrameStreamParser(receiveBufferLimit: 15)
        }
    }

    @Test func rejectsSingleAppendBeyondLimitBeforeDiagnosticsOrGrowth() throws {
        var parser = try FrameStreamParser(receiveBufferLimit: 16)
        #expect(throws: ProtocolError.receiveBufferLimitExceeded(limit: 16, attempted: 17)) {
            try parser.append([UInt8](repeating: 0xff, count: 17))
        }
        #expect(parser.bufferedByteCount == 0)
    }

    @Test func rejectsDeclaredFrameBeyondConfiguredReceiveLimit() throws {
        var parser = try FrameStreamParser(receiveBufferLimit: 32)
        #expect(throws: ProtocolError.receiveBufferLimitExceeded(limit: 32, attempted: 33)) {
            try parser.append([1, 16, 29, 0])
        }
        #expect(parser.bufferedByteCount == 4)
    }
}

private func manualStateFrame(_ json: String) -> Data {
    let body = Data(json.utf8)
    precondition(body.count <= 4092)
    return Data([1, 16, UInt8(body.count & 0xff), UInt8((body.count >> 8) & 0xff)]) + body
}

private func manualAudioFrame(payload: [UInt8]) -> Data {
    precondition(payload.count <= 4080)
    return Data([
        1, 1, 16, 0,
        0x04, 0x03, 0x02, 0x01,
        0x08, 0x07, 0x06, 0x05,
        1, 0xa5,
        UInt8(payload.count & 0xff), UInt8((payload.count >> 8) & 0xff),
    ] + payload)
}
