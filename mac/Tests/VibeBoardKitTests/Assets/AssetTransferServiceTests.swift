import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Asset transfer service outcomes")
struct AssetTransferServiceTests {
    @Test func beginRejectionFailsWithoutWaitingForTimeout() async throws {
        let fixture = try assetFixture()
        let session = TransferSessionDouble(context: fixture.context)
        await session.setOutcome(.rejected(transferID: 7, code: "no_space", nextOffset: nil, message: nil), for: 7)
        let service = AssetTransferService(session: session, timeout: .seconds(1))
        await #expect(throws: AssetServiceError.deviceRejected("no_space")) {
            try await service.upload(fixture.asset, transferID: 7)
        }
    }

    @Test func badOffsetPreservesTypedDurableEvidence() async throws {
        let fixture = try assetFixture()
        let session = TransferSessionDouble(context: fixture.context)
        await session.setOutcome(.rejected(transferID: 8, code: "bad_offset", nextOffset: 4, message: nil), for: 8)
        let service = AssetTransferService(session: session, timeout: .seconds(1))
        await #expect(throws: AssetServiceError.deviceRejected("bad_offset(next_offset=4)")) {
            try await service.upload(fixture.asset, transferID: 8)
        }
    }

    @Test func backendDetailIsSurfacedAndCancelWaitsForDeviceAbort() async throws {
        let fixture = try assetFixture()
        let session = TransferSessionDouble(context: fixture.context)
        await session.setOutcome(
            .rejected(
                transferID: 9,
                code: "write_failed",
                nextOffset: nil,
                message: "phase=end;esp_err=0xffffffff"
            ),
            for: 9
        )
        let service = AssetTransferService(session: session, timeout: .seconds(1))
        await #expect(
            throws: AssetServiceError.deviceRejected(
                "write_failed(phase=end;esp_err=0xffffffff)"
            )
        ) {
            try await service.upload(fixture.asset, transferID: 9)
        }
        try await service.cancel(transferID: 9)
        #expect(await session.currentAssetTransferOutcome(transferID: 9) == nil)
    }

    private func assetFixture() throws -> (asset: PreparedAsset, context: ReplacementSessionContext) {
        let limits = VKA1Limits(maxFrames: 1, minFrameDurationMS: 20, maxFrameDurationMS: 5_000, maxContainerBytes: 524_288, maxDecodedBytes: 131_072)
        let data = try VKA1Codec.encode(kind: .image, width: 1, height: 1, frames: [.init(pixels: [0], durationMS: 0)], limits: limits)
        let asset = try PreparedAsset(data: data, limits: limits)
        let json = #"{"display":{"format":"rgb565","height":142,"width":428},"event":"vk_capabilities","features":{"assets":{"available":true,"chunk_bytes":4084,"decoder_scratch_bytes":4096,"encodings":["raw","row_rle"],"free_bytes":1048576,"management":true,"max_active_decoded_bytes":131072,"max_asset_bytes":524288,"max_assets":64,"max_frame_ms":5000,"max_frames":32,"min_frame_ms":20,"reserve_bytes":65536,"revision":0,"storage_state":"ready","upload_max_bytes":524288,"version":1}},"protocol":1}"#
        return (asset, .init(epochGeneration: 1, snapshotGeneration: 1, snapshot: try .decode(Data(json.utf8))))
    }
}

private actor TransferSessionDouble: AssetTransferSession {
    private let context: ReplacementSessionContext
    private var outcomes: [UInt32: AssetTransferOutcome] = [:]

    init(context: ReplacementSessionContext) { self.context = context }
    func setOutcome(_ outcome: AssetTransferOutcome, for id: UInt32) { outcomes[id] = outcome }
    func sendAssetCommand(_ command: AssetCommand, timeout: Duration) async throws {
        if case .abort(let transferID) = command {
            outcomes[transferID] = .aborted(transferID: transferID)
        }
    }
    func sendAssetChunk(_ payload: Data, using authorization: ActiveAssetTransfer, timeout: Duration) async throws {}
    func currentActiveAssetTransfer() -> ActiveAssetTransfer? { nil }
    func currentAssetTransferOutcome(transferID: UInt32) -> AssetTransferOutcome? { outcomes[transferID] }
    func takeAssetTransferOutcome(transferID: UInt32) -> AssetTransferOutcome? { outcomes.removeValue(forKey: transferID) }
    func currentReplacementContext() -> ReplacementSessionContext? { context }
}
