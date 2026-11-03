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
import kotlin.math.exp
import kotlin.math.min
import kotlin.math.sin

/**
 * The watch emulator's sensors and sound (lib/emulator, phase 3):
 *  - "com.amoledwatch.emu_accel" streams the phone's accelerometer in g,
 *    [x, y, z] about 62.5 times a second (the watch's QMI8658 rate);
 *  - "com.amoledwatch.emu_sound" "tones" plays the watch's beeps, game
 *    effects and piano notes: [freq Hz, ms, kind | volume << 8, ...], one
 *    after another like the watch's sound queue; a new piano note cuts the
 *    one playing, as on the watch.
 */
class EmuIo(context: Context, messenger: BinaryMessenger) {
    private class Tone(val freq: Int, val ms: Int, val piano: Boolean, val volume: Int)

    private val queue = LinkedBlockingQueue<Tone>()
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
        try {
            while (true) {
                // idle for 3 s: stop the thread (and the track) until the next tone
                val t = queue.poll(3, TimeUnit.SECONDS) ?: break
                val n = (t.ms.toLong() * rate / 1000).toInt()
                val amp = 9000.0 * t.volume / 100.0
                var i = 0
                while (i < n) {
                    // a new piano note cuts this one (the watch's piano voice does)
                    if (t.piano && queue.peek()?.piano == true) break
                    val m = min(chunk.size, n - i)
                    for (k in 0 until m) {
                        val s = i + k
                        val tt = s.toDouble() / rate
                        val v = if (t.freq <= 0) 0.0 else if (t.piano) {
                            val w = 2 * PI * t.freq * tt
                            (sin(w) + 0.45 * sin(2 * w) + 0.2 * sin(3 * w) + 0.1 * sin(4 * w)) / 1.75 * exp(-3.2 * tt)
                        } else {
                            sin(2 * PI * t.freq * tt)
                        }
                        // 3 ms fade in / out, no clicks
                        val edge = min(1.0, min(s, n - s) / (0.003 * rate))
                        chunk[k] = (v * amp * edge).toInt().coerceIn(-32767, 32767).toShort()
                    }
                    track.write(chunk, 0, m)
                    i += m
                }
            }
        } catch (_: InterruptedException) {
        } finally {
            track.stop()
            track.release()
        }
    }
}
