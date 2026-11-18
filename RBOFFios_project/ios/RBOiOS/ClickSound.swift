import AVFoundation

/// Short UI click played from the launcher menu (bundled click.mp3).
enum ClickSound {
    private static var player: AVAudioPlayer? = {
        guard let url = Bundle.main.url(forResource: "click", withExtension: "mp3") else { return nil }
        let p = try? AVAudioPlayer(contentsOf: url)
        p?.prepareToPlay()
        return p
    }()

    static func play() {
        // .playback: keeps playing even with the iPhone silent switch on (same category the game audio uses).
        try? AVAudioSession.sharedInstance().setCategory(.playback, mode: .default)
        try? AVAudioSession.sharedInstance().setActive(true)
        player?.currentTime = 0
        player?.play()
    }
}
