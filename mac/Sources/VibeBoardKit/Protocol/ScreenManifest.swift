import Foundation

public struct ScreenAssetReference: Equatable, Sendable {
    public let bytes: UInt32
    public let kind: AssetKind
    public let sha256: String

    public init(bytes: UInt32, kind: AssetKind, sha256: String) {
        self.bytes = bytes
        self.kind = kind
        self.sha256 = sha256
    }
}

public struct ScreenImage: Equatable, Sendable {
    public enum Fit: String, Equatable, Sendable { case contain, cover, stretch, center }
    public let backgroundRGB888: UInt32
    public let fit: Fit
    public let sha256: String

    public init(backgroundRGB888: UInt32, fit: Fit, sha256: String) {
        self.backgroundRGB888 = backgroundRGB888
        self.fit = fit
        self.sha256 = sha256
    }
}

public struct ScreenPetManifest: Equatable, Sendable {
    public enum StateName: String, CaseIterable, Equatable, Sendable {
        case idle, active, recording, thinking, success, error
    }
    public enum State: Equatable, Sendable {
        case asset(sha256: String)
        case idleFallback
    }
    public let id: String
    public let states: [StateName: State]

    public init(id: String, states: [StateName: State]) {
        self.id = id
        self.states = states
    }
}

public struct ScreenFontReference: Equatable, Sendable {
    public let id: String
    public let version: UInt16
    public init(id: String, version: UInt16) { self.id = id; self.version = version }
}

public struct ScreenGlyphReference: Equatable, Sendable {
    public let advance: UInt16
    public let baseline: Int16
    public let bearingX: Int16
    public let bearingY: Int16
    public let sha256: String

    public init(advance: UInt16, baseline: Int16, bearingX: Int16, bearingY: Int16, sha256: String) {
        self.advance = advance
        self.baseline = baseline
        self.bearingX = bearingX
        self.bearingY = bearingY
        self.sha256 = sha256
    }
}

public struct ScreenObjectBase: Equatable, Sendable {
    public let id: String
    public let width: UInt16
    public let height: UInt16
    public let z: Int16
    public let clip: Bool
    public let visible: Bool

    public init(id: String, width: UInt16, height: UInt16, z: Int16, clip: Bool, visible: Bool) {
        self.id = id; self.width = width; self.height = height; self.z = z; self.clip = clip; self.visible = visible
    }
}

public indirect enum ScreenObjectNode: Equatable, Sendable {
    public enum Align: String, Equatable, Sendable { case left }
    public enum AxisAlign: String, Equatable, Sendable { case start, center, end }
    public enum ContainerKind: String, Equatable, Sendable { case row, column }

    case image(base: ScreenObjectBase, image: ScreenImage)
    case pet(base: ScreenObjectBase, backgroundRGB888: UInt32, fit: ScreenImage.Fit, manifest: ScreenPetManifest)
    case staticLabel(base: ScreenObjectBase, align: Align, colorRGB888: UInt32, font: ScreenFontReference, text: String)
    case glyphLabel(base: ScreenObjectBase, align: Align, colorRGB888: UInt32, glyph: ScreenGlyphReference)
    case dynamicLabel(base: ScreenObjectBase, align: Align, colorRGB888: UInt32, font: ScreenFontReference, widgetID: String)
    case progress(base: ScreenObjectBase, backgroundRGB888: UInt32, fillRGB888: UInt32, widgetID: String)
    case iconText(base: ScreenObjectBase, colorRGB888: UInt32, font: ScreenFontReference, gap: UInt16, sha256: String, widgetID: String)
    case container(base: ScreenObjectBase, kind: ContainerKind, crossAlign: AxisAlign, gap: UInt16, mainAlign: AxisAlign, children: [ScreenObjectNode])

    var base: ScreenObjectBase {
        switch self {
        case .image(let b, _), .pet(let b, _, _, _), .staticLabel(let b, _, _, _, _),
             .glyphLabel(let b, _, _, _), .dynamicLabel(let b, _, _, _, _),
             .progress(let b, _, _, _), .iconText(let b, _, _, _, _, _),
             .container(let b, _, _, _, _, _): return b
        }
    }
}

