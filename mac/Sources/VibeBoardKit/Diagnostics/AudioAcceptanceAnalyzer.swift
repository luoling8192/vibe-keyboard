import Foundation

public struct AudioAcceptanceResult: Equatable, Sendable {
    public let codecName: String
    public let decoderSampleRate: Int
    public let channels: Int
    public let durationSeconds: Double
    public let originalInputRate: UInt32
    public let rmsDecibels: Double
    public let peakDecibels: Double

    public init(
        codecName: String,
        decoderSampleRate: Int,
        channels: Int,
        durationSeconds: Double,
        originalInputRate: UInt32,
        rmsDecibels: Double,
        peakDecibels: Double
    ) {
        self.codecName = codecName
        self.decoderSampleRate = decoderSampleRate
        self.channels = channels
        self.durationSeconds = durationSeconds
        self.originalInputRate = originalInputRate
        self.rmsDecibels = rmsDecibels
        self.peakDecibels = peakDecibels
    }

    public var hasNonzeroAudio: Bool {
        durationSeconds > 0 && rmsDecibels.isFinite && peakDecibels.isFinite
    }
}

public enum AudioAcceptanceError: Error, Equatable, Sendable {
    case toolLaunch(String)
    case toolFailed(tool: String, status: Int32, diagnostic: String)
    case malformedProbe
    case malformedStatistics
    case invalidStream(String)
}

public enum AudioAcceptanceAnalyzer {
    public static func analyze(fileURL: URL) throws -> AudioAcceptanceResult {
        let fileData = try Data(contentsOf: fileURL, options: .mappedIfSafe)
        guard let originalRate = OpusHeadInspector.originalInputRate(in: fileData) else {
            throw AudioAcceptanceError.invalidStream("missing OpusHead")
        }

        let probe = try run(
            executable: "/opt/homebrew/bin/ffprobe",
            arguments: [
                "-v", "error",
                "-select_streams", "a:0",
                "-show_entries", "stream=codec_name,sample_rate,channels,duration",
                "-of", "json",
                fileURL.path,
            ]
        )
        let probeValues = try parseProbe(probe.standardOutput)

        let decode = try run(
            executable: "/opt/homebrew/bin/ffmpeg",
            arguments: [
                "-hide_banner", "-nostats",
                "-i", fileURL.path,
                "-af", "astats=metadata=0:reset=0",
                "-f", "null", "-",
            ]
        )
        let statistics = try parseStatistics(decode.standardError)
        let result = AudioAcceptanceResult(
            codecName: probeValues.codec,
            decoderSampleRate: probeValues.sampleRate,
            channels: probeValues.channels,
            durationSeconds: probeValues.duration,
            originalInputRate: originalRate,
            rmsDecibels: statistics.rms,
            peakDecibels: statistics.peak
        )
        guard result.codecName == "opus", result.channels == 1,
              result.decoderSampleRate == 48_000, result.originalInputRate == 16_000,
              result.hasNonzeroAudio else {
            throw AudioAcceptanceError.invalidStream("unexpected codec/rate/channels/energy")
        }
        return result
    }

    public static func parseStatistics(_ diagnostic: String) throws -> (rms: Double, peak: Double) {
        guard let rms = lastMetric(named: "RMS level dB", in: diagnostic),
              let peak = lastMetric(named: "Peak level dB", in: diagnostic),
              rms.isFinite, peak.isFinite else {
            throw AudioAcceptanceError.malformedStatistics
        }
        return (rms, peak)
    }

    private static func parseProbe(_ data: Data) throws -> (codec: String, sampleRate: Int, channels: Int, duration: Double) {
        guard let root = try JSONSerialization.jsonObject(with: data) as? [String: Any],
              let streams = root["streams"] as? [[String: Any]],
              let stream = streams.first,
              let codec = stream["codec_name"] as? String,
              let sampleRateText = stream["sample_rate"] as? String,
              let sampleRate = Int(sampleRateText),
              let channels = stream["channels"] as? Int,
              let durationText = stream["duration"] as? String,
              let duration = Double(durationText) else {
            throw AudioAcceptanceError.malformedProbe
        }
        return (codec, sampleRate, channels, duration)
    }

    private static func lastMetric(named name: String, in text: String) -> Double? {
        text.split(separator: "\n").reversed().compactMap { line -> Double? in
            guard let range = line.range(of: "\(name):") else { return nil }
            return Double(line[range.upperBound...].trimmingCharacters(in: .whitespaces))
        }.first
    }

    private static func run(executable: String, arguments: [String]) throws -> ProcessOutput {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: executable)
        process.arguments = arguments
        process.environment = [:]
        process.standardInput = FileHandle.nullDevice
        let output = Pipe()
        let error = Pipe()
        process.standardOutput = output
        process.standardError = error
        do {
            try process.run()
        } catch {
            throw AudioAcceptanceError.toolLaunch(String(describing: error))
        }
        let standardOutput = output.fileHandleForReading.readDataToEndOfFile()
        let standardErrorData = error.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        let standardError = String(decoding: standardErrorData, as: UTF8.self)
        guard process.terminationStatus == 0 else {
            throw AudioAcceptanceError.toolFailed(
                tool: URL(fileURLWithPath: executable).lastPathComponent,
                status: process.terminationStatus,
                diagnostic: String(standardError.prefix(1_024))
            )
        }
        return ProcessOutput(standardOutput: standardOutput, standardError: standardError)
    }
}

private struct ProcessOutput {
    let standardOutput: Data
    let standardError: String
}
