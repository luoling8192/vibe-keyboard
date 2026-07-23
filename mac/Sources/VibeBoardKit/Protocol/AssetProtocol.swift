import Foundation

public enum AssetKind: String, CaseIterable, Equatable, Sendable {
    case image
    case animation
    case glyphBitmap = "glyph_bitmap"
}

public enum AssetCommand: Equatable, Sendable {
    case storageFormat
    case begin(transferID: UInt32, sha256: String, totalBytes: UInt32, kind: AssetKind)
    case query(transferID: UInt32)
    case end(transferID: UInt32, sha256: String, totalBytes: UInt32, kind: AssetKind)
    case abort(transferID: UInt32)
    case list(snapshotID: UInt32, cursor: UInt32, limit: UInt8)
    case delete(sha256: String, expectedRevision: UInt32)
}

public enum ScreenCommand: Equatable, Sendable {
    case query
    case commit(ScreenCommit)
}

public enum ReplacementCommandEncoder {
    public static func encode(_ command: AssetCommand) throws -> Data {
        let body: String
        switch command {
        case .storageFormat:
            body = #"{"confirmation":"verified_erased_spiffs","event":"vk_storage_format"}"#
        case .begin(let id, let hash, let bytes, let kind):
            try validateTransfer(id); try validateSHA(hash); try validatePositive(bytes, "total_bytes")
            body = "{\"event\":\"vk_asset_begin\",\"kind\":\"\(kind.rawValue)\",\"sha256\":\"\(hash)\",\"total_bytes\":\(bytes),\"transfer_id\":\(id)}"
        case .query(let id):
            try validateTransfer(id)
            body = "{\"event\":\"vk_asset_query\",\"transfer_id\":\(id)}"
        case .end(let id, let hash, let bytes, let kind):
            try validateTransfer(id); try validateSHA(hash); try validatePositive(bytes, "total_bytes")
            body = "{\"event\":\"vk_asset_end\",\"kind\":\"\(kind.rawValue)\",\"sha256\":\"\(hash)\",\"total_bytes\":\(bytes),\"transfer_id\":\(id)}"
        case .abort(let id):
            try validateTransfer(id)
            body = "{\"event\":\"vk_asset_abort\",\"transfer_id\":\(id)}"
        case .list(let snapshot, let cursor, let limit):
            guard (1...64).contains(limit), (snapshot != 0 || cursor == 0) else { throw ReplacementProtocolError.invalidValue(field: "asset_list") }
            body = "{\"cursor\":\(cursor),\"event\":\"vk_asset_list\",\"limit\":\(limit),\"snapshot_id\":\(snapshot)}"
        case .delete(let hash, let revision):
            try validateSHA(hash)
            body = "{\"event\":\"vk_asset_delete\",\"expected_revision\":\(revision),\"sha256\":\"\(hash)\"}"
        }
        return try FrameEncoder.encodeOrdinary(type: .state, body: Data(body.utf8))
    }