public struct ScreenRootObject: Equatable, Sendable {
    public let x: Int16
    public let y: Int16
    public let node: ScreenObjectNode
    public init(x: Int16, y: Int16, node: ScreenObjectNode) { self.x = x; self.y = y; self.node = node }
}

public struct ScreenCanonicalNumber: Equatable, Sendable {
    public let coefficient: Int64
    public let scale: UInt8
    public init(coefficient: Int64, scale: UInt8) { self.coefficient = coefficient; self.scale = scale }

    var canonical: String? {
        guard scale <= 3 else { return nil }
        if coefficient == 0 { return "0" }
        let negative = coefficient < 0
        let magnitude: UInt64 = coefficient == Int64.min ? UInt64(Int64.max) + 1 : UInt64(Swift.abs(coefficient))
        var digits = String(magnitude)
        if scale > 0 {
            while digits.count <= Int(scale) { digits.insert("0", at: digits.startIndex) }
            let split = digits.index(digits.endIndex, offsetBy: -Int(scale))
            digits.insert(".", at: split)
            while digits.last == "0" { digits.removeLast() }
            if digits.last == "." { digits.removeLast() }
        }
        return (negative ? "-" : "") + digits
    }
}

public enum ScreenWidgetDeclaration: Equatable, Sendable {
    case text(id: String, target: String, fallback: String)
    case integer(id: String, target: String, fallback: Int64)
    case number(id: String, target: String, fallback: ScreenCanonicalNumber, min: ScreenCanonicalNumber, max: ScreenCanonicalNumber, decimals: UInt8)
    case progress(id: String, target: String, fallback: ScreenCanonicalNumber, min: ScreenCanonicalNumber, max: ScreenCanonicalNumber, decimals: UInt8)
}

public struct ScreenLayout: Equatable, Sendable {
    public enum Mode: String, Equatable, Sendable { case dashboard, custom }
    public let backgroundRGB888: UInt32
    public let mode: Mode
    public let revision: UInt32
    public let objects: [ScreenRootObject]
    public let widgets: [ScreenWidgetDeclaration]

    public init(backgroundRGB888: UInt32, mode: Mode, revision: UInt32, objects: [ScreenRootObject], widgets: [ScreenWidgetDeclaration]) {
        self.backgroundRGB888 = backgroundRGB888
        self.mode = mode
        self.revision = revision
        self.objects = objects
        self.widgets = widgets
    }
}

public enum ScreenConfiguredPayload: Equatable, Sendable {
    case image(ScreenImage)
    case pet(ScreenPetManifest)
    case dashboard(ScreenLayout)
    case custom(ScreenLayout)
}

public struct ScreenCommitLimits: Equatable, Sendable {
    public let displayWidth: UInt16
    public let displayHeight: UInt16
    public let screen: ScreenCapability

    public init(displayWidth: UInt16, displayHeight: UInt16, screen: ScreenCapability) {
        self.displayWidth = displayWidth; self.displayHeight = displayHeight; self.screen = screen
    }
}

public struct ScreenCommit: Equatable, Sendable {
    public let expectedRevision: UInt32
    public let revision: UInt32
    public let assets: [ScreenAssetReference]
    public let payload: ScreenConfiguredPayload
    public let limits: ScreenCommitLimits

    public init(expectedRevision: UInt32, revision: UInt32, assets: [ScreenAssetReference], payload: ScreenConfiguredPayload, limits: ScreenCommitLimits) {
        self.expectedRevision = expectedRevision; self.revision = revision; self.assets = assets; self.payload = payload; self.limits = limits
    }
}

extension ScreenCommit {
    func canonicalBody() throws -> Data {
        try ScreenCommitValidator(commit: self).body()
    }
}

private struct ScreenCommitValidator {
    let commit: ScreenCommit
    private var capability: ScreenCapability { commit.limits.screen }

