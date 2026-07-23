import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Ogg Opus muxer")
struct OggOpusMuxerTests {
    @Test func vendorHeadTagsAndEmptyEOSMatchIndependentGoldenBytes() throws {
        let sink = DataOggPageSink()
        let muxer = OggOpusMuxer(sink: sink)

        try muxer.finish()
        try muxer.commit()

        #expect(sink.data.hexString == Self.emptyStreamGoldenHex)
        #expect(sink.isCommitted)
    }

    @Test func vendorFinalPacketPageMatchesIndependentGoldenBytes() throws {
        let sink = DataOggPageSink()
        let muxer = OggOpusMuxer(sink: sink)

        try muxer.append(opusPacket: Data([0xf8, 0xff, 0xfe]), isLast: true)
        try muxer.commit()

        let finalPage = Data(sink.data.suffix(31))
        #expect(finalPage.hexString == "4f6767530004400b0000000000004b5453560200000071e837af0103f8fffe")
        #expect(readUInt64LE(finalPage, at: 6) == 2_880)
        #expect(finalPage[5] == OggPageHeaderType.endOfStream.rawValue)
    }

    @Test func pagesUseVendorSerialAndMonotonicGranules() throws {
        let sink = DataOggPageSink()
        let muxer = OggOpusMuxer(sink: sink)

        try muxer.append(opusPacket: Data([1]))
        try muxer.append(opusPacket: Data([2]), isLast: true)
        try muxer.commit()

        let pages = splitPages(sink.data)
        #expect(pages.count == 4)
        #expect(pages.map { readUInt32LE($0, at: 14) } == Array(repeating: 0x5653_544b, count: 4))
        #expect(pages.map { readUInt32LE($0, at: 18) } == [0, 1, 2, 3])
        #expect(pages.map { readUInt64LE($0, at: 6) } == [0, 0, 2_880, 5_760])
        #expect(pages[0][5] == OggPageHeaderType.beginningOfStream.rawValue)
        #expect(pages[3][5] == OggPageHeaderType.endOfStream.rawValue)
    }

    @Test func packetLengthDivisibleBy255HasZeroLacingTerminator() throws {
        let page = try OggPageEncoder.encode(
            packet: Data(repeating: 0xaa, count: 255),
            granulePosition: 2_880,
            streamSerial: 1,
            pageSequence: 0,
            headerType: []
        )

        #expect(page[26] == 2)
        #expect(Array(page[27..<29]) == [255, 0])
    }

    @Test func maximumCompletePacketUses255SegmentsAndNextByteIsRejected() throws {
        let maximum = OggPageEncoder.maximumCompletePacketBytes
        let page = try OggPageEncoder.encode(
            packet: Data(repeating: 0x55, count: maximum),
            granulePosition: 0,
            streamSerial: 1,
            pageSequence: 0,
            headerType: []
        )

        #expect(page[26] == 255)
        #expect(Array(page[27..<(27 + 254)]) == Array(repeating: 255, count: 254))
        #expect(page[27 + 254] == 254)
        #expect(throws: OggOpusError.packetTooLarge(maximum + 1)) {
            _ = try OggPageEncoder.encode(
                packet: Data(repeating: 0, count: maximum + 1),
                granulePosition: 0,
                streamSerial: 1,
                pageSequence: 0,
                headerType: []
            )
        }
    }

    @Test func emptyPacketAndPostFinishAppendAreRejected() throws {
        let sink = DataOggPageSink()
        let muxer = OggOpusMuxer(sink: sink)
        #expect(throws: OggOpusError.emptyPacket) {
            try muxer.append(opusPacket: Data())
        }
        try muxer.finish()
        #expect(throws: OggOpusError.alreadyFinished) {
            try muxer.append(opusPacket: Data([1]))
        }
    }

    private static let emptyStreamGoldenHex =
        "4f676753000200000000000000004b54535600000000353284b601134f7075734865616401013801803e0000000000" +
        "4f676753000000000000000000004b545356010000007386174401194f707573546167730900000056696265426f61726400000000" +
        "4f676753000400000000000000004b54535602000000524a33450100"
}

private func splitPages(_ data: Data) -> [Data] {
    var result: [Data] = []
    var offset = 0
    while offset < data.count {
        let segmentCount = Int(data[offset + 26])
        let bodyCount = data[(offset + 27)..<(offset + 27 + segmentCount)].reduce(0) { $0 + Int($1) }
        let pageCount = 27 + segmentCount + bodyCount
        result.append(Data(data[offset..<(offset + pageCount)]))
        offset += pageCount
    }
    return result
}

private func readUInt32LE(_ data: Data, at offset: Int) -> UInt32 {
    UInt32(data[offset])
        | UInt32(data[offset + 1]) << 8
        | UInt32(data[offset + 2]) << 16
        | UInt32(data[offset + 3]) << 24
}

private func readUInt64LE(_ data: Data, at offset: Int) -> UInt64 {
    (0..<8).reduce(0) { $0 | UInt64(data[offset + $1]) << UInt64($1 * 8) }
}

private extension Data {
    var hexString: String { map { String(format: "%02x", $0) }.joined() }
}
