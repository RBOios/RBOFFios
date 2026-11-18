import AVFoundation

/// Pulls the emulator's 44.1 kHz stereo mix from the C ring buffer.
final class AudioOut {
    private let engine = AVAudioEngine()
    private var node: AVAudioSourceNode?

    func start() {
        try? AVAudioSession.sharedInstance().setCategory(.playback, mode: .default)
        try? AVAudioSession.sharedInstance().setActive(true)
        let fmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 44100, channels: 2, interleaved: false)!
        let scratch = UnsafeMutablePointer<Int16>.allocate(capacity: 8192 * 2)
        node = AVAudioSourceNode(format: fmt) { _, _, frameCount, abl -> OSStatus in
            let n = min(Int(frameCount), 8192)
            _ = rbo_audio_pull(scratch, Int32(n))
            let bufs = UnsafeMutableAudioBufferListPointer(abl)
            if bufs.count >= 2 {
                let l = bufs[0].mData!.assumingMemoryBound(to: Float.self), r = bufs[1].mData!.assumingMemoryBound(to: Float.self)
                for i in 0..<n { l[i] = Float(scratch[2 * i]) / 32768; r[i] = Float(scratch[2 * i + 1]) / 32768 }
            }
            return noErr
        }
        engine.attach(node!); engine.connect(node!, to: engine.mainMixerNode, format: fmt)
        try? engine.start()
    }
    func stop() { engine.stop() }
}
