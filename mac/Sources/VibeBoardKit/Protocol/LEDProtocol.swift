import Foundation

public enum LEDEffectiveState: String, Equatable, Sendable {
    case off
    case connected
    case recording
    case mutation
}

public enum LEDStateSource: String, Equatable, Sendable {
    case query
    case applied
}

public struct LEDStateEvent: Equatable, Sendable {
    public let requestID: UInt32
    public let source: LEDStateSource
    public let available: Bool
    public let reason: String?
    public let enabled: Bool?
    public let brightness: UInt8?
    public let effective: LEDEffectiveState?
}

public struct LEDErrorEvent: Equatable, Sendable {
    public let requestID: UInt32?
    public let code: String
    public let message: String?
}

public enum LEDProtocolEvent: Equatable, Sendable {
    case state(LEDStateEvent)
    case error(LEDErrorEvent)
}

public enum LEDCommand: Equatable, Sendable {
    case query(requestID: UInt32)
    case config(requestID: UInt32, enabled: Bool, brightness: UInt8)
}

public enum LEDProtocolCodec {
    public static func encode(_ command: LEDCommand) throws -> Data {
        let body: String
        switch command {
        case .query(let requestID):
            guard requestID != 0 else { throw ReplacementProtocolError.invalidValue(field: "request_id") }
            body = "{\"event\":\"vk_led_query\",\"request_id\":\(requestID)}"
        case .config(let requestID, let enabled, let brightness):
            guard requestID != 0 else { throw ReplacementProtocolError.invalidValue(field: "request_id") }
            body = "{\"brightness\":\(brightness),\"enabled\":\(enabled ? "true" : "false"),\"event\":\"vk_led_config\",\"request_id\":\(requestID)}"
        }
        return try FrameEncoder.encodeOrdinary(type: .state, body: Data(body.utf8))
    }

    public static func decode(_ data: Data) throws -> LEDProtocolEvent {
        let object = try BoundedJSON.object(data)
        guard let event = BoundedJSON.string(object["event"]) else {
            throw ReplacementProtocolError.invalidValue(field: "event")
        }
        if event == "vk_led_state" { return .state(try decodeState(object)) }
        if event == "vk_error" { return .error(try decodeError(object)) }
        throw ReplacementProtocolError.invalidValue(field: "event")
    }

    private static func decodeState(_ object: [String: Any]) throws -> LEDStateEvent {
        guard let available = BoundedJSON.bool(object["available"]),
              let sourceRaw = BoundedJSON.string(object["source"]),
              let source = LEDStateSource(rawValue: sourceRaw),
              let requestID = BoundedJSON.uint(object["request_id"], as: UInt32.self), requestID != 0
        else { throw ReplacementProtocolError.invalidValue(field: "vk_led_state") }
        if !available {
            try BoundedJSON.exactKeys(object, ["available", "event", "reason", "request_id", "source"], "vk_led_state unavailable")
            guard let reason = BoundedJSON.string(object["reason"]),
                  ["calibration_required", "hardware_failed", "tainted"].contains(reason)
            else { throw ReplacementProtocolError.invalidValue(field: "reason") }
            return LEDStateEvent(requestID: requestID, source: source, available: false, reason: reason, enabled: nil, brightness: nil, effective: nil)
        }
        try BoundedJSON.exactKeys(object, ["available", "brightness", "effective", "enabled", "event", "request_id", "source"], "vk_led_state")
        guard let enabled = BoundedJSON.bool(object["enabled"]),
              let brightness = BoundedJSON.uint(object["brightness"], as: UInt8.self),
              let effectiveRaw = BoundedJSON.string(object["effective"]),
              let effective = LEDEffectiveState(rawValue: effectiveRaw),
              (enabled && brightness != 0) || effective == .off
        else { throw ReplacementProtocolError.invalidValue(field: "vk_led_state") }
        return LEDStateEvent(requestID: requestID, source: source, available: true, reason: nil, enabled: enabled, brightness: brightness, effective: effective)
    }

    private static func decodeError(_ object: [String: Any]) throws -> LEDErrorEvent {
        guard BoundedJSON.string(object["operation"]) == "led",
              let code = BoundedJSON.string(object["code"]),
              ["invalid_request", "wrong_epoch", "unavailable", "busy", "queue_overflow", "hardware_failed", "tainted"].contains(code)
        else { throw ReplacementProtocolError.invalidValue(field: "vk_error.led") }
        let allowed: Set<String> = ["code", "event", "message", "operation", "request_id"]
        guard Set(object.keys).isSubset(of: allowed), Set(["code", "event", "operation"]).isSubset(of: Set(object.keys)) else {
            throw ReplacementProtocolError.invalidKeys(context: "vk_error.led")
        }
        let requestID: UInt32?
        if object["request_id"] == nil { requestID = nil }
        else {
            guard let value = BoundedJSON.uint(object["request_id"], as: UInt32.self), value != 0 else {
                throw ReplacementProtocolError.invalidValue(field: "request_id")
            }
            requestID = value
        }
        let message = BoundedJSON.string(object["message"])
        guard object["message"] == nil || (message != nil && message!.utf8.count <= 96) else {
            throw ReplacementProtocolError.invalidValue(field: "message")
        }
        return LEDErrorEvent(requestID: requestID, code: code, message: message)
    }
}

public enum LEDServiceError: Error, Equatable, Sendable {
    case unavailable(String)
    case busy
    case timedOut
    case rejected(String)
    case staleEpoch
}
