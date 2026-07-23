import Foundation

public enum ReplacementProtocolError: Error, Equatable, Sendable {
    case invalidJSON
    case invalidEnvelope
    case invalidKeys(context: String)
    case invalidValue(field: String)
    case limitExceeded(String)
    case crossFeatureInvariant
}

public enum StorageState: String, Equatable, Sendable {
    case unformatted
    case ready
    case corrupt
    case mountFailed = "mount_failed"
    case busy
}

public enum FeatureAvailability<Available: Equatable & Sendable>: Equatable, Sendable {
    case available(Available)
    case unavailable(UnavailableFeature)
}

public struct UnavailableFeature: Equatable, Sendable {
    public let version: UInt16
    public let reason: String

    public init(version: UInt16 = 1, reason: String) {
        self.version = version
        self.reason = reason
    }
}

public struct AssetsCapability: Equatable, Sendable {
    public let management: Bool
    public let storageState: StorageState
    public let freeBytes: UInt32
    public let reserveBytes: UInt32
    public let uploadMaxBytes: UInt32
    public let maxAssetBytes: UInt32
    public let chunkBytes: UInt16
    public let maxAssets: UInt16
    public let maxFrames: UInt16
    public let minFrameMS: UInt16
    public let maxFrameMS: UInt16
    public let maxActiveDecodedBytes: UInt32
    public let decoderScratchBytes: UInt32
    public let encodings: [String]
    public let revision: UInt32
}

public struct ScreenFontCapability: Equatable, Sendable {
    public let id: String
    public let version: UInt16
    public let metricsSHA256: String
}

public struct LEDCapability: Equatable, Sendable {
    public let keyPixels: [String: UInt8]
    public let maxBrightness: UInt8
    public let maxFrameChannelSum: UInt16

    public init(keyPixels: [String: UInt8], maxBrightness: UInt8, maxFrameChannelSum: UInt16) {
        self.keyPixels = keyPixels
        self.maxBrightness = maxBrightness
        self.maxFrameChannelSum = maxFrameChannelSum
    }
}

public struct UpdateCapability: Equatable, Sendable {
    public let chunkBytes: UInt16
    public let maxImageBytes: UInt32
    public let target: String
    public let stagedMetadata: String
    public let rollback: String
}

public struct ScreenCapability: Equatable, Sendable {
    public let modes: [String]
    public let maxCommitBytes: UInt16
    public let maxLayoutBytes: UInt16
    public let maxAssets: UInt16
    public let maxObjects: UInt16
    public let maxDepth: UInt8
    public let maxWidgets: UInt16
    public let maxFonts: UInt16
    public let maxPetStates: UInt8
    public let maxStringBytes: UInt16
    public let maxJSONTokens: UInt16
    public let maxWidgetValueBytes: UInt16
    public let revision: UInt32
    public let configured: Bool
    public let fonts: [ScreenFontCapability]
}

public extension ScreenCapability {
    func selecting(revision: UInt32, configured: Bool) -> ScreenCapability {
        ScreenCapability(
            modes: modes,
            maxCommitBytes: maxCommitBytes,
            maxLayoutBytes: maxLayoutBytes,
            maxAssets: maxAssets,
            maxObjects: maxObjects,
            maxDepth: maxDepth,
            maxWidgets: maxWidgets,
            maxFonts: maxFonts,
            maxPetStates: maxPetStates,
            maxStringBytes: maxStringBytes,
            maxJSONTokens: maxJSONTokens,
            maxWidgetValueBytes: maxWidgetValueBytes,
            revision: revision,
            configured: configured,
            fonts: fonts
        )
    }
}

public struct ReplacementCapabilitySnapshot: Equatable, Sendable {
    public let protocolVersion: UInt16
    public let display: CapabilityDisplay
    public let assets: FeatureAvailability<AssetsCapability>?
    public let screen: FeatureAvailability<ScreenCapability>?
    public let update: FeatureAvailability<UpdateCapability>?
    public let led: FeatureAvailability<LEDCapability>?

