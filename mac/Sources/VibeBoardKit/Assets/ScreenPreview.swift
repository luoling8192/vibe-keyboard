import Foundation

public struct PreviewRect: Equatable, Sendable {
    public let x: Int
    public let y: Int
    public let width: Int
    public let height: Int
}

public struct PreviewObjectPlacement: Equatable, Sendable {
    public let id: String
    public let rect: PreviewRect
    public let z: Int16
    public let sourceOrder: Int
}

public enum ScreenPreviewError: Error, Equatable, Sendable {
    case invalidGeometry
    case duplicateIdentifier
    case missingWidget
    case invalidWidgetValue
    case staleSequence
}

public enum ScreenLayoutGeometry {
    public static func placements(_ layout: ScreenLayout, displayWidth: Int = 428, displayHeight: Int = 142) throws -> [PreviewObjectPlacement] {
        var output: [PreviewObjectPlacement] = []
        var ids = Set<String>()
        var order = 0
        for root in layout.objects {
            let base = root.node.base
            let rect = PreviewRect(x: Int(root.x), y: Int(root.y), width: Int(base.width), height: Int(base.height))
            guard fits(rect, in: PreviewRect(x: 0, y: 0, width: displayWidth, height: displayHeight)) else { throw ScreenPreviewError.invalidGeometry }
            try append(root.node, rect: rect, ids: &ids, order: &order, output: &output)
        }
        return output.sorted { lhs, rhs in lhs.z == rhs.z ? lhs.sourceOrder < rhs.sourceOrder : lhs.z < rhs.z }
    }

    private static func append(_ node: ScreenObjectNode, rect: PreviewRect, ids: inout Set<String>, order: inout Int, output: inout [PreviewObjectPlacement]) throws {
        let base = node.base
        guard ids.insert(base.id).inserted else { throw ScreenPreviewError.duplicateIdentifier }
        output.append(PreviewObjectPlacement(id: base.id, rect: rect, z: base.z, sourceOrder: order))
        order += 1
        guard case .container(_, let kind, let crossAlign, let gap, let mainAlign, let children) = node else { return }
        let mainAvailable = kind == .row ? rect.width : rect.height
        let used = children.reduce(0) { $0 + Int(kind == .row ? $1.base.width : $1.base.height) } + Int(gap) * max(0, children.count - 1)
        guard used <= mainAvailable else { throw ScreenPreviewError.invalidGeometry }
        var cursor = alignedOffset(available: mainAvailable, used: used, alignment: mainAlign)
        for child in children {
            let childWidth = Int(child.base.width), childHeight = Int(child.base.height)
            let crossAvailable = kind == .row ? rect.height : rect.width
            let crossUsed = kind == .row ? childHeight : childWidth
            guard crossUsed <= crossAvailable else { throw ScreenPreviewError.invalidGeometry }
            let cross = alignedOffset(available: crossAvailable, used: crossUsed, alignment: crossAlign)
            let childRect = kind == .row
                ? PreviewRect(x: rect.x + cursor, y: rect.y + cross, width: childWidth, height: childHeight)
                : PreviewRect(x: rect.x + cross, y: rect.y + cursor, width: childWidth, height: childHeight)
            guard fits(childRect, in: rect) else { throw ScreenPreviewError.invalidGeometry }
            try append(child, rect: childRect, ids: &ids, order: &order, output: &output)
            cursor += (kind == .row ? childWidth : childHeight) + Int(gap)
        }
    }

    private static func alignedOffset(available: Int, used: Int, alignment: ScreenObjectNode.AxisAlign) -> Int {
        switch alignment {
        case .start: 0
        case .center: (available - used) / 2
        case .end: available - used
        }
    }

    private static func fits(_ child: PreviewRect, in parent: PreviewRect) -> Bool {
        child.width > 0 && child.height > 0 && child.x >= parent.x && child.y >= parent.y &&
            child.x + child.width <= parent.x + parent.width && child.y + child.height <= parent.y + parent.height
    }
}

public enum WidgetPresentationState: Equatable, Sendable {
    case fallback(String)
    case fresh(String)
    case error
}

public struct WidgetPreviewState: Equatable, Sendable {
    public let sequence: UInt32
    public let presentation: WidgetPresentationState
}

public enum WidgetPreviewFormatter {
    public static func fallback(_ declaration: ScreenWidgetDeclaration) throws -> String {
        switch declaration {
        case .text(_, _, let value): return value
        case .integer(_, _, let value): return String(value)
        case .number(_, _, let value, _, _, let decimals), .progress(_, _, let value, _, _, let decimals):
            return try format(value, decimals: decimals)
        }
    }

    public static func format(_ value: ScreenCanonicalNumber, decimals: UInt8) throws -> String {
        guard decimals <= 3, value.scale <= 3 else { throw ScreenPreviewError.invalidWidgetValue }
        let targetScale = Int(decimals)
        var coefficient = value.coefficient
        let sourceScale = Int(value.scale)
        if sourceScale > targetScale {
            let divisor = power10(sourceScale - targetScale)
            let quotient = coefficient / divisor
            let remainder = coefficient % divisor
            let magnitude = remainder == Int64.min ? UInt64(Int64.max) + 1 : UInt64(abs(remainder))
            let threshold = UInt64(divisor) / 2
            coefficient = quotient
            if magnitude >= threshold { coefficient += value.coefficient < 0 ? -1 : 1 }
        } else if sourceScale < targetScale {
            let multiplier = power10(targetScale - sourceScale)
            let (result, overflow) = coefficient.multipliedReportingOverflow(by: multiplier)
            guard !overflow else { throw ScreenPreviewError.invalidWidgetValue }
            coefficient = result
        }
        let negative = coefficient < 0
        let magnitude = coefficient == Int64.min ? UInt64(Int64.max) + 1 : UInt64(abs(coefficient))
        var digits = String(magnitude)
        if targetScale > 0 {
            while digits.count <= targetScale { digits.insert("0", at: digits.startIndex) }
            digits.insert(".", at: digits.index(digits.endIndex, offsetBy: -targetScale))
        }
        return (negative && magnitude != 0 ? "-" : "") + digits
    }

    public static func accepts(newSequence: UInt32, after oldSequence: UInt32?) -> Bool {
        guard newSequence != 0 else { return false }
        guard let oldSequence else { return true }
        let delta = newSequence &- oldSequence
        return delta != 0 && delta < 0x8000_0000
    }

    private static func power10(_ exponent: Int) -> Int64 {
        (0..<exponent).reduce(1) { value, _ in value * 10 }
    }
}
