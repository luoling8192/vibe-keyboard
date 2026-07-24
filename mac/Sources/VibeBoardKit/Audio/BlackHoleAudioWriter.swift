import Foundation
import CoreAudio

/// Writes 16-bit mono PCM into a BlackHole virtual audio device so third-party
/// dictation apps (Typeless, Vokie, etc.) can capture it as a microphone.
///
/// The device is looked up by name at start time. Audio data is buffered in a
/// lock-free ring buffer; the CoreAudio HAL IO proc pulls from it in
/// real-time. Incoming 16-bit mono PCM is converted to the device's native
/// stream format (typically 32-bit float stereo for BlackHole 2ch).
public final class BlackHoleAudioWriter: @unchecked Sendable {

    // MARK: - Ring buffer

    /// Lock-free ring buffer holding float32 sample pairs (interleaved stereo).
    /// Capacity: ~1 second of 48 kHz stereo audio.
    private static let bufferCapacity = 48_000 * 2  // 96000 floats

    private var ringBuffer: [Float32]
    private var writePos: Int = 0
    private var readPos: Int = 0
    private let ringLock = NSLock()

    // MARK: - CoreAudio state

    private var deviceID: AudioDeviceID = 0
    private var ioProcID: AudioDeviceIOProcID?
    private var deviceSampleRate: Double = 48_000
    private var deviceChannels: UInt32 = 2
    private var isFloat: Bool = true
    private var running = false
    private let deviceName: String

    // MARK: - Init

    public init(deviceName: String = "BlackHole 2ch") {
        self.deviceName = deviceName
        self.ringBuffer = [Float32](repeating: 0, count: Self.bufferCapacity)
    }

    deinit {
        stop()
    }

    // MARK: - Device discovery

    /// Finds the BlackHole device by name. Returns the device ID, or nil if not found.
    public static func findDevice(named name: String) -> AudioDeviceID? {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioHardwarePropertyDevices,
            mScope: kAudioObjectPropertyScopeGlobal,
            mElement: kAudioObjectPropertyElementMain
        )
        var dataSize: UInt32 = 0
        var status = AudioObjectGetPropertyDataSize(
            AudioObjectID(kAudioObjectSystemObject),
            &propertyAddress,
            0, nil,
            &dataSize
        )
        guard status == noErr else { return nil }

        let count = Int(dataSize) / MemoryLayout<AudioDeviceID>.size
        var devices = [AudioDeviceID](repeating: 0, count: count)
        status = AudioObjectGetPropertyData(
            AudioObjectID(kAudioObjectSystemObject),
            &propertyAddress,
            0, nil,
            &dataSize,
            &devices
        )
        guard status == noErr else { return nil }