    func body() throws -> Data {
        guard commit.expectedRevision == capability.revision,
              isSerialNewer(commit.revision, than: capability.revision),
              commit.assets.count <= Int(capability.maxAssets)
        else { throw invalid("revision_or_assets") }
        var knownAssets: [String: AssetKind] = [:]
        var entries: [CanonicalJSON] = []
        for asset in commit.assets {
            guard asset.bytes != 0, BoundedJSON.validSHA(asset.sha256), knownAssets[asset.sha256] == nil else { throw invalid("assets") }
            knownAssets[asset.sha256] = asset.kind
            entries.append(.object(["bytes": .uint(UInt64(asset.bytes)), "kind": .string(asset.kind.rawValue), "sha256": .string(asset.sha256)]))
        }
        guard commit.assets.map(\.sha256) == commit.assets.map(\.sha256).sorted() else { throw invalid("assets_order") }

        let mode: String
        let image: CanonicalJSON
        let layout: CanonicalJSON
        let pet: CanonicalJSON
        switch commit.payload {
        case .image(let value):
            mode = "image"; image = try imageJSON(value, knownAssets: knownAssets); layout = .null; pet = .null
        case .pet(let value):
            mode = "pet"; image = .null; layout = .null; pet = try petJSON(value, knownAssets: knownAssets)
        case .dashboard(let value):
            guard value.mode == .dashboard else { throw invalid("layout_mode") }
            mode = "dashboard"; image = .null; layout = try layoutJSON(value, knownAssets: knownAssets); pet = .null
        case .custom(let value):
            guard value.mode == .custom else { throw invalid("layout_mode") }
            mode = "custom"; image = .null; layout = try layoutJSON(value, knownAssets: knownAssets); pet = .null
        }
        guard capability.modes.contains(mode) else { throw invalid("configured_mode") }
        let root = CanonicalJSON.object([
            "assets": .object(["assets": .array(entries)]),
            "event": .string("vk_screen_commit"),
            "expected_revision": .uint(UInt64(commit.expectedRevision)),
            "revision": .uint(UInt64(commit.revision)),
            "screen": .object(["configured_mode": .string(mode), "image": image, "layout": layout, "pet": pet])
        ])
        let data = Data(root.encoded.utf8)
        guard data.count <= Int(capability.maxCommitBytes), data.count <= BoundedJSON.maximumBytes else { throw ReplacementProtocolError.limitExceeded("max_commit_bytes") }
        try BoundedJSON.validateNegotiated(data, maximumDepth: UInt8(BoundedJSON.maximumDepth), maximumTokens: capability.maxJSONTokens, maximumStringBytes: capability.maxStringBytes)
        return data
    }

    private func imageJSON(_ value: ScreenImage, knownAssets: [String: AssetKind]) throws -> CanonicalJSON {
        guard value.backgroundRGB888 <= 0xFF_FFFF, BoundedJSON.validSHA(value.sha256), knownAssets[value.sha256] == .image else { throw invalid("image") }
        return .object(["background_rgb888": .uint(UInt64(value.backgroundRGB888)), "fit": .string(value.fit.rawValue), "sha256": .string(value.sha256)])
    }

    private func petJSON(_ value: ScreenPetManifest, knownAssets: [String: AssetKind]) throws -> CanonicalJSON {
        guard BoundedJSON.validIdentifier(value.id), value.states.count <= Int(capability.maxPetStates), case .asset? = value.states[.idle] else { throw invalid("pet") }
        var states: [String: CanonicalJSON] = [:]
        for (name, state) in value.states {
            switch state {
            case .asset(let hash):
                guard BoundedJSON.validSHA(hash), knownAssets[hash] == .image || knownAssets[hash] == .animation else { throw invalid("pet_asset") }
                states[name.rawValue] = .object(["sha256": .string(hash)])
            case .idleFallback:
                guard name != .idle else { throw invalid("pet_idle") }
                states[name.rawValue] = .object(["fallback": .string("idle")])
            }
        }
        return .object(["id": .string(value.id), "states": .object(states), "version": .uint(1)])
    }