    public static func decode(_ data: Data) throws -> ReplacementCapabilitySnapshot {
        let root = try BoundedJSON.object(data)
        try BoundedJSON.exactKeys(root, ["display", "event", "features", "protocol"], "vk_capabilities")
        guard BoundedJSON.string(root["event"]) == "vk_capabilities",
              BoundedJSON.uint(root["protocol"], as: UInt16.self) == 1,
              let displayObject = BoundedJSON.dictionary(root["display"]),
              let featureObject = BoundedJSON.dictionary(root["features"])
        else { throw ReplacementProtocolError.invalidEnvelope }

        try BoundedJSON.exactKeys(displayObject, ["format", "height", "width"], "display")
        guard let width = BoundedJSON.uint(displayObject["width"], as: UInt16.self),
              let height = BoundedJSON.uint(displayObject["height"], as: UInt16.self),
              let format = BoundedJSON.string(displayObject["format"])
        else { throw ReplacementProtocolError.invalidValue(field: "display") }

        let assets = try featureObject["assets"].map { try decodeAssets($0) }
        let screen = try featureObject["screen"].map { try decodeScreen($0) }
        let update = try featureObject["update"].map { try decodeUpdate($0) }
        let led = try featureObject["led"].map { try decodeLED($0) }
        if case .available(let screenProfile)? = screen {
            guard case .available(let assetsProfile)? = assets,
                  screenProfile.maxAssets <= assetsProfile.maxAssets
            else { throw ReplacementProtocolError.crossFeatureInvariant }
        }

        return ReplacementCapabilitySnapshot(
            protocolVersion: 1,
            display: CapabilityDisplay(width: width, height: height, format: format),
            assets: assets,
            screen: screen,
            update: update,
            led: led
        )
    }

    private static func decodeLED(_ value: Any) throws -> FeatureAvailability<LEDCapability> {
        guard let object = BoundedJSON.dictionary(value) else { throw ReplacementProtocolError.invalidValue(field: "led") }
        guard BoundedJSON.uint(object["version"], as: UInt16.self) == 1,
              let available = BoundedJSON.bool(object["available"])
        else { throw ReplacementProtocolError.invalidValue(field: "led") }
        if !available {
            try BoundedJSON.exactKeys(object, ["available", "reason", "version"], "led unavailable")
            guard let reason = BoundedJSON.string(object["reason"]),
                  ["calibration_required", "hardware_failed", "tainted"].contains(reason)
            else { throw ReplacementProtocolError.invalidValue(field: "led.reason") }
            return .unavailable(UnavailableFeature(reason: reason))
        }
        try BoundedJSON.exactKeys(object, ["available", "color_model", "key_pixels", "max_brightness", "max_frame_channel_sum", "pixel_count", "strip_count", "strip_first", "tick_ms", "version", "wire_order"], "led")
        guard BoundedJSON.uint(object["pixel_count"], as: UInt8.self) == 17,
              BoundedJSON.uint(object["strip_first"], as: UInt8.self) == 4,
              BoundedJSON.uint(object["strip_count"], as: UInt8.self) == 13,
              BoundedJSON.string(object["color_model"]) == "rgb8",
              BoundedJSON.string(object["wire_order"]) == "grb",
              BoundedJSON.uint(object["tick_ms"], as: UInt8.self) == 30,
              let brightness = BoundedJSON.range(object["max_brightness"], as: UInt8.self, 1...255),
              let frameSum = BoundedJSON.range(object["max_frame_channel_sum"], as: UInt16.self, 1...13_005),
              let pixels = BoundedJSON.dictionary(object["key_pixels"])
        else { throw ReplacementProtocolError.invalidValue(field: "led") }
        try BoundedJSON.exactKeys(pixels, ["k1", "k2", "k3", "k4"], "led.key_pixels")
        var mapping: [String: UInt8] = [:]
        for key in ["k1", "k2", "k3", "k4"] {
            guard let pixel = BoundedJSON.range(pixels[key], as: UInt8.self, 0...3) else {
                throw ReplacementProtocolError.invalidValue(field: "led.key_pixels")
            }
            mapping[key] = pixel
        }
        guard Set(mapping.values) == Set(UInt8(0)...UInt8(3)) else {
            throw ReplacementProtocolError.invalidValue(field: "led.key_pixels")
        }
        return .available(LEDCapability(keyPixels: mapping, maxBrightness: brightness, maxFrameChannelSum: frameSum))
    }

