import Foundation

public struct OggPageHeaderType: OptionSet, Sendable {
    public let rawValue: UInt8

    public init(rawValue: UInt8) {
        self.rawValue = rawValue
    }

    public static let continuation = Self(rawValue: 0x01)
    public static let beginningOfStream = Self(rawValue: 0x02)
    public static let endOfStream = Self(rawValue: 0x04)
}

public enum OggOpusError: Error, Equatable, Sendable {
    case emptyPacket
    case packetTooLarge(Int)
    case alreadyFinished
    case sequenceOverflow
    case granuleOverflow
    case outputFailure(String)
}

public protocol OggPageSink: AnyObject {
    func write(_ data: Data) throws
    func commit() throws
    func cancel()
}

public final class OggOpusMuxer {
    public static let streamSerial: UInt32 = 0x5653_544b
    public static let granuleIncrement: UInt64 = 2_880

    private let sink: any OggPageSink
    private var pageSequence: UInt32 = 0
    private var granulePosition: UInt64 = 0
    private var wroteHeaders = false
    private var finished = false

    public init(sink: any OggPageSink) {
        self.sink = sink
    }

    public func append(opusPacket: Data, isLast: Bool = false) throws {
        guard !finished else { throw OggOpusError.alreadyFinished }
        guard !opusPacket.isEmpty else { throw OggOpusError.emptyPacket }
        try writeHeadersIfNeeded()

        let (nextGranule, overflow) = granulePosition.addingReportingOverflow(Self.granuleIncrement)
        guard !overflow else { throw OggOpusError.granuleOverflow }
        granulePosition = nextGranule

        let headerType: OggPageHeaderType = isLast ? .endOfStream : []
        try writePage(packet: opusPacket, granule: granulePosition, headerType: headerType)
        if isLast {
            finished = true
        }
    }

    public func finish() throws {
        guard !finished else { throw OggOpusError.alreadyFinished }
        try writeHeadersIfNeeded()
        try writePage(packet: Data(), granule: granulePosition, headerType: .endOfStream)
        finished = true
    }

    public func commit() throws {
        if !finished {
            try finish()
        }
        do {
            try sink.commit()
        } catch {
            throw OggOpusError.outputFailure(String(describing: error))
        }
    }

    public func cancel() {
        sink.cancel()
        finished = true
    }

    private func writeHeadersIfNeeded() throws {
        guard !wroteHeaders else { return }
        try writePage(packet: Self.opusHead(), granule: 0, headerType: .beginningOfStream)
        try writePage(packet: Self.opusTags(), granule: 0, headerType: [])
        wroteHeaders = true
    }

    private func writePage(packet: Data, granule: UInt64, headerType: OggPageHeaderType) throws {
        let page = try OggPageEncoder.encode(
            packet: packet,
            granulePosition: granule,
            streamSerial: Self.streamSerial,
            pageSequence: pageSequence,
            headerType: headerType
        )
        do {
            try sink.write(page)
        } catch {
            throw OggOpusError.outputFailure(String(describing: error))
        }
        let (nextSequence, overflow) = pageSequence.addingReportingOverflow(1)
        guard !overflow else { throw OggOpusError.sequenceOverflow }
        pageSequence = nextSequence
    }

    private static func opusHead() -> Data {
        var data = Data("OpusHead".utf8)
        data.append(1)
        data.append(1)
        data.appendLittleEndian(UInt16(312))
        data.appendLittleEndian(UInt32(16_000))
        data.appendLittleEndian(UInt16(0))
        data.append(0)
        return data
    }

    private static func opusTags() -> Data {
        let vendor = Data("VibeBoard".utf8)
        var data = Data("OpusTags".utf8)
        data.appendLittleEndian(UInt32(vendor.count))
        data.append(vendor)
        data.appendLittleEndian(UInt32(0))
        return data
    }
}

struct OggPageEncoder {
    // A complete packet whose length is divisible by 255 needs a zero terminator.
    static let maximumCompletePacketBytes = (254 * 255) + 254

    static func encode(
        packet: Data,
        granulePosition: UInt64,
        streamSerial: UInt32,
        pageSequence: UInt32,
        headerType: OggPageHeaderType
    ) throws -> Data {
        guard packet.count <= maximumCompletePacketBytes else {
            throw OggOpusError.packetTooLarge(packet.count)
        }

        let lacing = makeLacing(packetByteCount: packet.count)
        var page = Data("OggS".utf8)
        page.append(0)
        page.append(headerType.rawValue)
        page.appendLittleEndian(granulePosition)
        page.appendLittleEndian(streamSerial)
        page.appendLittleEndian(pageSequence)
        page.appendLittleEndian(UInt32(0))
        page.append(UInt8(lacing.count))
        page.append(contentsOf: lacing)
        page.append(packet)

        let checksum = crc32(page)
        page.replaceSubrange(22..<26, with: checksum.littleEndianBytes)
        return page
    }

    private static func makeLacing(packetByteCount: Int) -> [UInt8] {
        var remaining = packetByteCount
        var values: [UInt8] = []
        while remaining >= 255 {
            values.append(255)
            remaining -= 255
        }
        values.append(UInt8(remaining))
        return values
    }

    private static func crc32(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0
        for byte in data {
            crc ^= UInt32(byte) << 24
            for _ in 0..<8 {
                crc = (crc & 0x8000_0000) != 0
                    ? (crc << 1) ^ 0x04c1_1db7
                    : crc << 1
            }
        }
        return crc
    }
}

private extension Data {
    mutating func appendLittleEndian<T: FixedWidthInteger>(_ value: T) {
        append(contentsOf: value.littleEndianBytes)
    }
}

private extension FixedWidthInteger {
    var littleEndianBytes: [UInt8] {
        withUnsafeBytes(of: littleEndian) { Array($0) }
    }
}
