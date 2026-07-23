import Darwin
import Foundation

struct AgentUsageSnapshot: Equatable, Sendable {
    var usedPercent: Int?
    var tokensToday: Int64?
    var status: String
    var error: String?

    static let unavailable = AgentUsageSnapshot(
        usedPercent: nil,
        tokensToday: nil,
        status: "unavailable",
        error: nil
    )
}

struct StockQuote: Equatable, Sendable {
    let symbol: String
    let price: String
    let changePercent: String

    var displayLine: String {
        "\(symbol) \(price) \(changePercent)"
    }
}

enum DashboardModule: String, CaseIterable, Identifiable, Sendable {
    case codex
    case claude
    case system
    case network
    case stocks
    case empty

    var id: String { rawValue }

    var label: String {
        switch self {
        case .codex: "Codex"
        case .claude: "Claude"
        case .system: "CPU & memory"
        case .network: "Network"
        case .stocks: "Stocks"
        case .empty: "Empty"
        }
    }
}

struct DashboardTileContent: Equatable, Sendable {
    let title: String
    let line1: String
    let line2: String
}

struct DashboardPageContent: Equatable, Sendable {
    let index: Int
    let left: DashboardTileContent
    let right: DashboardTileContent
}

struct LiveDashboardSnapshot: Equatable, Sendable {
    var codex: AgentUsageSnapshot
    var claude: AgentUsageSnapshot
    var cpuPercent: Int
    var memoryPercent: Int
    var downloadBytesPerSecond: Double
    var uploadBytesPerSecond: Double
    var stocks: [StockQuote]
    var sampledAt: Date

    static let empty = LiveDashboardSnapshot(
        codex: .unavailable,
        claude: .unavailable,
        cpuPercent: 0,
        memoryPercent: 0,
        downloadBytesPerSecond: 0,
        uploadBytesPerSecond: 0,
        stocks: [],
        sampledAt: .distantPast
    )

    var agentLine: String {
        "CODEX \(Self.usage(codex))  CLAUDE \(Self.usage(claude))"
    }

    var tokenLine: String {
        "TODAY C \(Self.count(codex.tokensToday))  A \(Self.count(claude.tokensToday))"
    }

    var systemLine: String {
        "CPU \(cpuPercent)%  MEM \(memoryPercent)%"
    }

    var networkLine: String {
        "DOWN \(Self.speed(downloadBytesPerSecond))  UP \(Self.speed(uploadBytesPerSecond))"
    }

    var stockLine: String {
        stocks.first.map(\.displayLine) ?? "STOCK --"
    }

    func page(
        modules: [DashboardModule],
        pageIndex: Int,
        stockOffset: Int
    ) -> DashboardPageContent {
        let normalized = Self.normalizedModules(modules)
        let index = pageIndex & 1
        let firstSlot = index * 2
        return DashboardPageContent(
            index: index,
            left: tile(
                module: normalized[firstSlot],
                slot: firstSlot,
                stockOffset: stockOffset
            ),
            right: tile(
                module: normalized[firstSlot + 1],
                slot: firstSlot + 1,
                stockOffset: stockOffset
            )
        )
    }

    static func normalizedModules(_ modules: [DashboardModule]) -> [DashboardModule] {
        let defaults: [DashboardModule] = [.codex, .claude, .system, .stocks]
        return Array((modules + defaults).prefix(4))
    }

    private func tile(
        module: DashboardModule,
        slot: Int,
        stockOffset: Int
    ) -> DashboardTileContent {
        let page = slot < 2 ? "A" : "B"
        let position = slot % 2 + 1
        let prefix = "\(page)\(position)"
        switch module {
        case .codex:
            return .init(
                title: "\(prefix) CODEX",
                line1: usageLine(codex),
                line2: "TODAY \(Self.count(codex.tokensToday))"
            )
        case .claude:
            return .init(
                title: "\(prefix) CLAUDE",
                line1: usageLine(claude),
                line2: "TODAY \(Self.count(claude.tokensToday))"
            )
        case .system:
            return .init(
                title: "\(prefix) SYSTEM",
                line1: "CPU \(cpuPercent)%",
                line2: "MEM \(memoryPercent)%"
            )
        case .network:
            return .init(
                title: "\(prefix) NETWORK",
                line1: "DOWN \(Self.speed(downloadBytesPerSecond))",
                line2: "UP \(Self.speed(uploadBytesPerSecond))"
            )
        case .stocks:
            guard !stocks.isEmpty else {
                return .init(
                    title: "\(prefix) STOCKS",
                    line1: "NO QUOTES",
                    line2: "CHECK SYMBOLS"
                )
            }
            let start = stockOffset % stocks.count
            let second = (start + 1) % stocks.count
            let range = stocks.count == 1
                ? "\(start + 1)/1"
                : "\(start + 1)-\(second + 1)/\(stocks.count)"
            return .init(
                title: "\(prefix) STOCKS \(range)",
                line1: stocks[start].displayLine,
                line2: stocks.count == 1 ? "NEXT UPDATE" : stocks[second].displayLine
            )
        case .empty:
            return .init(title: "\(prefix) EMPTY", line1: "--", line2: "--")
        }
    }

