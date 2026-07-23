import CryptoKit
import Foundation

public enum VKA1Error: Error, Equatable, Sendable {
    case invalidHeader
    case invalidKind
    case invalidPixelFormat
    case invalidEncoding
    case invalidDimensions
    case invalidFrameCount
    case invalidDuration
    case invalidRange
    case invalidHash
    case invalidRLE
    case limitExceeded
}

public enum VKA1Kind: UInt8, Sendable {
    case image = 1
    case animation = 2
    case glyphBitmap = 3
}

public enum VKA1FrameEncoding: UInt8, Sendable {
    case raw = 0
    case rowRLE = 1
}

public struct VKA1Limits: Equatable, Sendable {
    public let maxFrames: UInt16
    public let minFrameDurationMS: UInt16
    public let maxFrameDurationMS: UInt16
    public let maxContainerBytes: UInt32
    public let maxDecodedBytes: UInt32

    public init(maxFrames: UInt16, minFrameDurationMS: UInt16, maxFrameDurationMS: UInt16, maxContainerBytes: UInt32, maxDecodedBytes: UInt32) {
        self.maxFrames = maxFrames
        self.minFrameDurationMS = minFrameDurationMS
        self.maxFrameDurationMS = maxFrameDurationMS
        self.maxContainerBytes = maxContainerBytes
        self.maxDecodedBytes = maxDecodedBytes
    }
}

public struct VKA1SourceFrame: Equatable, Sendable {
    public let pixels: [UInt16]
    public let durationMS: UInt16

    public init(pixels: [UInt16], durationMS: UInt16) {
        self.pixels = pixels
        self.durationMS = durationMS
    }
}

public struct VKA1Frame: Equatable, Sendable {
    public let encoding: VKA1FrameEncoding
    public let durationMS: UInt16
    public let pixels: [UInt16]
}

public struct VKA1Container: Equatable, Sendable {
    public let kind: VKA1Kind
    public let width: UInt16
    public let height: UInt16
    public let frames: [VKA1Frame]
    public let sha256: String
}

public enum VKA1Codec {
    public static func encode(kind: VKA1Kind, width: UInt16, height: UInt16, frames: [VKA1SourceFrame], limits: VKA1Limits) throws -> Data {
        let pixelCount = try checkedGeometry(width: width, height: height, limits: limits)
        guard !frames.isEmpty, frames.count <= Int(limits.maxFrames), frames.count <= Int(UInt16.max) else { throw VKA1Error.invalidFrameCount }
        try validateKind(kind, frames: frames, limits: limits)

        var encoded: [(Data, VKA1FrameEncoding, UInt16)] = []
        encoded.reserveCapacity(frames.count)
        var encodingBits: UInt8 = 0
        for frame in frames {
            guard frame.pixels.count == pixelCount else { throw VKA1Error.invalidDimensions }
            let raw = rawData(frame.pixels)
            let rle = try rowRLE(frame.pixels, width: Int(width), height: Int(height))
            let choice: (Data, VKA1FrameEncoding) = rle.count < raw.count ? (rle, .rowRLE) : (raw, .raw)
            encodingBits |= choice.1 == .raw ? 1 : 2
            encoded.append((choice.0, choice.1, frame.durationMS))
        }

        let (tableBytes, tableOverflow) = frames.count.multipliedReportingOverflow(by: 12)
        let (headerBytes, headerOverflow) = 56.addingReportingOverflow(tableBytes)
        guard !tableOverflow, !headerOverflow, headerBytes <= Int(UInt16.max) else { throw VKA1Error.limitExceeded }
        var total = headerBytes
        for frame in encoded {
            let (next, overflow) = total.addingReportingOverflow(frame.0.count)
            guard !overflow, next <= Int(limits.maxContainerBytes), next <= Int(UInt32.max) else { throw VKA1Error.limitExceeded }
            total = next
        }

        var data = Data()
        data.reserveCapacity(total)
        data.append(contentsOf: [0x56, 0x4b, 0x41, 0x31, kind.rawValue, 1, encodingBits, 0])
        appendLE(width, to: &data)
        appendLE(height, to: &data)
        appendLE(UInt16(frames.count), to: &data)
        appendLE(UInt16(headerBytes), to: &data)
        appendLE(UInt32(pixelCount * 2), to: &data)
        appendLE(UInt32(total), to: &data)
        data.append(Data(repeating: 0, count: 32))

        var offset = headerBytes
        for frame in encoded {
            appendLE(UInt32(offset), to: &data)
            appendLE(UInt32(frame.0.count), to: &data)
            appendLE(frame.2, to: &data)
            data.append(frame.1.rawValue)
            data.append(0)
            offset += frame.0.count
        }
        for frame in encoded { data.append(frame.0) }
        let digest = SHA256.hash(data: data)
        data.replaceSubrange(24..<56, with: Data(digest))
        _ = try decode(data, limits: limits)
        return data
    }

