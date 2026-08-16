import AppKit
import SwiftUI

enum HeldKeyCapture {
    private static let namedKeys: [UInt16: String] = [
        36: "return", 48: "tab", 49: "space", 51: "delete", 53: "esc", 63: "fn",
        114: "help", 115: "home", 116: "pageup", 117: "forwarddelete", 119: "end", 121: "pagedown",
        123: "left", 124: "right", 125: "down", 126: "up",
        122: "f1", 120: "f2", 99: "f3", 118: "f4", 96: "f5", 97: "f6",
        98: "f7", 100: "f8", 101: "f9", 109: "f10", 103: "f11", 111: "f12",
        105: "f13", 107: "f14", 113: "f15", 106: "f16", 64: "f17", 79: "f18",
        80: "f19", 90: "f20",
        54: "right_command", 55: "left_command", 56: "left_shift", 60: "right_shift",
        58: "left_option", 61: "right_option", 59: "left_control", 62: "right_control",
    ]

    private static let modifierFlags: [UInt16: NSEvent.ModifierFlags] = [
        54: .command, 55: .command, 56: .shift, 60: .shift,
        58: .option, 61: .option, 59: .control, 62: .control, 63: .function,
    ]

    static func keyName(keyCode: UInt16, charactersIgnoringModifiers: String?) -> String? {
        if let named = namedKeys[keyCode] { return named }
        guard let text = charactersIgnoringModifiers?.lowercased(),
              text.unicodeScalars.count == 1,
              ProductionHostActionAdapter.supportsHeldKey(text) else {
            return nil
        }
        return text
    }

    static func isPressedModifier(keyCode: UInt16, flags: NSEvent.ModifierFlags) -> Bool {
        guard let flag = modifierFlags[keyCode] else { return false }
        return flags.contains(flag)
    }
}

struct HeldKeyCaptureButton: NSViewRepresentable {
    let key: String
    @Binding var isCapturing: Bool
    let onCapture: (String) -> Void

    func makeNSView(context: Context) -> KeyCaptureButton {
        let button = KeyCaptureButton()
        button.onCapturingChanged = { isCapturing = $0 }
        button.onCapture = onCapture
        button.configure(key: key, isCapturing: isCapturing)
        return button
    }

    func updateNSView(_ button: KeyCaptureButton, context: Context) {
        button.onCapturingChanged = { isCapturing = $0 }
        button.onCapture = onCapture
        button.configure(key: key, isCapturing: isCapturing)
    }
}

final class KeyCaptureButton: NSButton {
    var onCapturingChanged: ((Bool) -> Void)?
    var onCapture: ((String) -> Void)?
    private var isCapturing = false
    private var capturedKey = ""

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        bezelStyle = .rounded
        setButtonType(.momentaryPushIn)
        focusRingType = .default
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        bezelStyle = .rounded
        setButtonType(.momentaryPushIn)
        focusRingType = .default
    }

    override var acceptsFirstResponder: Bool { true }

    func configure(key: String, isCapturing: Bool) {
        capturedKey = key
        if self.isCapturing != isCapturing {
            self.isCapturing = isCapturing
            if isCapturing {
                DispatchQueue.main.async { [weak self] in
                    guard let self else { return }
                    self.window?.makeFirstResponder(self)
                }
            }
        }
        title = isCapturing ? "Press a key…" : "Capture key: \(key)"
    }

    override func mouseDown(with event: NSEvent) {
        beginCapture()
    }

    override func keyDown(with event: NSEvent) {
        guard isCapturing else {
            super.keyDown(with: event)
            return
        }
        if let key = HeldKeyCapture.keyName(
            keyCode: event.keyCode,
            charactersIgnoringModifiers: event.charactersIgnoringModifiers
        ) {
            finishCapture(key)
        }
    }

    override func keyUp(with event: NSEvent) {
        if !isCapturing { super.keyUp(with: event) }
    }

    override func flagsChanged(with event: NSEvent) {
        guard isCapturing else {
            super.flagsChanged(with: event)
            return
        }
        guard HeldKeyCapture.isPressedModifier(keyCode: event.keyCode, flags: event.modifierFlags),
              let key = HeldKeyCapture.keyName(keyCode: event.keyCode, charactersIgnoringModifiers: nil) else {
            return
        }
        finishCapture(key)
    }

    private func beginCapture() {
        isCapturing = true
        title = "Press a key…"
        window?.makeFirstResponder(self)
        onCapturingChanged?(true)
    }

    private func finishCapture(_ key: String) {
        isCapturing = false
        capturedKey = key
        title = "Capture key: \(key)"
        onCapturingChanged?(false)
        onCapture?(key)
    }
}
