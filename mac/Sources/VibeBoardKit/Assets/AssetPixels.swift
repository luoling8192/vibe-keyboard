import Foundation

public enum AssetConversionError: Error, Equatable, Sendable {
    case invalidDimensions
    case sourceLimitExceeded
    case decodedMemoryLimitExceeded
    case invalidPixelData
    case unsupportedSource
    case decodeFailed
    case invalidFrameDuration
}

public struct AssetRGB888: Equatable, Sendable {
    public let red: UInt8
    public let green: UInt8
    public let blue: UInt8

    public init(red: UInt8, green: UInt8, blue: UInt8) {
        self.red = red
        self.green = green
        self.blue = blue
    }

    public init(packed: UInt32) throws {
        guard packed <= 0xFF_FFFF else { throw AssetConversionError.invalidPixelData }
        red = UInt8((packed >> 16) & 0xff)
        green = UInt8((packed >> 8) & 0xff)
        blue = UInt8(packed & 0xff)
    }

    public var packed: UInt32 { UInt32(red) << 16 | UInt32(green) << 8 | UInt32(blue) }
}

public struct AssetRGBA8: Equatable, Sendable {
    public let red: UInt8
    public let green: UInt8
    public let blue: UInt8
    public let alpha: UInt8

    public init(red: UInt8, green: UInt8, blue: UInt8, alpha: UInt8 = 255) {
        self.red = red
        self.green = green
        self.blue = blue
        self.alpha = alpha
    }
}

public struct AssetRaster: Equatable, Sendable {
    public let width: Int
    public let height: Int
    public let pixels: [AssetRGBA8]

    public init(width: Int, height: Int, pixels: [AssetRGBA8]) throws {
        let (count, overflow) = width.multipliedReportingOverflow(by: height)
        guard width > 0, height > 0, !overflow, pixels.count == count else {
            throw AssetConversionError.invalidDimensions
        }
        self.width = width
        self.height = height
        self.pixels = pixels
    }
}

public struct AssetConversionLimits: Equatable, Sendable {
    public let maxSourceWidth: Int
    public let maxSourceHeight: Int
    public let maxSourcePixels: Int
    public let maxFrames: Int
    public let maxDecodedBytes: Int

    public init(
        maxSourceWidth: Int = 8_192,
        maxSourceHeight: Int = 8_192,
        maxSourcePixels: Int = 16_777_216,
        maxFrames: Int = 256,
        maxDecodedBytes: Int = 128 * 1_024 * 1_024
    ) {
        self.maxSourceWidth = maxSourceWidth
        self.maxSourceHeight = maxSourceHeight
        self.maxSourcePixels = maxSourcePixels
        self.maxFrames = maxFrames
        self.maxDecodedBytes = maxDecodedBytes
    }

    public func validate(width: Int, height: Int, frames: Int = 1, bytesPerPixel: Int = 4) throws {
        guard width > 0, height > 0, width <= maxSourceWidth, height <= maxSourceHeight,
              frames > 0, frames <= maxFrames else { throw AssetConversionError.sourceLimitExceeded }
        let (pixels, pixelOverflow) = width.multipliedReportingOverflow(by: height)
        let (allPixels, frameOverflow) = pixels.multipliedReportingOverflow(by: frames)
        let (bytes, byteOverflow) = allPixels.multipliedReportingOverflow(by: bytesPerPixel)
        guard !pixelOverflow, !frameOverflow, !byteOverflow,
              pixels <= maxSourcePixels, bytes <= maxDecodedBytes else {
            throw AssetConversionError.decodedMemoryLimitExceeded
        }
    }
}

public enum AssetEXIFOrientation: UInt8, Equatable, Sendable {
    case up = 1, upMirrored, down, downMirrored, leftMirrored, right, rightMirrored, left
}

public enum AssetFit: String, Equatable, Sendable {
    case contain, cover, stretch, center
}

public enum AssetPixelConverter {
    public static func applyOrientation(_ source: AssetRaster, _ orientation: AssetEXIFOrientation) throws -> AssetRaster {
        let swapsAxes = orientation == .leftMirrored || orientation == .right || orientation == .rightMirrored || orientation == .left
        let outputWidth = swapsAxes ? source.height : source.width
        let outputHeight = swapsAxes ? source.width : source.height
        var output = Array(repeating: AssetRGBA8(red: 0, green: 0, blue: 0, alpha: 0), count: outputWidth * outputHeight)
        for y in 0..<outputHeight {
            for x in 0..<outputWidth {
                let sourcePoint: (Int, Int)
                switch orientation {
                case .up: sourcePoint = (x, y)
                case .upMirrored: sourcePoint = (source.width - 1 - x, y)
                case .down: sourcePoint = (source.width - 1 - x, source.height - 1 - y)
                case .downMirrored: sourcePoint = (x, source.height - 1 - y)
                case .leftMirrored: sourcePoint = (y, x)
                case .right: sourcePoint = (y, source.height - 1 - x)
                case .rightMirrored: sourcePoint = (source.width - 1 - y, source.height - 1 - x)
                case .left: sourcePoint = (source.width - 1 - y, x)
                }
                output[y * outputWidth + x] = source.pixels[sourcePoint.1 * source.width + sourcePoint.0]
            }
        }
        return try AssetRaster(width: outputWidth, height: outputHeight, pixels: output)
    }