    public static func decode(_ data: Data, limits: VKA1Limits) throws -> VKA1Container {
        guard data.count >= 68, data.count <= Int(limits.maxContainerBytes), data.prefix(4) == Data([0x56, 0x4b, 0x41, 0x31]) else { throw VKA1Error.invalidHeader }
        guard let kind = VKA1Kind(rawValue: data[4]) else { throw VKA1Error.invalidKind }
        guard data[5] == 1, data[7] == 0, data[6] & ~UInt8(3) == 0 else { throw VKA1Error.invalidEncoding }
        let width = read16(data, 8), height = read16(data, 10), frameCount = read16(data, 12), headerBytes = read16(data, 14)
        let decodedBytes = read32(data, 16), totalBytes = read32(data, 20)
        let pixelCount = try checkedGeometry(width: width, height: height, limits: limits)
        guard frameCount > 0, frameCount <= limits.maxFrames else { throw VKA1Error.invalidFrameCount }
        let (aggregateDecodedBytes, aggregateOverflow) = Int(decodedBytes).multipliedReportingOverflow(by: Int(frameCount))
        guard !aggregateOverflow, aggregateDecodedBytes <= Int(limits.maxDecodedBytes) else {
            throw VKA1Error.limitExceeded
        }
        let expectedHeader = 56 + Int(frameCount) * 12
        guard Int(headerBytes) == expectedHeader, totalBytes == UInt32(data.count), decodedBytes == UInt32(pixelCount * 2) else { throw VKA1Error.invalidHeader }
        var zeroed = data
        let expectedHash = data[24..<56]
        zeroed.replaceSubrange(24..<56, with: repeatElement(UInt8(0), count: 32))
        guard Data(SHA256.hash(data: zeroed)) == expectedHash else { throw VKA1Error.invalidHash }

        var entries: [(Int, Int, UInt16, VKA1FrameEncoding)] = []
        var expectedOffset = expectedHeader
        var union: UInt8 = 0
        for index in 0..<Int(frameCount) {
            let base = 56 + index * 12
            let offset = Int(read32(data, base)), length = Int(read32(data, base + 4)), duration = read16(data, base + 8)
            guard let encoding = VKA1FrameEncoding(rawValue: data[base + 10]), data[base + 11] == 0 else { throw VKA1Error.invalidEncoding }
            guard offset == expectedOffset, length > 0 else { throw VKA1Error.invalidRange }
            let (end, overflow) = offset.addingReportingOverflow(length)
            guard !overflow, end <= data.count else { throw VKA1Error.invalidRange }
            expectedOffset = end
            union |= encoding == .raw ? 1 : 2
            entries.append((offset, length, duration, encoding))
        }
        guard expectedOffset == data.count, union == data[6] else { throw VKA1Error.invalidRange }
        try validateDurations(kind, entries.map(\.2), limits: limits)

        var frames: [VKA1Frame] = []
        frames.reserveCapacity(entries.count)
        for entry in entries {
            let payload = data[entry.0..<(entry.0 + entry.1)]
            let pixels: [UInt16]
            switch entry.3 {
            case .raw:
                guard payload.count == pixelCount * 2 else { throw VKA1Error.invalidRange }
                pixels = stride(from: payload.startIndex, to: payload.endIndex, by: 2).map { UInt16(payload[$0]) | UInt16(payload[$0 + 1]) << 8 }
                let canonicalRLEBytes = try rowRLE(pixels, width: Int(width), height: Int(height)).count
                guard canonicalRLEBytes >= payload.count else { throw VKA1Error.invalidEncoding }
            case .rowRLE:
                pixels = try decodeRLE(payload, width: Int(width), height: Int(height))
                guard payload.count < pixelCount * 2 else { throw VKA1Error.invalidEncoding }
            }
            frames.append(VKA1Frame(encoding: entry.3, durationMS: entry.2, pixels: pixels))
        }
        return VKA1Container(kind: kind, width: width, height: height, frames: frames, sha256: expectedHash.map { String(format: "%02x", $0) }.joined())
    }

