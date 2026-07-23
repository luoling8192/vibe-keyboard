import Foundation

public struct FrameStreamParser: Sendable {
    public static let protocolVersion: UInt8 = 0x01
    public static let maximumFrameLength = 4096

    private var buffer: [UInt8] = []
    public let receiveBufferLimit: Int

    public var bufferedByteCount: Int { buffer.count }

    public init(receiveBufferLimit: Int = Self.maximumFrameLength) throws {
        guard receiveBufferLimit >= 16 else {
            throw ProtocolError.receiveBufferLimitExceeded(
                limit: receiveBufferLimit,
                attempted: 16
            )
        }
        self.receiveBufferLimit = receiveBufferLimit
    }

    public mutating func append(_ data: Data) throws -> [FrameStreamEvent] {
        try append([UInt8](data))
    }

    public mutating func append(_ bytes: [UInt8]) throws -> [FrameStreamEvent] {
        guard bytes.count <= receiveBufferLimit else {
            throw ProtocolError.receiveBufferLimitExceeded(
                limit: receiveBufferLimit,
                attempted: bytes.count
            )
        }

        var events: [FrameStreamEvent] = []
        events.reserveCapacity(min(bytes.count, receiveBufferLimit))
        var offset = 0

        while offset < bytes.count {
            if buffer.count == receiveBufferLimit {
                events.append(contentsOf: try drain())
                guard buffer.count < receiveBufferLimit else {
                    throw ProtocolError.receiveBufferLimitExceeded(
                        limit: receiveBufferLimit,
                        attempted: buffer.count + bytes.count - offset
                    )
                }
            }

            let writableCount = receiveBufferLimit - buffer.count
            let count = min(writableCount, bytes.count - offset)
            buffer.append(contentsOf: bytes[offset..<(offset + count)])
            offset += count
            events.append(contentsOf: try drain())
        }

        return events
    }

    private mutating func drain() throws -> [FrameStreamEvent] {
        var events: [FrameStreamEvent] = []

        while buffer.count >= 4 {
            guard buffer[0] == Self.protocolVersion else {
                let byte = buffer.removeFirst()
                events.append(.discardedByte(byte, reason: .invalidVersion(byte)))
                continue
            }

            guard let type = FrameType(rawValue: buffer[1]) else {
                let unknownType = buffer[1]
                let version = buffer.removeFirst()
                events.append(.discardedByte(version, reason: .unknownType(unknownType)))
                continue
            }

            let totalLength: Int
            if type == .audio {
                guard buffer.count >= 16 else { break }
                totalLength = 16 + Int(readUInt16LE(buffer, at: 14))
            } else {
                totalLength = 4 + Int(readUInt16LE(buffer, at: 2))
            }

            guard totalLength <= Self.maximumFrameLength else {
                let byte = buffer.removeFirst()
                events.append(.discardedByte(byte, reason: .frameTooLarge(totalLength)))
                continue
            }

            guard totalLength <= receiveBufferLimit else {
                throw ProtocolError.receiveBufferLimitExceeded(
                    limit: receiveBufferLimit,
                    attempted: totalLength
                )
            }

            guard buffer.count >= totalLength else { break }

            let frameBytes = Data(buffer.prefix(totalLength))
            buffer.removeFirst(totalLength)
            events.append(.frame(RawFrame(type: type, bytes: frameBytes)))
        }

        return events
    }
}

@inline(__always)
func readUInt16LE(_ bytes: [UInt8], at offset: Int) -> UInt16 {
    UInt16(bytes[offset]) | (UInt16(bytes[offset + 1]) << 8)
}

@inline(__always)
func readUInt32LE(_ bytes: [UInt8], at offset: Int) -> UInt32 {
    UInt32(bytes[offset])
        | (UInt32(bytes[offset + 1]) << 8)
        | (UInt32(bytes[offset + 2]) << 16)
        | (UInt32(bytes[offset + 3]) << 24)
}
