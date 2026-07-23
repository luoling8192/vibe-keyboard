import Darwin
import Foundation
import Testing
@testable import VibeBoardKit

@Suite("USB session")
struct USBSessionTests {
    @Test func safeHandshakeParsesBootLogAndUsesExactCommandOrder() async throws {
        let operations = FakeSerialOperations(reads: [.value(Data("boot log\n".utf8) + deviceInfoFrame()), .failure(EAGAIN)])
        let session = try USBSession(descriptor: descriptor, operations: operations)
        let info = try await session.connect(handshakeTimeout: .seconds(1))

        #expect(info.registryDeviceID == "AABBCCDDEEFF")
        #expect(info.firmwareDeviceID == "VS-AABBCCDDEEFF")
        #expect(info.hardware == "vibe_keyboard")
        #expect(operations.openedFlags == USBSession.openFlags)
        let json = decodedStateBodies(operations.writtenData)
        #expect(json.prefix(3) == [
            #"{"event":"transport","kind":"usb"}"#,
            #"{"event":"get_device_info"}"#,
            #"{"event":"ui_state","state":"ready","text":""}"#
        ])
        let diagnostics = await session.diagnostics()
        #expect(diagnostics.text.contains("boot log"))
        await session.disconnect()
        #expect(operations.closeCount == 1)
    }

    @Test func productionFirmwareGoldenDecodesThroughSession() async throws {
        let url = try #require(Bundle.module.url(
            forResource: "replacement-handshake",
            withExtension: "bin",
            subdirectory: "Fixtures"
        ))
        let bytes = try Data(contentsOf: url)
        let operations = FakeSerialOperations(reads: [.value(bytes)])
        let session = try USBSession(descriptor: descriptor, operations: operations)
        let info = try await session.connect(handshakeTimeout: .seconds(1))
        #expect(info.hardware == "vibe_keyboard")
        #expect(info.firmwareDeviceID == "VS-TEST")
        await session.disconnect()
    }

    @Test func replacementHandshakeRequiresConsecutiveValidCapabilities() async throws {
        let valid = FakeSerialOperations(reads: [.value(replacementDeviceInfoFrame() + capabilityFrame())])
        let validSession = try USBSession(descriptor: descriptor, operations: valid)
        #expect(try await validSession.connect(handshakeTimeout: .seconds(1)).hardware == "vibe_keyboard")
        await validSession.disconnect()

        let missing = FakeSerialOperations(reads: [.value(replacementDeviceInfoFrame())])
        let missingSession = try USBSession(descriptor: descriptor, operations: missing)
        await #expect(throws: USBSessionError.handshakeTimedOut) {
            try await missingSession.connect(handshakeTimeout: .milliseconds(150))
        }

        let reversed = FakeSerialOperations(reads: [.value(capabilityFrame() + replacementDeviceInfoFrame())])
        let reversedSession = try USBSession(descriptor: descriptor, operations: reversed)
        await #expect(throws: USBSessionError.protocolFailure("invalid replacement handshake order")) {
            try await reversedSession.connect(handshakeTimeout: .seconds(1))
        }

        let nonConsecutive = FakeSerialOperations(reads: [.value(replacementDeviceInfoFrame() + stateFrame(#"{"event":"ping"}"#) + capabilityFrame())])
        let nonConsecutiveSession = try USBSession(descriptor: descriptor, operations: nonConsecutive)
        await #expect(throws: USBSessionError.protocolFailure("replacement capabilities not consecutive")) {
            try await nonConsecutiveSession.connect(handshakeTimeout: .seconds(1))
        }

        let invalid = FakeSerialOperations(reads: [.value(replacementDeviceInfoFrame() + capabilityFrame(width: 1))])
        let invalidSession = try USBSession(descriptor: descriptor, operations: invalid)
        await #expect(throws: USBSessionError.protocolFailure("invalid replacement capabilities")) {
            try await invalidSession.connect(handshakeTimeout: .seconds(1))
        }
    }

    @Test func duplicateCapabilitySnapshotIsIdempotentAndConflictIsTerminal() async throws {
        let identicalOperations = FakeSerialOperations(reads: [.value(
            replacementDeviceInfoFrame() + capabilityFrame() + capabilityFrame()
        )])
        let identicalSession = try USBSession(descriptor: descriptor, operations: identicalOperations)
        #expect(try await identicalSession.connect(handshakeTimeout: .seconds(1)).hardware == "vibe_keyboard")
        await identicalSession.disconnect()

        let conflictingOperations = FakeSerialOperations(reads: [.value(
            replacementDeviceInfoFrame() + capabilityFrame() + capabilityFrame(width: 427)
        )])
        let conflictingSession = try USBSession(descriptor: descriptor, operations: conflictingOperations)
        await #expect(throws: USBSessionError.protocolFailure("invalid replacement capabilities")) {
            try await conflictingSession.connect(handshakeTimeout: .seconds(1))
        }
    }

    @Test func reconnectRejectsStaleCapabilitiesAndImmutableMismatchSnapshots() async throws {
        let staleOperations = FakeSerialOperations(reads: [.value(capabilityFrame() + replacementDeviceInfoFrame())])
        let staleSession = try USBSession(descriptor: descriptor, operations: staleOperations)
        await #expect(throws: USBSessionError.protocolFailure("invalid replacement handshake order")) {
            try await staleSession.connect(handshakeTimeout: .seconds(1))
        }

        let operations = FakeSerialOperations(reads: [.value(replacementDeviceInfoFrame() + capabilityFrame())])
        let session = try USBSession(descriptor: descriptor, operations: operations)
        #expect(try await session.connect(handshakeTimeout: .seconds(1)).hardware == "vibe_keyboard")
        await session.disconnect()
        operations.enqueueRead(.value(
            replacementDeviceInfoFrame() + capabilityFrame() + capabilityFrame()
        ))
        #expect(try await session.connect(handshakeTimeout: .seconds(1)).hardware == "vibe_keyboard")
        await session.disconnect()

        operations.enqueueRead(.value(
            replacementDeviceInfoFrame() + capabilityFrame() + capabilityFrame(width: 427)
        ))
        await #expect(throws: USBSessionError.protocolFailure("invalid replacement capabilities")) {
            try await session.connect(handshakeTimeout: .seconds(1))
        }
    }

    @Test func strictCapabilitySnapshotsReplaceAtomicallyAndClearOnDisconnect() async throws {
        let first = capabilityFrame(features: #""assets":{"available":false,"reason":"policy_blocked","version":1}"#)
        let second = capabilityFrame(features: #""screen":{"available":false,"reason":"policy_blocked","version":1}"#)
        let operations = FakeSerialOperations(reads: [.value(replacementDeviceInfoFrame() + first + second)])
        let session = try USBSession(descriptor: descriptor, operations: operations)
        _ = try await session.connect(handshakeTimeout: .seconds(1))
        let context = try #require(await session.currentReplacementContext())
        #expect(context.epochGeneration != 0)
        #expect(context.snapshotGeneration == 2)
        #expect(context.snapshot.assets == nil)
        guard case .unavailable(let screen)? = context.snapshot.screen else {
            Issue.record("Expected whole-snapshot screen replacement")
            return
        }
        #expect(screen.reason == "policy_blocked")
        await session.disconnect()
        #expect(await session.currentReplacementContext() == nil)
    }

    @Test func malformedKnownCapabilityCannotCompleteHandshake() async throws {
        let extra = capabilityFrame(features: #""assets":{"available":false,"reason":"policy_blocked","rogue":1,"version":1}"#)
        let operations = FakeSerialOperations(reads: [.value(replacementDeviceInfoFrame() + extra)])
        let session = try USBSession(descriptor: descriptor, operations: operations)
        await #expect(throws: USBSessionError.protocolFailure("invalid replacement capabilities")) {
            try await session.connect(handshakeTimeout: .seconds(1))
        }
        #expect(await session.currentReplacementContext() == nil)
    }

    @Test func replacementEventsUseStrictTypedRouting() async throws {
        let operations = FakeSerialOperations(reads: [.value(
            replacementDeviceInfoFrame() + capabilityFrame() +
            stateFrame(#"{"event":"vk_asset_progress","next_offset":12,"transfer_id":7}"#)
        )])
        let session = try USBSession(descriptor: descriptor, operations: operations)
        let eventTask = Task { () -> ReplacementProtocolEvent? in
            for await event in session.events {
                if case .replacementEvent(let replacement) = event { return replacement }
            }
            return nil
        }
        _ = try await session.connect(handshakeTimeout: .seconds(1))
        let event = await eventTask.value
        #expect(event == .asset(.progress(transferID: 7, nextOffset: 12)))
        await session.disconnect()
    }

    @Test func malformedKnownReplacementEventIsTerminalAndNeverDowngrades() async throws {
        let operations = FakeSerialOperations(reads: [.value(
            replacementDeviceInfoFrame() + capabilityFrame() +
            stateFrame(#"{"event":"vk_asset_progress","next_offset":12,"rogue":1,"transfer_id":7}"#)
        )])
        let session = try USBSession(descriptor: descriptor, operations: operations)
        await #expect(throws: USBSessionError.protocolFailure("invalid replacement event")) {
            try await session.connect(handshakeTimeout: .seconds(1))
        }
        #expect(await session.currentReplacementContext() == nil)
    }

    @Test func reflectedInputStateIsAcceptedWithoutAllowingUnknownReplacementEvents() async throws {
        let acceptedOperations = FakeSerialOperations(reads: [.value(
            replacementDeviceInfoFrame() + capabilityFrame() +
            stateFrame(#"{"event":"vk_input_state","interaction_mode":"hold_to_talk","voice_key":"k1"}"#)
        )])
        let acceptedSession = try USBSession(descriptor: descriptor, operations: acceptedOperations)
        let inputState = Task { () -> StateEvent? in
            for await event in acceptedSession.events {
                guard case .stateEvent(let state) = event, state.event == "vk_input_state" else { continue }
                return state
            }
            return nil
        }
        #expect(try await acceptedSession.connect(handshakeTimeout: .seconds(1)).hardware == "vibe_keyboard")
        #expect(await inputState.value?.event == "vk_input_state")
        await acceptedSession.disconnect()

        let rejectedOperations = FakeSerialOperations(reads: [.value(
            replacementDeviceInfoFrame() + capabilityFrame() +
            stateFrame(#"{"event":"vk_future_state"}"#)
        )])
        let rejectedSession = try USBSession(descriptor: descriptor, operations: rejectedOperations)
        await #expect(throws: USBSessionError.protocolFailure("unknown replacement event")) {
            try await rejectedSession.connect(handshakeTimeout: .seconds(1))
        }
    }

    @Test func typedAssetCommandsRequireCurrentCapabilityOperationMatrix() async throws {
        let unavailable = FakeSerialOperations(reads: [.value(
            replacementDeviceInfoFrame() + capabilityFrame(features: #""assets":{"available":false,"reason":"policy_blocked","version":1}"#)
        )])
        let unavailableSession = try USBSession(descriptor: descriptor, operations: unavailable)
        _ = try await unavailableSession.connect(handshakeTimeout: .seconds(1))
        await #expect(throws: USBSessionError.protocolFailure("assets unavailable")) {
            try await unavailableSession.sendAssetCommand(.list(snapshotID: 0, cursor: 0, limit: 1))
        }
        await unavailableSession.disconnect()

        let zeroUpload = FakeSerialOperations(reads: [.value(
            replacementDeviceInfoFrame() + capabilityFrame(features: fullAssetsFeature(uploadMaxBytes: 0))
        )])
        let zeroSession = try USBSession(descriptor: descriptor, operations: zeroUpload)
        _ = try await zeroSession.connect(handshakeTimeout: .seconds(1))
        try await zeroSession.sendAssetCommand(.list(snapshotID: 0, cursor: 0, limit: 1))
        await #expect(throws: USBSessionError.protocolFailure("asset begin unauthorized")) {
            try await zeroSession.sendAssetCommand(.begin(transferID: 7, sha256: testHash, totalBytes: 1, kind: .image))
        }
        await #expect(throws: USBSessionError.protocolFailure("verified-erased authorization unavailable")) {
            try await zeroSession.sendAssetCommand(.storageFormat)
        }
        await zeroSession.disconnect()
    }

    @Test func opaqueAssetTransferAdvancesOnlyOnExactProgressAndInvalidatesOnSnapshot() async throws {
        let operations = FakeSerialOperations(reads: [.value(
            replacementDeviceInfoFrame() + capabilityFrame(features: fullAssetsFeature())
        )])
        let session = try USBSession(descriptor: descriptor, operations: operations)
        _ = try await session.connect(handshakeTimeout: .seconds(1))
        try await session.sendAssetCommand(.begin(transferID: 7, sha256: testHash, totalBytes: 3, kind: .image))

        operations.enqueueRead(.value(stateFrame(assetReadyJSON(totalBytes: 3))))
        try await waitUntil { await session.currentActiveAssetTransfer() != nil }
        let first = try #require(await session.currentActiveAssetTransfer())
        #expect(first.nextOffset == 0)

        try await session.sendAssetChunk(Data([0xaa]), using: first)
        #expect(await session.currentActiveAssetTransfer()?.nextOffset == 0)
        await #expect(throws: USBSessionError.protocolFailure("invalid asset transfer authorization")) {
            try await session.sendAssetChunk(Data([0xbb]), using: first)
        }

        operations.enqueueRead(.value(stateFrame(#"{"event":"vk_asset_progress","next_offset":1,"transfer_id":7}"#)))
        try await waitUntil { await session.currentActiveAssetTransfer()?.nextOffset == 1 }
        let second = try #require(await session.currentActiveAssetTransfer())
        #expect(second != first)

        operations.enqueueRead(.value(capabilityFrame(features: fullAssetsFeature())))
        try await waitUntil { await session.currentActiveAssetTransfer() == nil }
        await #expect(throws: USBSessionError.protocolFailure("invalid asset transfer authorization")) {
            try await session.sendAssetChunk(Data([0xcc]), using: second)
        }
        await session.disconnect()
    }

    @Test func transferCompletesOnlyAfterProgressEndAndCorrelatedStored() async throws {
        let operations = FakeSerialOperations(reads: [.value(
            replacementDeviceInfoFrame() + capabilityFrame(features: fullAssetsFeature())
        )])
        let session = try USBSession(descriptor: descriptor, operations: operations)
        _ = try await session.connect(handshakeTimeout: .seconds(1))
        try await session.sendAssetCommand(.begin(transferID: 7, sha256: testHash, totalBytes: 3, kind: .image))
        operations.enqueueRead(.value(stateFrame(assetReadyJSON(totalBytes: 3))))
        try await waitUntil { await session.currentActiveAssetTransfer() != nil }
        let handle = try #require(await session.currentActiveAssetTransfer())

        try await session.sendAssetChunk(Data([0xaa, 0xbb, 0xcc]), using: handle)
        #expect(operations.writtenData.suffix(15) == Data([0x01, 0x40, 0x0b, 0x00, 0x07, 0, 0, 0, 0, 0, 0, 0, 0xaa, 0xbb, 0xcc]))
        operations.enqueueRead(.value(stateFrame(#"{"event":"vk_asset_progress","next_offset":3,"transfer_id":7}"#)))
        try await waitUntil { await session.currentActiveAssetTransfer()?.nextOffset == 3 }

        try await session.sendAssetCommand(.end(transferID: 7, sha256: testHash, totalBytes: 3, kind: .image))
        operations.enqueueRead(.value(stateFrame(#"{"event":"vk_asset_stored","kind":"image","sha256":"\#(testHash)","total_bytes":3,"transfer_id":7}"#)))
        try await waitUntil { await session.currentActiveAssetTransfer() == nil }
        #expect(await session.takeAssetTransferOutcome(transferID: 7) == .stored(transferID: 7, sha256: testHash, totalBytes: 3, kind: .image))
        #expect(await session.takeAssetTransferOutcome(transferID: 7) == nil)
        await session.disconnect()
    }

    @Test func fastAssetResponsesAreCorrelatedBeforeSuspendedWriteReturns() async throws {
        let operations = FakeSerialOperations(
            reads: [.value(replacementDeviceInfoFrame() + capabilityFrame(features: fullAssetsFeature()))],
            writeWaits: [.value(true)],
            injectedReadOnWrite: (call: 4, data: stateFrame(assetReadyJSON(totalBytes: 3)))
        )
        let session = try USBSession(descriptor: descriptor, operations: operations)
        _ = try await session.connect(handshakeTimeout: .seconds(1))
        try await session.sendAssetCommand(.begin(transferID: 7, sha256: testHash, totalBytes: 3, kind: .image))
        try await waitUntil { await session.currentActiveAssetTransfer() != nil }
        guard case .ready = await session.currentState() else {
            Issue.record("Session did not remain ready")
            return
        }
        #expect(await session.currentActiveAssetTransfer()?.transferID == 7)
        await session.disconnect()
    }

    @Test func fastAssetProgressIsCorrelatedBeforeSuspendedChunkWriteReturns() async throws {
        let operations = FakeSerialOperations(
            reads: [.value(replacementDeviceInfoFrame() + capabilityFrame(features: fullAssetsFeature()))],
            writeWaits: [.value(true)],
            injectedReadOnWrite: (
                call: 5,
                data: stateFrame(#"{"event":"vk_asset_progress","next_offset":3,"transfer_id":7}"#)
            )
        )
        let session = try USBSession(descriptor: descriptor, operations: operations)
        _ = try await session.connect(handshakeTimeout: .seconds(1))
        try await session.sendAssetCommand(.begin(transferID: 7, sha256: testHash, totalBytes: 3, kind: .image))
        operations.enqueueRead(.value(stateFrame(assetReadyJSON(totalBytes: 3))))
        try await waitUntil { await session.currentActiveAssetTransfer() != nil }
        let handle = try #require(await session.currentActiveAssetTransfer())

        try await session.sendAssetChunk(Data([0xaa, 0xbb, 0xcc]), using: handle)
        try await waitUntil { await session.currentActiveAssetTransfer()?.nextOffset == 3 }
        guard case .ready = await session.currentState() else {
            Issue.record("Session did not remain ready")
            return
        }
        await session.disconnect()
    }

    @Test func correlatedAssetErrorHasTypedTerminalOutcome() async throws {
        let operations = FakeSerialOperations(reads: [.value(
            replacementDeviceInfoFrame() + capabilityFrame(features: fullAssetsFeature())
        )])
        let session = try USBSession(descriptor: descriptor, operations: operations)
        _ = try await session.connect(handshakeTimeout: .seconds(1))
        try await session.sendAssetCommand(.begin(transferID: 7, sha256: testHash, totalBytes: 3, kind: .image))
        operations.enqueueRead(.value(stateFrame(#"{"code":"no_space","event":"vk_error","operation":"asset","transfer_id":7}"#)))
        try await waitUntil { await session.currentAssetTransferOutcome(transferID: 7) != nil }
        #expect(await session.takeAssetTransferOutcome(transferID: 7) == .rejected(transferID: 7, code: "no_space", nextOffset: nil, message: nil))
        #expect(await session.currentActiveAssetTransfer() == nil)
        await session.disconnect()
    }

    @Test func activeAssetErrorCanBeAbortedWithoutLeavingDeviceBusy() async throws {
        let operations = FakeSerialOperations(reads: [.value(
            replacementDeviceInfoFrame() + capabilityFrame(features: fullAssetsFeature())
        )])
        let session = try USBSession(descriptor: descriptor, operations: operations)
        _ = try await session.connect(handshakeTimeout: .seconds(1))
        try await session.sendAssetCommand(.begin(transferID: 7, sha256: testHash, totalBytes: 3, kind: .image))
        operations.enqueueRead(.value(stateFrame(assetReadyJSON(totalBytes: 3))))
        try await waitUntil { await session.currentActiveAssetTransfer() != nil }
        let handle = try #require(await session.currentActiveAssetTransfer())
        try await session.sendAssetChunk(Data([0xaa, 0xbb, 0xcc]), using: handle)
        operations.enqueueRead(.value(stateFrame(#"{"event":"vk_asset_progress","next_offset":3,"transfer_id":7}"#)))
        try await waitUntil { await session.currentActiveAssetTransfer()?.nextOffset == 3 }

        try await session.sendAssetCommand(.end(transferID: 7, sha256: testHash, totalBytes: 3, kind: .image))
        operations.enqueueRead(.value(stateFrame(
            #"{"code":"write_failed","event":"vk_error","message":"phase=end;esp_err=0xffffffff","operation":"asset","transfer_id":7}"#
        )))
        try await waitUntil { await session.currentAssetTransferOutcome(transferID: 7) != nil }
        #expect(await session.currentActiveAssetTransfer()?.transferID == 7)

        try await session.sendAssetCommand(.abort(transferID: 7))
        operations.enqueueRead(.value(stateFrame(#"{"event":"vk_asset_aborted","transfer_id":7}"#)))
        try await waitUntil { await session.currentActiveAssetTransfer() == nil }
        #expect(decodedStateBodies(operations.writtenData).contains(#"{"event":"vk_asset_abort","transfer_id":7}"#))
        await session.disconnect()
    }

    @Test func uncorrelatedReadyAndWrongProgressFailClosed() async throws {
        let uncorrelatedOperations = FakeSerialOperations(reads: [.value(
            replacementDeviceInfoFrame() + capabilityFrame(features: fullAssetsFeature())
        )])
        let uncorrelatedSession = try USBSession(descriptor: descriptor, operations: uncorrelatedOperations)
        _ = try await uncorrelatedSession.connect(handshakeTimeout: .seconds(1))
        uncorrelatedOperations.enqueueRead(.value(stateFrame(assetReadyJSON(totalBytes: 3))))
        try await waitForState(uncorrelatedSession, .failed(.protocolFailure("uncorrelated asset ready")))
        #expect(await uncorrelatedSession.currentActiveAssetTransfer() == nil)

        let progressOperations = FakeSerialOperations(reads: [.value(
            replacementDeviceInfoFrame() + capabilityFrame(features: fullAssetsFeature())
        )])
        let progressSession = try USBSession(descriptor: descriptor, operations: progressOperations)
        _ = try await progressSession.connect(handshakeTimeout: .seconds(1))
        try await progressSession.sendAssetCommand(.begin(transferID: 7, sha256: testHash, totalBytes: 3, kind: .image))
        progressOperations.enqueueRead(.value(stateFrame(assetReadyJSON(totalBytes: 3))))
        try await waitUntil { await progressSession.currentActiveAssetTransfer() != nil }
        let handle = try #require(await progressSession.currentActiveAssetTransfer())
        try await progressSession.sendAssetChunk(Data([0xaa]), using: handle)
        progressOperations.enqueueRead(.value(stateFrame(#"{"event":"vk_asset_progress","next_offset":2,"transfer_id":7}"#)))
        try await waitForState(progressSession, .failed(.protocolFailure("invalid asset progress")))
        #expect(await progressSession.currentActiveAssetTransfer() == nil)
    }

    @Test func screenCommitAdvancesCurrentSelectionForWidgetAndNextCommit() async throws {
        let screenFeature = #""screen":{"available":true,"configured":false,"fonts":[{"id":"vk-sans","metrics_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","version":1}],"max_assets":64,"max_commit_bytes":4092,"max_depth":4,"max_fonts":4,"max_json_tokens":512,"max_layout_bytes":3072,"max_objects":32,"max_pet_states":6,"max_string_bytes":256,"max_widget_value_bytes":256,"max_widgets":16,"modes":["image","pet","dashboard","custom"],"revision":0,"version":1}"#
        let operations = FakeSerialOperations(reads: [.value(
            replacementDeviceInfoFrame() + capabilityFrame(features: fullAssetsFeature() + "," + screenFeature)
        )])
        let session = try USBSession(descriptor: descriptor, operations: operations)
        _ = try await session.connect(handshakeTimeout: .seconds(1))
        let context = try #require(await session.currentReplacementContext())
        guard case .available(let screen)? = context.snapshot.screen else { return }
        let reference = ScreenAssetReference(bytes: 1, kind: .image, sha256: testHash)
        let first = ScreenCommit(
            expectedRevision: 0,
            revision: 1,
            assets: [reference],
            payload: .image(.init(backgroundRGB888: 0, fit: .contain, sha256: testHash)),
            limits: .init(displayWidth: 428, displayHeight: 142, screen: screen)
        )
        try await session.sendScreenCommand(.commit(first))
        let committed = Task {
            for await event in session.events {
                if case .replacementEvent(.screen(.committed(_, _, 1, _))) = event { return }
            }
        }
        operations.enqueueRead(.value(stateFrame(#"{"assets_manifest_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","event":"vk_screen_committed","previous_revision":0,"revision":1,"screen_manifest_sha256":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}"#)))
        await committed.value
        try await session.sendWidgetUpdate(.init(revision: 1, widgetID: "status", sequence: 1, state: .freshText("Ready")))

        let second = ScreenCommit(
            expectedRevision: 1,
            revision: 2,
            assets: [reference],
            payload: .image(.init(backgroundRGB888: 0, fit: .contain, sha256: testHash)),
            limits: .init(displayWidth: 428, displayHeight: 142, screen: screen.selecting(revision: 1, configured: true))
        )
        try await session.sendScreenCommand(.commit(second))
        await session.disconnect()
    }

    @Test func typedScreenQueryRequiresCurrentScreenAndAssets() async throws {
        let operations = FakeSerialOperations(reads: [.value(
            replacementDeviceInfoFrame() + capabilityFrame(features: fullAssetsFeature())
        )])
        let session = try USBSession(descriptor: descriptor, operations: operations)
        _ = try await session.connect(handshakeTimeout: .seconds(1))
        await #expect(throws: USBSessionError.protocolFailure("screen unavailable")) {
            try await session.sendScreenCommand(.query)
        }
        await session.disconnect()
    }

    @Test func openAndConfigureFailuresAreTypedAndCloseCorrectly() async throws {
        let openFailure = FakeSerialOperations(openResult: .failure(EACCES))
        let first = try USBSession(descriptor: descriptor, operations: openFailure)
        await #expect(throws: USBSessionError.openFailed(EACCES)) { try await first.openForInspection() }
        #expect(openFailure.closeCount == 0)

        let configureFailure = FakeSerialOperations(configureResult: .failure(EIO))
        let second = try USBSession(descriptor: descriptor, operations: configureFailure)
        await #expect(throws: USBSessionError.configureFailed(EIO)) { try await second.openForInspection() }
        #expect(configureFailure.closeCount == 1)
    }

    @Test func partialWritesEINTRAndEAGAINComplete() async throws {
        let operations = FakeSerialOperations(
            writes: [.failure(EINTR), .value(2), .failure(EAGAIN), .value(18)],
            writeWaits: [.value(true)]
        )
        let session = try USBSession(descriptor: descriptor, operations: operations)
        try await session.openForInspection()
        try await session.send(.ping)
        #expect(operations.writeCallCount == 4)
        #expect(operations.writeWaitCount == 1)
        await session.disconnect()
        #expect(operations.closeCount == 1)
    }

    @Test(arguments: [0, 10_000])
    func invalidWriteCountsTerminateAndClose(count: Int) async throws {
        let operations = FakeSerialOperations(writes: [.value(count)])
        let session = try USBSession(descriptor: descriptor, operations: operations)
        try await session.openForInspection()
        await #expect(throws: USBSessionError.writeFailed(EIO)) { try await session.send(.ping) }
        #expect(await session.currentState() == .failed(.writeFailed(EIO)))
        #expect(operations.closeCount == 1)
        await session.disconnect()
        #expect(operations.closeCount == 1)
    }

    @Test func writeAndWaitErrorsPreserveCauseAndClose() async throws {
        let writeOperations = FakeSerialOperations(writes: [.failure(ENXIO)])
        let writeSession = try USBSession(descriptor: descriptor, operations: writeOperations)
        try await writeSession.openForInspection()
        await #expect(throws: USBSessionError.writeFailed(ENXIO)) { try await writeSession.send(.ping) }
        #expect(await writeSession.currentState() == .failed(.writeFailed(ENXIO)))
        #expect(writeOperations.closeCount == 1)

        let waitOperations = FakeSerialOperations(writes: [.failure(EAGAIN)], writeWaits: [.failure(EBADF)])
        let waitSession = try USBSession(descriptor: descriptor, operations: waitOperations)
        try await waitSession.openForInspection()
        await #expect(throws: USBSessionError.waitFailed(EBADF)) { try await waitSession.send(.ping) }
        #expect(await waitSession.currentState() == .failed(.waitFailed(EBADF)))
        #expect(waitOperations.closeCount == 1)
    }

    @Test func writeTimeoutAndDeadlineCrossingDoNotTrap() async throws {
        let timeoutOperations = FakeSerialOperations(writes: Array(repeating: .failure(EAGAIN), count: 8))
        let timeoutClock = AdvancingClock(step: 1_000_000)
        let timeoutSession = try USBSession(descriptor: descriptor, operations: timeoutOperations, clock: timeoutClock)
        try await timeoutSession.openForInspection()
        await #expect(throws: USBSessionError.writeTimedOut) {
            try await timeoutSession.send(.ping, timeout: .milliseconds(2))
        }
        #expect(timeoutOperations.closeCount == 1)

        let crossingOperations = FakeSerialOperations(writes: [.failure(EAGAIN)])
        let crossingClock = SequenceClock(values: [0, 0, 3_000_000])
        let crossingSession = try USBSession(descriptor: descriptor, operations: crossingOperations, clock: crossingClock)
        try await crossingSession.openForInspection()
        await #expect(throws: USBSessionError.writeTimedOut) {
            try await crossingSession.send(.ping, timeout: .milliseconds(2))
        }
        #expect(crossingOperations.writeWaitCount == 0)
        #expect(crossingOperations.closeCount == 1)
    }

    @Test func ledAndWidgetUseCapabilityGatedSerializedSessionRouting() async throws {
        let ledFeature = #""led":{"available":true,"color_model":"rgb8","key_pixels":{"k1":0,"k2":1,"k3":2,"k4":3},"max_brightness":64,"max_frame_channel_sum":3264,"pixel_count":17,"strip_count":13,"strip_first":4,"tick_ms":30,"version":1,"wire_order":"grb"}"#
        let screenFeature = #""screen":{"available":true,"configured":true,"fonts":[{"id":"vk-sans","metrics_sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","version":1}],"max_assets":64,"max_commit_bytes":4092,"max_depth":4,"max_fonts":4,"max_json_tokens":512,"max_layout_bytes":3072,"max_objects":32,"max_pet_states":6,"max_string_bytes":256,"max_widget_value_bytes":256,"max_widgets":16,"modes":["image","pet","dashboard","custom"],"revision":7,"version":1}"#
        let operations = FakeSerialOperations(reads: [.value(
            replacementDeviceInfoFrame() + capabilityFrame(features: fullAssetsFeature() + "," + screenFeature + "," + ledFeature)
        )])
        let session = try USBSession(descriptor: descriptor, operations: operations)
        _ = try await session.connect(handshakeTimeout: .seconds(1))

        try await session.sendLEDCommand(.config(requestID: 9, enabled: true, brightness: 32))
        try await session.sendWidgetUpdate(.init(revision: 7, widgetID: "cpu", sequence: 19, state: .freshNumber(.init(coefficient: 425, scale: 1))))
        let bodies = decodedStateBodies(operations.writtenData)
        #expect(bodies.contains(#"{"brightness":32,"enabled":true,"event":"vk_led_config","request_id":9}"#))
        #expect(bodies.contains(#"{"event":"vk_widget_update","revision":7,"sequence":19,"state":"fresh","value":42.5,"widget_id":"cpu"}"#))

        operations.enqueueRead(.value(
            stateFrame(#"{"available":true,"brightness":32,"effective":"connected","enabled":true,"event":"vk_led_state","request_id":9,"source":"applied"}"#) +
            stateFrame(#"{"event":"vk_widget_applied","revision":7,"sequence":19,"state":"fresh","widget_id":"cpu"}"#)
        ))
        let eventTask = Task { () -> Set<String> in
            var result = Set<String>()
            for await event in session.events {
                if case .replacementEvent(.led) = event { result.insert("led") }
                if case .replacementEvent(.widget) = event { result.insert("widget") }
                if result.count == 2 { return result }
            }
            return result
        }
        #expect(await eventTask.value == Set(["led", "widget"]))
        await session.disconnect()
    }

    @Test func unavailableLEDAndStaleWidgetNeverWrite() async throws {
        let unavailable = #""led":{"available":false,"reason":"calibration_required","version":1}"#
        let operations = FakeSerialOperations(reads: [.value(replacementDeviceInfoFrame() + capabilityFrame(features: unavailable))])
        let session = try USBSession(descriptor: descriptor, operations: operations)
        _ = try await session.connect(handshakeTimeout: .seconds(1))
        let before = operations.writtenData.count
        await #expect(throws: LEDServiceError.unavailable("calibration_required")) {
            try await session.sendLEDCommand(.config(requestID: 1, enabled: true, brightness: 1))
        }
        await #expect(throws: USBSessionError.protocolFailure("screen unavailable")) {
            try await session.sendWidgetUpdate(.init(revision: 1, widgetID: "cpu", sequence: 1, state: .stale))
        }
        #expect(operations.writtenData.count == before)
        await session.disconnect()
    }

    @Test func cancelledSendTerminatesAndCloses() async throws {
        let operations = FakeSerialOperations(
            writes: Array(repeating: .failure(EAGAIN), count: 10_000),
            defaultWriteWait: .value(false)
        )
        let session = try USBSession(descriptor: descriptor, operations: operations)
        try await session.openForInspection()
        let task = Task { try await session.send(.ping, timeout: .seconds(30)) }
        try await waitUntil { operations.writeCallCount > 0 }
        task.cancel()
        await #expect(throws: USBSessionError.cancelled) { try await task.value }
        #expect(await session.currentState() == .failed(.cancelled))
        #expect(operations.closeCount == 1)
    }

    @Test func eofAndConcurrentDisconnectCloseExactlyOnce() async throws {
        let operations = FakeSerialOperations(reads: [.value(Data())])
        let session = try USBSession(descriptor: descriptor, operations: operations)
        try await session.openForInspection()
        async let disconnect: Void = session.disconnect()
        _ = await disconnect
        try await Task.sleep(for: .milliseconds(20))
        #expect(operations.closeCount == 1)
    }

    @Test func readRetriesAndUsesConfiguredMaximumChunk() async throws {
        let operations = FakeSerialOperations(
            reads: [.failure(EINTR), .failure(EAGAIN), .value(Data("x".utf8))],
            readWaits: [.value(false)]
        )
        let session = try USBSession(
            descriptor: descriptor,
            operations: operations,
            readChunkSize: 128,
            receiveBufferLimit: 256
        )
        try await session.openForInspection()
        try await Task.sleep(for: .milliseconds(30))
        #expect(operations.readMaximumCounts.allSatisfy { $0 == 128 })
        #expect(operations.readWaitCount >= 1)
        await session.disconnect()
    }

    @Test func readAndReadWaitFailuresTerminateAndClose() async throws {
        let readOperations = FakeSerialOperations(reads: [.failure(ENXIO)])
        let readSession = try USBSession(descriptor: descriptor, operations: readOperations)
        try await readSession.openForInspection()
        try await waitForState(readSession, .failed(.readFailed(ENXIO)))
        #expect(readOperations.closeCount == 1)

        let waitOperations = FakeSerialOperations(reads: [.failure(EAGAIN)], readWaits: [.failure(EBADF)])
        let waitSession = try USBSession(descriptor: descriptor, operations: waitOperations)
        try await waitSession.openForInspection()
        try await waitForState(waitSession, .failed(.waitFailed(EBADF)))
        #expect(waitOperations.closeCount == 1)
    }

    @Test func parserDeclaredFrameBeyondReceiveLimitTerminates() async throws {
        let oversizedDeclaration = Data([0x01, 0x10, 0x20, 0x00])
        let operations = FakeSerialOperations(reads: [.value(oversizedDeclaration)])
        let session = try USBSession(
            descriptor: descriptor,
            operations: operations,
            readChunkSize: 16,
            receiveBufferLimit: 16
        )
        try await session.openForInspection()
        try await waitUntil { operations.closeCount == 1 }
        guard case .failed(.protocolFailure) = await session.currentState() else {
            Issue.record("Expected protocol failure")
            return
        }
        #expect(operations.closeCount == 1)
    }

    @Test func parserResynchronizationDiagnosticsStayBounded() async throws {
        let operations = FakeSerialOperations(reads: [.value(Data(repeating: 0x55, count: 8))])
        let session = try USBSession(
            descriptor: descriptor,
            operations: operations,
            readChunkSize: 8,
            receiveBufferLimit: 16,
            diagnosticByteLimit: 4,
            diagnosticEntryLimit: 2,
            eventBufferCapacity: 32
        )
        try await session.openForInspection()
        try await Task.sleep(for: .milliseconds(20))
        let diagnostics = await session.diagnostics()
        #expect(diagnostics.text.utf8.count == 4)
        #expect(diagnostics.entries.count <= 2)
        await session.disconnect()
        #expect(operations.closeCount == 1)
    }

    @Test func inspectionConsumerCancellationClosesAndPreservesTerminalState() async throws {
        let operations = FakeSerialOperations()
        let session = try USBSession(descriptor: descriptor, operations: operations)
        let consumer = Task {
            for await _ in session.events {}
        }
        try await session.openForInspection()
        consumer.cancel()
        await consumer.value
        try await waitForState(session, .failed(.eventConsumerTerminated))
        async let disconnect: Void = session.disconnect()
        _ = await disconnect
        #expect(await session.currentState() == .failed(.eventConsumerTerminated))
        #expect(operations.closeCount == 1)
    }

    @Test func readyConsumerCancellationStopsHeartbeatAndClosesOnce() async throws {
        let operations = FakeSerialOperations(reads: [.value(deviceInfoFrame())])
        let session = try USBSession(
            descriptor: descriptor,
            operations: operations,
            heartbeatInterval: .milliseconds(5)
        )
        let consumer = Task {
            for await _ in session.events {}
        }
        _ = try await session.connect(handshakeTimeout: .seconds(1))
        consumer.cancel()
        await consumer.value
        try await waitForState(session, .failed(.eventConsumerTerminated))
        let writesAtTermination = operations.writeCallCount
        try await Task.sleep(for: .milliseconds(20))
        #expect(operations.writeCallCount == writesAtTermination)
        await session.disconnect()
        #expect(await session.currentState() == .failed(.eventConsumerTerminated))
        #expect(operations.closeCount == 1)
    }

    @Test func eventOverflowIsTerminalBoundedAndDoesNotLogAudioPayload() async throws {
        let secretPayload = Data("raw-secret-audio".utf8)
        let operations = FakeSerialOperations(reads: [.value(audioFrame(payload: secretPayload))])
        let session = try USBSession(
            descriptor: descriptor,
            operations: operations,
            diagnosticByteLimit: 8,
            diagnosticEntryLimit: 2,
            eventBufferCapacity: 2
        )
        try await session.openForInspection()
        try await waitForState(session, .failed(.eventBufferOverflow))
        #expect(operations.closeCount == 1)
        let diagnostics = await session.diagnostics()
        #expect(!diagnostics.text.contains("raw-secret-audio"))
        #expect(!diagnostics.entries.joined().contains("raw-secret-audio"))
        #expect(diagnostics.entries.count <= 2)

        var received: [USBSessionEvent] = []
        for await event in session.events { received.append(event) }
        #expect(received.count <= 2)
        #expect(received.contains(.stateChanged(.failed(.eventBufferOverflow))))
    }

    @Test func replacementEventOverflowClearsCurrentEpochBeforeClosing() async throws {
        let operations = FakeSerialOperations(reads: [
            .value(replacementDeviceInfoFrame() + capabilityFrame()),
            .failure(EAGAIN),
        ])
        let session = try USBSession(
            descriptor: descriptor,
            operations: operations,
            eventBufferCapacity: 16
        )
        _ = try await session.connect(handshakeTimeout: .seconds(1))
        #expect(await session.currentReplacementContext() != nil)

        let events = Array(repeating: stateFrame(#"{"event":"ping"}"#), count: 32)
            .reduce(into: Data()) { $0.append($1) }
        operations.enqueueRead(.value(events))
        try await waitForState(session, .failed(.eventBufferOverflow))

        #expect(await session.currentReplacementContext() == nil)
        #expect(operations.closeCount == 1)
        await #expect(throws: USBSessionError.eventBufferOverflow) {
            try await session.connect(handshakeTimeout: .milliseconds(50))
        }
        #expect(await session.currentReplacementContext() == nil)
        #expect(operations.closeCount == 1)
    }

    @Test func wrongHardwareAndHandshakeTimeoutCloseOnce() async throws {
        let wrongOperations = FakeSerialOperations(reads: [.value(deviceInfoFrame(hardware: "other"))])
        let wrongSession = try USBSession(descriptor: descriptor, operations: wrongOperations)
        await #expect(throws: USBSessionError.incompatibleHardware("other")) {
            try await wrongSession.connect(handshakeTimeout: .seconds(1))
        }
        #expect(wrongOperations.closeCount == 1)

        let timeoutOperations = FakeSerialOperations()
        let timeoutClock = AdvancingClock(step: 500_000_000)
        let timeoutSession = try USBSession(descriptor: descriptor, operations: timeoutOperations, clock: timeoutClock)
        await #expect(throws: USBSessionError.handshakeTimedOut) {
            try await timeoutSession.connect(handshakeTimeout: .seconds(5))
        }
        let commands = decodedStateBodies(timeoutOperations.writtenData)
        #expect(commands.filter { $0.contains("get_device_info") }.count >= 2)
        #expect(commands.contains { $0 == #"{"event":"ping"}"# })
        #expect(timeoutOperations.closeCount == 1)
    }

    @Test func heartbeatFailurePreservesConcreteCauseAndCloses() async throws {
        let operations = FakeSerialOperations(
            reads: [.value(deviceInfoFrame())],
            writes: [.value(38), .value(31), .value(50), .failure(ENXIO)]
        )
        let session = try USBSession(
            descriptor: descriptor,
            operations: operations,
            heartbeatInterval: .milliseconds(5)
        )
        _ = try await session.connect(handshakeTimeout: .seconds(1))
        try await waitForState(session, .failed(.writeFailed(ENXIO)))
        #expect(operations.closeCount == 1)
    }

    @Test func reconnectResetsParserDiagnosticsAndSessionEpoch() async throws {
        let operations = FakeSerialOperations()
        let session = try USBSession(descriptor: descriptor, operations: operations)
        try await session.openForInspection()
        operations.enqueueRead(.value(Data("old boot text".utf8)))
        try await Task.sleep(for: .milliseconds(20))
        await session.disconnect()

        operations.enqueueRead(.value(deviceInfoFrame()))
        let info = try await session.connect(handshakeTimeout: .seconds(1))
        #expect(info.hardware == "vibe_keyboard")
        let diagnostics = await session.diagnostics()
        #expect(!diagnostics.text.contains("old boot text"))
        await session.disconnect()
        #expect(operations.closeCount == 2)
    }

    private var descriptor: USBDeviceDescriptor {
        USBDeviceDescriptor(
            registryEntryID: 7,
            vendorID: 0x303a,
            productID: 0x1001,
            serialNumber: "AA:BB:CC:DD:EE:FF",
            normalizedDeviceID: "AABBCCDDEEFF",
            calloutPath: "/dev/cu.fake"
        )
    }
}

private func deviceInfoFrame(hardware: String = "vibe_keyboard") -> Data {
    let body = Data(#"{"event":"device_info","hardware":"\#(hardware)","firmware_version":"0.3.8","device_id":"VS-AABBCCDDEEFF"}"#.utf8)
    var frame = Data([0x01, 0x10, UInt8(body.count), UInt8(body.count >> 8)])
    frame.append(body)
    return frame
}

private func replacementDeviceInfoFrame() -> Data {
    stateFrame(#"{"event":"device_info","hardware":"vibe_keyboard","firmware_version":"test","device_id":"VS-AABBCCDDEEFF","replacement_protocol":1}"#)
}

private let testHash = String(repeating: "a", count: 64)

private func fullAssetsFeature(uploadMaxBytes: UInt32 = 1_048_575) -> String {
    #""assets":{"available":true,"chunk_bytes":4084,"decoder_scratch_bytes":4096,"encodings":["raw","row_rle"],"free_bytes":1048576,"management":true,"max_active_decoded_bytes":243104,"max_asset_bytes":1048576,"max_assets":64,"max_frame_ms":65535,"max_frames":4,"min_frame_ms":1,"reserve_bytes":1,"revision":0,"storage_state":"ready","upload_max_bytes":\#(uploadMaxBytes),"version":1}"#
}

private func assetReadyJSON(totalBytes: UInt32) -> String {
    #"{"chunk_bytes":4084,"event":"vk_asset_ready","kind":"image","next_offset":0,"sha256":"\#(testHash)","total_bytes":\#(totalBytes),"transfer_id":7}"#
}

private func capabilityFrame(width: Int = 428, features: String = "") -> Data {
    stateFrame(#"{"event":"vk_capabilities","protocol":1,"display":{"width":\#(width),"height":142,"format":"rgb565"},"features":{\#(features)}}"#)
}

private func stateFrame(_ json: String) -> Data {
    let body = Data(json.utf8)
    precondition(body.count <= Int(UInt16.max))
    let length = UInt16(body.count)
    var frame = Data([0x01, 0x10, UInt8(truncatingIfNeeded: length), UInt8(truncatingIfNeeded: length >> 8)])
    frame.append(body)
    return frame
}

private func audioFrame(payload: Data) -> Data {
    var data = Data([0x01, 0x01, 0x10, 0x00])
    data.append(contentsOf: [1, 0, 0, 0, 0, 0, 0, 0, 1, 0])
    data.append(UInt8(payload.count))
    data.append(UInt8(payload.count >> 8))
    data.append(payload)
    return data
}

private func decodedStateBodies(_ data: Data) -> [String] {
    var offset = 0
    var values: [String] = []
    while offset + 4 <= data.count {
        let length = Int(data[offset + 2]) | (Int(data[offset + 3]) << 8)
        guard offset + 4 + length <= data.count else { break }
        values.append(String(decoding: data[(offset + 4)..<(offset + 4 + length)], as: UTF8.self))
        offset += 4 + length
    }
    return values
}

private func waitForState(_ session: USBSession, _ expected: USBSessionState) async throws {
    try await waitUntil { await session.currentState() == expected }
}

private func waitUntil(_ predicate: @escaping @Sendable () async -> Bool) async throws {
    for _ in 0..<200 {
        if await predicate() { return }
        try await Task.sleep(for: .milliseconds(2))
    }
    Issue.record("Timed out waiting for condition")
}

private final class FakeSerialOperations: SerialSystemOperating, @unchecked Sendable {
    private let lock = NSLock()
    private var openResult: SerialIOResult<Int32>
    private var configureResult: SerialIOResult<Void>
    private var reads: [SerialIOResult<Data>]
    private var writes: [SerialIOResult<Int>]
    private var readWaits: [SerialIOResult<Bool>]
    private var writeWaits: [SerialIOResult<Bool>]
    private let defaultReadWait: SerialIOResult<Bool>
    private let defaultWriteWait: SerialIOResult<Bool>
    private let injectedReadOnWrite: (call: Int, data: Data)?
    private var _openedFlags: Int32?
    private var _writtenData = Data()
    private var _writeCallCount = 0
    private var _readWaitCount = 0
    private var _writeWaitCount = 0
    private var _closeCount = 0
    private var _readMaximumCounts: [Int] = []

    init(
        openResult: SerialIOResult<Int32> = .value(42),
        configureResult: SerialIOResult<Void> = .value(()),
        reads: [SerialIOResult<Data>] = [],
        writes: [SerialIOResult<Int>] = [],
        readWaits: [SerialIOResult<Bool>] = [],
        writeWaits: [SerialIOResult<Bool>] = [],
        defaultReadWait: SerialIOResult<Bool> = .value(false),
        defaultWriteWait: SerialIOResult<Bool> = .value(true),
        injectedReadOnWrite: (call: Int, data: Data)? = nil
    ) {
        self.openResult = openResult
        self.configureResult = configureResult
        self.reads = reads
        self.writes = writes
        self.readWaits = readWaits
        self.writeWaits = writeWaits
        self.defaultReadWait = defaultReadWait
        self.defaultWriteWait = defaultWriteWait
        self.injectedReadOnWrite = injectedReadOnWrite
    }

    var openedFlags: Int32? { lock.withLock { _openedFlags } }
    var writtenData: Data { lock.withLock { _writtenData } }
    var writeCallCount: Int { lock.withLock { _writeCallCount } }
    var readWaitCount: Int { lock.withLock { _readWaitCount } }
    var writeWaitCount: Int { lock.withLock { _writeWaitCount } }
    var closeCount: Int { lock.withLock { _closeCount } }
    var readMaximumCounts: [Int] { lock.withLock { _readMaximumCounts } }

    func enqueueRead(_ result: SerialIOResult<Data>) { lock.withLock { reads.append(result) } }

    func open(path: String, flags: Int32) -> SerialIOResult<Int32> {
        lock.withLock {
            _openedFlags = flags
            return openResult
        }
    }

    func configureRaw(fileDescriptor: Int32) -> SerialIOResult<Void> { lock.withLock { configureResult } }

    func read(fileDescriptor: Int32, maximumCount: Int) -> SerialIOResult<Data> {
        lock.withLock {
            _readMaximumCounts.append(maximumCount)
            return reads.isEmpty ? .failure(EAGAIN) : reads.removeFirst()
        }
    }

    func write(fileDescriptor: Int32, data: Data) -> SerialIOResult<Int> {
        lock.withLock {
            _writeCallCount += 1
            if let injection = injectedReadOnWrite, injection.call == _writeCallCount {
                reads.append(.value(injection.data))
                return .failure(EAGAIN)
            }
            let result = writes.isEmpty ? .value(data.count) : writes.removeFirst()
            if case .value(let count) = result, count > 0, count <= data.count {
                _writtenData.append(data.prefix(count))
            }
            return result
        }
    }

    func wait(fileDescriptor: Int32, events: Int16, timeoutMilliseconds: Int32) -> SerialIOResult<Bool> {
        lock.withLock {
            if events & Int16(POLLOUT) != 0 {
                _writeWaitCount += 1
                return writeWaits.isEmpty ? defaultWriteWait : writeWaits.removeFirst()
            }
            _readWaitCount += 1
            return readWaits.isEmpty ? defaultReadWait : readWaits.removeFirst()
        }
    }

    func close(fileDescriptor: Int32) -> SerialIOResult<Void> {
        lock.withLock { _closeCount += 1 }
        return .value(())
    }
}

private final class AdvancingClock: USBMonotonicClock, @unchecked Sendable {
    private let lock = NSLock()
    private let step: UInt64
    private var value: UInt64 = 0

    init(step: UInt64) { self.step = step }

    func nowNanoseconds() -> UInt64 {
        lock.withLock {
            defer { value &+= step }
            return value
        }
    }
}

private final class SequenceClock: USBMonotonicClock, @unchecked Sendable {
    private let lock = NSLock()
    private var values: [UInt64]
    private var last: UInt64

    init(values: [UInt64]) {
        self.values = values
        self.last = values.last ?? 0
    }

    func nowNanoseconds() -> UInt64 {
        lock.withLock {
            guard !values.isEmpty else { return last }
            last = values.removeFirst()
            return last
        }
    }
}