    private static func decodeUpdate(_ value: Any) throws -> FeatureAvailability<UpdateCapability> {
        guard let object = BoundedJSON.dictionary(value) else { throw ReplacementProtocolError.invalidValue(field: "update") }
        guard BoundedJSON.uint(object["version"], as: UInt16.self) == 1,
              let available = BoundedJSON.bool(object["available"])
        else { throw ReplacementProtocolError.invalidValue(field: "update") }
        if !available {
            try BoundedJSON.exactKeys(object, ["available", "reason", "version"], "update unavailable")
            guard let reason = BoundedJSON.string(object["reason"]),
                  ["bootloader_migration_required", "busy", "wrong_running_slot", "target_unavailable", "integrity_unavailable", "policy_blocked"].contains(reason)
            else { throw ReplacementProtocolError.invalidValue(field: "update.reason") }
            return .unavailable(UnavailableFeature(reason: reason))
        }
        try BoundedJSON.exactKeys(object, ["available", "chunk_bytes", "max_image_bytes", "rollback", "staged_metadata", "target", "version"], "update")
        guard let chunk = BoundedJSON.range(object["chunk_bytes"], as: UInt16.self, 1...512),
              let maximum = BoundedJSON.positive(object["max_image_bytes"], as: UInt32.self),
              let target = BoundedJSON.string(object["target"]), ["ota_0", "ota_1"].contains(target),
              BoundedJSON.string(object["staged_metadata"]) == "ram_epoch",
              BoundedJSON.string(object["rollback"]) == "bootloader_pending_verify"
        else { throw ReplacementProtocolError.invalidValue(field: "update") }
        return .available(UpdateCapability(chunkBytes: chunk, maxImageBytes: maximum, target: target, stagedMetadata: "ram_epoch", rollback: "bootloader_pending_verify"))
    }

    private static func decodeAssets(_ value: Any) throws -> FeatureAvailability<AssetsCapability> {
        guard let object = BoundedJSON.dictionary(value) else { throw ReplacementProtocolError.invalidValue(field: "assets") }
        guard BoundedJSON.uint(object["version"], as: UInt16.self) == 1,
              let available = BoundedJSON.bool(object["available"])
        else { throw ReplacementProtocolError.invalidValue(field: "assets") }
        if !available {
            try BoundedJSON.exactKeys(object, ["available", "reason", "version"], "assets unavailable")
            guard let reason = BoundedJSON.string(object["reason"]), ["display_acceptance_required", "storage_unavailable", "integrity_unavailable", "policy_blocked"].contains(reason) else { throw ReplacementProtocolError.invalidValue(field: "assets.reason") }
            return .unavailable(UnavailableFeature(reason: reason))
        }
        let keys: Set<String> = ["available", "chunk_bytes", "decoder_scratch_bytes", "encodings", "free_bytes", "management", "max_active_decoded_bytes", "max_asset_bytes", "max_assets", "max_frame_ms", "max_frames", "min_frame_ms", "reserve_bytes", "revision", "storage_state", "upload_max_bytes", "version"]
        try BoundedJSON.exactKeys(object, keys, "assets")
        guard BoundedJSON.bool(object["management"]) == true,
              let storageRaw = BoundedJSON.string(object["storage_state"]), let storage = StorageState(rawValue: storageRaw),
              let free = BoundedJSON.uint(object["free_bytes"], as: UInt32.self),
              let reserve = BoundedJSON.positive(object["reserve_bytes"], as: UInt32.self),
              let upload = BoundedJSON.uint(object["upload_max_bytes"], as: UInt32.self),
              let maxAsset = BoundedJSON.positive(object["max_asset_bytes"], as: UInt32.self),
              let chunk = BoundedJSON.range(object["chunk_bytes"], as: UInt16.self, 1...4084),
              let maxAssets = BoundedJSON.range(object["max_assets"], as: UInt16.self, 1...1024),
              let maxFrames = BoundedJSON.positive(object["max_frames"], as: UInt16.self),
              let minFrame = BoundedJSON.positive(object["min_frame_ms"], as: UInt16.self),
              let maxFrame = BoundedJSON.positive(object["max_frame_ms"], as: UInt16.self), minFrame <= maxFrame,
              let decoded = BoundedJSON.positive(object["max_active_decoded_bytes"], as: UInt32.self),
              let scratch = BoundedJSON.range(object["decoder_scratch_bytes"], as: UInt32.self, 1...decoded),
              BoundedJSON.stringArray(object["encodings"]) == ["raw", "row_rle"],
              let revision = BoundedJSON.uint(object["revision"], as: UInt32.self),
              upload <= maxAsset,
              upload <= (free > reserve ? free - reserve : 0),
              (storage == .ready || storage == .busy) || free == 0,
              storage == .ready || upload == 0
        else { throw ReplacementProtocolError.invalidValue(field: "assets") }
        return .available(AssetsCapability(management: true, storageState: storage, freeBytes: free, reserveBytes: reserve, uploadMaxBytes: upload, maxAssetBytes: maxAsset, chunkBytes: chunk, maxAssets: maxAssets, maxFrames: maxFrames, minFrameMS: minFrame, maxFrameMS: maxFrame, maxActiveDecodedBytes: decoded, decoderScratchBytes: scratch, encodings: ["raw", "row_rle"], revision: revision))
    }