        for device in devices {
            var nameProperty = AudioObjectPropertyAddress(
                mSelector: kAudioDevicePropertyDeviceName,
                mScope: kAudioObjectPropertyScopeGlobal,
                mElement: kAudioObjectPropertyElementMain
            )
            var cname = [CChar](repeating: 0, count: 256)
            var nameSize: UInt32 = UInt32(cname.count)
            let nameStatus = AudioObjectGetPropertyData(
                device,
                &nameProperty,
                0, nil,
                &nameSize,
                &cname
            )
            guard nameStatus == noErr else { continue }
            let deviceName = String(cString: cname)
            if deviceName == name {
                return device
            }
        }
        return nil
    }

    /// Returns true if any BlackHole variant is currently available on the system.
    public static func isAvailable(deviceName: String = "BlackHole 2ch") -> Bool {
        findDevice(named: deviceName) != nil
            || findDevice(named: "BlackHole 16ch") != nil
            || findDevice(named: "BlackHole 64ch") != nil
    }

    // MARK: - Lifecycle

    public func start() throws {
        guard !running else { return }

        guard let id = BlackHoleAudioWriter.findDevice(named: deviceName) else {
            throw BlackHoleAudioWriterError.deviceNotFound(deviceName)
        }
        deviceID = id

        try readStreamFormat()
        resetBuffer()

        // Register IO proc
        let ioProc: AudioDeviceIOProc = { _, _, _, _, outOutputData, _, clientData in
            guard let writer = clientData?.assumingMemoryBound(to: BlackHoleAudioWriter.self).pointee else {
                return noErr
            }
            writer.fillOutput(outOutputData)
            return noErr
        }

        var procID: AudioDeviceIOProcID?
        let status = withUnsafeMutablePointer(to: &procID) { procPtr in
            AudioDeviceCreateIOProcID(
                deviceID,
                ioProc,
                Unmanaged.passUnretained(self).toOpaque(),
                procPtr
            )
        }
        guard status == noErr, let procID else {
            throw BlackHoleAudioWriterError.ioProcCreateFailed(status)
        }
        ioProcID = procID

        let startStatus = AudioDeviceStart(deviceID, procID)
        guard startStatus == noErr else {
            AudioDeviceDestroyIOProcID(deviceID, procID)
            ioProcID = nil
            throw BlackHoleAudioWriterError.deviceStartFailed(startStatus)
        }

        running = true
    }

    public func stop() {
        guard running, let procID = ioProcID else { return }
        AudioDeviceStop(deviceID, procID)
        AudioDeviceDestroyIOProcID(deviceID, procID)
        ioProcID = nil
        running = false
        resetBuffer()
    }

    // MARK: - Writing PCM

    /// Pushes 16-bit mono PCM samples into the ring buffer.
    /// Converts to float32 and duplicates to stereo for the device's stream format.
    public func write(pcm: [Int16]) {
        guard running else { return }
        guard !pcm.isEmpty else { return }

        ringLock.lock()
        defer { ringLock.unlock() }

        for sample in pcm {
            let f = Float32(sample) / 32_768.0
            // Duplicate mono to both stereo channels if device is stereo
            if deviceChannels >= 2 {
                ringBuffer[writePos] = f
                advanceWritePos()
                ringBuffer[writePos] = f
                advanceWritePos()
            } else {
                ringBuffer[writePos] = f
                advanceWritePos()
            }
        }
    }

    // MARK: - IO proc callback

    private func fillOutput(_ outputData: UnsafeMutablePointer<AudioBufferList>) {
        let bufferCount = Int(outputData.pointee.mNumberBuffers)
        guard bufferCount > 0 else { return }

        // Only fill the first buffer (BlackHole interleaves all channels)
        let audioBuffer = withUnsafePointer(to: outputData.pointee.mBuffers) { $0.pointee }
        guard let mData = audioBuffer.mData else { return }
        let frameCount = Int(audioBuffer.mDataByteSize) / Int(deviceChannels) / bytesPerSample
        let output = mData.assumingMemoryBound(to: Float32.self)

        ringLock.lock()
        defer { ringLock.unlock() }

        for i in 0..<(frameCount * Int(deviceChannels)) {
            if availableSamples() > 0 {
                output[i] = ringBuffer[readPos]
                advanceReadPos()
            } else {
                output[i] = 0.0  // underrun → silence
            }
        }
    }

    // MARK: - Stream format

    private var bytesPerSample: Int {
        isFloat ? MemoryLayout<Float32>.size : MemoryLayout<Int16>.size
    }

    private func readStreamFormat() throws {
        var propertyAddress = AudioObjectPropertyAddress(
            mSelector: kAudioDevicePropertyStreamFormat,
            mScope: kAudioDevicePropertyScopeOutput,
            mElement: kAudioObjectPropertyElementMain
        )
        var format = AudioStreamBasicDescription()
        var size: UInt32 = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
        let status = AudioObjectGetPropertyData(
            deviceID,
            &propertyAddress,
            0, nil,
            &size,
            &format
        )
        guard status == noErr else {
            throw BlackHoleAudioWriterError.streamFormatReadFailed(status)
        }
        deviceSampleRate = format.mSampleRate
        deviceChannels = format.mChannelsPerFrame
        isFloat = (format.mFormatFlags & kAudioFormatFlagIsFloat) != 0
    }

    // MARK: - Ring buffer helpers

    private func resetBuffer() {
        ringLock.lock()
        writePos = 0
        readPos = 0
        for i in 0..<ringBuffer.count { ringBuffer[i] = 0 }
        ringLock.unlock()
    }

    private func advanceWritePos() {
        writePos = (writePos + 1) % ringBuffer.count
    }

    private func advanceReadPos() {
        readPos = (readPos + 1) % ringBuffer.count
    }

    private func availableSamples() -> Int {
        let diff = writePos - readPos
        return diff >= 0 ? diff : diff + ringBuffer.count
    }
}

public enum BlackHoleAudioWriterError: Error, CustomStringConvertible {
    case deviceNotFound(String)
    case ioProcCreateFailed(OSStatus)
    case deviceStartFailed(OSStatus)
    case streamFormatReadFailed(OSStatus)

    public var description: String {
        switch self {
        case .deviceNotFound(let name):
            "BlackHole device '\(name)' not found — install BlackHole"
        case .ioProcCreateFailed(let status):
            "Failed to create audio IO proc (status \(status))"
        case .deviceStartFailed(let status):
            "Failed to start BlackHole device (status \(status))"
        case .streamFormatReadFailed(let status):
            "Failed to read BlackHole stream format (status \(status))"
        }
    }
}
