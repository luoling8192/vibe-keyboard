import Foundation

public enum VoiceKey: String, CaseIterable, Sendable {
    case none
    case k1
    case k2
    case k3
    case k4
}

public enum InteractionMode: String, CaseIterable, Sendable {
    case holdToTalk = "hold_to_talk"
    case clickToTalk = "click_to_talk"
}

public enum DeviceUIState: String, CaseIterable, Sendable {
    case ready
    case thinking
    case listening
    case processing
    case error
}

public enum ControlCommand: Equatable, Sendable {
    case announceUSBTransport
    case getDeviceInfo
    case uiState(DeviceUIState, text: String)
    case ping
    case interactionMode(InteractionMode)
    case voiceKey(VoiceKey)
}

public enum FrameEncoder {
    public static func encode(_ command: ControlCommand) throws -> Data {
        let body: Data

        switch command {
        case .announceUSBTransport:
            body = Data(#"{"event":"transport","kind":"usb"}"#.utf8)
        case .getDeviceInfo:
            body = Data(#"{"event":"get_device_info"}"#.utf8)
        case .uiState(let state, let text):
            body = Data(
                (#"{"event":"ui_state","state":""#
                    + state.rawValue
                    + #"","text":""#
                    + jsonEscapedContent(text)
                    + #""}"#).utf8
            )
        case .ping:
            body = Data(#"{"event":"ping"}"#.utf8)
        case .interactionMode(let mode):
            body = Data(
                (#"{"event":"interaction_mode","mode":""#
                    + mode.rawValue
                    + #""}"#).utf8
            )
        case .voiceKey(let key):
            body = Data(
                (#"{"event":"voice_key","key":""#
                    + key.rawValue
                    + #""}"#).utf8
            )
        }

        return try encodeOrdinary(type: .state, body: body)
    }

    public static func encodeOrdinary(type: FrameType, body: Data) throws -> Data {
        guard type == .state else {
            throw ProtocolError.unsupportedFrameType(type.rawValue)
        }
        guard body.count <= Int(UInt16.max) else {
            throw ProtocolError.bodyLengthOutOfRange(body.count)
        }

        let totalLength = 4 + body.count
        guard totalLength <= FrameStreamParser.maximumFrameLength else {
            throw ProtocolError.frameTooLarge(
                actual: totalLength,
                maximum: FrameStreamParser.maximumFrameLength
            )
        }

        let length = UInt16(body.count)
        var frame = Data([
            FrameStreamParser.protocolVersion,
            type.rawValue,
            UInt8(truncatingIfNeeded: length),
            UInt8(truncatingIfNeeded: length >> 8)
        ])
        frame.append(body)
        return frame
    }

    private static func jsonEscapedContent(_ value: String) -> String {
        var result = ""
        result.reserveCapacity(value.utf8.count)

        for scalar in value.unicodeScalars {
            switch scalar.value {
            case 0x22:
                result += #"\""#
            case 0x5c:
                result += #"\\"#
            case 0x08:
                result += #"\b"#
            case 0x0c:
                result += #"\f"#
            case 0x0a:
                result += #"\n"#
            case 0x0d:
                result += #"\r"#
            case 0x09:
                result += #"\t"#
            case 0x00...0x1f:
                result += String(format: "\\u%04x", scalar.value)
            default:
                result.unicodeScalars.append(scalar)
            }
        }

        return result
    }
}
