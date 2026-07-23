import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Screen manifest protocol")
struct ScreenManifestTests {
    private let imageHash = String(repeating: "a", count: 64)
    private let glyphHash = String(repeating: "b", count: 64)

    @Test func imageCommitIsCanonicalAndDerivesNoPersistedFields() throws {
        let commit = ScreenCommit(
            expectedRevision: 0,
            revision: 1,
            assets: [.init(bytes: 10, kind: .image, sha256: imageHash)],
            payload: .image(.init(backgroundRGB888: 0, fit: .contain, sha256: imageHash)),
            limits: limits()
        )
        let frame = try ReplacementCommandEncoder.encode(.commit(commit))
        let body = String(decoding: frame.dropFirst(4), as: UTF8.self)
        let expected = "{\"assets\":{\"assets\":[{\"bytes\":10,\"kind\":\"image\",\"sha256\":\"\(imageHash)\"}]},\"event\":\"vk_screen_commit\",\"expected_revision\":0,\"revision\":1,\"screen\":{\"configured_mode\":\"image\",\"image\":{\"background_rgb888\":0,\"fit\":\"contain\",\"sha256\":\"\(imageHash)\"},\"layout\":null,\"pet\":null}}"
        #expect(body == expected)
        #expect(!body.contains("previous_revision"))
        #expect(!body.contains("schema"))
    }

    @Test func validatesWidgetBindingAndCanonicalZero() throws {
        let base = ScreenObjectBase(id: "bar", width: 100, height: 8, z: 0, clip: true, visible: true)
        let layout = ScreenLayout(
            backgroundRGB888: 0,
            mode: .custom,
            revision: 1,
            objects: [.init(x: 0, y: 0, node: .progress(base: base, backgroundRGB888: 0, fillRGB888: 0x00ff00, widgetID: "cpu"))],
            widgets: [.progress(id: "cpu", target: "bar", fallback: .init(coefficient: 0, scale: 3), min: .init(coefficient: 0, scale: 0), max: .init(coefficient: 100, scale: 0), decimals: 1)]
        )
        let commit = ScreenCommit(expectedRevision: 0, revision: 1, assets: [], payload: .custom(layout), limits: limits())
        let body = String(decoding: try ReplacementCommandEncoder.encode(.commit(commit)).dropFirst(4), as: UTF8.self)
        #expect(body.contains("\"fallback\":0"))
        #expect(!body.contains("-0"))
        #expect(!body.contains("0.0"))
    }

    @Test func rejectsWrongDuplicateAndStaticWidgetTargets() {
        let progressA = ScreenObjectNode.progress(base: .init(id: "bar1", width: 10, height: 2, z: 0, clip: true, visible: true), backgroundRGB888: 0, fillRGB888: 1, widgetID: "cpu")
        let progressB = ScreenObjectNode.progress(base: .init(id: "bar2", width: 10, height: 2, z: 0, clip: true, visible: true), backgroundRGB888: 0, fillRGB888: 1, widgetID: "cpu2")
        let value = ScreenCanonicalNumber(coefficient: 0, scale: 0)
        let max = ScreenCanonicalNumber(coefficient: 100, scale: 0)
        let duplicate = ScreenLayout(backgroundRGB888: 0, mode: .custom, revision: 1,
            objects: [.init(x: 0, y: 0, node: progressA), .init(x: 0, y: 3, node: progressB)],
            widgets: [.progress(id: "cpu", target: "bar1", fallback: value, min: value, max: max, decimals: 0), .progress(id: "other", target: "bar1", fallback: value, min: value, max: max, decimals: 0)])
        #expect(throws: ReplacementProtocolError.invalidValue(field: "widget_binding")) {
            try ReplacementCommandEncoder.encode(.commit(.init(expectedRevision: 0, revision: 1, assets: [], payload: .custom(duplicate), limits: limits())))
        }

