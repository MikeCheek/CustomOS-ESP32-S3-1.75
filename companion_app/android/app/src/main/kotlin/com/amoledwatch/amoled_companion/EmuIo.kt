package com.amoledwatch.amoled_companion

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodChannel
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.ln
import kotlin.math.sqrt
import kotlin.math.tanh
import kotlin.math.exp
import kotlin.math.min
import kotlin.math.sin

/**
 * The watch emulator's sensors and sound (lib/emulator, phase 3):
 *  - "com.amoledwatch.emu_accel" streams the phone's accelerometer in g,
 *    [x, y, z] about 62.5 times a second (the watch's QMI8658 rate);
 *  - "com.amoledwatch.emu_sound" "tones" plays the watch's beeps, game
 *    effects and piano notes: [freq Hz, ms, kind | volume << 8, ...]. Beeps
 *    play one after another like the watch's sound queue; piano notes use
 *    the watch's piano voice (hal_audio.cpp): up to 6 ringing together, the
 *    same partials, decays, hammer and damper, so the emulator sounds like
 *    the watch.
 */
class EmuIo(context: Context, messenger: BinaryMessenger) {
    private class Tone(val freq: Int, val ms: Int, val piano: Boolean, val volume: Int)

    private val queue = java.util.concurrent.LinkedBlockingDeque<Tone>()
    private var thread: Thread? = null
    private val rate = 22050

    private val sensors = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager
    private var listener: SensorEventListener? = null

