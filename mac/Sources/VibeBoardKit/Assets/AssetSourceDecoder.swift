import CoreGraphics
import Foundation
import ImageIO

public struct DecodedAssetFrame: Equatable, Sendable {
    public let raster: AssetRaster
    public let durationMS: UInt16

    public init(raster: AssetRaster, durationMS: UInt16) {
        self.raster = raster
        self.durationMS = durationMS
    }
}

public struct DecodedAssetSource: Equatable, Sendable {
    public let frames: [DecodedAssetFrame]
    public let animated: Bool
}

public enum AssetSourceDecoder {
    public static func decode(
        _ data: Data,
        limits: AssetConversionLimits = AssetConversionLimits(),
        minimumFrameMS: UInt16,
        maximumFrameMS: UInt16
    ) throws -> DecodedAssetSource {
        guard !data.isEmpty, let source = CGImageSourceCreateWithData(data as CFData, nil) else {
            throw AssetConversionError.unsupportedSource
        }
        let count = CGImageSourceGetCount(source)
        guard count > 0 else { throw AssetConversionError.decodeFailed }
        let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any]
        guard let width = integer(properties?[kCGImagePropertyPixelWidth]),
              let height = integer(properties?[kCGImagePropertyPixelHeight]) else {
            throw AssetConversionError.decodeFailed
        }
        try limits.validate(width: width, height: height, frames: count)
        let type = CGImageSourceGetType(source) as String?
        let animated = count > 1
        if animated, type != "com.compuserve.gif", type != "public.png" {
            throw AssetConversionError.unsupportedSource
        }

