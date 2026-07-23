import Foundation

public enum FrameDecoder {
    public static func decode(_ frame: RawFrame) throws -> ParsedFrame {
        switch frame.type {
        case .audio:
            return .audio(try decodeAudio(frame.bytes))
        case .state:
            return .state(try decodeState(frame.bytes))
        case .otaBegin, .otaData, .otaFinish, .otaCancel, .otaState:
            throw ProtocolError.unsupportedFrameType(frame.type.rawValue)
        }
    }

    public static func decodeState(_ data: Data) throws -> StateEvent {
        let json = try decodeStateBody(data)
        do {
            return try JSONDecoder().decode(StateEvent.self, from: json)
        } catch DecodingError.keyNotFound(let key, _) where key.stringValue == "event" {
            throw ProtocolError.missingRequiredField("event")
        } catch {
            throw ProtocolError.invalidJSON
        }
    }

    public static func decodeStateBody(_ data: Data) throws -> Data {
        let bytes = [UInt8](data)
        guard bytes.count >= 4 else {
            throw ProtocolError.truncatedFrame(required: 4, actual: bytes.count)
        }
        guard bytes[0] == FrameStreamParser.protocolVersion else {
            throw ProtocolError.unsupportedVersion(bytes[0])
        }
        guard bytes[1] == FrameType.state.rawValue else {
            throw ProtocolError.unsupportedFrameType(bytes[1])
        }

        let bodyLength = Int(readUInt16LE(bytes, at: 2))
        let totalLength = 4 + bodyLength
        guard totalLength <= FrameStreamParser.maximumFrameLength else {
            throw ProtocolError.frameTooLarge(actual: totalLength, maximum: FrameStreamParser.maximumFrameLength)
        }
        guard bytes.count >= totalLength else {
            throw ProtocolError.truncatedFrame(required: totalLength, actual: bytes.count)
        }
        let body = data.subdata(in: 4..<totalLength)
        guard String(data: body, encoding: .utf8) != nil else { throw ProtocolError.invalidUTF8 }
        return body
    }

    public static func decodeAudio(_ data: Data) throws -> AudioFrame {
        let bytes = [UInt8](data)
        guard bytes.count >= 16 else {
            throw ProtocolError.truncatedFrame(required: 16, actual: bytes.count)
        }
        guard bytes[0] == FrameStreamParser.protocolVersion else {
            throw ProtocolError.unsupportedVersion(bytes[0])
        }
        guard bytes[1] == FrameType.audio.rawValue else {
            throw ProtocolError.unsupportedFrameType(bytes[1])
        }

        let headerLength = readUInt16LE(bytes, at: 2)
        guard headerLength == 16 else {
            throw ProtocolError.invalidAudioHeaderLength(headerLength)
        }

        let payloadLength = Int(readUInt16LE(bytes, at: 14))
        let totalLength = 16 + payloadLength
        guard totalLength <= FrameStreamParser.maximumFrameLength else {
            throw ProtocolError.frameTooLarge(
                actual: totalLength,
                maximum: FrameStreamParser.maximumFrameLength
            )
        }
        guard bytes.count >= totalLength else {
            throw ProtocolError.truncatedFrame(required: totalLength, actual: bytes.count)
        }

        return AudioFrame(
            session: readUInt32LE(bytes, at: 4),
            sequence: readUInt32LE(bytes, at: 8),
            flags: bytes[12],
            payload: data.subdata(in: 16..<totalLength)
        )
    }
}
