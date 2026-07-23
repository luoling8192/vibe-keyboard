import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Frame decoding")
struct FrameDecoderTests {
    @Test func decodesAllStateCodingKeysFromRealDeviceInfoFixture() throws {
        let json = #"{"event":"device_info","button":"k4","session_id":42,"duration_ms":120,"hardware":"vibe_keyboard","firmware_version":"0.3.8","buttons":["k1","k2","k3","k4"],"ui_states":["ready","recording","thinking","pending_confirmation","error"],"interaction_modes":["hold_to_talk","click_to_talk"],"message":"ok","device_id":"VS-020000000001","provisioned":true}"#
        let event = try FrameDecoder.decodeState(stateFrame(json))
        #expect(event.event == "device_info")
        #expect(event.button == "k4")
        #expect(event.sessionID == 42)
        #expect(event.durationMS == 120)
        #expect(event.hardware == "vibe_keyboard")
        #expect(event.firmwareVersion == "0.3.8")
        #expect(event.buttons == ["k1", "k2", "k3", "k4"])
        #expect(event.uiStates == ["ready", "recording", "thinking", "pending_confirmation", "error"])
        #expect(event.interactionModes == ["hold_to_talk", "click_to_talk"])
        #expect(event.message == "ok")
        #expect(event.deviceID == "VS-020000000001")
        #expect(event.provisioned == true)
        #expect(event.replacementProtocol == nil)
    }

