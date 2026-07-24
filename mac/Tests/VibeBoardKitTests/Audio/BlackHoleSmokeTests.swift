import Testing
import Foundation
@testable import VibeBoardKit

@Suite("BlackHole + Opus Smoke")
struct BlackHoleSmokeTests {
    @Test func blackHoleDeviceIsPresentOnThisMac() {
        let available = BlackHoleAudioWriter.isAvailable(deviceName: "BlackHole 2ch")
        #expect(available)
        let deviceID = BlackHoleAudioWriter.findDevice(named: "BlackHole 2ch")
        #expect(deviceID != nil)
    }

    @Test func opusDecoderProducesPCMSamplesForPacketLoss() throws {
        let decoder = try OpusStreamDecoder()
        let pcm = try decoder.decode(packet: Data())
        #expect(pcm.count > 0)
        // PLC with NULL data - libopus returns its preferred frame size
        // Common sizes: 120(2.5ms), 240(5ms), 480(10ms), 960(20ms), 1920(40ms), 2880(60ms)
        print("PLC output: \(pcm.count) samples")
        #expect([120, 240, 480, 960, 1920, 2880].contains(pcm.count))
    }

    @Test func liveAudioPipelineStartsAndStopsCleanly() throws {
        let pipeline = LiveAudioPipeline(deviceName: "BlackHole 2ch")
        try pipeline.start()
        #expect(pipeline.isActive)
        pipeline.stop()
        #expect(!pipeline.isActive)
    }
}
