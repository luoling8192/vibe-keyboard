import Foundation

public enum AssetServiceError: Error, Equatable, Sendable {
    case invalidAsset
    case capabilityUnavailable
    case transferTimedOut
    case transferInvalidated
    case deviceRejected(String)
    case invalidScreenState
}

public struct PreparedAsset: Equatable, Sendable {
    public let data: Data
    public let sha256: String
    public let kind: AssetKind

    public init(data: Data, limits: VKA1Limits) throws {
        let container = try VKA1Codec.decode(data, limits: limits)
        self.data = data
        sha256 = container.sha256
        switch container.kind {
        case .image: kind = .image
        case .animation: kind = .animation
        case .glyphBitmap: kind = .glyphBitmap
        }
    }
}

public protocol AssetTransferSession: Sendable {
    func sendAssetCommand(_ command: AssetCommand, timeout: Duration) async throws
    func sendAssetChunk(_ payload: Data, using authorization: ActiveAssetTransfer, timeout: Duration) async throws
    func currentActiveAssetTransfer() async -> ActiveAssetTransfer?
    func currentAssetTransferOutcome(transferID: UInt32) async -> AssetTransferOutcome?
    func takeAssetTransferOutcome(transferID: UInt32) async -> AssetTransferOutcome?
    func currentReplacementContext() async -> ReplacementSessionContext?
}

extension USBSession: AssetTransferSession {}

