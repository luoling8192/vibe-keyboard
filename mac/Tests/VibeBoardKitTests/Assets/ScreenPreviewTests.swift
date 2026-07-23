import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Screen preview")
struct ScreenPreviewTests {
    @Test func containerPlacementUsesFloorRemainderAndSourceOrder() throws {
        let childA = ScreenObjectNode.staticLabel(
            base: ScreenObjectBase(id: "a", width: 10, height: 5, z: 0, clip: true, visible: true),
            align: .left, colorRGB888: 0xffffff, font: ScreenFontReference(id: "vk-sans", version: 1), text: "A"
        )
        let childB = ScreenObjectNode.staticLabel(
            base: ScreenObjectBase(id: "b", width: 10, height: 5, z: 0, clip: true, visible: true),
            align: .left, colorRGB888: 0xffffff, font: ScreenFontReference(id: "vk-sans", version: 1), text: "B"
        )
        let root = ScreenObjectNode.container(
            base: ScreenObjectBase(id: "root", width: 31, height: 9, z: 0, clip: true, visible: true),
            kind: .row, crossAlign: .center, gap: 1, mainAlign: .center, children: [childA, childB]
        )
        let layout = ScreenLayout(backgroundRGB888: 0, mode: .custom, revision: 1, objects: [ScreenRootObject(x: 0, y: 0, node: root)], widgets: [])
        let placements = try ScreenLayoutGeometry.placements(layout)
        #expect(placements.map(\.id) == ["root", "a", "b"])
        #expect(placements[1].rect == PreviewRect(x: 5, y: 2, width: 10, height: 5))
        #expect(placements[2].rect == PreviewRect(x: 16, y: 2, width: 10, height: 5))
    }

    @Test func widgetFormattingRoundsHalfAwayFromZeroAndUsesCanonicalZero() throws {
        #expect(try WidgetPreviewFormatter.format(ScreenCanonicalNumber(coefficient: 125, scale: 2), decimals: 1) == "1.3")
        #expect(try WidgetPreviewFormatter.format(ScreenCanonicalNumber(coefficient: -125, scale: 2), decimals: 1) == "-1.3")
        #expect(try WidgetPreviewFormatter.format(ScreenCanonicalNumber(coefficient: 0, scale: 3), decimals: 2) == "0.00")
        #expect(WidgetPreviewFormatter.accepts(newSequence: 1, after: nil))
        #expect(!WidgetPreviewFormatter.accepts(newSequence: 1, after: 1))
        #expect(!WidgetPreviewFormatter.accepts(newSequence: 0x8000_0001, after: 1))
        #expect(WidgetPreviewFormatter.accepts(newSequence: 0, after: UInt32.max) == false)
    }

    @Test func sharedLanguageNeutralFixtureMatchesPreviewContracts() throws {
        let url = try #require(Bundle.module.url(forResource: "preview-geometry-color-font-v1", withExtension: "json", subdirectory: "Fixtures"))
        let root = try #require(try JSONSerialization.jsonObject(with: Data(contentsOf: url)) as? [String: Any])
        let pixels = try #require(root["pixels"] as? [String: Any])
        #expect(pixels["black_rgb565"] as? Int == Int(AssetPixelConverter.rgb565(AssetRGB888(red: 0, green: 0, blue: 0))))
        #expect(pixels["white_rgb565"] as? Int == Int(AssetPixelConverter.rgb565(AssetRGB888(red: 255, green: 255, blue: 255))))
        #expect(pixels["alpha_half_red_over_black_rgb565"] as? Int == Int(AssetPixelConverter.rgb565(AssetPixelConverter.composite(AssetRGBA8(red: 255, green: 0, blue: 0, alpha: 128), over: AssetRGB888(red: 0, green: 0, blue: 0)))))
        let widget = try #require(root["widget"] as? [String: Any])
        let positive = try WidgetPreviewFormatter.format(ScreenCanonicalNumber(coefficient: 125, scale: 2), decimals: 1)
        let negative = try WidgetPreviewFormatter.format(ScreenCanonicalNumber(coefficient: -125, scale: 2), decimals: 1)
        #expect(widget["positive_half_away"] as? String == positive)
        #expect(widget["negative_half_away"] as? String == negative)
    }

    @Test func invalidContainerGeometryRejects() {
        let child = ScreenObjectNode.progress(
            base: ScreenObjectBase(id: "bar", width: 20, height: 5, z: 0, clip: true, visible: true),
            backgroundRGB888: 0, fillRGB888: 0xffffff, widgetID: "cpu"
        )
        let root = ScreenObjectNode.container(
            base: ScreenObjectBase(id: "root", width: 10, height: 10, z: 0, clip: true, visible: true),
            kind: .row, crossAlign: .start, gap: 0, mainAlign: .start, children: [child]
        )
        let layout = ScreenLayout(backgroundRGB888: 0, mode: .custom, revision: 1, objects: [ScreenRootObject(x: 0, y: 0, node: root)], widgets: [])
        #expect(throws: ScreenPreviewError.invalidGeometry) { try ScreenLayoutGeometry.placements(layout) }
    }
}
