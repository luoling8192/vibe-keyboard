import Foundation

public enum AnimationDisposal: String, Equatable, Sendable {
    case keep
    case background
    case previous
}

public enum AnimationBlend: String, Equatable, Sendable {
    case source
    case over
}

public struct AnimationPatch: Equatable, Sendable {
    public let raster: AssetRaster
    public let x: Int
    public let y: Int
    public let durationMS: UInt16
    public let disposal: AnimationDisposal
    public let blend: AnimationBlend

    public init(raster: AssetRaster, x: Int, y: Int, durationMS: UInt16, disposal: AnimationDisposal, blend: AnimationBlend) {
        self.raster = raster
        self.x = x
        self.y = y
        self.durationMS = durationMS
        self.disposal = disposal
        self.blend = blend
    }
}

/// A bounded GIF/APNG canvas owner. Each returned frame is the displayed canvas before
/// the current patch's disposal is applied.
public enum AnimationCanvasCompositor {
    public static func compose(
        width: Int,
        height: Int,
        patches: [AnimationPatch],
        limits: AssetConversionLimits
    ) throws -> [DecodedAssetFrame] {
        try limits.validate(width: width, height: height, frames: patches.count)
        let transparent = AssetRGBA8(red: 0, green: 0, blue: 0, alpha: 0)
        let count = try checkedPixelCount(width: width, height: height)
        var canvas = [AssetRGBA8](repeating: transparent, count: count)
        var output: [DecodedAssetFrame] = []
        output.reserveCapacity(patches.count)

        for patch in patches {
            guard patch.x >= 0, patch.y >= 0,
                  patch.x <= width - patch.raster.width,
                  patch.y <= height - patch.raster.height else {
                throw AssetConversionError.invalidDimensions
            }
            let previous = patch.disposal == .previous ? canvas : []
            for y in 0..<patch.raster.height {
                for x in 0..<patch.raster.width {
                    let source = patch.raster.pixels[y * patch.raster.width + x]
                    let index = (patch.y + y) * width + patch.x + x
                    canvas[index] = patch.blend == .source ? source : over(source, canvas[index])
                }
            }
            output.append(DecodedAssetFrame(raster: try AssetRaster(width: width, height: height, pixels: canvas), durationMS: patch.durationMS))
            switch patch.disposal {
            case .keep:
                break
            case .background:
                for y in 0..<patch.raster.height {
                    for x in 0..<patch.raster.width {
                        canvas[(patch.y + y) * width + patch.x + x] = transparent
                    }
                }
            case .previous:
                canvas = previous
            }
        }
        return output
    }

    private static func checkedPixelCount(width: Int, height: Int) throws -> Int {
        let (count, overflow) = width.multipliedReportingOverflow(by: height)
        guard width > 0, height > 0, !overflow else { throw AssetConversionError.invalidDimensions }
        return count
    }

    private static func over(_ source: AssetRGBA8, _ destination: AssetRGBA8) -> AssetRGBA8 {
        let sourceAlpha = UInt32(source.alpha)
        let destinationAlpha = UInt32(destination.alpha)
        let inverse = 255 - sourceAlpha
        let outputAlpha = sourceAlpha + (destinationAlpha * inverse + 127) / 255
        guard outputAlpha > 0 else { return AssetRGBA8(red: 0, green: 0, blue: 0, alpha: 0) }
        func channel(_ sourceChannel: UInt8, _ destinationChannel: UInt8) -> UInt8 {
            let premultiplied = UInt32(sourceChannel) * sourceAlpha +
                (UInt32(destinationChannel) * destinationAlpha * inverse + 127) / 255
            return UInt8(min(255, (premultiplied + outputAlpha / 2) / outputAlpha))
        }
        return AssetRGBA8(
            red: channel(source.red, destination.red),
            green: channel(source.green, destination.green),
            blue: channel(source.blue, destination.blue),
            alpha: UInt8(outputAlpha)
        )
    }
}