    init {
        MethodChannel(messenger, "com.amoledwatch.emu_sound").setMethodCallHandler { call, result ->
            when (call.method) {
                "tones" -> {
                    val l = call.arguments as? List<*> ?: emptyList<Any>()
                    var i = 0
                    while (i + 2 < l.size) {
                        val f = (l[i] as Number).toInt()
                        val ms = (l[i + 1] as Number).toInt()
                        val k = (l[i + 2] as Number).toInt()
                        queue.offer(Tone(f, ms, (k and 0xFF) == 1, (k shr 8) and 0xFF))
                        i += 3
                    }
                    ensureThread()
                    result.success(true)
                }
                "stop" -> { queue.clear(); result.success(true) }
                else -> result.notImplemented()
            }
        }
        EventChannel(messenger, "com.amoledwatch.emu_accel").setStreamHandler(object : EventChannel.StreamHandler {
            override fun onListen(arguments: Any?, sink: EventChannel.EventSink) {
                val acc = sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) ?: return
                val l = object : SensorEventListener {
                    override fun onSensorChanged(e: SensorEvent) {
                        val g = SensorManager.GRAVITY_EARTH
                        sink.success(doubleArrayOf((e.values[0] / g).toDouble(), (e.values[1] / g).toDouble(), (e.values[2] / g).toDouble()))
                    }
                    override fun onAccuracyChanged(s: Sensor?, a: Int) {}
                }
                listener = l
                sensors.registerListener(l, acc, 16000)   // 16 ms = 62.5 Hz
            }

            override fun onCancel(arguments: Any?) {
                listener?.let { sensors.unregisterListener(it) }
                listener = null
            }
        })
    }

    private fun ensureThread() {
        if (thread?.isAlive == true) return
        thread = Thread({ play() }, "emu-sound").apply { isDaemon = true; start() }
    }

    // ---- the watch's piano voice (firmware/hal_audio.cpp, pn_start / pn_render) ----
    private class Voice(f: Double, val midi: Int, ms: Int, vel: Double, rate: Int) {
        var on = true
        var released = false
        var age = 0
        val len = rate * maxOf(ms, 200) / 1000
        var damp = 1.0
        val np: Int
        val inc = DoubleArray(7); val ph = DoubleArray(7)
        val inc2 = DoubleArray(2); val ph2 = DoubleArray(2)
        val e1 = DoubleArray(7); val e2 = DoubleArray(7); val d1 = DoubleArray(7); val d2 = DoubleArray(7)
        val noise: Double
        var nlp = 0.0
        var seed = (0x9E3779B9L xor (midi * 2654435761L)) and 0xFFFFFFFFL
        val hammerN = rate / 160
        val attackN = (rate * 0.0015).toInt().coerceAtLeast(1)
        val dampStep = exp(-1.0 / (0.139 * rate))

        init {
            val b = 0.00025 + 0.0005 * (f / 1000.0)
            val tau = (3.2 * Math.pow(261.6 / f, 0.65)).coerceIn(0.35, 4.0)
            val w = doubleArrayOf(1.0, 0.62, 0.36, 0.24, 0.14, 0.09, 0.05)
            val bass = if (f < 400) 0.45 + 0.55 * (f / 400.0) else 1.0
            val bright = if (f < 1000) 1.0 else 1000.0 / f
            var n = 0
            for (i in 0 until 7) {
                val k = (i + 1).toDouble()
                val fn = k * f * sqrt(1.0 + b * k * k)
                if (fn > 7200) break   // what the watch's 16 kHz carries
                val wt = w[i] * (if (i == 0) bass else 1.0) * (if (i >= 2) bright else 1.0) * vel
                inc[i] = 2 * PI * fn / rate
                ph[i] = i * 0.37
                val t2 = tau / Math.pow(k, 0.75); val t1 = t2 * 0.16
                e1[i] = wt * 0.55; e2[i] = wt * 0.45
                d1[i] = exp(-1.0 / (t1 * rate)); d2[i] = exp(-1.0 / (t2 * rate))
                n++
            }
            np = n
            for (i in 0 until minOf(2, np)) inc2[i] = inc[i] * 1.0012
            noise = 0.18 * vel * (0.6 + 0.4 * minOf(f / 800.0, 1.0))
        }

        fun next(): Double {
            var s = 0.0
            for (p in 0 until np) {
                var x = sin(ph[p])
                if (p < 2) { x = 0.5 * (x + sin(ph2[p])); ph2[p] += inc2[p] }
                s += x * (e1[p] + e2[p])
                ph[p] += inc[p]
                e1[p] *= d1[p]; e2[p] *= d2[p]
            }
            if (age < hammerN) {
                seed = (seed * 1664525L + 1013904223L) and 0xFFFFFFFFL
                val r = seed.toInt() / 2147483648.0
                nlp += 0.35 * (r - nlp)
                s += nlp * noise * (1.0 - age.toDouble() / hammerN)
            }
            val a = if (age < attackN) age.toDouble() / attackN else 1.0
            if (age >= len) released = true
            if (released) damp *= dampStep
            age++
            if (damp < 0.002 || (age > len / 4 && age > 4000 && e1[0] + e2[0] < 0.0015)) on = false
            return s * a * damp
        }
    }

    private fun play() {
        val minBuf = AudioTrack.getMinBufferSize(rate, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT)
        val track = AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME)
                .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION).build())
            .setAudioFormat(AudioFormat.Builder().setSampleRate(rate).setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
            .setBufferSizeInBytes(maxOf(minBuf, rate / 10 * 2))
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        track.play()
        val chunk = ShortArray(rate / 100)   // 10 ms
        val voices = ArrayList<Voice>()
        val beeps = ArrayDeque<Tone>()
        var beep: Tone? = null
        var beepPos = 0
        var beepN = 0
        var volume = 70
        try {
            while (true) {
                // idle for 3 s: stop the thread (and the track) until the next tone
                if (voices.isEmpty() && beep == null && beeps.isEmpty()) {
                    val t = queue.poll(3, TimeUnit.SECONDS) ?: break
                    queue.offerFirst(t)
                }
                while (true) {
                    val t = queue.poll() ?: break
                    volume = t.volume
                    if (!t.piano) { beeps.addLast(t); continue }
                    if (t.freq <= 0) continue
                    val midi = Math.round(69 + 12 * ln(t.freq / 440.0) / ln(2.0)).toInt()
                    for (v in voices) if (v.midi == midi) v.released = true   // the same key again
                    if (voices.size >= 6) voices.removeAt(voices.indices.minByOrNull { voices[it].damp * (voices[it].e1[0] + voices[it].e2[0]) } ?: 0)
                    voices.add(Voice(t.freq.toDouble(), midi, t.ms, 0.9, rate))
                }
                for (k in chunk.indices) {
                    if (beep == null && beeps.isNotEmpty()) {
                        val nb = beeps.removeFirst()
                        beep = nb; beepPos = 0; beepN = (nb.ms.toLong() * rate / 1000).toInt()
                    }
                    var b = 0.0
                    val bt = beep
                    if (bt != null) {
                        if (bt.freq > 0) {
                            val edge = min(1.0, min(beepPos, beepN - beepPos) / (0.003 * rate))   // no clicks
                            b = sin(2 * PI * bt.freq * beepPos / rate) * 0.27 * edge
                        }
                        beepPos++
                        if (beepPos >= beepN) beep = null
                    }
                    var mix = 0.0
                    for (v in voices) if (v.on) mix += v.next()
                    // the watch's level, then the same limiter (only near full scale)
                    var y = mix * 0.24
                    val ay = abs(y)
                    if (ay > 0.6) { val o = 0.6 + 0.38 * tanh((ay - 0.6) / 0.38); y = if (y < 0) -o else o }
                    chunk[k] = (((y + b) * volume / 100.0) * 32767).toInt().coerceIn(-32767, 32767).toShort()
                }
                voices.removeAll { !it.on }
                track.write(chunk, 0, chunk.size)
            }
        } catch (_: InterruptedException) {
        } finally {
            track.stop()
            track.release()
        }
    }
}