    private static func decodeScreen(_ value: Any) throws -> FeatureAvailability<ScreenCapability> {
        guard let object = BoundedJSON.dictionary(value) else { throw ReplacementProtocolError.invalidValue(field: "screen") }
        guard BoundedJSON.uint(object["version"], as: UInt16.self) == 1,
              let available = BoundedJSON.bool(object["available"])
        else { throw ReplacementProtocolError.invalidValue(field: "screen") }
        if !available {
            try BoundedJSON.exactKeys(object, ["available", "reason", "version"], "screen unavailable")
            guard let reason = BoundedJSON.string(object["reason"]), ["display_acceptance_required", "panel_unavailable", "model_unavailable", "storage_unavailable", "policy_blocked"].contains(reason) else { throw ReplacementProtocolError.invalidValue(field: "screen.reason") }
            return .unavailable(UnavailableFeature(reason: reason))
        }
        let keys: Set<String> = ["available", "configured", "fonts", "max_assets", "max_commit_bytes", "max_depth", "max_fonts", "max_json_tokens", "max_layout_bytes", "max_objects", "max_pet_states", "max_string_bytes", "max_widget_value_bytes", "max_widgets", "modes", "revision", "version"]
        try BoundedJSON.exactKeys(object, keys, "screen")
        guard let modes = BoundedJSON.stringArray(object["modes"]), !modes.isEmpty, modes == ["image", "pet", "dashboard", "custom"].filter(modes.contains), Set(modes).count == modes.count,
              let commit = BoundedJSON.range(object["max_commit_bytes"], as: UInt16.self, 1...4092),
              let layout = BoundedJSON.range(object["max_layout_bytes"], as: UInt16.self, 1...commit),
              let maxAssets = BoundedJSON.range(object["max_assets"], as: UInt16.self, 1...1024),
              let objects = BoundedJSON.positive(object["max_objects"], as: UInt16.self),
              let depth = BoundedJSON.range(object["max_depth"], as: UInt8.self, 1...8),
              let widgets = BoundedJSON.positive(object["max_widgets"], as: UInt16.self),
              let maxFonts = BoundedJSON.positive(object["max_fonts"], as: UInt16.self),
              let petStates = BoundedJSON.range(object["max_pet_states"], as: UInt8.self, 1...6),
              let strings = BoundedJSON.range(object["max_string_bytes"], as: UInt16.self, 1...512),
              let tokens = BoundedJSON.range(object["max_json_tokens"], as: UInt16.self, 32...1024),
              let widgetBytes = BoundedJSON.range(object["max_widget_value_bytes"], as: UInt16.self, 1...512),
              let revision = BoundedJSON.uint(object["revision"], as: UInt32.self),
              let configured = BoundedJSON.bool(object["configured"]), configured ? revision != 0 : revision == 0,
              let fontValues = BoundedJSON.array(object["fonts"]), !fontValues.isEmpty, fontValues.count <= Int(maxFonts)
        else { throw ReplacementProtocolError.invalidValue(field: "screen") }
        let fonts = try fontValues.map { value -> ScreenFontCapability in
            guard let font = BoundedJSON.dictionary(value) else { throw ReplacementProtocolError.invalidValue(field: "screen.fonts") }
            try BoundedJSON.exactKeys(font, ["id", "metrics_sha256", "version"], "font")
            guard let id = BoundedJSON.string(font["id"]), BoundedJSON.validIdentifier(id),
                  let version = BoundedJSON.positive(font["version"], as: UInt16.self),
                  let hash = BoundedJSON.string(font["metrics_sha256"]), BoundedJSON.validSHA(hash)
            else { throw ReplacementProtocolError.invalidValue(field: "screen.font") }
            return ScreenFontCapability(id: id, version: version, metricsSHA256: hash)
        }
        guard fonts.map(\.id) == fonts.map(\.id).sorted(), Set(fonts.map(\.id)).count == fonts.count else { throw ReplacementProtocolError.invalidValue(field: "screen.fonts") }
        return .available(ScreenCapability(modes: modes, maxCommitBytes: commit, maxLayoutBytes: layout, maxAssets: maxAssets, maxObjects: objects, maxDepth: depth, maxWidgets: widgets, maxFonts: maxFonts, maxPetStates: petStates, maxStringBytes: strings, maxJSONTokens: tokens, maxWidgetValueBytes: widgetBytes, revision: revision, configured: configured, fonts: fonts))
    }
}