    private func layoutJSON(_ value: ScreenLayout, knownAssets: [String: AssetKind]) throws -> CanonicalJSON {
        guard value.revision == commit.revision, value.backgroundRGB888 <= 0xFF_FFFF,
              value.objects.count <= Int(capability.maxObjects), value.widgets.count <= Int(capability.maxWidgets) else { throw invalid("layout") }
        var ids = Set<String>(); var targets: [String: (widgetID: String, kind: TargetKind)] = [:]; var count = 0
        let objects = try value.objects.map { root -> CanonicalJSON in
            var object = try nodeJSON(root.node, depth: 1, ids: &ids, targets: &targets, count: &count, knownAssets: knownAssets)
            object.add("x", .int(Int64(root.x))); object.add("y", .int(Int64(root.y)))
            let b = root.node.base
            guard fits(position: root.x, size: b.width, limit: commit.limits.displayWidth), fits(position: root.y, size: b.height, limit: commit.limits.displayHeight) else { throw invalid("root_bounds") }
            return object.value
        }
        let widgets = try widgetJSON(value.widgets, targets: targets)
        let result = CanonicalJSON.object(["background_rgb888": .uint(UInt64(value.backgroundRGB888)), "mode": .string(value.mode.rawValue), "objects": .array(objects), "revision": .uint(UInt64(value.revision)), "version": .uint(1), "widgets": .array(widgets)])
        guard result.encoded.utf8.count <= Int(capability.maxLayoutBytes) else { throw ReplacementProtocolError.limitExceeded("max_layout_bytes") }
        return result
    }

    private func nodeJSON(_ node: ScreenObjectNode, depth: Int, ids: inout Set<String>, targets: inout [String: (widgetID: String, kind: TargetKind)], count: inout Int, knownAssets: [String: AssetKind]) throws -> ObjectBuilder {
        guard depth <= Int(capability.maxDepth) else { throw ReplacementProtocolError.limitExceeded("max_depth") }
        count += 1; guard count <= Int(capability.maxObjects) else { throw ReplacementProtocolError.limitExceeded("max_objects") }
        let base = node.base
        guard BoundedJSON.validIdentifier(base.id), ids.insert(base.id).inserted, base.width > 0, base.height > 0 else { throw invalid("object_base") }
        var b = ObjectBuilder(base: base)
        switch node {
        case .image(_, let image):
            let v = try imageJSON(image, knownAssets: knownAssets); b.merge(v); b.add("type", .string("image"))
        case .pet(_, let background, let fit, let manifest):
            guard background <= 0xFF_FFFF else { throw invalid("background_rgb888") }
            b.add("background_rgb888", .uint(UInt64(background))); b.add("fit", .string(fit.rawValue)); b.add("pet", try petJSON(manifest, knownAssets: knownAssets)); b.add("type", .string("pet"))
        case .staticLabel(_, let align, let color, let font, let text):
            try validateFont(font); try validateText(text); guard color <= 0xFF_FFFF else { throw invalid("color_rgb888") }
            b.add("align", .string(align.rawValue)); b.add("color_rgb888", .uint(UInt64(color))); b.add("font", fontJSON(font)); b.add("overflow", .string("clip")); b.add("text", .string(text)); b.add("type", .string("static_label"))
        case .glyphLabel(_, let align, let color, let glyph):
            guard color <= 0xFF_FFFF, glyph.advance > 0, BoundedJSON.validSHA(glyph.sha256), knownAssets[glyph.sha256] == .glyphBitmap else { throw invalid("glyph") }
            b.add("align", .string(align.rawValue)); b.add("color_rgb888", .uint(UInt64(color))); b.add("font", .null); b.add("glyph", .object(["advance": .uint(UInt64(glyph.advance)), "baseline": .int(Int64(glyph.baseline)), "bearing_x": .int(Int64(glyph.bearingX)), "bearing_y": .int(Int64(glyph.bearingY)), "sha256": .string(glyph.sha256)])); b.add("overflow", .string("clip")); b.add("type", .string("glyph_label"))
        case .dynamicLabel(_, let align, let color, let font, let widgetID):
            try validateFont(font); guard color <= 0xFF_FFFF, BoundedJSON.validIdentifier(widgetID) else { throw invalid("dynamic_label") }
            b.add("align", .string(align.rawValue)); b.add("color_rgb888", .uint(UInt64(color))); b.add("font", fontJSON(font)); b.add("overflow", .string("clip")); b.add("type", .string("dynamic_label")); b.add("widget_id", .string(widgetID)); targets[base.id] = (widgetID, .text)
        case .progress(_, let background, let fill, let widgetID):
            guard background <= 0xFF_FFFF, fill <= 0xFF_FFFF, BoundedJSON.validIdentifier(widgetID) else { throw invalid("progress") }
            b.add("background_rgb888", .uint(UInt64(background))); b.add("fill_rgb888", .uint(UInt64(fill))); b.add("type", .string("progress")); b.add("widget_id", .string(widgetID)); targets[base.id] = (widgetID, .progress)
        case .iconText(_, let color, let font, let gap, let hash, let widgetID):
            try validateFont(font); guard color <= 0xFF_FFFF, BoundedJSON.validSHA(hash), knownAssets[hash] == .image, BoundedJSON.validIdentifier(widgetID) else { throw invalid("icon_text") }
            b.add("color_rgb888", .uint(UInt64(color))); b.add("font", fontJSON(font)); b.add("gap", .uint(UInt64(gap))); b.add("sha256", .string(hash)); b.add("type", .string("icon_text")); b.add("widget_id", .string(widgetID)); targets[base.id] = (widgetID, .text)
        case .container(_, let kind, let cross, let gap, let main, let children):
            guard !children.isEmpty, children.count <= Int(capability.maxObjects) else { throw invalid("children") }
            var childJSON: [CanonicalJSON] = []; var mainSize: UInt64 = 0
            for child in children {
                let cb = child.base
                mainSize += UInt64(kind == .row ? cb.width : cb.height)
                childJSON.append(try nodeJSON(child, depth: depth + 1, ids: &ids, targets: &targets, count: &count, knownAssets: knownAssets).value)
            }
            mainSize += UInt64(gap) * UInt64(children.count - 1)
            guard mainSize <= UInt64(kind == .row ? base.width : base.height), children.allSatisfy({ UInt64(kind == .row ? $0.base.height : $0.base.width) <= UInt64(kind == .row ? base.height : base.width) }) else { throw invalid("container_bounds") }
            b.add("children", .array(childJSON)); b.add("cross_align", .string(cross.rawValue)); b.add("gap", .uint(UInt64(gap))); b.add("main_align", .string(main.rawValue)); b.add("type", .string(kind.rawValue))
        }
        return b
    }