        let animationMetadata = animated ? try AnimationMetadataParser.parse(data, type: type, frameCount: count) : nil
        var frames: [DecodedAssetFrame] = []
        var patches: [AnimationPatch] = []
        frames.reserveCapacity(count)
        patches.reserveCapacity(count)
        for index in 0..<count {
            guard let image = CGImageSourceCreateImageAtIndex(source, index, [
                kCGImageSourceShouldCache: false,
                kCGImageSourceShouldAllowFloat: false
            ] as CFDictionary) else { throw AssetConversionError.decodeFailed }
            try limits.validate(width: image.width, height: image.height, frames: count)
            let raster = try raster(image)
            let frameProperties = CGImageSourceCopyPropertiesAtIndex(source, index, nil) as? [CFString: Any]
            let orientation = try orientation(frameProperties)
            let oriented = try AssetPixelConverter.applyOrientation(raster, orientation)
            let duration: UInt16
            if animated {
                let seconds = animationDelay(frameProperties, type: type)
                let milliseconds = Int((seconds * 1_000).rounded(.toNearestOrAwayFromZero))
                guard milliseconds >= Int(minimumFrameMS), milliseconds <= Int(maximumFrameMS),
                      let exact = UInt16(exactly: milliseconds) else {
                    throw AssetConversionError.invalidFrameDuration
                }
                duration = exact
            } else {
                duration = 0
            }
            if let animationMetadata {
                let metadata = animationMetadata.frames[index]
                guard orientation == .up else { throw AssetConversionError.decodeFailed }
                let patchRaster: AssetRaster
                if oriented.width == metadata.width, oriented.height == metadata.height {
                    patchRaster = oriented
                } else if oriented.width == animationMetadata.width, oriented.height == animationMetadata.height {
                    patchRaster = try crop(oriented, x: metadata.x, y: metadata.y, width: metadata.width, height: metadata.height)
                } else {
                    throw AssetConversionError.decodeFailed
                }
                patches.append(AnimationPatch(
                    raster: patchRaster,
                    x: metadata.x,
                    y: metadata.y,
                    durationMS: duration,
                    disposal: metadata.disposal,
                    blend: metadata.blend
                ))
            } else {
                frames.append(DecodedAssetFrame(raster: oriented, durationMS: duration))
            }
        }
        if let animationMetadata {
            frames = try AnimationCanvasCompositor.compose(
                width: animationMetadata.width,
                height: animationMetadata.height,
                patches: patches,
                limits: limits
            )
        }
        return DecodedAssetSource(frames: frames, animated: animated)
    }

    private static func raster(_ image: CGImage) throws -> AssetRaster {
        let width = image.width
        let height = image.height
        let (pixelCount, pixelOverflow) = width.multipliedReportingOverflow(by: height)
        let (byteCount, byteOverflow) = pixelCount.multipliedReportingOverflow(by: 4)
        guard !pixelOverflow, !byteOverflow else { throw AssetConversionError.decodedMemoryLimitExceeded }
        var bytes = [UInt8](repeating: 0, count: byteCount)
        let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
        let rendered = bytes.withUnsafeMutableBytes { storage -> Bool in
            guard let base = storage.baseAddress,
                  let context = CGContext(
                    data: base,
                    width: width,
                    height: height,
                    bitsPerComponent: 8,
                    bytesPerRow: width * 4,
                    space: colorSpace,
                    bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue
                  ) else { return false }
            context.interpolationQuality = .none
            context.setBlendMode(.copy)
            context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
            return true
        }
        guard rendered else { throw AssetConversionError.decodeFailed }
        var pixels: [AssetRGBA8] = []
        pixels.reserveCapacity(width * height)
        for index in stride(from: 0, to: bytes.count, by: 4) {
            let alpha = bytes[index + 3]
            if alpha == 0 {
                pixels.append(AssetRGBA8(red: 0, green: 0, blue: 0, alpha: 0))
            } else {
                func unpremultiply(_ value: UInt8) -> UInt8 {
                    UInt8(min(255, (UInt32(value) * 255 + UInt32(alpha) / 2) / UInt32(alpha)))
                }
                pixels.append(AssetRGBA8(red: unpremultiply(bytes[index]), green: unpremultiply(bytes[index + 1]), blue: unpremultiply(bytes[index + 2]), alpha: alpha))
            }
        }
        return try AssetRaster(width: width, height: height, pixels: pixels)
    }

    private static func crop(_ raster: AssetRaster, x: Int, y: Int, width: Int, height: Int) throws -> AssetRaster {
        guard x >= 0, y >= 0, width > 0, height > 0,
              x <= raster.width - width, y <= raster.height - height else {
            throw AssetConversionError.decodeFailed
        }
        var pixels: [AssetRGBA8] = []
        pixels.reserveCapacity(width * height)
        for row in y..<(y + height) {
            pixels.append(contentsOf: raster.pixels[(row * raster.width + x)..<(row * raster.width + x + width)])
        }
        return try AssetRaster(width: width, height: height, pixels: pixels)
    }

    private static func orientation(_ properties: [CFString: Any]?) throws -> AssetEXIFOrientation {
        let raw = integer(properties?[kCGImagePropertyOrientation]) ?? 1
        guard let orientation = AssetEXIFOrientation(rawValue: UInt8(raw)) else { throw AssetConversionError.decodeFailed }
        return orientation
    }

    private static func animationDelay(_ properties: [CFString: Any]?, type: String?) -> Double {
        if type == "com.compuserve.gif",
           let dictionary = properties?[kCGImagePropertyGIFDictionary] as? [CFString: Any] {
            return number(dictionary[kCGImagePropertyGIFUnclampedDelayTime]) ?? number(dictionary[kCGImagePropertyGIFDelayTime]) ?? 0
        }
        if type == "public.png",
           let dictionary = properties?[kCGImagePropertyPNGDictionary] as? [String: Any] {
            return (dictionary["UnclampedDelayTime"] as? NSNumber)?.doubleValue ??
                (dictionary["DelayTime"] as? NSNumber)?.doubleValue ?? 0
        }
        return 0
    }

    private static func integer(_ value: Any?) -> Int? { (value as? NSNumber)?.intValue }
    private static func number(_ value: Any?) -> Double? { (value as? NSNumber)?.doubleValue }
}

public enum ConvertedAssetFactory {
    public static func makeVKA1(
        source: DecodedAssetSource,
        fit: AssetFit,
        background: AssetRGB888,
        width: UInt16 = 428,
        height: UInt16 = 142,
        limits: VKA1Limits
    ) throws -> Data {
        let frames = try source.frames.map { frame in
            VKA1SourceFrame(
                pixels: try AssetPixelConverter.convert(frame.raster, width: Int(width), height: Int(height), fit: fit, background: background),
                durationMS: frame.durationMS
            )
        }
        return try VKA1Codec.encode(kind: source.animated ? .animation : .image, width: width, height: height, frames: frames, limits: limits)
    }
}
