import CryptoKit
import Foundation

public enum FontPreviewError: Error, Equatable, Sendable {
    case invalidMetrics
    case capabilityMismatch
    case unsupportedGlyph(UInt32)
    case invalidGeometry
}

public struct PreviewFontGlyph: Equatable, Sendable {
    public let scalar: UInt32
    public let advance: UInt16
    public let bearingX: Int16
    public let bearingY: Int16
}

public struct PreviewFontMetrics: Equatable, Sendable {
    public let id: String
    public let version: UInt16
    public let metricsSHA256: String
    public let ascent: Int16
    public let descent: Int16
    public let lineHeight: UInt16
    public let glyphs: [UInt32: PreviewFontGlyph]

    public static func decodeCanonical(
        data: Data,
        id: String,
        capability: ScreenFontCapability
    ) throws -> PreviewFontMetrics {
        guard capability.id == id,
              data.last != 0x0a,
              String(decoding: data, as: UTF8.self).data(using: .utf8) == data else {
            throw FontPreviewError.invalidMetrics
        }
        let digest = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
        guard digest == capability.metricsSHA256 else { throw FontPreviewError.capabilityMismatch }
        let root = try JSONDecoder().decode(FontFile.self, from: data)
        guard root.version == capability.version, root.lineHeight > 0, !root.glyphs.isEmpty else {
            throw FontPreviewError.capabilityMismatch
        }
        var glyphs: [UInt32: PreviewFontGlyph] = [:]
        var previous: UInt32?
        for entry in root.glyphs {
            let scalar = try parseScalar(entry.scalar)
            guard entry.advance > 0, previous.map({ $0 < scalar }) ?? true, glyphs[scalar] == nil else {
                throw FontPreviewError.invalidMetrics
            }
            previous = scalar
            glyphs[scalar] = PreviewFontGlyph(
                scalar: scalar,
                advance: entry.advance,
                bearingX: entry.bearingX,
                bearingY: entry.bearingY
            )
        }
        return PreviewFontMetrics(
            id: id,
            version: root.version,
            metricsSHA256: digest,
            ascent: root.ascent,
            descent: root.descent,
            lineHeight: root.lineHeight,
            glyphs: glyphs
        )
    }

    private static func parseScalar(_ value: String) throws -> UInt32 {
        let bytes = Array(value.utf8)
        guard (bytes.count == 6 || bytes.count == 8), bytes[0...1] == [0x55, 0x2b],
              bytes.dropFirst(2).allSatisfy({ (0x30...0x39).contains($0) || (0x41...0x46).contains($0) }),
              let scalar = UInt32(String(value.dropFirst(2)), radix: 16),
              Unicode.Scalar(scalar) != nil,
              !(0xd800...0xdfff).contains(scalar),
              !isNoncharacter(scalar) else { throw FontPreviewError.invalidMetrics }
        return scalar
    }

    private static func isNoncharacter(_ scalar: UInt32) -> Bool {
        (0xfdd0...0xfdef).contains(scalar) || (scalar & 0xffff) == 0xfffe || (scalar & 0xffff) == 0xffff
    }

    private struct FontFile: Decodable {
        let ascent: Int16
        let descent: Int16
        let glyphs: [Glyph]
        let lineHeight: UInt16
        let version: UInt16

        enum CodingKeys: String, CodingKey { case ascent, descent, glyphs, lineHeight = "line_height", version }
    }

    private struct Glyph: Decodable {
        let advance: UInt16
        let bearingX: Int16
        let bearingY: Int16
        let scalar: String

        enum CodingKeys: String, CodingKey { case advance, bearingX = "bearing_x", bearingY = "bearing_y", scalar }
    }
}

public struct GlyphPreviewPlacement: Equatable, Sendable {
    public let scalar: UInt32
    public let originX: Int
    public let baselineY: Int
    public let bearingX: Int16
    public let bearingY: Int16
    public let advance: UInt16
}

public enum FontPreviewRenderer {
    public static func placements(
        text: String,
        font: PreviewFontMetrics,
        capability: ScreenFontCapability,
        rect: PreviewRect
    ) throws -> [GlyphPreviewPlacement] {
        guard font.id == capability.id, font.version == capability.version,
              font.metricsSHA256 == capability.metricsSHA256,
              rect.width > 0, rect.height > 0 else { throw FontPreviewError.capabilityMismatch }
        let baseline = rect.y + Int(font.ascent)
        guard baseline >= rect.y, baseline < rect.y + rect.height else { throw FontPreviewError.invalidGeometry }
        var cursor = rect.x
        var output: [GlyphPreviewPlacement] = []
        output.reserveCapacity(text.unicodeScalars.count)
        for unicode in text.unicodeScalars {
            guard let glyph = font.glyphs[unicode.value] else { throw FontPreviewError.unsupportedGlyph(unicode.value) }
            let (next, overflow) = cursor.addingReportingOverflow(Int(glyph.advance))
            guard !overflow, next <= rect.x + rect.width else { throw FontPreviewError.invalidGeometry }
            output.append(GlyphPreviewPlacement(
                scalar: unicode.value,
                originX: cursor,
                baselineY: baseline,
                bearingX: glyph.bearingX,
                bearingY: glyph.bearingY,
                advance: glyph.advance
            ))
            cursor = next
        }
        return output
    }
}
