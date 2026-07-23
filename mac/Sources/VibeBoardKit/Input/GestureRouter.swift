import Foundation

public struct GesturePolicy: Equatable, Sendable {
    public let doubleClickWindowMilliseconds: UInt32
    public let longPressThresholdMilliseconds: UInt32
    public let derivesDoubleClick: Bool
    public let derivesLongPress: Bool

    public init(
        doubleClickWindowMilliseconds: UInt32,
        longPressThresholdMilliseconds: UInt32,
        derivesDoubleClick: Bool,
        derivesLongPress: Bool
    ) throws {
        guard doubleClickWindowMilliseconds > 0 else {
            throw GestureError.invalidPolicy("doubleClickWindowMilliseconds")
        }
        guard longPressThresholdMilliseconds > 0 else {
            throw GestureError.invalidPolicy("longPressThresholdMilliseconds")
        }
        self.doubleClickWindowMilliseconds = doubleClickWindowMilliseconds
        self.longPressThresholdMilliseconds = longPressThresholdMilliseconds
        self.derivesDoubleClick = derivesDoubleClick
        self.derivesLongPress = derivesLongPress
    }
}

public enum DeviceKeyEvent: Equatable, Sendable {
    case down(CanonicalKey)
    case up(CanonicalKey, durationMilliseconds: UInt32?)
    case click(CanonicalKey, durationMilliseconds: UInt32?)
    case disconnect
}

public struct RoutedGesture: Equatable, Sendable {
    public let key: CanonicalKey
    public let gesture: KeyGesture

    public init(key: CanonicalKey, gesture: KeyGesture) {
        self.key = key
        self.gesture = gesture
    }
}

public enum GestureError: Error, Equatable, Sendable {
    case invalidPolicy(String)
    case duplicateDown(CanonicalKey)
    case upWithoutDown(CanonicalKey)
    case timestampRegression
}

public struct GestureRouter: Sendable {
    private struct PendingClick: Sendable {
        let timestampMilliseconds: UInt64
    }

    private let policy: GesturePolicy
    private var downTimestamps: [CanonicalKey: UInt64] = [:]
    private var pendingClicks: [CanonicalKey: PendingClick] = [:]
    private var suppressedClicks: Set<CanonicalKey> = []
    private var lastTimestampMilliseconds: UInt64?

    public init(policy: GesturePolicy) {
        self.policy = policy
    }

    public mutating func handle(_ event: DeviceKeyEvent, at timestampMilliseconds: UInt64) throws -> [RoutedGesture] {
        if case .disconnect = event {
            resetSession()
            return []
        }
        try validateTimestamp(timestampMilliseconds)
        var output = flushExpired(at: timestampMilliseconds)

        switch event {
        case let .down(key):
            guard downTimestamps[key] == nil else { throw GestureError.duplicateDown(key) }
            suppressedClicks.remove(key)
            downTimestamps[key] = timestampMilliseconds

        case let .up(key, _):
            guard let downTimestamp = downTimestamps.removeValue(forKey: key) else {
                throw GestureError.upWithoutDown(key)
            }
            if policy.derivesLongPress {
                let measured = timestampMilliseconds - downTimestamp
                if measured >= UInt64(policy.longPressThresholdMilliseconds) {
                    pendingClicks.removeValue(forKey: key)
                    suppressedClicks.insert(key)
                    output.append(RoutedGesture(key: key, gesture: .long))
                }
            }

        case let .click(key, _):
            if suppressedClicks.remove(key) != nil {
                break
            }
            if policy.derivesDoubleClick {
                if let pending = pendingClicks[key],
                   timestampMilliseconds - pending.timestampMilliseconds <= UInt64(policy.doubleClickWindowMilliseconds) {
                    pendingClicks.removeValue(forKey: key)
                    output.append(RoutedGesture(key: key, gesture: .double))
                } else {
                    pendingClicks[key] = PendingClick(timestampMilliseconds: timestampMilliseconds)
                }
            } else {
                output.append(RoutedGesture(key: key, gesture: .single))
            }

        case .disconnect:
            // Handled before timestamp validation so a new session can reset its clock.
            break
        }
        return output
    }

    public mutating func flush(at timestampMilliseconds: UInt64) throws -> [RoutedGesture] {
        try validateTimestamp(timestampMilliseconds)
        return flushExpired(at: timestampMilliseconds)
    }

    private mutating func resetSession() {
        downTimestamps.removeAll(keepingCapacity: true)
        pendingClicks.removeAll(keepingCapacity: true)
        suppressedClicks.removeAll(keepingCapacity: true)
        lastTimestampMilliseconds = nil
    }

    private mutating func validateTimestamp(_ timestampMilliseconds: UInt64) throws {
        if let lastTimestampMilliseconds, timestampMilliseconds < lastTimestampMilliseconds {
            throw GestureError.timestampRegression
        }
        lastTimestampMilliseconds = timestampMilliseconds
    }

    private mutating func flushExpired(at timestampMilliseconds: UInt64) -> [RoutedGesture] {
        let window = UInt64(policy.doubleClickWindowMilliseconds)
        let expired = pendingClicks.compactMap { key, pending -> (CanonicalKey, UInt64)? in
            guard timestampMilliseconds - pending.timestampMilliseconds > window else { return nil }
            return (key, pending.timestampMilliseconds)
        }.sorted {
            if $0.1 == $1.1 { return $0.0.rawValue < $1.0.rawValue }
            return $0.1 < $1.1
        }
        for (key, _) in expired {
            pendingClicks.removeValue(forKey: key)
        }
        return expired.map { RoutedGesture(key: $0.0, gesture: .single) }
    }
}