public actor AssetTransferService {
    public struct Progress: Equatable, Sendable {
        public let transferID: UInt32
        public let nextOffset: UInt32
        public let totalBytes: UInt32

        public init(transferID: UInt32, nextOffset: UInt32, totalBytes: UInt32) {
            self.transferID = transferID
            self.nextOffset = nextOffset
            self.totalBytes = totalBytes
        }
    }

    private let session: any AssetTransferSession
    private let timeout: Duration

    public init(session: any AssetTransferSession, timeout: Duration = .seconds(30)) {
        self.session = session
        self.timeout = timeout
    }

    public func upload(_ asset: PreparedAsset, transferID: UInt32) async throws -> Progress {
        guard transferID != 0, let totalBytes = UInt32(exactly: asset.data.count), totalBytes > 0 else {
            throw AssetServiceError.invalidAsset
        }
        try await session.sendAssetCommand(.begin(transferID: transferID, sha256: asset.sha256, totalBytes: totalBytes, kind: asset.kind), timeout: .seconds(2))
        var authorization = try await awaitAuthorization(transferID: transferID, expectedSHA: asset.sha256, totalBytes: totalBytes, kind: asset.kind)
        while authorization.nextOffset < totalBytes {
            try Task.checkCancellation()
            let offset = Int(authorization.nextOffset)
            let length = min(Int(authorization.chunkBytes), asset.data.count - offset)
            guard length > 0 else { throw AssetServiceError.transferInvalidated }
            try await session.sendAssetChunk(asset.data.subdata(in: offset..<(offset + length)), using: authorization, timeout: .seconds(2))
            authorization = try await awaitProgress(from: authorization, expected: authorization.nextOffset + UInt32(length))
        }
        try await session.sendAssetCommand(.end(transferID: transferID, sha256: asset.sha256, totalBytes: totalBytes, kind: asset.kind), timeout: .seconds(2))
        try await awaitCompletion(transferID: transferID)
        return Progress(transferID: transferID, nextOffset: totalBytes, totalBytes: totalBytes)
    }

    public func resume(_ asset: PreparedAsset, transferID: UInt32) async throws -> Progress {
        guard transferID != 0, let totalBytes = UInt32(exactly: asset.data.count), totalBytes > 0 else { throw AssetServiceError.invalidAsset }
        try await session.sendAssetCommand(.query(transferID: transferID), timeout: .seconds(2))
        var authorization = try await awaitAuthorization(transferID: transferID, expectedSHA: asset.sha256, totalBytes: totalBytes, kind: asset.kind)
        while authorization.nextOffset < totalBytes {
            let offset = Int(authorization.nextOffset)
            let length = min(Int(authorization.chunkBytes), asset.data.count - offset)
            try await session.sendAssetChunk(asset.data.subdata(in: offset..<(offset + length)), using: authorization, timeout: .seconds(2))
            authorization = try await awaitProgress(from: authorization, expected: authorization.nextOffset + UInt32(length))
        }
        try await session.sendAssetCommand(.end(transferID: transferID, sha256: asset.sha256, totalBytes: totalBytes, kind: asset.kind), timeout: .seconds(2))
        try await awaitCompletion(transferID: transferID)
        return Progress(transferID: transferID, nextOffset: totalBytes, totalBytes: totalBytes)
    }

    public func cancel(transferID: UInt32) async throws {
        try await session.sendAssetCommand(.abort(transferID: transferID), timeout: .seconds(2))
    }

    private func awaitAuthorization(transferID: UInt32, expectedSHA: String, totalBytes: UInt32, kind: AssetKind) async throws -> ActiveAssetTransfer {
        try await poll {
            try await self.throwCorrelatedOutcomeIfPresent(transferID: transferID)
            guard let authorization = await self.session.currentActiveAssetTransfer() else {
                guard await self.session.currentReplacementContext() != nil else {
                    throw AssetServiceError.transferInvalidated
                }
                return nil
            }
            guard authorization.transferID == transferID,
                  authorization.sha256 == expectedSHA,
                  authorization.totalBytes == totalBytes,
                  authorization.kind == kind else { throw AssetServiceError.transferInvalidated }
            return authorization
        }
    }

    private func awaitProgress(from authorization: ActiveAssetTransfer, expected: UInt32) async throws -> ActiveAssetTransfer {
        try await poll {
            try await self.throwCorrelatedOutcomeIfPresent(transferID: authorization.transferID)
            guard let current = await self.session.currentActiveAssetTransfer(),
                  current.transferID == authorization.transferID else { throw AssetServiceError.transferInvalidated }
            return current.nextOffset == expected ? current : nil
        }
    }

    private func throwCorrelatedOutcomeIfPresent(transferID: UInt32) async throws {
        guard let outcome = await session.currentAssetTransferOutcome(transferID: transferID) else { return }
        _ = await session.takeAssetTransferOutcome(transferID: transferID)
        switch outcome {
        case .rejected(_, let code, let nextOffset, _):
            if code == "bad_offset", let nextOffset {
                throw AssetServiceError.deviceRejected("bad_offset(next_offset=\(nextOffset))")
            }
            throw AssetServiceError.deviceRejected(code)
        case .aborted:
            throw AssetServiceError.deviceRejected("aborted")
        case .invalidated:
            throw AssetServiceError.transferInvalidated
        case .stored:
            throw AssetServiceError.transferInvalidated
        }
    }

    private func awaitCompletion(transferID: UInt32) async throws {
        _ = try await poll { () async throws -> Bool? in
            if let outcome = await self.session.takeAssetTransferOutcome(transferID: transferID) {
                switch outcome {
                case .stored(let storedID, _, _, _) where storedID == transferID:
                    return true
                case .aborted:
                    throw AssetServiceError.deviceRejected("aborted")
                case .rejected(_, let code, _, _):
                    throw AssetServiceError.deviceRejected(code)
                case .invalidated:
                    throw AssetServiceError.transferInvalidated
                default:
                    throw AssetServiceError.transferInvalidated
                }
            }
            guard await self.session.currentReplacementContext() != nil else {
                throw AssetServiceError.transferInvalidated
            }
            return nil
        }
    }

    private func poll<T: Sendable>(_ body: () async throws -> T?) async throws -> T {
        let clock = ContinuousClock()
        let deadline = clock.now.advanced(by: timeout)
        while clock.now < deadline {
            try Task.checkCancellation()
            if let value = try await body() { return value }
            try await Task.sleep(for: .milliseconds(5))
        }
        throw AssetServiceError.transferTimedOut
    }
}

public actor ScreenConfigurationService {
    private let session: USBSession
    public init(session: USBSession) { self.session = session }

    public func query() async throws { try await session.sendScreenCommand(.query) }
    public func commit(_ commit: ScreenCommit) async throws { try await session.sendScreenCommand(.commit(commit)) }

    public func formatStorage() async throws {
        throw AssetServiceError.deviceRejected("verified-erased authorization unavailable")
    }
}
