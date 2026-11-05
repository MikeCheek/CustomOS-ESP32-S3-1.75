import AVFoundation
import Flutter
import UIKit

/// One audio session for recording and playing: the app records memos and
/// questions, plays memos and the watch emulator's sounds, sometimes at once.
enum AudioSession {
    static func activate() {
        let s = AVAudioSession.sharedInstance()
        try? s.setCategory(.playAndRecord, mode: .default,
                           options: [.defaultToSpeaker, .allowBluetoothA2DP, .mixWithOthers])
        try? s.setActive(true)
    }
}

/// "com.amoledwatch.recorder" (lib/services/phone_recorder.dart, MemoRecorder.kt):
/// the phone's microphone to a 16 kHz mono 16-bit WAV, the watch's format.
final class MemoRecorder {
    private static let rate = 16000.0
    private let engine = AVAudioEngine()
    private let outFormat = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: MemoRecorder.rate,
                                          channels: 1, interleaved: true)!
    private var converter: AVAudioConverter?
    private var file: FileHandle?
    private var path = ""
    private var dataBytes = 0
    private var peak = 0
    private var paused = false
    private var running = false
    private var error: String?
    private let lock = NSLock()

    init(messenger: FlutterBinaryMessenger) {
        FlutterMethodChannel(name: "com.amoledwatch.recorder", binaryMessenger: messenger)
            .setMethodCallHandler { [weak self] call, result in
                guard let self = self else { return }
                let args = call.arguments as? [String: Any]
                switch call.method {
                case "start": result(self.start(args?["path"] as? String ?? ""))
                case "pause": self.paused = true; result(true)
                case "resume": self.paused = false; result(true)
                case "level":
                    self.lock.lock()
                    let p = self.peak
                    self.peak = 0
                    let ms = self.dataBytes * 1000 / Int(MemoRecorder.rate * 2)
                    self.lock.unlock()
                    result(["level": p * 100 / 32767, "ms": ms, "running": self.running, "error": self.error.map { $0 as Any } ?? NSNull()])
                case "stop": result(self.stop(keep: true))
                case "cancel": _ = self.stop(keep: false); result(true)
                default: result(FlutterMethodNotImplemented)
                }
            }
    }

    private func start(_ p: String) -> Bool {
        if running { _ = stop(keep: false) }
        error = nil
        if AVAudioSession.sharedInstance().recordPermission == .denied {
            error = "Microphone permission needed"
            return false
        }
        AudioSession.activate()
        let url = URL(fileURLWithPath: p)
        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        FileManager.default.createFile(atPath: p, contents: Data(count: 44))   // header written on stop
        guard let fh = FileHandle(forWritingAtPath: p) else { error = "Can't write the recording"; return false }
        fh.seekToEndOfFile()
        let input = engine.inputNode
        let inFormat = input.outputFormat(forBus: 0)
        guard inFormat.sampleRate > 0, inFormat.channelCount > 0,
              let conv = AVAudioConverter(from: inFormat, to: outFormat) else {
            fh.closeFile()
            error = "Microphone not available"
            return false
        }
        converter = conv
        file = fh
        path = p
        dataBytes = 0
        peak = 0
        paused = false
        input.installTap(onBus: 0, bufferSize: 4096, format: inFormat) { [weak self] buf, _ in self?.process(buf) }
        engine.prepare()
        do {
            try engine.start()
        } catch {
            input.removeTap(onBus: 0)
            fh.closeFile()
            file = nil
            self.error = "Microphone busy"
            return false
        }
        running = true
        UIApplication.shared.isIdleTimerDisabled = true
        return true
    }

    private func process(_ buf: AVAudioPCMBuffer) {
        guard let conv = converter, !paused else { return }
        let cap = AVAudioFrameCount(Double(buf.frameLength) * MemoRecorder.rate / buf.format.sampleRate) + 64
        guard let out = AVAudioPCMBuffer(pcmFormat: outFormat, frameCapacity: cap) else { return }
        var given = false
        var err: NSError?
        conv.convert(to: out, error: &err) { _, status in
            if given { status.pointee = .noDataNow; return nil }
            given = true
            status.pointee = .haveData
            return buf
        }
        let n = Int(out.frameLength)
        guard n > 0, let ch = out.int16ChannelData else { return }
        var pk = 0
        for i in 0..<n { pk = max(pk, abs(Int(ch[0][i]))) }
        let data = Data(bytes: ch[0], count: n * 2)
        lock.lock()
        file?.write(data)
        dataBytes += n * 2
        peak = min(32767, max(peak, pk))
        lock.unlock()
    }

    /// {path, ms, bytes} once the file is complete.
    private func stop(keep: Bool) -> [String: Any] {
        if running {
            engine.inputNode.removeTap(onBus: 0)
            engine.stop()
        }
        running = false
        UIApplication.shared.isIdleTimerDisabled = false
        lock.lock()
        if let fh = file {
            fh.seek(toFileOffset: 0)
            fh.write(MemoRecorder.wavHeader(dataBytes: dataBytes, rate: Int(MemoRecorder.rate)))
            fh.closeFile()
        }
        file = nil
        let bytes = dataBytes
        lock.unlock()
        if !keep { try? FileManager.default.removeItem(atPath: path) }
        return ["path": path, "ms": bytes * 1000 / Int(MemoRecorder.rate * 2), "bytes": bytes]
    }

    static func wavHeader(dataBytes: Int, rate: Int) -> Data {
        var h = Data()
        func u32(_ v: Int) { var x = UInt32(v).littleEndian; h.append(Data(bytes: &x, count: 4)) }
        func u16(_ v: Int) { var x = UInt16(v).littleEndian; h.append(Data(bytes: &x, count: 2)) }
        h.append(contentsOf: Array("RIFF".utf8)); u32(36 + dataBytes)
        h.append(contentsOf: Array("WAVEfmt ".utf8)); u32(16); u16(1); u16(1)
        u32(rate); u32(rate * 2); u16(2); u16(16)
        h.append(contentsOf: Array("data".utf8)); u32(dataBytes)
        return h
    }
}