enum BoundedJSON {
    static let maximumBytes = 4092
    static let maximumDepth = 12
    static let maximumTokens = 1024
    static let maximumStringBytes = 512

    static func object(_ data: Data) throws -> [String: Any] {
        let value = try decodedValue(data)
        var tokens = 0
        try validate(value, depth: 0, tokens: &tokens)
        guard let object = value as? [String: Any] else { throw ReplacementProtocolError.invalidEnvelope }
        return object
    }

    static func validateNegotiated(_ data: Data, maximumDepth: UInt8, maximumTokens: UInt16, maximumStringBytes: UInt16) throws {
        let value = try decodedValue(data)
        var tokens = 0
        try validate(value, depth: 0, tokens: &tokens, depthLimit: Int(maximumDepth), tokenLimit: Int(maximumTokens), stringLimit: Int(maximumStringBytes))
    }

    private static func decodedValue(_ data: Data) throws -> Any {
        guard data.count <= maximumBytes else { throw ReplacementProtocolError.limitExceeded("bytes") }
        var parser = BoundedJSONParser(
            data,
            depthLimit: maximumDepth,
            tokenLimit: maximumTokens,
            stringLimit: maximumStringBytes
        )
        return try parser.parse()
    }

    private static func validate(_ value: Any, depth: Int, tokens: inout Int, depthLimit: Int = maximumDepth, tokenLimit: Int = maximumTokens, stringLimit: Int = maximumStringBytes) throws {
        guard depth <= depthLimit else { throw ReplacementProtocolError.limitExceeded("depth") }
        tokens += 1
        guard tokens <= tokenLimit else { throw ReplacementProtocolError.limitExceeded("tokens") }
        if let object = value as? [String: Any] {
            for (key, child) in object {
                guard key.utf8.count <= stringLimit else { throw ReplacementProtocolError.limitExceeded("string") }
                tokens += 1
                try validate(child, depth: depth + 1, tokens: &tokens, depthLimit: depthLimit, tokenLimit: tokenLimit, stringLimit: stringLimit)
            }
        } else if let array = value as? [Any] {
            for child in array { try validate(child, depth: depth + 1, tokens: &tokens, depthLimit: depthLimit, tokenLimit: tokenLimit, stringLimit: stringLimit) }
        } else if let string = value as? String {
            guard string.utf8.count <= stringLimit else { throw ReplacementProtocolError.limitExceeded("string") }
        } else if value is NSNumber || value is JSONNumber || value is NSNull {
            return
        } else { throw ReplacementProtocolError.invalidJSON }
    }

