import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Asset conversion")
struct AssetConversionTests {
    @Test func exactAlphaAndRGB565() {
        let background = AssetRGB888(red: 10, green: 20, blue: 30)
        #expect(AssetPixelConverter.composite(AssetRGBA8(red: 250, green: 100, blue: 0, alpha: 0), over: background) == background)
        #expect(AssetPixelConverter.composite(AssetRGBA8(red: 255, green: 0, blue: 0, alpha: 128), over: AssetRGB888(red: 0, green: 0, blue: 0)) == AssetRGB888(red: 128, green: 0, blue: 0))
        #expect(AssetPixelConverter.rgb565(AssetRGB888(red: 255, green: 255, blue: 255)) == 0xffff)
        #expect(AssetPixelConverter.rgb565(AssetRGB888(red: 255, green: 0, blue: 0)) == 0xf800)
    }

    @Test func orientationAndPixelCenterBilinear() throws {
        let source = try AssetRaster(width: 2, height: 1, pixels: [
            AssetRGBA8(red: 0, green: 0, blue: 0), AssetRGBA8(red: 255, green: 255, blue: 255)
        ])
        let rotated = try AssetPixelConverter.applyOrientation(source, .right)
        #expect(rotated.width == 1 && rotated.height == 2)
        #expect(rotated.pixels[0].red == 0 && rotated.pixels[1].red == 255)
        let pixels = try AssetPixelConverter.convert(source, width: 3, height: 1, fit: .stretch, background: AssetRGB888(red: 0, green: 0, blue: 0))
        #expect(pixels == [0, 0x8410, 0xffff])
    }

    @Test func containAndCenterUseFloorOnTopLeft() throws {
        let red = AssetRGBA8(red: 255, green: 0, blue: 0)
        let source = try AssetRaster(width: 1, height: 1, pixels: [red])
        let centered = try AssetPixelConverter.convert(source, width: 4, height: 2, fit: .center, background: AssetRGB888(red: 0, green: 0, blue: 0))
        #expect(centered[1] == 0xf800)
        #expect(centered.filter { $0 == 0xf800 }.count == 1)
        let contained = try AssetPixelConverter.convert(source, width: 3, height: 2, fit: .contain, background: AssetRGB888(red: 0, green: 0, blue: 0))
        #expect(contained.filter { $0 == 0xf800 }.count == 4)
    }

    @Test func limitsRejectBeforeDecodedAllocation() {
        let limits = AssetConversionLimits(maxSourceWidth: 100, maxSourceHeight: 100, maxSourcePixels: 1_000, maxFrames: 2, maxDecodedBytes: 4_000)
        #expect(throws: AssetConversionError.decodedMemoryLimitExceeded) {
            try limits.validate(width: 100, height: 100)
        }
        #expect(throws: AssetConversionError.sourceLimitExceeded) {
            try limits.validate(width: 101, height: 1)
        }
    }

    @Test func deterministicVKA1Factory() throws {
        let source = DecodedAssetSource(frames: [DecodedAssetFrame(raster: try AssetRaster(width: 1, height: 1, pixels: [AssetRGBA8(red: 255, green: 0, blue: 0)]), durationMS: 0)], animated: false)
        let limits = VKA1Limits(maxFrames: 8, minFrameDurationMS: 20, maxFrameDurationMS: 1_000, maxContainerBytes: 1_000_000, maxDecodedBytes: 428 * 142 * 2)
        let a = try ConvertedAssetFactory.makeVKA1(source: source, fit: .stretch, background: AssetRGB888(red: 0, green: 0, blue: 0), limits: limits)
        let b = try ConvertedAssetFactory.makeVKA1(source: source, fit: .stretch, background: AssetRGB888(red: 0, green: 0, blue: 0), limits: limits)
        #expect(a == b)
        #expect(try VKA1Codec.decode(a, limits: limits).width == 428)
    }
}
