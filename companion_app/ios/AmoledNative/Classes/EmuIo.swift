import AVFoundation
import CoreMotion
import Flutter

/// The watch emulator's sensors and sound on iPhone (EmuIo.kt on Android):
///  - "com.amoledwatch.emu_accel" streams the accelerometer in g, [x, y, z],
///    62.5 times a second, the watch's way round (Android's axes: lying flat
///    face up reads z = +1; iOS reports -1 there, so it's flipped);
///  - "com.amoledwatch.emu_sound" "tones": [freq Hz, ms, kind | volume << 8, ...]:
///    beeps one after another; kind 1 a piano note in the watch's piano voice
///    (hal_audio.cpp) - up to 6 ringing together, as on the watch.
final class EmuIo: NSObject, FlutterStreamHandler {
    private struct Tone { let freq: Double; let samples: Int; let piano: Bool; let amp: Double }

    private let motion = CMMotionManager()
    private let engine = AVAudioEngine()
    private var source: AVAudioSourceNode?
    private let lock = NSLock()
    private var queue: [Tone] = []
    private var current: Tone?
    private var pos = 0
    private var voices: [PianoVoice] = []
    private var volume = 0.7
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
                    self.voices.removeAll()
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
        // piano notes start at once (a chord together); beeps wait their turn
        var rest: [Tone] = []
        for t in queue {
            if t.piano {
                if t.freq <= 0 { continue }
                volume = t.amp / 0.28
                let midi = Int((69 + 12 * log2(t.freq / 440)).rounded())
                for v in voices where v.midi == midi { v.released = true }   // the same key again
                if voices.count >= 6, let q = voices.indices.min(by: { voices[$0].level < voices[$1].level }) {
                    voices.remove(at: q)
                }
                voices.append(PianoVoice(f: t.freq, midi: midi, samples: t.samples, rate: rate))
            } else {
                rest.append(t)
            }
        }
        queue = rest
        for i in 0..<n {
            if current == nil || pos >= current!.samples {
                current = queue.isEmpty ? nil : queue.removeFirst()
                pos = 0
            }
            var b = 0.0
            if let t = current {
                if t.freq > 0 {
                    let tt = Double(pos) / rate
                    let edge = min(1.0, Double(min(pos, t.samples - pos)) / (0.003 * rate))   // 3 ms fades, no clicks
                    b = sin(2 * Double.pi * t.freq * tt) * t.amp * edge
                }
                pos += 1
            }
            var mix = 0.0
            for v in voices where v.on { mix += v.next() }
            // the watch's level, then the same limiter (only near full scale)
            var y = mix * 0.24
            let ay = abs(y)
            if ay > 0.6 { let o = 0.6 + 0.38 * tanh((ay - 0.6) / 0.38); y = y < 0 ? -o : o }
            out[i] = Float(y * volume + b)
        }
        voices.removeAll { !$0.on }
    }
}

/// The watch's piano voice (firmware/hal_audio.cpp, pn_start / pn_render).
private final class PianoVoice {
    var on = true
    var released = false
    let midi: Int
    private var age = 0
    private let len: Int
    private var damp = 1.0
    private var np = 0
    private var inc = [Double](repeating: 0, count: 7), ph = [Double](repeating: 0, count: 7)
    private var inc2 = [0.0, 0.0], ph2 = [0.0, 0.0]
    private var e1 = [Double](repeating: 0, count: 7), e2 = [Double](repeating: 0, count: 7)
    private var d1 = [Double](repeating: 0, count: 7), d2 = [Double](repeating: 0, count: 7)
    private let noise: Double
    private var nlp = 0.0
    private var seed: UInt32
    private let hammerN: Int, attackN: Int
    private let dampStep: Double
    var level: Double { damp * (e1[0] + e2[0]) * (released ? 0.5 : 1) }

    init(f: Double, midi: Int, samples: Int, rate: Double) {
        self.midi = midi
        len = max(samples, Int(rate * 0.2))
        hammerN = Int(rate / 160)
        attackN = max(1, Int(rate * 0.0015))
        dampStep = exp(-1 / (0.139 * rate))
        let vel = 0.9
        let b = 0.00025 + 0.0005 * (f / 1000)
        let tau = min(4.0, max(0.35, 3.2 * pow(261.6 / f, 0.65)))
        let w: [Double] = [1, 0.62, 0.36, 0.24, 0.14, 0.09, 0.05]
        let bass = f < 400 ? 0.45 + 0.55 * (f / 400) : 1
        let bright = f < 1000 ? 1 : 1000 / f
        for i in 0..<7 {
            let k = Double(i + 1)
            let fn = k * f * (1 + b * k * k).squareRoot()
            if fn > 7200 { break }   // what the watch's 16 kHz carries
            let wt = w[i] * (i == 0 ? bass : 1) * (i >= 2 ? bright : 1) * vel
            inc[i] = 2 * Double.pi * fn / rate
            ph[i] = Double(i) * 0.37
            let t2 = tau / pow(k, 0.75), t1 = t2 * 0.16
            e1[i] = wt * 0.55; e2[i] = wt * 0.45
            d1[i] = exp(-1 / (t1 * rate)); d2[i] = exp(-1 / (t2 * rate))
            np += 1
        }
        for i in 0..<min(2, np) { inc2[i] = inc[i] * 1.0012 }
        noise = 0.18 * vel * (0.6 + 0.4 * min(f / 800, 1))
        seed = 0x9E37_79B9 ^ UInt32(truncatingIfNeeded: midi &* 2_654_435_761)
    }

    func next() -> Double {
        var s = 0.0
        for p in 0..<np {
            var x = sin(ph[p])
            if p < 2 { x = 0.5 * (x + sin(ph2[p])); ph2[p] += inc2[p] }
            s += x * (e1[p] + e2[p])
            ph[p] += inc[p]
            e1[p] *= d1[p]; e2[p] *= d2[p]
        }
        if age < hammerN {
            seed = seed &* 1_664_525 &+ 1_013_904_223
            let r = Double(Int32(bitPattern: seed)) / 2_147_483_648
            nlp += 0.35 * (r - nlp)
            s += nlp * noise * (1 - Double(age) / Double(hammerN))
        }
        let a = age < attackN ? Double(age) / Double(attackN) : 1
        if age >= len { released = true }
        if released { damp *= dampStep }
        age += 1
        if damp < 0.002 || (age > 4000 && e1[0] + e2[0] < 0.0015) { on = false }
        return s * a * damp
    }
}