    private static func checkedGeometry(width: UInt16, height: UInt16, limits: VKA1Limits) throws -> Int {
        guard (1...428).contains(width), (1...142).contains(height) else { throw VKA1Error.invalidDimensions }
        let pixels = Int(width) * Int(height)
        guard pixels <= Int(UInt32.max / 2), pixels * 2 <= Int(limits.maxDecodedBytes) else { throw VKA1Error.limitExceeded }
        return pixels
    }

    private static func validateKind(_ kind: VKA1Kind, frames: [VKA1SourceFrame], limits: VKA1Limits) throws {
        try validateDurations(kind, frames.map(\.durationMS), limits: limits)
    }

    private static func validateDurations(_ kind: VKA1Kind, _ durations: [UInt16], limits: VKA1Limits) throws {
        switch kind {
        case .image, .glyphBitmap:
            guard durations.count == 1, durations[0] == 0 else { throw VKA1Error.invalidDuration }
        case .animation:
            guard durations.allSatisfy({ $0 > 0 && $0 >= limits.minFrameDurationMS && $0 <= limits.maxFrameDurationMS }) else { throw VKA1Error.invalidDuration }
        }
    }

    private static func rawData(_ pixels: [UInt16]) -> Data {
        var data = Data(); data.reserveCapacity(pixels.count * 2)
        for pixel in pixels { appendLE(pixel, to: &data) }
        return data
    }

    private static func rowRLE(_ pixels: [UInt16], width: Int, height: Int) throws -> Data {
        guard width > 0, height > 0, pixels.count == width * height, width <= Int(UInt16.max) else { throw VKA1Error.invalidDimensions }
        var data = Data()
        for row in 0..<height {
            var column = 0
            while column < width {
                let pixel = pixels[row * width + column]
                var end = column + 1
                while end < width, pixels[row * width + end] == pixel { end += 1 }
                guard let count = UInt16(exactly: end - column) else { throw VKA1Error.limitExceeded }
                appendLE(count, to: &data); appendLE(pixel, to: &data)
                column = end
            }
        }
        return data
    }

    private static func decodeRLE(_ payload: Data.SubSequence, width: Int, height: Int) throws -> [UInt16] {
        var pixels: [UInt16] = []; pixels.reserveCapacity(width * height)
        var index = payload.startIndex
        for _ in 0..<height {
            var columns = 0
            var prior: UInt16?
            while columns < width {
                guard index + 4 <= payload.endIndex else { throw VKA1Error.invalidRLE }
                let count = Int(UInt16(payload[index]) | UInt16(payload[index + 1]) << 8)
                let pixel = UInt16(payload[index + 2]) | UInt16(payload[index + 3]) << 8
                guard count > 0, count <= width - columns, prior != pixel else { throw VKA1Error.invalidRLE }
                pixels.append(contentsOf: repeatElement(pixel, count: count))
                columns += count; prior = pixel; index += 4
            }
        }
        guard index == payload.endIndex else { throw VKA1Error.invalidRLE }
        return pixels
    }

    private static func appendLE(_ value: UInt16, to data: inout Data) { data.append(UInt8(truncatingIfNeeded: value)); data.append(UInt8(truncatingIfNeeded: value >> 8)) }
    private static func appendLE(_ value: UInt32, to data: inout Data) { for shift in stride(from: 0, through: 24, by: 8) { data.append(UInt8(truncatingIfNeeded: value >> UInt32(shift))) } }
    private static func read16(_ data: Data, _ offset: Int) -> UInt16 { UInt16(data[offset]) | UInt16(data[offset + 1]) << 8 }
    private static func read32(_ data: Data, _ offset: Int) -> UInt32 { UInt32(data[offset]) | UInt32(data[offset + 1]) << 8 | UInt32(data[offset + 2]) << 16 | UInt32(data[offset + 3]) << 24 }
}