    private func usageLine(_ value: AgentUsageSnapshot) -> String {
        if let percent = value.usedPercent {
            return "LIMIT \(percent)%"
        }
        return "STATE \(value.status.uppercased())"
    }

    private static func usage(_ value: AgentUsageSnapshot) -> String {
        value.usedPercent.map { "\($0)%" } ?? value.status.uppercased()
    }

    static func count(_ value: Int64?) -> String {
        guard let value else { return "--" }
        if value >= 1_000_000_000 {
            return String(format: "%.1fB", Double(value) / 1_000_000_000)
        }
        if value >= 1_000_000 {
            return String(format: "%.1fM", Double(value) / 1_000_000)
        }
        if value >= 1_000 {
            return String(format: "%.0fK", Double(value) / 1_000)
        }
        return String(value)
    }

    static func speed(_ bytesPerSecond: Double) -> String {
        if bytesPerSecond >= 1_000_000 {
            return String(format: "%.1fM/s", bytesPerSecond / 1_000_000)
        }
        if bytesPerSecond >= 1_000 {
            return String(format: "%.0fK/s", bytesPerSecond / 1_000)
        }
        return String(format: "%.0fB/s", bytesPerSecond)
    }
}

protocol LiveDashboardProviding: Sendable {
    func snapshot(stockSymbols: String) async -> LiveDashboardSnapshot
}

struct EmptyLiveDashboardProvider: LiveDashboardProviding {
    func snapshot(stockSymbols: String) async -> LiveDashboardSnapshot {
        .empty
    }
}

actor ProductionLiveDashboardProvider: LiveDashboardProviding {
    private var sampler = SystemMetricSampler()
    private var cachedCodex = AgentUsageSnapshot.unavailable
    private var cachedClaude = AgentUsageSnapshot.unavailable
    private var cachedStocks: [StockQuote] = []
    private var lastUsageRefresh = Date.distantPast
    private var lastStockRefresh = Date.distantPast
    private var cachedStockSymbols = ""

    func snapshot(stockSymbols: String) async -> LiveDashboardSnapshot {
        let now = Date()
        let metrics = sampler.sample(at: now)

        if now.timeIntervalSince(lastUsageRefresh) >= 120 {
            lastUsageRefresh = now
            async let codex = CodexUsageReader.read()
            async let claude = ClaudeUsageReader.read()
            cachedCodex = await codex
            cachedClaude = await claude
        }

        let normalizedSymbols = StockReader.normalizedList(stockSymbols)
        let normalizedKey = normalizedSymbols.joined(separator: ",")
        if normalizedKey != cachedStockSymbols || now.timeIntervalSince(lastStockRefresh) >= 15 {
            cachedStockSymbols = normalizedKey
            lastStockRefresh = now
            cachedStocks = await StockReader.read(symbols: normalizedSymbols)
        }

        return LiveDashboardSnapshot(
            codex: cachedCodex,
            claude: cachedClaude,
            cpuPercent: metrics.cpuPercent,
            memoryPercent: metrics.memoryPercent,
            downloadBytesPerSecond: metrics.downloadBytesPerSecond,
            uploadBytesPerSecond: metrics.uploadBytesPerSecond,
            stocks: cachedStocks,
            sampledAt: now
        )
    }
}

private struct SystemMetricSampler {
    private var lastCPUTicks: (busy: UInt64, idle: UInt64)?
    private var lastNetworkCounters: (received: UInt64, sent: UInt64, at: Date)?

    mutating func sample(at date: Date) -> (
        cpuPercent: Int,
        memoryPercent: Int,
        downloadBytesPerSecond: Double,
        uploadBytesPerSecond: Double
    ) {
        let cpuPercent = sampleCPU()
        let memoryPercent = Self.memoryPercent()
        let network = sampleNetwork(at: date)
        return (cpuPercent, memoryPercent, network.received, network.sent)
    }

