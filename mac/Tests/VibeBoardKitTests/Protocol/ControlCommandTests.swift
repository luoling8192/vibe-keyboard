import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Control command encoding")
struct ControlCommandTests {
    @Test(arguments: commandGoldenVectors)
    func encodesVerifiedCommands(command: ControlCommand, expectedHex: String) throws {
        #expect(try FrameEncoder.encode(command).hexString == expectedHex)
    }

    @Test func escapesDynamicUITextAsJSONContent() throws {
        let frame = try FrameEncoder.encode(.uiState(.processing, text: "line\n\"quoted\"\\\u{0001}"))
        #expect(frame.hexString == "01104b007b226576656e74223a2275695f7374617465222c227374617465223a2270726f63657373696e67222c2274657874223a226c696e655c6e5c2271756f7465645c225c5c5c7530303031227d")
        let event = try FrameDecoder.decodeState(frame)
        #expect(event.event == "ui_state")
    }

    @Test func ordinaryFrameUsesBodyLengthAndAcceptsMaximumTotalLength() throws {
        let body = Data(repeating: 0x61, count: 4092)
        let frame = try FrameEncoder.encodeOrdinary(type: .state, body: body)
        #expect(frame.count == 4096)
        #expect(Array(frame.prefix(4)) == [0x01, 0x10, 0xfc, 0x0f])
        #expect(frame.dropFirst(4) == body)
    }

    @Test func rejectsFrameBeyondMaximumTotalLength() {
        #expect(throws: ProtocolError.frameTooLarge(actual: 4097, maximum: 4096)) {
            try FrameEncoder.encodeOrdinary(type: .state, body: Data(repeating: 0, count: 4093))
        }
    }

    @Test func rejectsBodyBeyondUInt16() {
        #expect(throws: ProtocolError.bodyLengthOutOfRange(65_536)) {
            try FrameEncoder.encodeOrdinary(type: .state, body: Data(repeating: 0, count: 65_536))
        }
    }

    @Test(arguments: FrameType.allCases.filter { $0 != .state })
    func ordinaryEncoderRejectsEveryNonStateType(type: FrameType) {
        #expect(throws: ProtocolError.unsupportedFrameType(type.rawValue)) {
            try FrameEncoder.encodeOrdinary(type: type, body: Data())
        }
    }
}

private let commandGoldenVectors: [(ControlCommand, String)] = [
    (.announceUSBTransport, "011022007b226576656e74223a227472616e73706f7274222c226b696e64223a22757362227d"),
    (.getDeviceInfo, "01101b007b226576656e74223a226765745f6465766963655f696e666f227d"),
    (.uiState(.ready, text: ""), "01102e007b226576656e74223a2275695f7374617465222c227374617465223a227265616479222c2274657874223a22227d"),
    (.uiState(.thinking, text: ""), "011031007b226576656e74223a2275695f7374617465222c227374617465223a227468696e6b696e67222c2274657874223a22227d"),
    (.uiState(.listening, text: ""), "011032007b226576656e74223a2275695f7374617465222c227374617465223a226c697374656e696e67222c2274657874223a22227d"),
    (.uiState(.processing, text: ""), "011033007b226576656e74223a2275695f7374617465222c227374617465223a2270726f63657373696e67222c2274657874223a22227d"),
    (.uiState(.error, text: ""), "01102e007b226576656e74223a2275695f7374617465222c227374617465223a226572726f72222c2274657874223a22227d"),
    (.ping, "011010007b226576656e74223a2270696e67227d"),
    (.interactionMode(.holdToTalk), "011032007b226576656e74223a22696e746572616374696f6e5f6d6f6465222c226d6f6465223a22686f6c645f746f5f74616c6b227d"),
    (.interactionMode(.clickToTalk), "011033007b226576656e74223a22696e746572616374696f6e5f6d6f6465222c226d6f6465223a22636c69636b5f746f5f74616c6b227d"),
    (.voiceKey(.none), "011022007b226576656e74223a22766f6963655f6b6579222c226b6579223a226e6f6e65227d"),
    (.voiceKey(.k1), "011020007b226576656e74223a22766f6963655f6b6579222c226b6579223a226b31227d"),
    (.voiceKey(.k2), "011020007b226576656e74223a22766f6963655f6b6579222c226b6579223a226b32227d"),
    (.voiceKey(.k3), "011020007b226576656e74223a22766f6963655f6b6579222c226b6579223a226b33227d"),
    (.voiceKey(.k4), "011020007b226576656e74223a22766f6963655f6b6579222c226b6579223a226b34227d"),
]

private extension Data {
    var hexString: String { map { String(format: "%02x", $0) }.joined() }
}