    public static func encode(_ command: ScreenCommand) throws -> Data {
        switch command {
        case .query:
            return try FrameEncoder.encodeOrdinary(type: .state, body: Data(#"{"event":"vk_screen_query"}"#.utf8))
        case .commit(let commit):
            return try FrameEncoder.encodeOrdinary(type: .state, body: commit.canonicalBody())
        }
    }

    private static func validateTransfer(_ id: UInt32) throws { guard id != 0 else { throw ReplacementProtocolError.invalidValue(field: "transfer_id") } }
    private static func validateSHA(_ hash: String) throws { guard BoundedJSON.validSHA(hash) else { throw ReplacementProtocolError.invalidValue(field: "sha256") } }
    private static func validatePositive(_ value: UInt32, _ field: String) throws { guard value != 0 else { throw ReplacementProtocolError.invalidValue(field: field) } }
}

public enum AssetChunkEncoder {
    public static let frameType: UInt8 = 0x40
    public static let maximumPayloadLength = 4084

    public static func encode(transferID: UInt32, nextOffset: UInt32, payload: Data) throws -> Data {
        guard transferID != 0 else { throw ReplacementProtocolError.invalidValue(field: "transfer_id") }
        guard (1...maximumPayloadLength).contains(payload.count) else { throw ReplacementProtocolError.invalidValue(field: "payload") }
        let bodyLength = UInt16(8 + payload.count)
        var frame = Data([FrameStreamParser.protocolVersion, frameType, UInt8(truncatingIfNeeded: bodyLength), UInt8(truncatingIfNeeded: bodyLength >> 8)])
        appendLE(transferID, to: &frame)
        appendLE(nextOffset, to: &frame)
        frame.append(payload)
        return frame
    }

    private static func appendLE(_ value: UInt32, to data: inout Data) {
        data.append(UInt8(truncatingIfNeeded: value))
        data.append(UInt8(truncatingIfNeeded: value >> 8))
        data.append(UInt8(truncatingIfNeeded: value >> 16))
        data.append(UInt8(truncatingIfNeeded: value >> 24))
    }
}

public struct AssetListEntry: Equatable, Sendable {
    public let sha256: String
    public let totalBytes: UInt32
    public let kind: AssetKind
    public let referenced: Bool
}

public enum ReplacementAssetEvent: Equatable, Sendable {
    case storageFormatted(revision: UInt32)
    case ready(transferID: UInt32, sha256: String, totalBytes: UInt32, kind: AssetKind, nextOffset: UInt32, chunkBytes: UInt16)
    case progress(transferID: UInt32, nextOffset: UInt32)
    case stored(transferID: UInt32, sha256: String, totalBytes: UInt32, kind: AssetKind)
    case aborted(transferID: UInt32)
    case page(snapshotID: UInt32, cursor: UInt32, entries: [AssetListEntry], nextCursor: UInt32?, revision: UInt32)
    case deleted(sha256: String, revision: UInt32)
}

public enum ReplacementScreenEvent: Equatable, Sendable {
    case state(configured: Bool, mode: ScreenMode?, revision: UInt32, assetsManifestSHA256: String?, screenManifestSHA256: String?)
    case committed(assetsManifestSHA256: String, previousRevision: UInt32, revision: UInt32, screenManifestSHA256: String)
}

public struct ReplacementErrorEvent: Equatable, Sendable {
    public let operation: String
    public let code: String
    public let transferID: UInt32?
    public let nextOffset: UInt32?
    public let sha256: String?
    public let message: String?
}

public enum ReplacementProtocolEvent: Equatable, Sendable {
    case asset(ReplacementAssetEvent)
    case screen(ReplacementScreenEvent)
    case widget(WidgetProtocolEvent)
    case led(LEDProtocolEvent)
    case error(ReplacementErrorEvent)
}

public enum ReplacementEventDecoder {
    public static func decode(_ data: Data) throws -> ReplacementProtocolEvent {
        let object = try BoundedJSON.object(data)
        guard let event = BoundedJSON.string(object["event"]) else { throw ReplacementProtocolError.invalidValue(field: "event") }
        switch event {
        case "vk_storage_formatted":
            try BoundedJSON.exactKeys(object, ["event", "revision"], event)
            return .asset(.storageFormatted(revision: try requiredUInt32(object, "revision")))
        case "vk_asset_ready":
            try BoundedJSON.exactKeys(object, ["chunk_bytes", "event", "kind", "next_offset", "sha256", "total_bytes", "transfer_id"], event)
            return .asset(.ready(transferID: try requiredNonzeroUInt32(object, "transfer_id"), sha256: try requiredSHA(object, "sha256"), totalBytes: try requiredNonzeroUInt32(object, "total_bytes"), kind: try requiredKind(object), nextOffset: try requiredUInt32(object, "next_offset"), chunkBytes: try requiredRange(object, "chunk_bytes", 1...4084)))
        case "vk_asset_progress":
            try BoundedJSON.exactKeys(object, ["event", "next_offset", "transfer_id"], event)
            return .asset(.progress(transferID: try requiredNonzeroUInt32(object, "transfer_id"), nextOffset: try requiredUInt32(object, "next_offset")))
        case "vk_asset_stored":
            try BoundedJSON.exactKeys(object, ["event", "kind", "sha256", "total_bytes", "transfer_id"], event)
            return .asset(.stored(transferID: try requiredNonzeroUInt32(object, "transfer_id"), sha256: try requiredSHA(object, "sha256"), totalBytes: try requiredNonzeroUInt32(object, "total_bytes"), kind: try requiredKind(object)))
        case "vk_asset_aborted":
            try BoundedJSON.exactKeys(object, ["event", "transfer_id"], event)
            return .asset(.aborted(transferID: try requiredNonzeroUInt32(object, "transfer_id")))
        case "vk_asset_deleted":
            try BoundedJSON.exactKeys(object, ["event", "revision", "sha256"], event)
            return .asset(.deleted(sha256: try requiredSHA(object, "sha256"), revision: try requiredUInt32(object, "revision")))
        case "vk_asset_page": return .asset(try decodePage(object))
        case "vk_screen_state": return .screen(try decodeScreenState(object))
        case "vk_screen_committed":
            try BoundedJSON.exactKeys(object, ["assets_manifest_sha256", "event", "previous_revision", "revision", "screen_manifest_sha256"], event)
            return .screen(.committed(assetsManifestSHA256: try requiredSHA(object, "assets_manifest_sha256"), previousRevision: try requiredUInt32(object, "previous_revision"), revision: try requiredNonzeroUInt32(object, "revision"), screenManifestSHA256: try requiredSHA(object, "screen_manifest_sha256")))
        case "vk_error": return .error(try decodeError(object))
        default: throw ReplacementProtocolError.invalidValue(field: "event")
        }
    }

    private static func decodePage(_ object: [String: Any]) throws -> ReplacementAssetEvent {
        try BoundedJSON.exactKeys(object, ["cursor", "entries", "event", "next_cursor", "revision", "snapshot_id"], "vk_asset_page")
        guard let values = BoundedJSON.array(object["entries"]) else { throw ReplacementProtocolError.invalidValue(field: "entries") }
        let entries = try values.map { value -> AssetListEntry in
            guard let entry = BoundedJSON.dictionary(value) else { throw ReplacementProtocolError.invalidValue(field: "entry") }
            try BoundedJSON.exactKeys(entry, ["kind", "referenced", "sha256", "total_bytes"], "entry")
            guard let referenced = BoundedJSON.bool(entry["referenced"]) else { throw ReplacementProtocolError.invalidValue(field: "referenced") }
            return AssetListEntry(sha256: try requiredSHA(entry, "sha256"), totalBytes: try requiredNonzeroUInt32(entry, "total_bytes"), kind: try requiredKind(entry), referenced: referenced)
        }
        let next: UInt32?
        if object["next_cursor"] is NSNull { next = nil } else { next = try requiredNonzeroUInt32(object, "next_cursor") }
        return .page(snapshotID: try requiredNonzeroUInt32(object, "snapshot_id"), cursor: try requiredUInt32(object, "cursor"), entries: entries, nextCursor: next, revision: try requiredUInt32(object, "revision"))
    }

    private static func decodeScreenState(_ object: [String: Any]) throws -> ReplacementScreenEvent {
        try BoundedJSON.exactKeys(object, ["assets_manifest_sha256", "configured", "configured_mode", "event", "revision", "screen_manifest_sha256"], "vk_screen_state")
        guard let configured = BoundedJSON.bool(object["configured"]) else { throw ReplacementProtocolError.invalidValue(field: "configured") }
        if !configured {
            guard object["configured_mode"] is NSNull, object["assets_manifest_sha256"] is NSNull, object["screen_manifest_sha256"] is NSNull, try requiredUInt32(object, "revision") == 0 else { throw ReplacementProtocolError.invalidValue(field: "screen_state") }
            return .state(configured: false, mode: nil, revision: 0, assetsManifestSHA256: nil, screenManifestSHA256: nil)
        }
        guard let rawMode = BoundedJSON.string(object["configured_mode"]), let mode = ScreenMode(rawValue: rawMode) else { throw ReplacementProtocolError.invalidValue(field: "configured_mode") }
        return .state(configured: true, mode: mode, revision: try requiredNonzeroUInt32(object, "revision"), assetsManifestSHA256: try requiredSHA(object, "assets_manifest_sha256"), screenManifestSHA256: try requiredSHA(object, "screen_manifest_sha256"))
    }

    private static func decodeError(_ object: [String: Any]) throws -> ReplacementErrorEvent {
        guard let operation = BoundedJSON.string(object["operation"]),
              let code = BoundedJSON.string(object["code"])
        else { throw ReplacementProtocolError.invalidKeys(context: "vk_error") }
        let baseKeys: Set<String> = ["event", "operation", "code"]
        let assetOptionalKeys: Set<String> = ["transfer_id", "next_offset", "sha256", "message"]
        let allowed: Set<String>
        switch operation {
        case "asset", "storage": allowed = baseKeys.union(assetOptionalKeys)
        case "screen": allowed = baseKeys.union(["message"])
        default: throw ReplacementProtocolError.invalidValue(field: "operation")
        }
        guard baseKeys.isSubset(of: Set(object.keys)), Set(object.keys).isSubset(of: allowed) else {
            throw ReplacementProtocolError.invalidKeys(context: "vk_error.\(operation)")
        }
        let assetCodes: Set<String> = ["invalid_request", "unavailable", "wrong_epoch", "busy", "conflict", "not_found", "bad_offset", "bad_size", "bad_hash", "kind_mismatch", "write_failed", "incomplete", "invalid_asset", "timeout", "no_space", "referenced", "revision_conflict", "snapshot_expired", "partition_mismatch", "not_erased", "format_failed", "internal"]
        let screenCodes: Set<String> = ["invalid_request", "unavailable", "wrong_epoch", "revision_conflict", "conflict", "invalid_manifest", "missing_asset", "font_mismatch", "limit_exceeded", "allocation_failed", "render_failed", "internal"]
        guard ((operation == "asset" || operation == "storage") && assetCodes.contains(code)) || (operation == "screen" && screenCodes.contains(code)) else { throw ReplacementProtocolError.invalidValue(field: "error") }
        let transfer = try object["transfer_id"].map { _ in try requiredNonzeroUInt32(object, "transfer_id") }
        let next = try object["next_offset"].map { _ in try requiredUInt32(object, "next_offset") }
        let hash = try object["sha256"].map { _ in try requiredSHA(object, "sha256") }
        let message = BoundedJSON.string(object["message"])
        guard object["message"] == nil || (message != nil && message!.utf8.count <= 96) else { throw ReplacementProtocolError.invalidValue(field: "message") }
        return ReplacementErrorEvent(operation: operation, code: code, transferID: transfer, nextOffset: next, sha256: hash, message: message)
    }

    private static func requiredUInt32(_ object: [String: Any], _ key: String) throws -> UInt32 { guard let v = BoundedJSON.uint(object[key], as: UInt32.self) else { throw ReplacementProtocolError.invalidValue(field: key) }; return v }
    private static func requiredNonzeroUInt32(_ object: [String: Any], _ key: String) throws -> UInt32 { let v = try requiredUInt32(object, key); guard v != 0 else { throw ReplacementProtocolError.invalidValue(field: key) }; return v }
    private static func requiredRange(_ object: [String: Any], _ key: String, _ range: ClosedRange<UInt16>) throws -> UInt16 { guard let v = BoundedJSON.range(object[key], as: UInt16.self, range) else { throw ReplacementProtocolError.invalidValue(field: key) }; return v }
    private static func requiredSHA(_ object: [String: Any], _ key: String) throws -> String { guard let v = BoundedJSON.string(object[key]), BoundedJSON.validSHA(v) else { throw ReplacementProtocolError.invalidValue(field: key) }; return v }
    private static func requiredKind(_ object: [String: Any]) throws -> AssetKind { guard let raw = BoundedJSON.string(object["kind"]), let kind = AssetKind(rawValue: raw) else { throw ReplacementProtocolError.invalidValue(field: "kind") }; return kind }
}