        let staticNode = ScreenObjectNode.staticLabel(base: .init(id: "label", width: 20, height: 10, z: 0, clip: true, visible: true), align: .left, colorRGB888: 0, font: .init(id: "vk-sans", version: 1), text: "x")
        let staticTarget = ScreenLayout(backgroundRGB888: 0, mode: .custom, revision: 1, objects: [.init(x: 0, y: 0, node: staticNode)], widgets: [.text(id: "status", target: "label", fallback: "-")])
        #expect(throws: ReplacementProtocolError.invalidValue(field: "widget_binding")) {
            try ReplacementCommandEncoder.encode(.commit(.init(expectedRevision: 0, revision: 1, assets: [], payload: .custom(staticTarget), limits: limits())))
        }
    }

    @Test func rejectsFontAssetAndMemoryRelevantCountViolations() {
        let badFont = ScreenObjectNode.staticLabel(base: .init(id: "label", width: 20, height: 10, z: 0, clip: true, visible: true), align: .left, colorRGB888: 0, font: .init(id: "unknown", version: 1), text: "x")
        let layout = ScreenLayout(backgroundRGB888: 0, mode: .custom, revision: 1, objects: [.init(x: 0, y: 0, node: badFont)], widgets: [])
        #expect(throws: ReplacementProtocolError.invalidValue(field: "font_mismatch")) {
            try ReplacementCommandEncoder.encode(.commit(.init(expectedRevision: 0, revision: 1, assets: [], payload: .custom(layout), limits: limits())))
        }

        let duplicateAssets = [ScreenAssetReference(bytes: 1, kind: .image, sha256: imageHash), ScreenAssetReference(bytes: 2, kind: .image, sha256: imageHash)]
        #expect(throws: ReplacementProtocolError.invalidValue(field: "assets")) {
            try ReplacementCommandEncoder.encode(.commit(.init(expectedRevision: 0, revision: 1, assets: duplicateAssets, payload: .image(.init(backgroundRGB888: 0, fit: .contain, sha256: imageHash)), limits: limits())))
        }
    }

    @Test func revisionIsBoundToCurrentCapabilityUsingSerialArithmetic() throws {
        let current = limits(revision: UInt32.max - 1, configured: true)
        let wrapped = ScreenCommit(expectedRevision: UInt32.max - 1, revision: 1, assets: [.init(bytes: 1, kind: .image, sha256: imageHash)], payload: .image(.init(backgroundRGB888: 0, fit: .contain, sha256: imageHash)), limits: current)
        #expect(try ReplacementCommandEncoder.encode(.commit(wrapped)).count > 4)

        for invalid in [
            ScreenCommit(expectedRevision: UInt32.max, revision: 1, assets: [.init(bytes: 1, kind: .image, sha256: imageHash)], payload: .image(.init(backgroundRGB888: 0, fit: .contain, sha256: imageHash)), limits: current),
            ScreenCommit(expectedRevision: UInt32.max - 1, revision: UInt32.max - 1, assets: [.init(bytes: 1, kind: .image, sha256: imageHash)], payload: .image(.init(backgroundRGB888: 0, fit: .contain, sha256: imageHash)), limits: current),
            ScreenCommit(expectedRevision: UInt32.max - 1, revision: (UInt32.max - 1) &+ 0x8000_0000, assets: [.init(bytes: 1, kind: .image, sha256: imageHash)], payload: .image(.init(backgroundRGB888: 0, fit: .contain, sha256: imageHash)), limits: current),
        ] {
            #expect(throws: ReplacementProtocolError.invalidValue(field: "revision_or_assets")) {
                try ReplacementCommandEncoder.encode(.commit(invalid))
            }
        }
    }

    @Test func petAndGlyphReferencesAreKindChecked() throws {
        let pet = ScreenPetManifest(id: "pet", states: [.idle: .asset(sha256: imageHash), .active: .idleFallback])
        let petCommit = ScreenCommit(expectedRevision: 0, revision: 1, assets: [.init(bytes: 1, kind: .image, sha256: imageHash)], payload: .pet(pet), limits: limits())
        #expect(try ReplacementCommandEncoder.encode(.commit(petCommit)).count > 4)

        let glyph = ScreenObjectNode.glyphLabel(base: .init(id: "glyph", width: 16, height: 16, z: 0, clip: true, visible: true), align: .left, colorRGB888: 0xffffff, glyph: .init(advance: 16, baseline: 12, bearingX: 0, bearingY: 12, sha256: glyphHash))
        let layout = ScreenLayout(backgroundRGB888: 0, mode: .custom, revision: 1, objects: [.init(x: 0, y: 0, node: glyph)], widgets: [])
        let commit = ScreenCommit(expectedRevision: 0, revision: 1, assets: [.init(bytes: 1, kind: .glyphBitmap, sha256: glyphHash)], payload: .custom(layout), limits: limits())
        #expect(try ReplacementCommandEncoder.encode(.commit(commit)).count > 4)
    }

    private func limits(revision: UInt32 = 0, configured: Bool = false) -> ScreenCommitLimits {
        let screen = ScreenCapability(modes: ["image", "pet", "dashboard", "custom"], maxCommitBytes: 4092, maxLayoutBytes: 3072, maxAssets: 64, maxObjects: 32, maxDepth: 4, maxWidgets: 16, maxFonts: 4, maxPetStates: 6, maxStringBytes: 256, maxJSONTokens: 512, maxWidgetValueBytes: 256, revision: revision, configured: configured, fonts: [.init(id: "vk-sans", version: 1, metricsSHA256: String(repeating: "c", count: 64))])
        return ScreenCommitLimits(displayWidth: 428, displayHeight: 142, screen: screen)
    }
}