    @Test func decodesReplacementProtocolDiscriminator() throws {
        let event = try FrameDecoder.decodeState(stateFrame(#"{"event":"device_info","hardware":"vibe_keyboard","replacement_protocol":1}"#))
        #expect(event.replacementProtocol == 1)
    }

    @Test func decodesReplacementFirmwareCapabilityGolden() throws {
        let json = #"{"event":"vk_capabilities","protocol":1,"display":{"width":428,"height":142,"format":"rgb565"},"features":{}}"#
        let event = try FrameDecoder.decodeState(stateFrame(json))
        #expect(event.event == "vk_capabilities")
        #expect(event.protocolVersion == 1)
        #expect(event.display == CapabilityDisplay(width: 428, height: 142, format: "rgb565"))
        #expect(event.features == CapabilityFeatures(assets: nil, screen: nil, update: nil))
    }

    @Test func rejectsMalformedReplacementCapabilityTypes() {
        let json = #"{"event":"vk_capabilities","protocol":"1","display":{"width":428,"height":142,"format":"rgb565"},"features":{}}"#
        #expect(throws: ProtocolError.invalidJSON) {
            try FrameDecoder.decodeState(stateFrame(json))
        }
    }

    @Test func stateDecoderUsesDeclaredBodyOnly() throws {
        var frame = stateFrame(#"{"event":"ping"}"#)
        frame.append(contentsOf: Data("not-json".utf8))
        #expect(try FrameDecoder.decodeState(frame).event == "ping")
    }

    @Test func rejectsStateHeaderAndJSONErrors() {
        #expect(throws: ProtocolError.truncatedFrame(required: 4, actual: 3)) {
            try FrameDecoder.decodeState(Data([1, 16, 0]))
        }
        #expect(throws: ProtocolError.unsupportedVersion(2)) {
            try FrameDecoder.decodeState(Data([2, 16, 0, 0]))
        }
        #expect(throws: ProtocolError.unsupportedFrameType(1)) {
            try FrameDecoder.decodeState(Data([1, 1, 0, 0]))
        }
        #expect(throws: ProtocolError.truncatedFrame(required: 8, actual: 4)) {
            try FrameDecoder.decodeState(Data([1, 16, 4, 0]))
        }
        #expect(throws: ProtocolError.invalidUTF8) {
            try FrameDecoder.decodeState(Data([1, 16, 1, 0, 0xff]))
        }
        #expect(throws: ProtocolError.invalidJSON) {
            try FrameDecoder.decodeState(stateFrame("{"))
        }
        #expect(throws: ProtocolError.missingRequiredField("event")) {
            try FrameDecoder.decodeState(stateFrame(#"{"button":"k1"}"#))
        }
    }

    @Test func rejectsOversizedDeclaredStateFrame() {
        #expect(throws: ProtocolError.frameTooLarge(actual: 4097, maximum: 4096)) {
            try FrameDecoder.decodeState(Data([1, 16, 0xfd, 0x0f]))
        }
    }

    @Test func decodesAudioFixedHeaderLittleEndianAndReservedByte() throws {
        let bytes = Data([
            0x01, 0x01, 0x10, 0x00,
            0x78, 0x56, 0x34, 0x12,
            0xef, 0xcd, 0xab, 0x90,
            0x01, 0xa5, 0x03, 0x00,
            0xde, 0xad, 0xbe,
        ])
        let frame = try FrameDecoder.decodeAudio(bytes)
        #expect(frame.session == 0x1234_5678)
        #expect(frame.sequence == 0x90ab_cdef)
        #expect(frame.flags == 0x01)
        #expect(frame.payload == Data([0xde, 0xad, 0xbe]))
        #expect(try FrameDecoder.decode(RawFrame(type: .audio, bytes: bytes)) == .audio(frame))
    }

    @Test func audioDecoderUsesDeclaredPayloadOnly() throws {
        var bytes = Data([
            1, 1, 16, 0,
            1, 0, 0, 0,
            0, 0, 0, 0,
            2, 0xff, 1, 0,
            0xaa,
        ])
        bytes.append(contentsOf: [0xbb, 0xcc])
        #expect(try FrameDecoder.decodeAudio(bytes).payload == Data([0xaa]))
    }

    @Test func rejectsAudioHeaderAndPayloadErrors() {
        #expect(throws: ProtocolError.truncatedFrame(required: 16, actual: 15)) {
            try FrameDecoder.decodeAudio(Data(repeating: 0, count: 15))
        }
        var version = validEmptyAudioFrame
        version[0] = 2
        #expect(throws: ProtocolError.unsupportedVersion(2)) { try FrameDecoder.decodeAudio(version) }
        var type = validEmptyAudioFrame
        type[1] = 16
        #expect(throws: ProtocolError.unsupportedFrameType(16)) { try FrameDecoder.decodeAudio(type) }
        var marker = validEmptyAudioFrame
        marker[2] = 15
        #expect(throws: ProtocolError.invalidAudioHeaderLength(15)) { try FrameDecoder.decodeAudio(marker) }
        var truncated = validEmptyAudioFrame
        truncated[14] = 2
        #expect(throws: ProtocolError.truncatedFrame(required: 18, actual: 16)) {
            try FrameDecoder.decodeAudio(truncated)
        }
        var oversized = validEmptyAudioFrame
        oversized[14] = 0xf1
        oversized[15] = 0x0f
        #expect(throws: ProtocolError.frameTooLarge(actual: 4097, maximum: 4096)) {
            try FrameDecoder.decodeAudio(oversized)
        }
    }

    @Test(arguments: [FrameType.otaBegin, .otaData, .otaFinish, .otaCancel, .otaState])
    func typedDecoderRejectsUnsupportedCompleteTypes(type: FrameType) {
        #expect(throws: ProtocolError.unsupportedFrameType(type.rawValue)) {
            try FrameDecoder.decode(RawFrame(type: type, bytes: Data([1, type.rawValue, 0, 0])))
        }
    }
}

private let validEmptyAudioFrame = Data([
    1, 1, 16, 0,
    0, 0, 0, 0,
    0, 0, 0, 0,
    2, 0, 0, 0,
])

private func stateFrame(_ json: String) -> Data {
    let body = Data(json.utf8)
    precondition(body.count <= Int(UInt16.max))
    return Data([1, 16, UInt8(body.count & 0xff), UInt8((body.count >> 8) & 0xff)]) + body
}