    private func widgetJSON(_ values: [ScreenWidgetDeclaration], targets: [String: (widgetID: String, kind: TargetKind)]) throws -> [CanonicalJSON] {
        var ids = Set<String>(); var usedTargets = Set<String>(); var output: [CanonicalJSON] = []
        for value in values {
            let id: String, target: String, type: String, expected: TargetKind, fields: [String: CanonicalJSON]
            switch value {
            case .text(let i, let t, let fallback):
                id = i; target = t; type = "text"; expected = .text; try validateWidgetText(fallback); fields = ["fallback": .string(fallback)]
            case .integer(let i, let t, let fallback):
                id = i; target = t; type = "integer"; expected = .text; fields = ["fallback": .int(fallback)]
            case .number(let i, let t, let fallback, let min, let max, let decimals):
                id = i; target = t; type = "number"; expected = .text; fields = try numericFields(fallback, min, max, decimals)
            case .progress(let i, let t, let fallback, let min, let max, let decimals):
                id = i; target = t; type = "progress"; expected = .progress; fields = try numericFields(fallback, min, max, decimals)
            }
            guard BoundedJSON.validIdentifier(id), BoundedJSON.validIdentifier(target), ids.insert(id).inserted, usedTargets.insert(target).inserted,
                  let actual = targets[target], actual.widgetID == id, actual.kind == expected else { throw invalid("widget_binding") }
            var object = fields; object["id"] = .string(id); object["target"] = .string(target); object["type"] = .string(type); output.append(.object(object))
        }
        guard usedTargets.count == targets.count else { throw invalid("widget_missing_declaration") }
        return output
    }

