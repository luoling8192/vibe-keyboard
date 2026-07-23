import Foundation

struct AnimationFrameMetadata: Equatable, Sendable {
    let x: Int
    let y: Int
    let width: Int
    let height: Int
    let durationMS: UInt16?
    let disposal: AnimationDisposal
    let blend: AnimationBlend
}

enum AnimationMetadataParser {
    static func parse(_ data: Data, type: String?, frameCount: Int) throws -> (width: Int, height: Int, frames: [AnimationFrameMetadata])? {
        let bytes = [UInt8](data)
        if type == "com.compuserve.gif" { return try parseGIF(bytes, frameCount: frameCount) }
        if type == "public.png", bytes.count >= 8, Array(bytes.prefix(8)) == [137, 80, 78, 71, 13, 10, 26, 10] {
            return try parseAPNG(bytes, frameCount: frameCount)
        }
        return nil
    }

    private static func parseGIF(_ bytes: [UInt8], frameCount: Int) throws -> (Int, Int, [AnimationFrameMetadata]) {
        guard bytes.count >= 13, String(decoding: bytes[0..<6], as: UTF8.self).hasPrefix("GIF") else { throw AssetConversionError.decodeFailed }
        let canvasWidth = intLE(bytes, 6), canvasHeight = intLE(bytes, 8)
        guard canvasWidth > 0, canvasHeight > 0 else { throw AssetConversionError.decodeFailed }
        var index = 13
        let packed = bytes[10]
        if packed & 0x80 != 0 { index += 3 * (1 << (Int(packed & 0x07) + 1)) }
        var disposal: AnimationDisposal = .keep
        var duration: UInt16?
        var frames: [AnimationFrameMetadata] = []
        while index < bytes.count {
            switch bytes[index] {
            case 0x21:
                guard index + 1 < bytes.count else { throw AssetConversionError.decodeFailed }
                if bytes[index + 1] == 0xf9 {
                    guard index + 7 < bytes.count, bytes[index + 2] == 4, bytes[index + 7] == 0 else { throw AssetConversionError.decodeFailed }
                    let code = (bytes[index + 3] >> 2) & 0x07
                    disposal = code == 2 ? .background : (code == 3 ? .previous : .keep)
                    duration = UInt16(intLE(bytes, index + 4) * 10)
                    index += 8
                } else {
                    index += 2
                    try skipSubblocks(bytes, index: &index)
                }
            case 0x2c:
                guard index + 9 < bytes.count else { throw AssetConversionError.decodeFailed }
                let x = intLE(bytes, index + 1), y = intLE(bytes, index + 3)
                let width = intLE(bytes, index + 5), height = intLE(bytes, index + 7)
                let descriptorPacked = bytes[index + 9]
                index += 10
                if descriptorPacked & 0x80 != 0 { index += 3 * (1 << (Int(descriptorPacked & 0x07) + 1)) }
                guard index < bytes.count else { throw AssetConversionError.decodeFailed }
                index += 1
                try skipSubblocks(bytes, index: &index)
                frames.append(AnimationFrameMetadata(x: x, y: y, width: width, height: height, durationMS: duration, disposal: disposal, blend: .over))
                disposal = .keep
                duration = nil
            case 0x3b:
                index = bytes.count
            default:
                throw AssetConversionError.decodeFailed
            }
        }
        guard frames.count == frameCount else { throw AssetConversionError.decodeFailed }
        return (canvasWidth, canvasHeight, frames)
    }

    private static func parseAPNG(_ bytes: [UInt8], frameCount: Int) throws -> (Int, Int, [AnimationFrameMetadata]) {
        var index = 8
        var canvasWidth = 0, canvasHeight = 0
        var frames: [AnimationFrameMetadata] = []
        var sawAnimation = false
        while index < bytes.count {
            guard index + 12 <= bytes.count else { throw AssetConversionError.decodeFailed }
            let length = intBE32(bytes, index)
            let (end, overflow) = index.addingReportingOverflow(12 + length)
            guard !overflow, length >= 0, end <= bytes.count else { throw AssetConversionError.decodeFailed }
            let type = String(decoding: bytes[(index + 4)..<(index + 8)], as: UTF8.self)
            let payload = index + 8
            if type == "IHDR" {
                guard length == 13 else { throw AssetConversionError.decodeFailed }
                canvasWidth = intBE32(bytes, payload)
                canvasHeight = intBE32(bytes, payload + 4)
            } else if type == "acTL" {
                sawAnimation = true
            } else if type == "fcTL" {
                guard length == 26 else { throw AssetConversionError.decodeFailed }
                let width = intBE32(bytes, payload + 4), height = intBE32(bytes, payload + 8)
                let x = intBE32(bytes, payload + 12), y = intBE32(bytes, payload + 16)
                let numerator = intBE16(bytes, payload + 20)
                let denominator = max(1, intBE16(bytes, payload + 22))
                let milliseconds = (numerator * 1_000 + denominator / 2) / denominator
                guard let duration = UInt16(exactly: milliseconds) else { throw AssetConversionError.invalidFrameDuration }
                let disposal: AnimationDisposal
                switch bytes[payload + 24] {
                case 0: disposal = .keep
                case 1: disposal = .background
                case 2: disposal = .previous
                default: throw AssetConversionError.decodeFailed
                }
                let blend: AnimationBlend
                switch bytes[payload + 25] {
                case 0: blend = .source
                case 1: blend = .over
                default: throw AssetConversionError.decodeFailed
                }
                frames.append(AnimationFrameMetadata(x: x, y: y, width: width, height: height, durationMS: duration, disposal: disposal, blend: blend))
            }
            index = end
        }
        guard sawAnimation, canvasWidth > 0, canvasHeight > 0, frames.count == frameCount else { throw AssetConversionError.decodeFailed }
        return (canvasWidth, canvasHeight, frames)
    }

    private static func skipSubblocks(_ bytes: [UInt8], index: inout Int) throws {
        while true {
            guard index < bytes.count else { throw AssetConversionError.decodeFailed }
            let count = Int(bytes[index]); index += 1
            guard count != 0 else { return }
            guard index <= bytes.count - count else { throw AssetConversionError.decodeFailed }
            index += count
        }
    }

    private static func intLE(_ bytes: [UInt8], _ index: Int) -> Int {
        Int(bytes[index]) | Int(bytes[index + 1]) << 8
    }

    private static func intBE16(_ bytes: [UInt8], _ index: Int) -> Int {
        Int(bytes[index]) << 8 | Int(bytes[index + 1])
    }

    private static func intBE32(_ bytes: [UInt8], _ index: Int) -> Int {
        Int(bytes[index]) << 24 | Int(bytes[index + 1]) << 16 | Int(bytes[index + 2]) << 8 | Int(bytes[index + 3])
    }
}
