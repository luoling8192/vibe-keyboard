import Testing
@testable import VibeBoardKit

@Suite("Gesture routing")
struct GestureRouterTests {
    private func policy(double: Bool = true, long: Bool = true) throws -> GesturePolicy {
        try GesturePolicy(
            doubleClickWindowMilliseconds: 250,
            longPressThresholdMilliseconds: 700,
            derivesDoubleClick: double,
            derivesLongPress: long
        )
    }

    @Test func firmwareClickIsImmediateWhenDoubleDerivationIsDisabled() throws {
        var router = GestureRouter(policy: try policy(double: false))
        #expect(try router.handle(.click(.k1, durationMilliseconds: 80), at: 100) == [RoutedGesture(key: .k1, gesture: .single)])
    }

    @Test func singleWaitsUntilDoubleWindowExpires() throws {
        var router = GestureRouter(policy: try policy())
        #expect(try router.handle(.click(.k1, durationMilliseconds: nil), at: 100).isEmpty)
        #expect(try router.flush(at: 350).isEmpty)
        #expect(try router.flush(at: 351) == [RoutedGesture(key: .k1, gesture: .single)])
    }

    @Test func twoClicksExecuteOnlyDouble() throws {
        var router = GestureRouter(policy: try policy())
        #expect(try router.handle(.click(.k2, durationMilliseconds: nil), at: 100).isEmpty)
        #expect(try router.handle(.click(.k2, durationMilliseconds: nil), at: 350) == [RoutedGesture(key: .k2, gesture: .double)])
        #expect(try router.flush(at: 1000).isEmpty)
    }

    @Test func longPressSuppressesFollowingFirmwareClick() throws {
        var router = GestureRouter(policy: try policy())
        #expect(try router.handle(.down(.k3), at: 100).isEmpty)
        #expect(try router.handle(.up(.k3, durationMilliseconds: 700), at: 800) == [RoutedGesture(key: .k3, gesture: .long)])
        #expect(try router.handle(.click(.k3, durationMilliseconds: 700), at: 801).isEmpty)
        #expect(try router.flush(at: 2000).isEmpty)
    }

    @Test func productionThresholdBoundaryUsesHostClockAndSuppressesClick() throws {
        let production = try GesturePolicy(
            doubleClickWindowMilliseconds: 300,
            longPressThresholdMilliseconds: 1_500,
            derivesDoubleClick: false,
            derivesLongPress: true
        )
        var below = GestureRouter(policy: production)
        _ = try below.handle(.down(.k1), at: 10_000)
        #expect(try below.handle(.up(.k1, durationMilliseconds: 99_999), at: 11_499).isEmpty)
        #expect(try below.handle(.click(.k1, durationMilliseconds: 99_999), at: 11_500) == [.init(key: .k1, gesture: .single)])

        var exact = GestureRouter(policy: production)
        _ = try exact.handle(.down(.k1), at: 20_000)
        #expect(try exact.handle(.up(.k1, durationMilliseconds: 1), at: 21_500) == [.init(key: .k1, gesture: .long)])
        #expect(try exact.handle(.click(.k1, durationMilliseconds: 1), at: 21_501).isEmpty)
    }

    @Test func thresholdBelowIsNotLong() throws {
        var router = GestureRouter(policy: try policy(double: false))
        _ = try router.handle(.down(.k4), at: 100)
        #expect(try router.handle(.up(.k4, durationMilliseconds: 699), at: 799).isEmpty)
        #expect(try router.handle(.click(.k4, durationMilliseconds: 699), at: 800) == [RoutedGesture(key: .k4, gesture: .single)])
    }

    @Test func duplicateAndOutOfOrderEventsFail() throws {
        var router = GestureRouter(policy: try policy())
        _ = try router.handle(.down(.k1), at: 100)
        #expect(throws: GestureError.duplicateDown(.k1)) { try router.handle(.down(.k1), at: 101) }
        #expect(throws: GestureError.upWithoutDown(.k2)) { try router.handle(.up(.k2, durationMilliseconds: nil), at: 102) }
        #expect(throws: GestureError.timestampRegression) { try router.flush(at: 99) }
    }

    @Test func disconnectCancelsPendingWorkWithoutEmittingAction() throws {
        var router = GestureRouter(policy: try policy())
        _ = try router.handle(.click(.k1, durationMilliseconds: nil), at: 100)
        #expect(try router.handle(.disconnect, at: 150).isEmpty)
        #expect(try router.flush(at: 1000).isEmpty)
        #expect(throws: GestureError.upWithoutDown(.k1)) { try router.handle(.up(.k1, durationMilliseconds: nil), at: 1001) }
    }

    @Test func regressedDisconnectResetsTimestampEpochAndPendingState() throws {
        var router = GestureRouter(policy: try policy())
        _ = try router.handle(.down(.k1), at: 10_000)
        _ = try router.handle(.click(.k2, durationMilliseconds: nil), at: 10_001)

        #expect(try router.handle(.disconnect, at: 0).isEmpty)
        #expect(try router.handle(.down(.k1), at: 0).isEmpty)
        #expect(try router.handle(.up(.k1, durationMilliseconds: nil), at: 10).isEmpty)
        #expect(try router.flush(at: 1_000).isEmpty)
    }

    @Test func hostMeasuredDurationIsAuthoritative() throws {
        var router = GestureRouter(policy: try policy(double: false))
        _ = try router.handle(.down(.k1), at: 100)
        #expect(try router.handle(.up(.k1, durationMilliseconds: 10_000), at: 200).isEmpty)
        #expect(try router.handle(.click(.k1, durationMilliseconds: 10_000), at: 201) == [
            RoutedGesture(key: .k1, gesture: .single)
        ])

        _ = try router.handle(.down(.k2), at: 300)
        #expect(try router.handle(.up(.k2, durationMilliseconds: 1), at: 1_000) == [
            RoutedGesture(key: .k2, gesture: .long)
        ])
    }

    @Test func policyThresholdsMustBeExplicitAndValid() {
        #expect(throws: GestureError.invalidPolicy("doubleClickWindowMilliseconds")) {
            try GesturePolicy(doubleClickWindowMilliseconds: 0, longPressThresholdMilliseconds: 1, derivesDoubleClick: true, derivesLongPress: true)
        }
        #expect(throws: GestureError.invalidPolicy("longPressThresholdMilliseconds")) {
            try GesturePolicy(doubleClickWindowMilliseconds: 1, longPressThresholdMilliseconds: 0, derivesDoubleClick: true, derivesLongPress: true)
        }
    }
}