    static func exactKeys(_ object: [String: Any], _ keys: Set<String>, _ context: String) throws {
        guard Set(object.keys) == keys else { throw ReplacementProtocolError.invalidKeys(context: context) }
    }
    static func dictionary(_ value: Any?) -> [String: Any]? { value as? [String: Any] }
    static func array(_ value: Any?) -> [Any]? { value as? [Any] }
    static func string(_ value: Any?) -> String? { value as? String }
    static func stringArray(_ value: Any?) -> [String]? { value as? [String] }
    static func bool(_ value: Any?) -> Bool? {
        guard let number = value as? NSNumber, CFGetTypeID(number) == CFBooleanGetTypeID() else { return nil }
        return number.boolValue
    }
    static func uint<T: FixedWidthInteger & UnsignedInteger>(_ value: Any?, as: T.Type) -> T? {
        guard let number = value as? JSONNumber, number.isCanonicalUnsignedInteger else { return nil }
        var result: T = 0
        for byte in number.lexeme.utf8 {
            let digit = T(byte - 0x30)
            let (multiplied, multiplyOverflow) = result.multipliedReportingOverflow(by: 10)
            guard !multiplyOverflow else { return nil }
            let (added, addOverflow) = multiplied.addingReportingOverflow(digit)
            guard !addOverflow else { return nil }
            result = added
        }
        return result
    }
    static func positive<T: FixedWidthInteger & UnsignedInteger>(_ value: Any?, as: T.Type) -> T? {
        guard let result = uint(value, as: T.self), result > 0 else { return nil }
        return result
    }
    static func range<T: FixedWidthInteger & UnsignedInteger>(_ value: Any?, as: T.Type, _ range: ClosedRange<T>) -> T? {
        guard let result = uint(value, as: T.self), range.contains(result) else { return nil }
        return result
    }
    static func validSHA(_ value: String) -> Bool {
        let bytes = Array(value.utf8)
        return bytes.count == 64 && bytes.allSatisfy { (0x30...0x39).contains($0) || (0x61...0x66).contains($0) }
    }
    static func validIdentifier(_ value: String) -> Bool {
        guard (1...32).contains(value.utf8.count) else { return false }
        return value.utf8.allSatisfy { (48...57).contains($0) || (65...90).contains($0) || (97...122).contains($0) || $0 == 45 || $0 == 95 }
    }
}

private struct JSONNumber {
    let lexeme: String

    var isCanonicalUnsignedInteger: Bool {
        let bytes = Array(lexeme.utf8)
        guard !bytes.isEmpty else { return false }
        if bytes == [0x30] { return true }
        guard (0x31...0x39).contains(bytes[0]) else { return false }
        return bytes.dropFirst().allSatisfy { (0x30...0x39).contains($0) }
    }
}

private struct BoundedJSONParser {
    private let bytes: [UInt8]
    private let depthLimit: Int
    private let tokenLimit: Int
    private let stringLimit: Int
    private var index = 0
    private var tokens = 0

    init(_ data: Data, depthLimit: Int, tokenLimit: Int, stringLimit: Int) {
        bytes = Array(data)
        self.depthLimit = depthLimit
        self.tokenLimit = tokenLimit
        self.stringLimit = stringLimit
    }

    mutating func parse() throws -> Any {
        skipWhitespace()
        let value = try parseValue(depth: 0)
        skipWhitespace()
        guard index == bytes.count else { throw ReplacementProtocolError.invalidJSON }
        return value
    }

    private mutating func parseValue(depth: Int) throws -> Any {
        guard depth <= depthLimit else { throw ReplacementProtocolError.limitExceeded("depth") }
        try consumeToken()
        skipWhitespace()
        guard index < bytes.count else { throw ReplacementProtocolError.invalidJSON }
        switch bytes[index] {
        case 0x7b: return try parseObject(depth: depth)
        case 0x5b: return try parseArray(depth: depth)
        case 0x22:
            guard let value = parseString() else { throw ReplacementProtocolError.invalidJSON }
            try validateString(value)
            return value
        case 0x74:
            guard consume("true") else { throw ReplacementProtocolError.invalidJSON }
            return NSNumber(value: true)
        case 0x66:
            guard consume("false") else { throw ReplacementProtocolError.invalidJSON }
            return NSNumber(value: false)
        case 0x6e:
            guard consume("null") else { throw ReplacementProtocolError.invalidJSON }
            return NSNull()
        default:
            guard let value = parseNumber() else { throw ReplacementProtocolError.invalidJSON }
            return value
        }
    }

