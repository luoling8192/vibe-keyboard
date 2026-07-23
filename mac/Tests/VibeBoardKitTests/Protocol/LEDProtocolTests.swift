import Foundation
import Testing
@testable import VibeBoardKit

struct LEDProtocolTests {
    @Test func decodesUnavailableCapabilityExactly() throws {
        let snapshot = try ReplacementCapabilitySnapshot.decode(capabilities(#"{"version":1,"available":false,"reason":"calibration_required"}"#))
        #expect(snapshot.led == .unavailable(UnavailableFeature(reason: "calibration_required")))
    }

    @Test func decodesAvailableCapabilityAndRejectsDuplicateMapping() throws {
        let available = #"{"version":1,"available":true,"pixel_count":17,"key_pixels":{"k1":0,"k2":1,"k3":2,"k4":3},"strip_first":4,"strip_count":13,"color_model":"rgb8","wire_order":"grb","tick_ms":30,"max_brightness":64,"max_frame_channel_sum":3264}"#
        let snapshot = try ReplacementCapabilitySnapshot.decode(capabilities(available))
        guard case .available(let led)? = snapshot.led else { Issue.record("missing LED"); return }
        #expect(led.maxBrightness == 64)
        let duplicate = available.replacingOccurrences(of: #""k2":1"#, with: #""k2":0"#)
        #expect(throws: ReplacementProtocolError.self) { try ReplacementCapabilitySnapshot.decode(capabilities(duplicate)) }
    }

    @Test func exactStateAndErrorSchemas() throws {
        let state = try LEDProtocolCodec.decode(Data(#"{"event":"vk_led_state","request_id":2,"source":"applied","available":true,"enabled":true,"brightness":32,"effective":"connected"}"#.utf8))
        #expect(state == .state(LEDStateEvent(requestID: 2, source: .applied, available: true, reason: nil, enabled: true, brightness: 32, effective: .connected)))
        let error = try LEDProtocolCodec.decode(Data(#"{"event":"vk_error","operation":"led","request_id":2,"code":"unavailable"}"#.utf8))
        #expect(error == .error(LEDErrorEvent(requestID: 2, code: "unavailable", message: nil)))
        #expect(throws: ReplacementProtocolError.self) {
            try LEDProtocolCodec.decode(Data(#"{"event":"vk_led_state","request_id":2,"source":"applied","available":false,"reason":"calibration_required","brightness":0}"#.utf8))
        }
    }

    private func capabilities(_ led: String) -> Data {
        Data("{\"event\":\"vk_capabilities\",\"protocol\":1,\"display\":{\"width\":428,\"height\":142,\"format\":\"rgb565\"},\"features\":{\"led\":\(led)}}".utf8)
    }
}