    public static func convert(
        _ source: AssetRaster,
        width targetWidth: Int = 428,
        height targetHeight: Int = 142,
        fit: AssetFit,
        background: AssetRGB888
    ) throws -> [UInt16] {
        guard targetWidth > 0, targetHeight > 0, targetWidth <= 428, targetHeight <= 142 else {
            throw AssetConversionError.invalidDimensions
        }
        let placement = try placement(sourceWidth: source.width, sourceHeight: source.height, targetWidth: targetWidth, targetHeight: targetHeight, fit: fit)
        var output = Array(repeating: rgb565(background), count: targetWidth * targetHeight)
        for y in 0..<placement.height {
            let targetY = placement.y + y
            guard targetY >= 0, targetY < targetHeight else { continue }
            for x in 0..<placement.width {
                let targetX = placement.x + x
                guard targetX >= 0, targetX < targetWidth else { continue }
                let rgba = bilinear(source, x: x, y: y, width: placement.width, height: placement.height)
                output[targetY * targetWidth + targetX] = rgb565(composite(rgba, over: background))
            }
        }
        return output
    }

    public static func rgb565(_ pixel: AssetRGB888) -> UInt16 {
        (UInt16(pixel.red >> 3) << 11) | (UInt16(pixel.green >> 2) << 5) | UInt16(pixel.blue >> 3)
    }

    public static func composite(_ source: AssetRGBA8, over background: AssetRGB888) -> AssetRGB888 {
        let alpha = UInt32(source.alpha)
        func channel(_ foreground: UInt8, _ destination: UInt8) -> UInt8 {
            let value = UInt32(foreground) * alpha + UInt32(destination) * (255 - alpha) + 127
            return UInt8(value / 255)
        }
        return AssetRGB888(
            red: channel(source.red, background.red),
            green: channel(source.green, background.green),
            blue: channel(source.blue, background.blue)
        )
    }

    private struct Placement { let x: Int; let y: Int; let width: Int; let height: Int }

    private static func placement(sourceWidth: Int, sourceHeight: Int, targetWidth: Int, targetHeight: Int, fit: AssetFit) throws -> Placement {
        guard sourceWidth > 0, sourceHeight > 0 else { throw AssetConversionError.invalidDimensions }
        switch fit {
        case .stretch:
            return Placement(x: 0, y: 0, width: targetWidth, height: targetHeight)
        case .center:
            return Placement(x: (targetWidth - sourceWidth) / 2, y: (targetHeight - sourceHeight) / 2, width: sourceWidth, height: sourceHeight)
        case .contain, .cover:
            let widthLimited = Int64(targetWidth) * Int64(sourceHeight)
            let heightLimited = Int64(targetHeight) * Int64(sourceWidth)
            let useWidth = fit == .contain ? widthLimited <= heightLimited : widthLimited >= heightLimited
            let width: Int
            let height: Int
            if useWidth {
                width = targetWidth
                height = max(1, Int((Int64(sourceHeight) * Int64(targetWidth)) / Int64(sourceWidth)))
            } else {
                height = targetHeight
                width = max(1, Int((Int64(sourceWidth) * Int64(targetHeight)) / Int64(sourceHeight)))
            }
            return Placement(x: (targetWidth - width) / 2, y: (targetHeight - height) / 2, width: width, height: height)
        }
    }

    private static func bilinear(_ source: AssetRaster, x: Int, y: Int, width: Int, height: Int) -> AssetRGBA8 {
        let xAxis = axis(position: x, source: source.width, destination: width)
        let yAxis = axis(position: y, source: source.height, destination: height)
        let p00 = source.pixels[yAxis.low * source.width + xAxis.low]
        let p10 = source.pixels[yAxis.low * source.width + xAxis.high]
        let p01 = source.pixels[yAxis.high * source.width + xAxis.low]
        let p11 = source.pixels[yAxis.high * source.width + xAxis.high]
        func sample(_ keyPath: KeyPath<AssetRGBA8, UInt8>) -> UInt8 {
            let top = UInt64(p00[keyPath: keyPath]) * UInt64(xAxis.denominator - xAxis.remainder) + UInt64(p10[keyPath: keyPath]) * UInt64(xAxis.remainder)
            let bottom = UInt64(p01[keyPath: keyPath]) * UInt64(xAxis.denominator - xAxis.remainder) + UInt64(p11[keyPath: keyPath]) * UInt64(xAxis.remainder)
            let denominator = UInt64(xAxis.denominator) * UInt64(yAxis.denominator)
            let numerator = top * UInt64(yAxis.denominator - yAxis.remainder) + bottom * UInt64(yAxis.remainder)
            return UInt8(min(255, (numerator + denominator / 2) / denominator))
        }
        return AssetRGBA8(red: sample(\.red), green: sample(\.green), blue: sample(\.blue), alpha: sample(\.alpha))
    }

    private struct Axis { let low: Int; let high: Int; let remainder: Int; let denominator: Int }

    private static func axis(position: Int, source: Int, destination: Int) -> Axis {
        let denominator = 2 * destination
        let numerator = (2 * position + 1) * source - destination
        if numerator <= 0 { return Axis(low: 0, high: 0, remainder: 0, denominator: denominator) }
        let maximum = (source - 1) * denominator
        if numerator >= maximum { return Axis(low: source - 1, high: source - 1, remainder: 0, denominator: denominator) }
        let low = numerator / denominator
        return Axis(low: low, high: low + 1, remainder: numerator % denominator, denominator: denominator)
    }
}