    private func numericFields(_ fallback: ScreenCanonicalNumber, _ min: ScreenCanonicalNumber, _ max: ScreenCanonicalNumber, _ decimals: UInt8) throws -> [String: CanonicalJSON] {
        guard decimals <= 3, let f = fallback.canonical, let lo = min.canonical, let hi = max.canonical,
              compare(min, max) == .orderedAscending, compare(fallback, min) != .orderedAscending, compare(fallback, max) != .orderedDescending else { throw invalid("widget_number") }
        return ["fallback":.number(f), "format":.object(["decimals":.uint(UInt64(decimals))]), "max":.number(hi), "min":.number(lo)]
    }

    private func compare(_ lhs: ScreenCanonicalNumber, _ rhs: ScreenCanonicalNumber) -> ComparisonResult {
        let scale = max(lhs.scale, rhs.scale)
        func scaled(_ value: ScreenCanonicalNumber) -> Decimal { Decimal(value.coefficient) * pow(Decimal(10), Int(scale - value.scale)) }
        let l=scaled(lhs), r=scaled(rhs); return l < r ? .orderedAscending : (l > r ? .orderedDescending : .orderedSame)
    }
    private func validateFont(_ font: ScreenFontReference) throws {
        guard capability.fonts.contains(where: { $0.id == font.id && $0.version == font.version }) else { throw invalid("font_mismatch") }
    }
    private func fontJSON(_ font: ScreenFontReference) -> CanonicalJSON { .object(["id":.string(font.id), "version":.uint(UInt64(font.version))]) }
    private func validateText(_ value: String) throws { guard value.utf8.count <= Int(capability.maxStringBytes) else { throw ReplacementProtocolError.limitExceeded("max_string_bytes") } }
    private func validateWidgetText(_ value: String) throws { guard value.utf8.count <= Int(capability.maxWidgetValueBytes) else { throw ReplacementProtocolError.limitExceeded("max_widget_value_bytes") } }
    private func isSerialNewer(_ candidate: UInt32, than current: UInt32) -> Bool {
        let delta = candidate &- current
        return delta != 0 && delta < 0x8000_0000
    }
    private func fits(position: Int16, size: UInt16, limit: UInt16) -> Bool { Int64(position) >= 0 && Int64(position) + Int64(size) <= Int64(limit) }
    private func invalid(_ field: String) -> ReplacementProtocolError { .invalidValue(field: field) }
}

private enum TargetKind { case text, progress }

private struct ObjectBuilder {
    private var fields: [String: CanonicalJSON]
    init(base: ScreenObjectBase) {
        fields=["clip":.bool(base.clip),"height":.uint(UInt64(base.height)),"id":.string(base.id),"visible":.bool(base.visible),"width":.uint(UInt64(base.width)),"z":.int(Int64(base.z))]
    }
    mutating func add(_ key: String, _ value: CanonicalJSON) { fields[key]=value }
    mutating func merge(_ value: CanonicalJSON) { if case .object(let source)=value { for (k,v) in source { fields[k]=v } } }
    var value: CanonicalJSON { .object(fields) }
}

private indirect enum CanonicalJSON {
    case object([String: CanonicalJSON]), array([CanonicalJSON]), string(String), uint(UInt64), int(Int64), number(String), bool(Bool), null
    var encoded: String {
        switch self {
        case .object(let v): return "{" + v.keys.sorted().map { quote($0)+":"+v[$0]!.encoded }.joined(separator:",") + "}"
        case .array(let v): return "[" + v.map(\.encoded).joined(separator:",") + "]"
        case .string(let v): return quote(v)
        case .uint(let v): return String(v)
        case .int(let v): return String(v)
        case .number(let v): return v
        case .bool(let v): return v ? "true" : "false"
        case .null: return "null"
        }
    }
    private func quote(_ value: String) -> String {
        let data = try! JSONSerialization.data(withJSONObject: [value], options: [.withoutEscapingSlashes])
        return String(decoding: data.dropFirst().dropLast(), as: UTF8.self)
    }
}

private extension Decimal {
    static func * (lhs: Decimal, rhs: Decimal) -> Decimal { var l=lhs,r=rhs,result=Decimal(); NSDecimalMultiply(&result,&l,&r,.plain); return result }
}
private func pow(_ base: Decimal, _ exponent: Int) -> Decimal { (0..<exponent).reduce(Decimal(1)) { value,_ in value * base } }
