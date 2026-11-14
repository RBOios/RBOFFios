import AVFoundation

/// Looping launcher background music (bundled bgm.mp3).
enum BGM {
    private static var player: AVAudioPlayer? = {
        guard let url = Bundle.main.url(forResource: "bgm", withExtension: "mp3") else { return nil }
        let p = try? AVAudioPlayer(contentsOf: url)
        p?.numberOfLoops = -1          // loop forever
        p?.prepareToPlay()
        return p
    }()

    static func play() {
        guard let p = player, !p.isPlaying else { return }
        try? AVAudioSession.sharedInstance().setCategory(.playback, mode: .default)
        try? AVAudioSession.sharedInstance().setActive(true)
        p.play()
    }

    static func stop() {
        player?.stop()
        player?.currentTime = 0
    }
}
