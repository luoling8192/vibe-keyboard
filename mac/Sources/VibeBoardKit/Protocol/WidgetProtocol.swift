import Foundation

public enum WidgetUpdateState: Equatable, Sendable {
    case freshText(String)
    case freshNumber(ScreenCanonicalNumber)
    case stale
    case error(message: String?)
}

public struct WidgetUpdateCommand: Equatable, Sendable {
    public let revision: UInt32
    public let widgetID: String
    public let sequence: UInt32
    public let state: WidgetUpdateState

    public init(revision: UInt32, widgetID: String, sequence: UInt32, state: WidgetUpdateState) {
        self.revision = revision
        self.widgetID = widgetID
        self.sequence = sequence
        self.state = state
    }
}

public enum WidgetAppliedState: String, Equatable, Sendable {
    case fresh
    case stale
    case error
}

public struct WidgetAppliedEvent: Equatable, Sendable {
    public let revision: UInt32
    public let widgetID: String
    public let sequence: UInt32
    public let state: WidgetAppliedState
}

public struct WidgetErrorEvent: Equatable, Sendable {
    public let code: String
    public let widgetID: String?
    public let sequence: UInt32?
    public let message: String?
}

public enum WidgetProtocolEvent: Equatable, Sendable {
    case applied(WidgetAppliedEvent)
    case error(WidgetErrorEvent)
}

enum WidgetProtocolCodec {
    static func encode(_ command: WidgetUpdateCommand, maximumValueBytes: UInt16) throws -> Data {
        guard command.revision != 0, command.sequence != 0, BoundedJSON.validIdentifier(command.widgetID) else {
            throw ReplacementProtocolError.invalidValue(field: "widget_update")
        }
        let suffix: String
        switch command.state {
        case .freshText(let value):
            guard value.utf8.count <= Int(maximumValueBytes) else {
                throw ReplacementProtocolError.limitExceeded("max_widget_value_bytes")
            }
            suffix = ",\"state\":\"fresh\",\"value\":\(try quote(value))"
        case .freshNumber(let value):
            guard let canonical = value.canonical else {
                throw ReplacementProtocolError.invalidValue(field: "widget.value")
            }
            suffix = ",\"state\":\"fresh\",\"value\":\(canonical)"
        case .stale:
            suffix = ",\"state\":\"stale\""
        case .error(let message):
            if let message {
                guard message.utf8.count <= 96 else { throw ReplacementProtocolError.limitExceeded("message") }
                suffix = ",\"message\":\(try quote(message)),\"state\":\"error\""
            } else {
                suffix = ",\"state\":\"error\""
            }
        }
        let body = "{\"event\":\"vk_widget_update\",\"revision\":\(command.revision),\"sequence\":\(command.sequence)\(suffix),\"widget_id\":\(try quote(command.widgetID))}"
        return try FrameEncoder.encodeOrdinary(type: .state, body: Data(body.utf8))
    }

    static func decode(_ data: Data) throws -> WidgetProtocolEvent {
        let object = try BoundedJSON.object(data)
        guard let event = BoundedJSON.string(object["event"]) else {
            throw ReplacementProtocolError.invalidValue(field: "event")
        }
        if event == "vk_widget_applied" {
            try BoundedJSON.exactKeys(object, ["event", "revision", "sequence", "state", "widget_id"], event)
            guard let revision = BoundedJSON.positive(object["revision"], as: UInt32.self),
                  let sequence = BoundedJSON.positive(object["sequence"], as: UInt32.self),
                  let widgetID = BoundedJSON.string(object["widget_id"]), BoundedJSON.validIdentifier(widgetID),
                  let rawState = BoundedJSON.string(object["state"]), let state = WidgetAppliedState(rawValue: rawState)
            else { throw ReplacementProtocolError.invalidValue(field: event) }
            return .applied(.init(revision: revision, widgetID: widgetID, sequence: sequence, state: state))
        }
        guard event == "vk_error", BoundedJSON.string(object["operation"]) == "widget",
              let code = BoundedJSON.string(object["code"]),
              ["not_configured", "wrong_revision", "not_found", "stale_sequence", "type_mismatch", "out_of_range", "too_large", "invalid_state", "internal"].contains(code)
        else { throw ReplacementProtocolError.invalidValue(field: "vk_error.widget") }
        let allowed: Set<String> = ["code", "event", "message", "operation", "sequence", "widget_id"]
        let required: Set<String> = ["code", "event", "operation"]
        guard required.isSubset(of: Set(object.keys)), Set(object.keys).isSubset(of: allowed) else {
            throw ReplacementProtocolError.invalidKeys(context: "vk_error.widget")
        }
        let widgetID = BoundedJSON.string(object["widget_id"])
        guard object["widget_id"] == nil || (widgetID != nil && BoundedJSON.validIdentifier(widgetID!)) else {
            throw ReplacementProtocolError.invalidValue(field: "widget_id")
        }
        let sequence: UInt32?
        if object["sequence"] == nil { sequence = nil }
        else {
            guard let value = BoundedJSON.positive(object["sequence"], as: UInt32.self) else {
                throw ReplacementProtocolError.invalidValue(field: "sequence")
            }
            sequence = value
        }
        let message = BoundedJSON.string(object["message"])
        guard object["message"] == nil || (message != nil && message!.utf8.count <= 96) else {
            throw ReplacementProtocolError.invalidValue(field: "message")
        }
        return .error(.init(code: code, widgetID: widgetID, sequence: sequence, message: message))
    }

    private static func quote(_ value: String) throws -> String {
        let data = try JSONSerialization.data(withJSONObject: value, options: [.fragmentsAllowed, .withoutEscapingSlashes])
        return String(decoding: data, as: UTF8.self)
    }
}