    private mutating func sampleCPU() -> Int {
        guard let ticks = Self.cpuTicks() else { return 0 }
        let busy = ticks.user + ticks.system + ticks.nice
        defer { lastCPUTicks = (busy, ticks.idle) }
        guard let previous = lastCPUTicks,
              busy >= previous.busy,
              ticks.idle >= previous.idle else { return 0 }
        let busyDelta = busy - previous.busy
        let idleDelta = ticks.idle - previous.idle
        let total = busyDelta + idleDelta
        guard total > 0 else { return 0 }
        return min(100, max(0, Int((Double(busyDelta) / Double(total) * 100).rounded())))
    }

    private mutating func sampleNetwork(at date: Date) -> (received: Double, sent: Double) {
        let counters = Self.networkCounters()
        defer { lastNetworkCounters = (counters.received, counters.sent, date) }
        guard let previous = lastNetworkCounters,
              counters.received >= previous.received,
              counters.sent >= previous.sent else { return (0, 0) }
        let interval = date.timeIntervalSince(previous.at)
        guard interval > 0 else { return (0, 0) }
        return (
            Double(counters.received - previous.received) / interval,
            Double(counters.sent - previous.sent) / interval
        )
    }

    private static func cpuTicks() -> (user: UInt64, system: UInt64, idle: UInt64, nice: UInt64)? {
        var count = mach_msg_type_number_t(
            MemoryLayout<host_cpu_load_info_data_t>.size / MemoryLayout<integer_t>.size
        )
        var info = host_cpu_load_info_data_t()
        let result = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, $0, &count)
            }
        }
        guard result == KERN_SUCCESS else { return nil }
        return (
            UInt64(info.cpu_ticks.0),
            UInt64(info.cpu_ticks.1),
            UInt64(info.cpu_ticks.2),
            UInt64(info.cpu_ticks.3)
        )
    }

    private static func memoryPercent() -> Int {
        var count = mach_msg_type_number_t(
            MemoryLayout<vm_statistics64_data_t>.size / MemoryLayout<integer_t>.size
        )
        var info = vm_statistics64_data_t()
        let result = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                host_statistics64(mach_host_self(), HOST_VM_INFO64, $0, &count)
            }
        }
        guard result == KERN_SUCCESS else { return 0 }
        let usedPages = UInt64(info.active_count) + UInt64(info.wire_count) +
            UInt64(info.compressor_page_count)
        var pageSize: vm_size_t = 0
        guard host_page_size(mach_host_self(), &pageSize) == KERN_SUCCESS else { return 0 }
        let usedBytes = usedPages * UInt64(pageSize)
        let totalBytes = ProcessInfo.processInfo.physicalMemory
        guard totalBytes > 0 else { return 0 }
        return min(100, max(0, Int((Double(usedBytes) / Double(totalBytes) * 100).rounded())))
    }

    private static func networkCounters() -> (received: UInt64, sent: UInt64) {
        var addresses: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&addresses) == 0, let first = addresses else { return (0, 0) }
        defer { freeifaddrs(addresses) }
        var received: UInt64 = 0
        var sent: UInt64 = 0
        for address in sequence(first: first, next: { $0.pointee.ifa_next }) {
            let value = address.pointee
            guard let socketAddress = value.ifa_addr,
                  socketAddress.pointee.sa_family == UInt8(AF_LINK),
                  String(cString: value.ifa_name).hasPrefix("en"),
                  let rawData = value.ifa_data else { continue }
            let data = rawData.assumingMemoryBound(to: if_data.self).pointee
            received += UInt64(data.ifi_ibytes)
            sent += UInt64(data.ifi_obytes)
        }
        return (received, sent)
    }
}

private enum ClaudeUsageReader {
    static func read() async -> AgentUsageSnapshot {
        await Task.detached(priority: .utility) { scan() }.value
    }

    private static func scan() -> AgentUsageSnapshot {
        let root = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".claude/projects", isDirectory: true)
        let start = Calendar.current.startOfDay(for: Date())
        let now = Date()
        var tokens: Int64 = 0
        var latest = Date.distantPast
        guard let files = FileManager.default.enumerator(
            at: root,
            includingPropertiesForKeys: [.contentModificationDateKey, .isRegularFileKey],
            options: [.skipsHiddenFiles]
        ) else {
            return AgentUsageSnapshot(
                usedPercent: nil,
                tokensToday: nil,
                status: "offline",
                error: "Claude session logs were not found"
            )
        }

