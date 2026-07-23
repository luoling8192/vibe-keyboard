import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Replacement asset protocol")
struct AssetProtocolTests {
    private let hash = String(repeating: "a", count: 64)

    @Test func assetChunkUsesDirectionSpecificExactLayout() throws {
        let frame = try AssetChunkEncoder.encode(transferID: 0x01020304, nextOffset: 0x05060708, payload: Data([0xaa]))
        #expect(frame == Data([0x01, 0x40, 0x09, 0x00, 0x04, 0x03, 0x02, 0x01, 0x08, 0x07, 0x06, 0x05, 0xaa]))
        #expect(!FrameType.allCases.map(\.rawValue).contains(0x40))
    }

    @Test func assetChunkBoundaries() throws {
        #expect(throws: ReplacementProtocolError.invalidValue(field: "payload")) {
            try AssetChunkEncoder.encode(transferID: 1, nextOffset: 0, payload: Data())
        }
        #expect(try AssetChunkEncoder.encode(transferID: 1, nextOffset: 4084, payload: Data(repeating: 1, count: 4084)).count == 4096)
        #expect(throws: ReplacementProtocolError.invalidValue(field: "payload")) {
            try AssetChunkEncoder.encode(transferID: 1, nextOffset: 0, payload: Data(repeating: 1, count: 4085))
        }
        #expect(throws: ReplacementProtocolError.invalidValue(field: "transfer_id")) {
            try AssetChunkEncoder.encode(transferID: 0, nextOffset: 0, payload: Data([1]))
        }
    }

    @Test func decodesAtomicCapabilitySnapshot() throws {
        let data = Data(capabilityJSON.utf8)
        let snapshot = try ReplacementCapabilitySnapshot.decode(data)
        guard case .available(let assets)? = snapshot.assets, case .available(let screen)? = snapshot.screen else {
            Issue.record("Expected available assets and screen")
            return
        }
        #expect(assets.chunkBytes == 4084)
        #expect(assets.decoderScratchBytes == 4096)
        #expect(screen.maxLayoutBytes == 3072)
    }

    @Test func updateCapabilityIsKnownAndStrictlyValidated() throws {
        let unavailable = capabilityJSON.replacingOccurrences(
            of: #""screen":\#(screenJSON)"#,
            with: #""screen":\#(screenJSON),"update":{"available":false,"reason":"bootloader_migration_required","version":1}"#
        )
        let unavailableSnapshot = try ReplacementCapabilitySnapshot.decode(Data(unavailable.utf8))
        guard case .unavailable(let feature)? = unavailableSnapshot.update else {
            Issue.record("Expected unavailable update")
            return
        }
        #expect(feature.reason == "bootloader_migration_required")

        let available = capabilityJSON.replacingOccurrences(
            of: #""screen":\#(screenJSON)"#,
            with: #""screen":\#(screenJSON),"update":{"available":true,"chunk_bytes":512,"max_image_bytes":5242880,"rollback":"bootloader_pending_verify","staged_metadata":"ram_epoch","target":"ota_0","version":1}"#
        )
        guard case .available(let feature)? = try ReplacementCapabilitySnapshot.decode(Data(available.utf8)).update else {
            Issue.record("Expected typed available update")
            return
        }
        #expect(feature.chunkBytes == 512)
        #expect(feature.target == "ota_0")

        let extra = unavailable.replacingOccurrences(of: #""reason":"bootloader_migration_required""#, with: #""reason":"bootloader_migration_required","rogue":1"#)
        #expect(throws: ReplacementProtocolError.invalidKeys(context: "update unavailable")) {
            try ReplacementCapabilitySnapshot.decode(Data(extra.utf8))
        }
    }

    @Test func screenCannotBorrowMissingAssetsProfile() {
        let data = Data(capabilityJSON.replacingOccurrences(of: #""assets":\#(assetsJSON),"#, with: "").utf8)
        #expect(throws: ReplacementProtocolError.crossFeatureInvariant) {
            try ReplacementCapabilitySnapshot.decode(data)
        }
    }

    @Test func rejectsFloatingIntegerLexemesAndCrossFeatureLimitMismatch() {
        for lexeme in ["64.0", "64e0"] {
            let altered = capabilityJSON.replacingOccurrences(of: #""max_assets":64,"max_frame_ms"#, with: #""max_assets":\#(lexeme),"max_frame_ms"#)
            #expect(throws: (any Error).self) {
                try ReplacementCapabilitySnapshot.decode(Data(altered.utf8))
            }
        }
        let mismatch = capabilityJSON.replacingOccurrences(of: #""max_assets":64,"max_commit_bytes"#, with: #""max_assets":65,"max_commit_bytes"#)
        #expect(throws: ReplacementProtocolError.crossFeatureInvariant) {
            try ReplacementCapabilitySnapshot.decode(Data(mismatch.utf8))
        }
    }

    @Test func rejectsNonCanonicalUnsignedIntegerLexemes() {
        for lexeme in ["-0", "00", "01", "+0", "0.0", "0e0"] {
            let capability = capabilityJSON.replacingOccurrences(
                of: #""revision":0,"storage_state"#,
                with: #""revision":\#(lexeme),"storage_state"#
            )
            #expect(throws: (any Error).self) {
                try ReplacementCapabilitySnapshot.decode(Data(capability.utf8))
            }
        }

        for lexeme in ["-0", "00", "01", "+0", "12.0", "12e0"] {
            let event = #"{"event":"vk_asset_progress","next_offset":\#(lexeme),"transfer_id":7}"#
            #expect(throws: (any Error).self) {
                try ReplacementEventDecoder.decode(Data(event.utf8))
            }
        }

        let nestedInteger = capabilityJSON.replacingOccurrences(
            of: #""metrics_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","version":1"#,
            with: #""metrics_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","version":-0"#
        )
        #expect(throws: (any Error).self) {
            try ReplacementCapabilitySnapshot.decode(Data(nestedInteger.utf8))
        }

        let numericText = capabilityJSON.replacingOccurrences(
            of: #""id":"vk-sans""#,
            with: #""id":"vk-sans--0-01-640""#
        )
        #expect((try? ReplacementCapabilitySnapshot.decode(Data(numericText.utf8))) != nil)
    }

    @Test func distinguishesAbsentUnavailableAndInvalidScreen() throws {
        let absent = capabilityJSON.replacingOccurrences(of: #", "screen":\#(screenJSON)"#.replacingOccurrences(of: " ", with: ""), with: "")
        let absentSnapshot = try ReplacementCapabilitySnapshot.decode(Data(absent.utf8))
        #expect(absentSnapshot.screen == nil)

        let unavailable = #"{"available":false,"reason":"policy_blocked","version":1}"#
        let unavailableJSON = capabilityJSON.replacingOccurrences(of: screenJSON, with: unavailable)
        let unavailableSnapshot = try ReplacementCapabilitySnapshot.decode(Data(unavailableJSON.utf8))
        guard case .unavailable(let feature)? = unavailableSnapshot.screen else {
            Issue.record("Expected explicit unavailable screen")
            return
        }
        #expect(feature.reason == "policy_blocked")

        let invalid = capabilityJSON.replacingOccurrences(of: #""max_assets":64,"max_commit_bytes"#, with: #""max_assets":65,"max_commit_bytes"#)
        #expect(throws: ReplacementProtocolError.crossFeatureInvariant) {
            try ReplacementCapabilitySnapshot.decode(Data(invalid.utf8))
        }
    }

    @Test func rejectsKnownCapabilityExtraAndDuplicateKeys() {
        let altered = capabilityJSON.replacingOccurrences(of: #""revision":0,"storage_state"#, with: #""revision":0,"rogue":1,"storage_state"#)
        #expect(throws: ReplacementProtocolError.invalidKeys(context: "assets")) {
            try ReplacementCapabilitySnapshot.decode(Data(altered.utf8))
        }
        let duplicate = capabilityJSON.replacingOccurrences(of: #""chunk_bytes":4084"#, with: #""chunk_bytes":1,"chunk_bytes":4084"#)
        #expect(throws: ReplacementProtocolError.invalidJSON) {
            try ReplacementCapabilitySnapshot.decode(Data(duplicate.utf8))
        }
    }

    @Test func boundedParserRejectsDepthBeforeRecursiveDescent() throws {
        func nestedArray(_ depth: Int) -> Data {
            Data((String(repeating: "[", count: depth) + "0" + String(repeating: "]", count: depth)).utf8)
        }

        try BoundedJSON.validateNegotiated(
            nestedArray(BoundedJSON.maximumDepth - 1),
            maximumDepth: UInt8(BoundedJSON.maximumDepth),
            maximumTokens: UInt16(BoundedJSON.maximumTokens),
            maximumStringBytes: UInt16(BoundedJSON.maximumStringBytes)
        )
        try BoundedJSON.validateNegotiated(
            nestedArray(BoundedJSON.maximumDepth),
            maximumDepth: UInt8(BoundedJSON.maximumDepth),
            maximumTokens: UInt16(BoundedJSON.maximumTokens),
            maximumStringBytes: UInt16(BoundedJSON.maximumStringBytes)
        )
        #expect(throws: ReplacementProtocolError.limitExceeded("depth")) {
            try BoundedJSON.validateNegotiated(
                nestedArray(BoundedJSON.maximumDepth + 1),
                maximumDepth: UInt8(BoundedJSON.maximumDepth),
                maximumTokens: UInt16(BoundedJSON.maximumTokens),
                maximumStringBytes: UInt16(BoundedJSON.maximumStringBytes)
            )
        }

        let deepValue = String(repeating: "[", count: 1_500) + "0" + String(repeating: "]", count: 1_500)
        let deeplyNested = Data(deepValue.utf8)
        #expect(deeplyNested.count < BoundedJSON.maximumBytes)
        #expect(throws: ReplacementProtocolError.limitExceeded("depth")) {
            try BoundedJSON.validateNegotiated(
                deeplyNested,
                maximumDepth: UInt8(BoundedJSON.maximumDepth),
                maximumTokens: UInt16(BoundedJSON.maximumTokens),
                maximumStringBytes: UInt16(BoundedJSON.maximumStringBytes)
            )
        }

        let unknownFeature = Data(#"{"display":{"format":"rgb565","height":142,"width":428},"event":"vk_capabilities","features":{"future":\#(deepValue)},"protocol":1}"#.utf8)
        #expect(unknownFeature.count < BoundedJSON.maximumBytes)
        #expect(throws: ReplacementProtocolError.limitExceeded("depth")) {
            try ReplacementCapabilitySnapshot.decode(unknownFeature)
        }
    }

    @Test func boundedParserAppliesDepthToObjectsAndMixedContainers() throws {
        func nestedObject(_ depth: Int) -> Data {
            Data((String(repeating: "{\"a\":", count: depth) + "0" + String(repeating: "}", count: depth)).utf8)
        }

        try BoundedJSON.validateNegotiated(
            nestedObject(BoundedJSON.maximumDepth),
            maximumDepth: UInt8(BoundedJSON.maximumDepth),
            maximumTokens: UInt16(BoundedJSON.maximumTokens),
            maximumStringBytes: UInt16(BoundedJSON.maximumStringBytes)
        )
        #expect(throws: ReplacementProtocolError.limitExceeded("depth")) {
            try BoundedJSON.validateNegotiated(
                nestedObject(BoundedJSON.maximumDepth + 1),
                maximumDepth: UInt8(BoundedJSON.maximumDepth),
                maximumTokens: UInt16(BoundedJSON.maximumTokens),
                maximumStringBytes: UInt16(BoundedJSON.maximumStringBytes)
            )
        }

        let mixedAtLimit = Data((String(repeating: "[{\"a\":", count: 6) + "0" + String(repeating: "}]", count: 6)).utf8)
        try BoundedJSON.validateNegotiated(
            mixedAtLimit,
            maximumDepth: UInt8(BoundedJSON.maximumDepth),
            maximumTokens: UInt16(BoundedJSON.maximumTokens),
            maximumStringBytes: UInt16(BoundedJSON.maximumStringBytes)
        )
        let mixedOverLimit = Data(("[" + String(decoding: mixedAtLimit, as: UTF8.self) + "]").utf8)
        #expect(throws: ReplacementProtocolError.limitExceeded("depth")) {
            try BoundedJSON.validateNegotiated(
                mixedOverLimit,
                maximumDepth: UInt8(BoundedJSON.maximumDepth),
                maximumTokens: UInt16(BoundedJSON.maximumTokens),
                maximumStringBytes: UInt16(BoundedJSON.maximumStringBytes)
            )
        }
    }

    @Test func boundedParserRejectsTokensBeforeBuildingWideValues() throws {
        let atLimit = Data(("[" + Array(repeating: "0", count: BoundedJSON.maximumTokens - 1).joined(separator: ",") + "]").utf8)
        try BoundedJSON.validateNegotiated(
            atLimit,
            maximumDepth: UInt8(BoundedJSON.maximumDepth),
            maximumTokens: UInt16(BoundedJSON.maximumTokens),
            maximumStringBytes: UInt16(BoundedJSON.maximumStringBytes)
        )

        let overLimit = Data(("[" + Array(repeating: "0", count: BoundedJSON.maximumTokens).joined(separator: ",") + "]").utf8)
        #expect(overLimit.count < BoundedJSON.maximumBytes)
        #expect(throws: ReplacementProtocolError.limitExceeded("tokens")) {
            try BoundedJSON.validateNegotiated(
                overLimit,
                maximumDepth: UInt8(BoundedJSON.maximumDepth),
                maximumTokens: UInt16(BoundedJSON.maximumTokens),
                maximumStringBytes: UInt16(BoundedJSON.maximumStringBytes)
            )
        }
    }

    @Test func boundedParserRejectsMalformedAndTruncatedInputsWithoutTrapping() {
        for text in ["[", "{\"a\":", "[0,]", "{\"a\":0,}", "[[[[[[[[[[[[["] {
            #expect(throws: (any Error).self) {
                try BoundedJSON.validateNegotiated(
                    Data(text.utf8),
                    maximumDepth: UInt8(BoundedJSON.maximumDepth),
                    maximumTokens: UInt16(BoundedJSON.maximumTokens),
                    maximumStringBytes: UInt16(BoundedJSON.maximumStringBytes)
                )
            }
        }
    }

    @Test func sharedFixtureIsLanguageNeutralAndDirectionSafe() throws {
        let url = try #require(Bundle.module.url(forResource: "asset-protocol-v1", withExtension: "json", subdirectory: "Fixtures"))
        let object = try #require(try JSONSerialization.jsonObject(with: Data(contentsOf: url)) as? [String: Any])
        let chunk = try #require(object["asset_chunk"] as? [String: Any])
        #expect(chunk["one_byte_hex"] as? String == "014009000403020108070605aa")
        let direction = try #require(object["direction"] as? [String: Any])
        #expect(direction["host_to_device_asset_chunk_type"] as? Int == 64)
        #expect(!FrameType.allCases.map(\.rawValue).contains(64))
    }

    @Test func encodesTypedAssetCommandWithoutRawSender() throws {
        let frame = try ReplacementCommandEncoder.encode(.begin(transferID: 7, sha256: hash, totalBytes: 12, kind: .image))
        #expect(frame[0] == 1)
        #expect(frame[1] == FrameType.state.rawValue)
        let body = String(decoding: frame.dropFirst(4), as: UTF8.self)
        #expect(body == "{\"event\":\"vk_asset_begin\",\"kind\":\"image\",\"sha256\":\"\(hash)\",\"total_bytes\":12,\"transfer_id\":7}")
    }

    @Test func decodesExactAssetAndScreenEvents() throws {
        let progress = try ReplacementEventDecoder.decode(Data(#"{"event":"vk_asset_progress","next_offset":12,"transfer_id":7}"#.utf8))
        #expect(progress == .asset(.progress(transferID: 7, nextOffset: 12)))
        let state = try ReplacementEventDecoder.decode(Data(#"{"assets_manifest_sha256":null,"configured":false,"configured_mode":null,"event":"vk_screen_state","revision":0,"screen_manifest_sha256":null}"#.utf8))
        #expect(state == .screen(.state(configured: false, mode: nil, revision: 0, assetsManifestSHA256: nil, screenManifestSHA256: nil)))
        let inputError = try ReplacementEventDecoder.decode(Data(#"{"code":"audio_start_failed","event":"vk_error","operation":"input"}"#.utf8))
        #expect(inputError == .error(.init(
            operation: "input",
            code: "audio_start_failed",
            transferID: nil,
            nextOffset: nil,
            sha256: nil,
            message: nil
        )))
    }

    @Test func rejectsOperationSpecificErrorFieldsAndFloatingEventIntegers() {
        #expect(throws: ReplacementProtocolError.invalidKeys(context: "vk_error.screen")) {
            try ReplacementEventDecoder.decode(Data(#"{"code":"internal","event":"vk_error","operation":"screen","transfer_id":1}"#.utf8))
        }
        #expect(throws: ReplacementProtocolError.invalidKeys(context: "vk_error.input")) {
            try ReplacementEventDecoder.decode(Data(#"{"code":"audio_start_failed","event":"vk_error","message":"extra","operation":"input"}"#.utf8))
        }
        #expect(throws: ReplacementProtocolError.invalidValue(field: "next_offset")) {
            try ReplacementEventDecoder.decode(Data(#"{"event":"vk_asset_progress","next_offset":12.0,"transfer_id":7}"#.utf8))
        }
    }

    @Test func rejectsEventExtraKeyAndUppercaseHash() {
        #expect(throws: ReplacementProtocolError.invalidKeys(context: "vk_asset_progress")) {
            try ReplacementEventDecoder.decode(Data(#"{"event":"vk_asset_progress","extra":0,"next_offset":12,"transfer_id":7}"#.utf8))
        }
        #expect(throws: ReplacementProtocolError.invalidValue(field: "sha256")) {
            try ReplacementEventDecoder.decode(Data("{\"event\":\"vk_asset_deleted\",\"revision\":0,\"sha256\":\"\(String(repeating: "A", count: 64))\"}".utf8))
        }
        #expect(throws: ReplacementProtocolError.invalidValue(field: "sha256")) {
            try ReplacementEventDecoder.decode(Data("{\"event\":\"vk_asset_deleted\",\"revision\":0,\"sha256\":\"\(String(repeating: "٠", count: 32))\"}".utf8))
        }
    }
}

private let assetsJSON = #"{"available":true,"chunk_bytes":4084,"decoder_scratch_bytes":4096,"encodings":["raw","row_rle"],"free_bytes":1048576,"management":true,"max_active_decoded_bytes":243104,"max_asset_bytes":1048576,"max_assets":64,"max_frame_ms":65535,"max_frames":4,"min_frame_ms":1,"reserve_bytes":1,"revision":0,"storage_state":"ready","upload_max_bytes":1048575,"version":1}"#
private let screenJSON = #"{"available":true,"configured":false,"fonts":[{"id":"vk-sans","metrics_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","version":1}],"max_assets":64,"max_commit_bytes":4092,"max_depth":4,"max_fonts":4,"max_json_tokens":512,"max_layout_bytes":3072,"max_objects":32,"max_pet_states":6,"max_string_bytes":256,"max_widget_value_bytes":256,"max_widgets":16,"modes":["image","pet","dashboard","custom"],"revision":0,"version":1}"#
private let capabilityJSON = #"{"display":{"format":"rgb565","height":142,"width":428},"event":"vk_capabilities","features":{"assets":\#(assetsJSON),"screen":\#(screenJSON)},"protocol":1}"#
