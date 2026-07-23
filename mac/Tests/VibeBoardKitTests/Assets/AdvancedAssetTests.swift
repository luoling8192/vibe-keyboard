import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Advanced asset behavior")
struct AdvancedAssetTests {
    @Test func animationCanvasHonorsBackgroundPreviousSourceAndOver() throws {
        let clear = AssetRGBA8(red: 0, green: 0, blue: 0, alpha: 0)
        let red = AssetRGBA8(red: 255, green: 0, blue: 0)
        let blue = AssetRGBA8(red: 0, green: 0, blue: 255)
        let halfGreen = AssetRGBA8(red: 0, green: 255, blue: 0, alpha: 128)
        let pixel: (AssetRGBA8) throws -> AssetRaster = { try AssetRaster(width: 1, height: 1, pixels: [$0]) }
        let frames = try AnimationCanvasCompositor.compose(
            width: 2,
            height: 1,
            patches: [
                AnimationPatch(raster: pixel(red), x: 0, y: 0, durationMS: 20, disposal: .keep, blend: .source),
                AnimationPatch(raster: pixel(blue), x: 1, y: 0, durationMS: 30, disposal: .previous, blend: .source),
                AnimationPatch(raster: pixel(halfGreen), x: 0, y: 0, durationMS: 40, disposal: .background, blend: .over),
                AnimationPatch(raster: pixel(clear), x: 1, y: 0, durationMS: 50, disposal: .keep, blend: .source)
            ],
            limits: AssetConversionLimits(maxSourceWidth: 2, maxSourceHeight: 1, maxSourcePixels: 2, maxFrames: 4, maxDecodedBytes: 32)
        )
        #expect(frames.map(\.durationMS) == [20, 30, 40, 50])
        #expect(frames[0].raster.pixels == [red, clear])
        #expect(frames[1].raster.pixels == [red, blue])
        #expect(frames[2].raster.pixels[0] == AssetRGBA8(red: 127, green: 128, blue: 0))
        #expect(frames[2].raster.pixels[1] == clear)
        #expect(frames[3].raster.pixels == [clear, clear])
    }