        let parser = ISO8601DateFormatter()
        parser.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        for case let url as URL in files where url.pathExtension == "jsonl" {
            guard let values = try? url.resourceValues(
                forKeys: [.contentModificationDateKey, .isRegularFileKey]
            ), values.isRegularFile == true, let modified = values.contentModificationDate else {
                continue
            }
            latest = max(latest, modified)
            guard modified >= start, let data = try? Data(contentsOf: url, options: .mappedIfSafe) else {
                continue
            }
            for line in data.split(separator: 0x0A) where line.contains(Data(#""usage""#.utf8)) {
                guard let object = try? JSONSerialization.jsonObject(with: Data(line)) as? [String: Any],
                      let timestamp = object["timestamp"] as? String,
                      let date = parser.date(from: timestamp),
                      date >= start,
                      let message = object["message"] as? [String: Any],
                      let usage = message["usage"] as? [String: Any] else { continue }
                tokens += tokenValue(usage["input_tokens"])
                tokens += tokenValue(usage["output_tokens"])
                tokens += tokenValue(usage["cache_creation_input_tokens"])
                tokens += tokenValue(usage["cache_read_input_tokens"])
            }
        }

        let age = now.timeIntervalSince(latest)
        let status = age < 30 ? "working" : age < 1_800 ? "idle" : "offline"
        return AgentUsageSnapshot(
            usedPercent: nil,
            tokensToday: tokens,
            status: status,
            error: nil
        )
    }

    private static func tokenValue(_ value: Any?) -> Int64 {
        (value as? NSNumber)?.int64Value ?? 0
    }
}

private enum CodexUsageReader {
    private enum ReaderError: Error {
        case executableNotFound
        case invalidResponse
        case timedOut
    }

    static func read() async -> AgentUsageSnapshot {
        guard let executable = executableURL() else {
            return AgentUsageSnapshot(
                usedPercent: nil,
                tokensToday: nil,
                status: "offline",
                error: "Codex CLI was not found"
            )
        }
        do {
            return try await withThrowingTaskGroup(of: AgentUsageSnapshot.self) { group in
                group.addTask { try await query(executable: executable) }
                group.addTask {
                    try await Task.sleep(for: .seconds(10))
                    throw ReaderError.timedOut
                }
                guard let first = try await group.next() else { throw ReaderError.invalidResponse }
                group.cancelAll()
                return first
            }
        } catch {
            return AgentUsageSnapshot(
                usedPercent: nil,
                tokensToday: nil,
                status: "offline",
                error: "Codex usage read failed"
            )
        }
    }

    private static func query(executable: URL) async throws -> AgentUsageSnapshot {
        let process = Process()
        let input = Pipe()
        let output = Pipe()
        process.executableURL = executable
        process.arguments = ["app-server", "--stdio"]
        process.standardInput = input
        process.standardOutput = output
        process.standardError = FileHandle.nullDevice
        try process.run()
        defer {
            try? input.fileHandleForWriting.close()
            if process.isRunning { process.terminate() }
        }

        try write([
            "id": 1,
            "method": "initialize",
            "params": [
                "clientInfo": [
                    "name": "vibe-keyboard",
                    "title": "Vibe Keyboard",
                    "version": "0.1",
                ],
                "capabilities": NSNull(),
            ],
        ], to: input.fileHandleForWriting)

        var usedPercent: Int?
        var tokensToday: Int64?
        var initialized = false
        var receivedRateLimits = false
        var receivedUsage = false
        for try await line in output.fileHandleForReading.bytes.lines {
            try Task.checkCancellation()
            guard let data = line.data(using: .utf8),
                  let object = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
                continue
            }
            let identifier = (object["id"] as? NSNumber)?.intValue
            if identifier == 1, !initialized {
                initialized = true
                try write(["method": "initialized", "params": [:]], to: input.fileHandleForWriting)
                try write(["id": 2, "method": "account/rateLimits/read"], to: input.fileHandleForWriting)
                try write(["id": 3, "method": "account/usage/read"], to: input.fileHandleForWriting)
                continue
            }
            if identifier == 2,
               let result = object["result"] as? [String: Any],
               let rateLimits = result["rateLimits"] as? [String: Any],
               let primary = rateLimits["primary"] as? [String: Any] {
                usedPercent = (primary["usedPercent"] as? NSNumber)?.intValue
                receivedRateLimits = true
            } else if identifier == 3,
                      let result = object["result"] as? [String: Any],
                      let buckets = result["dailyUsageBuckets"] as? [[String: Any]] {
                let today = Self.dateKey(Date())
                tokensToday = buckets.first {
                    ($0["startDate"] as? String) == today
                }.flatMap { ($0["tokens"] as? NSNumber)?.int64Value }
                receivedUsage = true
            }
            if receivedRateLimits, receivedUsage {
                try? input.fileHandleForWriting.close()
                return AgentUsageSnapshot(
                    usedPercent: usedPercent,
                    tokensToday: tokensToday,
                    status: "ready",
                    error: nil
                )
            }
        }
        throw ReaderError.invalidResponse
    }

    private static func write(_ object: [String: Any], to handle: FileHandle) throws {
        var data = try JSONSerialization.data(withJSONObject: object)
        data.append(0x0A)
        try handle.write(contentsOf: data)
    }

    private static func dateKey(_ date: Date) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.calendar = Calendar(identifier: .gregorian)
        formatter.timeZone = .current
        formatter.dateFormat = "yyyy-MM-dd"
        return formatter.string(from: date)
    }

    private static func executableURL() -> URL? {
        let fileManager = FileManager.default
        let home = fileManager.homeDirectoryForCurrentUser
        var candidates = [
            "/opt/homebrew/bin/codex",
            "/usr/local/bin/codex",
            home.appendingPathComponent(".local/bin/codex").path,
        ]
        let nvmRoot = home.appendingPathComponent(".nvm/versions/node", isDirectory: true)
        if let versions = try? fileManager.contentsOfDirectory(
            at: nvmRoot,
            includingPropertiesForKeys: nil,
            options: [.skipsHiddenFiles]
        ) {
            candidates.append(contentsOf: versions.sorted { $0.lastPathComponent > $1.lastPathComponent }
                .map { $0.appendingPathComponent("bin/codex").path })
        }
        if let path = ProcessInfo.processInfo.environment["PATH"] {
            candidates.append(contentsOf: path.split(separator: ":").map {
                URL(fileURLWithPath: String($0)).appendingPathComponent("codex").path
            })
        }
        guard let path = candidates.first(where: fileManager.isExecutableFile(atPath:)) else {
            return nil
        }
        return URL(fileURLWithPath: path)
    }
}

