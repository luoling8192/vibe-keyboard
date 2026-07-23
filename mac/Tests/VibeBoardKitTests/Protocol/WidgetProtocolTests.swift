import Foundation
import Testing
@testable import VibeBoardKit

@Suite("Widget protocol")
struct WidgetProtocolTests {
    @Test func canonicalTypedCommands() throws {
        let text = try WidgetProtocolCodec.encode(.init(revision: 7, widgetID: "cpu", sequence: 1, state: .freshText("ok")), maximumValueBytes: 8)
        #expect(try body(text) == #"{"event":"vk_widget_update","revision":7,"sequence":1,"state":"fresh","value":"ok","widget_id":"cpu"}"#)
        let number = try WidgetProtocolCodec.encode(.init(revision: 7, widgetID: "cpu", sequence: 2, state: .freshNumber(.init(coefficient: 425, scale: 1))), maximumValueBytes: 8)
        #expect(try body(number) == #"{"event":"vk_widget_update","revision":7,"sequence":2,"state":"fresh","value":42.5,"widget_id":"cpu"}"#)
        #expect(throws: ReplacementProtocolError.limitExceeded("max_widget_value_bytes")) {
            try WidgetProtocolCodec.encode(.init(revision: 7, widgetID: "cpu", sequence: 3, state: .freshText("too long")), maximumValueBytes: 2)
        }
    }

    @Test func exactAppliedAndErrorEvents() throws {
        #expect(try WidgetProtocolCodec.decode(Data(#"{"event":"vk_widget_applied","revision":7,"sequence":19,"state":"fresh","widget_id":"cpu"}"#.utf8)) ==
            .applied(.init(revision: 7, widgetID: "cpu", sequence: 19, state: .fresh)))
        #expect(try WidgetProtocolCodec.decode(Data(#"{"code":"stale_sequence","event":"vk_error","operation":"widget","sequence":19,"widget_id":"cpu"}"#.utf8)) ==
            .error(.init(code: "stale_sequence", widgetID: "cpu", sequence: 19, message: nil)))
        #expect(throws: ReplacementProtocolError.invalidKeys(context: "vk_error.widget")) {
            try WidgetProtocolCodec.decode(Data(#"{"code":"stale_sequence","event":"vk_error","operation":"widget","rogue":1}"#.utf8))
        }
    }

    private func body(_ frame: Data) throws -> String {
        String(decoding: try FrameDecoder.decodeStateBody(frame), as: UTF8.self)
    }
}