    private mutating func parseObject(depth: Int) throws -> [String: Any] {
        index += 1
        skipWhitespace()
        if consumeByte(0x7d) { return [:] }
        var result: [String: Any] = [:]
        while true {
            skipWhitespace()
            try consumeToken() // Object keys count as JSON tokens.
            guard let key = parseString(), result[key] == nil else { throw ReplacementProtocolError.invalidJSON }
            try validateString(key)
            skipWhitespace()
            guard consumeByte(0x3a) else { throw ReplacementProtocolError.invalidJSON }
            let value = try parseValue(depth: checkedChildDepth(depth))
            result[key] = value
            skipWhitespace()
            if consumeByte(0x7d) { return result }
            guard consumeByte(0x2c) else { throw ReplacementProtocolError.invalidJSON }
        }
    }

    private mutating func parseArray(depth: Int) throws -> [Any] {
        index += 1
        skipWhitespace()
        if consumeByte(0x5d) { return [] }
        var result: [Any] = []
        while true {
            let value = try parseValue(depth: checkedChildDepth(depth))
            result.append(value)
            skipWhitespace()
            if consumeByte(0x5d) { return result }
            guard consumeByte(0x2c) else { throw ReplacementProtocolError.invalidJSON }
        }
    }

    private func checkedChildDepth(_ depth: Int) throws -> Int {
        let (childDepth, overflow) = depth.addingReportingOverflow(1)
        guard !overflow, childDepth <= depthLimit else {
            throw ReplacementProtocolError.limitExceeded("depth")
        }
        return childDepth
    }

    private mutating func consumeToken() throws {
        let (next, overflow) = tokens.addingReportingOverflow(1)
        guard !overflow, next <= tokenLimit else {
            throw ReplacementProtocolError.limitExceeded("tokens")
        }
        tokens = next
    }

    private func validateString(_ value: String) throws {
        guard value.utf8.count <= stringLimit else {
            throw ReplacementProtocolError.limitExceeded("string")
        }
    }

    private mutating func parseString() -> String? {
        let opening = index
        guard consumeByte(0x22) else { return nil }
        var escaped = false
        while index < bytes.count {
            let byte = bytes[index]
            if !escaped && byte == 0x22 {
                index += 1
                let quoted = Data(bytes[opening..<index])
                return try? JSONSerialization.jsonObject(with: quoted, options: [.fragmentsAllowed]) as? String
            }
            if !escaped && byte == 0x5c {
                escaped = true
                index += 1
                continue
            }
            escaped = false
            index += 1
        }
        return nil
    }

    private mutating func parseNumber() -> JSONNumber? {
        let start = index
        if consumeByte(0x2d), index >= bytes.count { return nil }

        if consumeByte(0x30) {
            if index < bytes.count, (0x30...0x39).contains(bytes[index]) { return nil }
        } else {
            guard index < bytes.count, (0x31...0x39).contains(bytes[index]) else { return nil }
            index += 1
            while index < bytes.count, (0x30...0x39).contains(bytes[index]) { index += 1 }
        }

        if consumeByte(0x2e) {
            let fractionStart = index
            while index < bytes.count, (0x30...0x39).contains(bytes[index]) { index += 1 }
            guard index > fractionStart else { return nil }
        }

        if index < bytes.count, (bytes[index] == 0x65 || bytes[index] == 0x45) {
            index += 1
            if index < bytes.count, (bytes[index] == 0x2b || bytes[index] == 0x2d) { index += 1 }
            let exponentStart = index
            while index < bytes.count, (0x30...0x39).contains(bytes[index]) { index += 1 }
            guard index > exponentStart else { return nil }
        }

        guard index > start, isDelimiter(at: index) else { return nil }
        return JSONNumber(lexeme: String(decoding: bytes[start..<index], as: UTF8.self))
    }

    private func isDelimiter(at position: Int) -> Bool {
        guard position < bytes.count else { return true }
        return [0x20, 0x09, 0x0a, 0x0d, 0x2c, 0x5d, 0x7d].contains(bytes[position])
    }

    private mutating func consume(_ value: StaticString) -> Bool {
        let expected = Array(String(describing: value).utf8)
        guard index + expected.count <= bytes.count,
              Array(bytes[index..<(index + expected.count)]) == expected
        else { return false }
        index += expected.count
        return true
    }

    private mutating func consumeByte(_ byte: UInt8) -> Bool {
        guard index < bytes.count, bytes[index] == byte else { return false }
        index += 1
        return true
    }

    private mutating func skipWhitespace() {
        while index < bytes.count, [0x20, 0x09, 0x0a, 0x0d].contains(bytes[index]) { index += 1 }
    }
}
