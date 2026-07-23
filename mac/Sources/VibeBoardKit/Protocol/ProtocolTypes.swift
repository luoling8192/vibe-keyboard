import Foundation

public enum FrameType: UInt8, CaseIterable, Sendable {
    case audio = 0x01
    case state = 0x10
    case otaBegin = 0x20
    case otaData = 0x21
    case otaFinish = 0x22
    case otaCancel = 0x23
    case otaState = 0x30
}

public struct RawFrame: Equatable, Sendable {
    public let type: FrameType
    public let bytes: Data

    public init(type: FrameType, bytes: Data) {
        self.type = type
        self.bytes = bytes
    }
}

public struct AudioFrame: Equatable, Sendable {
    public let session: UInt32
    public let sequence: UInt32
    public let flags: UInt8
    public let payload: Data

    public init(session: UInt32, sequence: UInt32, flags: UInt8, payload: Data) {
        self.session = session
        self.sequence = sequence
        self.flags = flags
        self.payload = payload
    }
}

public struct CapabilityDisplay: Decodable, Equatable, Sendable {
    public let width: UInt16
    public let height: UInt16
    public let format: String

    public init(width: UInt16, height: UInt16, format: String) {
        self.width = width
        self.height = height
        self.format = format
    }
}

public struct CapabilityFeature: Decodable, Equatable, Sendable {
    public let version: UInt16
    public let available: Bool
    public let reason: String?
}

public struct CapabilityFeatures: Decodable, Equatable, Sendable {
    public let assets: CapabilityFeature?
    public let screen: CapabilityFeature?
    public let update: CapabilityFeature?
}

public struct StateEvent: Decodable, Equatable, Sendable {
    public let event: String
    public let button: String?
    public let sessionID: UInt32?
    public let durationMS: UInt32?
    public let hardware: String?
    public let firmwareVersion: String?
    public let buttons: [String]?
    public let uiStates: [String]?
    public let interactionModes: [String]?
    public let message: String?
    public let operation: String?
    public let code: String?
    public let deviceID: String?
    public let provisioned: Bool?
    public let replacementProtocol: UInt16?
    public let protocolVersion: UInt16?
    public let display: CapabilityDisplay?
    public let features: CapabilityFeatures?

    enum CodingKeys: String, CodingKey {
        case event
        case button
        case sessionID = "session_id"
        case durationMS = "duration_ms"
        case hardware
        case firmwareVersion = "firmware_version"
        case buttons
        case uiStates = "ui_states"
        case interactionModes = "interaction_modes"
        case message
        case operation
        case code
        case deviceID = "device_id"
        case provisioned
        case replacementProtocol = "replacement_protocol"
        case protocolVersion = "protocol"
        case display
        case features
    }
}

public enum ParsedFrame: Equatable, Sendable {
    case audio(AudioFrame)
    case state(StateEvent)
}

public enum ProtocolError: Error, Equatable, Sendable {
    case unsupportedVersion(UInt8)
    case unsupportedFrameType(UInt8)
    case frameTooLarge(actual: Int, maximum: Int)
    case receiveBufferLimitExceeded(limit: Int, attempted: Int)
    case invalidAudioHeaderLength(UInt16)
    case truncatedFrame(required: Int, actual: Int)
    case invalidUTF8
    case invalidJSON
    case missingRequiredField(String)
    case bodyLengthOutOfRange(Int)
    case invalidCommandValue(field: String, value: String)
}

public enum FrameDiscardReason: Equatable, Sendable {
    case invalidVersion(UInt8)
    case unknownType(UInt8)
    case frameTooLarge(Int)
}

public enum FrameStreamEvent: Equatable, Sendable {
    case frame(RawFrame)
    case discardedByte(UInt8, reason: FrameDiscardReason)
}