enum StockReader {
    static func normalizedList(_ raw: String) -> [String] {
        raw.replacingOccurrences(of: "，", with: ",")
            .split(separator: ",")
            .map { normalize(String($0)) }
            .filter { !$0.isEmpty }
            .prefix(12)
            .map(\.self)
    }

    static func normalize(_ raw: String) -> String {
        let trimmed = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed.count > 2 else { return "" }
        let prefix = trimmed.prefix(2).lowercased()
        guard ["sh", "sz", "bj", "hk", "us"].contains(prefix) else { return "" }
        var body = String(trimmed.dropFirst(2)).uppercased()
        guard body.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber || $0 == ".") }) else {
            return ""
        }
        if prefix == "hk", body.allSatisfy(\.isNumber), body.count < 5 {
            body = String(repeating: "0", count: 5 - body.count) + body
        }
        return prefix + body
    }

    static func read(symbols: [String]) async -> [StockQuote] {
        guard !symbols.isEmpty,
              let url = URL(string: "https://qt.gtimg.cn/q=" + symbols.joined(separator: ","))
        else { return [] }
        var request = URLRequest(url: url)
        request.timeoutInterval = 8
        do {
            let (data, response) = try await URLSession.shared.data(for: request)
            guard let http = response as? HTTPURLResponse,
                  (200...299).contains(http.statusCode),
                  data.count <= 1_000_000 else { return [] }
            return parse(data: data, order: symbols)
        } catch {
            return []
        }
    }

    static func parse(data: Data, order: [String]) -> [StockQuote] {
        let text = String(data: data, encoding: .isoLatin1) ?? ""
        var values: [String: StockQuote] = [:]
        for record in text.split(separator: ";") {
            guard let equals = record.firstIndex(of: "="),
                  record.hasPrefix("v_") else { continue }
            let symbolStart = record.index(record.startIndex, offsetBy: 2)
            let symbol = String(record[symbolStart..<equals]).lowercased()
            let fields = record[record.index(after: equals)...]
                .trimmingCharacters(in: CharacterSet(charactersIn: "\"\r\n"))
                .components(separatedBy: "~")
            guard fields.count > 32,
                  let price = Double(fields[3]),
                  let change = Double(fields[32]) else { continue }
            values[symbol] = StockQuote(
                symbol: displaySymbol(symbol),
                price: formatPrice(price),
                changePercent: String(format: "%+.2f%%", change)
            )
        }
        return order.compactMap { values[$0.lowercased()] }
    }

    private static func displaySymbol(_ symbol: String) -> String {
        String(symbol.dropFirst(2)).uppercased()
    }

    private static func formatPrice(_ value: Double) -> String {
        if value >= 10_000 { return String(format: "%.0f", value) }
        if value >= 1_000 { return String(format: "%.1f", value) }
        return String(format: "%.2f", value)
    }
}