/// "com.amoledwatch.player" (lib/services/memo_player.dart, MemoPlayer.kt):
/// plays a memo (WAV/MP3) with seek and speed.
final class MemoPlayer: NSObject, AVAudioPlayerDelegate {
    private var player: AVAudioPlayer?
    private var path = ""
    private var completed = false
    private var speed: Float = 1

    init(messenger: FlutterBinaryMessenger) {
        super.init()
        FlutterMethodChannel(name: "com.amoledwatch.player", binaryMessenger: messenger)
            .setMethodCallHandler { [weak self] call, result in
                guard let self = self else { return }
                let args = call.arguments as? [String: Any]
                switch call.method {
                case "play":
                    do {
                        result(try self.play(args?["path"] as? String ?? "", from: args?["from"] as? Int ?? 0))
                    } catch {
                        result(FlutterError(code: "player", message: error.localizedDescription, details: nil))
                    }
                case "pause": self.player?.pause(); result(self.state())
                case "resume":
                    if let p = self.player {
                        if self.completed { p.currentTime = 0; self.completed = false }
                        p.play()
                    }
                    result(self.state())
                case "seek":
                    self.player?.currentTime = Double(args?["ms"] as? Int ?? 0) / 1000
                    self.completed = false
                    result(self.state())
                case "speed":
                    self.speed = Float(min(2.5, max(0.5, args?["v"] as? Double ?? 1)))
                    self.player?.rate = self.speed
                    result(self.state())
                case "stop": self.release(); result(self.state())
                case "state": result(self.state())
                default: result(FlutterMethodNotImplemented)
                }
            }
    }

    private func play(_ p: String, from: Int) throws -> [String: Any] {
        release()
        AudioSession.activate()
        let pl = try AVAudioPlayer(contentsOf: URL(fileURLWithPath: p))
        pl.enableRate = true
        pl.rate = speed
        pl.delegate = self
        pl.prepareToPlay()
        if from > 0 { pl.currentTime = Double(from) / 1000 }
        pl.play()
        player = pl
        path = p
        completed = false
        return state()
    }

    private func release() {
        player?.stop()
        player = nil
        completed = false
    }

    func audioPlayerDidFinishPlaying(_ player: AVAudioPlayer, successfully flag: Bool) { completed = true }

    private func state() -> [String: Any] {
        let dur = Int((player?.duration ?? 0) * 1000)
        let pos = player == nil ? 0 : (completed ? dur : Int((player?.currentTime ?? 0) * 1000))
        return ["path": path, "playing": player?.isPlaying ?? false, "pos": pos, "dur": dur, "done": completed,
                "speed": Double(speed)]
    }
}