    @Test func targetMacOSAPNGGoldenUsesFrameRectsSourceBlendAndPreviousDisposal() throws {
        let url = try #require(Bundle.module.url(forResource: "apng-source-previous", withExtension: "png", subdirectory: "Fixtures/Assets"))
        let source = try AssetSourceDecoder.decode(
            Data(contentsOf: url),
            limits: AssetConversionLimits(maxSourceWidth: 2, maxSourceHeight: 1, maxSourcePixels: 2, maxFrames: 2, maxDecodedBytes: 16),
            minimumFrameMS: 20,
            maximumFrameMS: 1_000
        )
        #expect(source.animated)
        #expect(source.frames.map(\.durationMS) == [20, 30])
        #expect(source.frames[0].raster.pixels == [
            AssetRGBA8(red: 255, green: 0, blue: 0),
            AssetRGBA8(red: 255, green: 0, blue: 0)
        ])
        #expect(source.frames[1].raster.pixels == [
            AssetRGBA8(red: 255, green: 0, blue: 0),
            AssetRGBA8(red: 0, green: 0, blue: 255)
        ])
    }

    @Test func targetMacOSGIFGoldenUsesFrameRectsAndDisposal() throws {
        let url = try #require(Bundle.module.url(forResource: "gif-background-previous", withExtension: "gif", subdirectory: "Fixtures/Assets"))
        let source = try AssetSourceDecoder.decode(
            Data(contentsOf: url),
            limits: AssetConversionLimits(maxSourceWidth: 2, maxSourceHeight: 1, maxSourcePixels: 2, maxFrames: 2, maxDecodedBytes: 16),
            minimumFrameMS: 20,
            maximumFrameMS: 1_000
        )
        #expect(source.frames.map(\.durationMS) == [20, 30])
        #expect(source.frames[0].raster.pixels[0].alpha == 255)
        #expect(source.frames[1].raster.pixels[0].alpha == 0)
        #expect(source.frames[1].raster.pixels[1].alpha == 255)
    }

    @Test func targetMacOSAPNGGoldenUsesOverBlend() throws {
        let url = try #require(Bundle.module.url(forResource: "apng-over-alpha", withExtension: "png", subdirectory: "Fixtures/Assets"))
        let source = try AssetSourceDecoder.decode(
            Data(contentsOf: url),
            limits: AssetConversionLimits(maxSourceWidth: 1, maxSourceHeight: 1, maxSourcePixels: 1, maxFrames: 2, maxDecodedBytes: 8),
            minimumFrameMS: 20,
            maximumFrameMS: 1_000
        )
        #expect(source.frames[0].raster.pixels == [AssetRGBA8(red: 255, green: 0, blue: 0)])
        #expect(source.frames[1].raster.pixels == [AssetRGBA8(red: 127, green: 0, blue: 128)])
    }

    @Test func canonicalFontFixtureBindsHashAndRejectsMissingGlyph() throws {
        let url = try #require(Bundle.module.url(forResource: "vk-sans-v1.metrics", withExtension: "json", subdirectory: "Fixtures/fonts"))
        let data = try Data(contentsOf: url)
        let capability = ScreenFontCapability(
            id: "vk-sans",
            version: 1,
            metricsSHA256: "b6567a24b312e6e80c2f5ea200e4377d42926e11bd55544752b2533c2235b22b"
        )
        let metrics = try PreviewFontMetrics.decodeCanonical(data: data, id: "vk-sans", capability: capability)
        let placements = try FontPreviewRenderer.placements(text: " A", font: metrics, capability: capability, rect: PreviewRect(x: 4, y: 2, width: 20, height: 16))
        #expect(placements.map(\.originX) == [4, 8])
        #expect(placements.map(\.baselineY) == [15, 15])
        #expect(throws: FontPreviewError.unsupportedGlyph(0x00e9)) {
            try FontPreviewRenderer.placements(text: "é", font: metrics, capability: capability, rect: PreviewRect(x: 0, y: 0, width: 20, height: 16))
        }
        let wrong = ScreenFontCapability(id: "vk-sans", version: 1, metricsSHA256: String(repeating: "a", count: 64))
        #expect(throws: FontPreviewError.capabilityMismatch) {
            try PreviewFontMetrics.decodeCanonical(data: data, id: "vk-sans", capability: wrong)
        }
    }

    @Test func petStatesRemainExplicitAndAnimationDurationsRemainVariable() throws {
        let hash = String(repeating: "a", count: 64)
        let pet = ScreenPetManifest(id: "pet", states: [.idle: .asset(sha256: hash), .active: .idleFallback])
        #expect(pet.states[.recording] == nil)
        #expect(pet.states[.active] == .idleFallback)
        let source = DecodedAssetSource(frames: [
            DecodedAssetFrame(raster: try AssetRaster(width: 1, height: 1, pixels: [AssetRGBA8(red: 1, green: 2, blue: 3)]), durationMS: 20),
            DecodedAssetFrame(raster: try AssetRaster(width: 1, height: 1, pixels: [AssetRGBA8(red: 4, green: 5, blue: 6)]), durationMS: 75)
        ], animated: true)
        let limits = VKA1Limits(maxFrames: 2, minFrameDurationMS: 20, maxFrameDurationMS: 100, maxContainerBytes: 1_000_000, maxDecodedBytes: 428 * 142 * 2 * 2)
        let data = try ConvertedAssetFactory.makeVKA1(source: source, fit: .stretch, background: AssetRGB888(red: 0, green: 0, blue: 0), limits: limits)
        #expect(try VKA1Codec.decode(data, limits: limits).frames.map(\.durationMS) == [20, 75])
    }
}
