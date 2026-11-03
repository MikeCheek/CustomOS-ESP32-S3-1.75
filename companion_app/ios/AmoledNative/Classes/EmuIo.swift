import AVFoundation
import CoreMotion
import Flutter

/// The watch emulator's sensors and sound on iPhone (EmuIo.kt on Android):
///  - "com.amoledwatch.emu_accel" streams the accelerometer in g, [x, y, z],
///    62.5 times a second, the watch's way round (Android's axes: lying flat
///    face up reads z = +1; iOS reports -1 there, so it's flipped);
///  - "com.amoledwatch.emu_sound" "tones": [freq Hz, ms, kind | volume << 8, ...]
///    played one after another, kind 1 a piano note (a new one cuts it).
final class EmuIo: NSObject, FlutterStreamHandler {
    private struct Tone { let freq: Double; let samples: Int; let piano: Bool; let amp: Double }

    private let motion = CMMotionManager()
    private let engine = AVAudioEngine()
    private var source: AVAudioSourceNode?
    private let lock = NSLock()
    private var queue: [Tone] = []
    private var current: Tone?
    private var pos = 0
    private var rate = 44100.0

    init(messenger: FlutterBinaryMessenger) {
        super.init()
        FlutterEventChannel(name: "com.amoledwatch.emu_accel", binaryMessenger: messenger).setStreamHandler(self)
        FlutterMethodChannel(name: "com.amoledwatch.emu_sound", binaryMessenger: messenger)
            .setMethodCallHandler { [weak self] call, result in
                guard let self = self else { return }
                switch call.method {
                case "tones":
                    let l = (call.arguments as? [NSNumber]) ?? []
                    var add: [Tone] = []
                    var i = 0
                    while i + 2 < l.count {
                        let k = l[i + 2].intValue
                        add.append(Tone(freq: l[i].doubleValue,
                                        samples: Int(l[i + 1].doubleValue * self.rate / 1000),
                                        piano: (k & 0xFF) == 1,
                                        amp: 0.28 * Double((k >> 8) & 0xFF) / 100))
                        i += 3
                    }
                    self.lock.lock()
                    self.queue.append(contentsOf: add)
                    self.lock.unlock()
                    self.ensureEngine()
                    result(true)
                case "stop":
                    self.lock.lock()
                    self.queue.removeAll()
                    self.current = nil
                    self.lock.unlock()
                    result(true)
                default: result(FlutterMethodNotImplemented)
                }
            }
    }

    // ---- accelerometer ----
    func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
        guard motion.isAccelerometerAvailable else { return nil }
        motion.accelerometerUpdateInterval = 1.0 / 62.5
        motion.startAccelerometerUpdates(to: .main) { data, _ in
            guard let a = data?.acceleration else { return }
            events([-a.x, -a.y, -a.z])
        }
        return nil
    }

    func onCancel(withArguments arguments: Any?) -> FlutterError? {
        motion.stopAccelerometerUpdates()
        return nil
    }

    // ---- tones ----
    private func ensureEngine() {
        if source != nil { return }
        AudioSession.activate()
        let fmt = engine.outputNode.inputFormat(forBus: 0)
        rate = fmt.sampleRate > 0 ? fmt.sampleRate : 44100
        guard let mono = AVAudioFormat(standardFormatWithSampleRate: rate, channels: 1) else { return }
        let node = AVAudioSourceNode(format: mono) { [weak self] _, _, frames, abl -> OSStatus in
            guard let self = self else { return noErr }
            let buf = UnsafeMutableAudioBufferListPointer(abl)
            guard let out = buf.first?.mData?.assumingMemoryBound(to: Float.self) else { return noErr }
            self.render(out, Int(frames))
            return noErr
        }
        source = node
        engine.attach(node)
        engine.connect(node, to: engine.mainMixerNode, format: mono)
        do { try engine.start() } catch { source = nil }
    }

    private func render(_ out: UnsafeMutablePointer<Float>, _ n: Int) {
        lock.lock()
        defer { lock.unlock() }
        for i in 0..<n {
            if current == nil || pos >= current!.samples || (current!.piano && queue.first?.piano == true) {
                current = queue.isEmpty ? nil : queue.removeFirst()
                pos = 0
            }
            guard let t = current, t.freq > 0 else {
                out[i] = 0
                if current != nil { pos += 1 }
                continue
            }
            let tt = Double(pos) / rate
            var v: Double
            if t.piano {
                let w = 2 * Double.pi * t.freq * tt
                v = (sin(w) + 0.45 * sin(2 * w) + 0.2 * sin(3 * w) + 0.1 * sin(4 * w)) / 1.75 * exp(-3.2 * tt)
            } else {
                v = sin(2 * Double.pi * t.freq * tt)
            }
            let edge = min(1.0, Double(min(pos, t.samples - pos)) / (0.003 * rate))   // 3 ms fades, no clicks
            out[i] = Float(v * t.amp * edge)
            pos += 1
        }
    }
}
