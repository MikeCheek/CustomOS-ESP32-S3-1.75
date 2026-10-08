package com.amoledwatch.amoled_companion

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.MediaRecorder
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.MethodChannel
import java.io.File
import java.io.RandomAccessFile
import kotlin.math.abs

/**
 * Records voice memos with the phone's microphone straight to a WAV file:
 * 16 kHz, mono, 16-bit - the same format the watch records, and what the
 * transcription model wants. A thread reads AudioRecord and appends to the
 * file; the WAV header's sizes are filled in on stop. The Dart side polls
 * "level" for the meter. The screen stays on while recording: Android gives
 * apps in the background silence instead of the microphone. No extra
 * Flutter package needed.
 */
class MemoRecorder(private val context: Context, messenger: BinaryMessenger) {
    companion object {
        const val RATE = 16000
    }

    @Volatile private var running = false
    @Volatile private var paused = false
    @Volatile private var peak = 0          // since the last "level" call, 0..32767
    @Volatile private var dataBytes = 0L
    private var thread: Thread? = null
    private var path: String? = null
    private var error: String? = null

    init {
        MethodChannel(messenger, "com.amoledwatch.recorder").setMethodCallHandler { call, result ->
            try {
                when (call.method) {
                    "start" -> result.success(start(call.argument<String>("path") ?: ""))
                    "pause" -> { paused = true; result.success(true) }
                    "resume" -> { paused = false; result.success(true) }
                    "level" -> {
                        val p = peak
                        peak = 0
                        result.success(mapOf(
                            "level" to (p * 100 / 32767),
                            "ms" to (dataBytes * 1000 / (RATE * 2)),
                            "running" to running,
                            "error" to error,
                        ))
                    }
                    "stop" -> result.success(stop(keep = true))
                    "cancel" -> { stop(keep = false); result.success(true) }
                    else -> result.notImplemented()
                }
            } catch (e: Exception) {
                result.error("recorder", e.message, null)
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun start(p: String): Boolean {
        if (running) stop(keep = false)
        if (context.checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            error = "Microphone permission needed"
            return false
        }
        val minBuf = AudioRecord.getMinBufferSize(RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        if (minBuf <= 0) { error = "Microphone not available"; return false }
        val rec = AudioRecord(MediaRecorder.AudioSource.VOICE_RECOGNITION, RATE,
            AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, maxOf(minBuf, RATE))   // >= 0.5 s
        if (rec.state != AudioRecord.STATE_INITIALIZED) {
            rec.release()
            error = "Microphone busy"
            return false
        }
        val file = File(p)
        file.parentFile?.mkdirs()
        val out = RandomAccessFile(file, "rw")
        out.setLength(0)
        out.write(ByteArray(44))   // header written on stop
        path = p
        error = null
        dataBytes = 0
        peak = 0
        paused = false
        running = true
        rec.startRecording()
        MainActivity.keepScreenOn(true)
        thread = Thread {
            val buf = ShortArray(RATE / 10)       // 100 ms
            val bytes = ByteArray(buf.size * 2)
            try {
                while (running) {
                    val n = rec.read(buf, 0, buf.size)
                    if (n <= 0) continue
                    if (paused) continue
                    var pk = peak
                    for (i in 0 until n) {
                        val s = buf[i].toInt()
                        val a = abs(s)
                        if (a > pk) pk = a
                        bytes[2 * i] = (s and 0xFF).toByte()
                        bytes[2 * i + 1] = ((s shr 8) and 0xFF).toByte()
                    }
                    peak = if (pk > 32767) 32767 else pk
                    out.write(bytes, 0, n * 2)
                    dataBytes += n * 2
                }
            } catch (e: Exception) {
                error = e.message ?: "Recording failed"
            } finally {
                try { rec.stop() } catch (_: Exception) {}
                rec.release()
                try { writeHeader(out, dataBytes) } catch (_: Exception) {}
                out.close()
            }
        }.apply { name = "memo-recorder"; start() }
        return true
    }

    /** Stops; returns {path, ms, bytes} (keep) after the file is complete. */
    private fun stop(keep: Boolean): Map<String, Any?> {
        running = false
        MainActivity.keepScreenOn(false)
        thread?.join(2000)
        thread = null
        val p = path
        path = null
        val ms = dataBytes * 1000 / (RATE * 2)
        if (!keep && p != null) File(p).delete()
        return mapOf("path" to p, "ms" to ms, "bytes" to dataBytes + 44, "error" to error)
    }

    private fun writeHeader(f: RandomAccessFile, data: Long) {
        fun le32(v: Long) = byteArrayOf((v and 0xFF).toByte(), ((v shr 8) and 0xFF).toByte(),
            ((v shr 16) and 0xFF).toByte(), ((v shr 24) and 0xFF).toByte())
        fun le16(v: Int) = byteArrayOf((v and 0xFF).toByte(), ((v shr 8) and 0xFF).toByte())
        f.seek(0)
        f.write("RIFF".toByteArray()); f.write(le32(36 + data))
        f.write("WAVE".toByteArray())
        f.write("fmt ".toByteArray()); f.write(le32(16))
        f.write(le16(1)); f.write(le16(1))                 // PCM, mono
        f.write(le32(RATE.toLong())); f.write(le32(RATE * 2L))
        f.write(le16(2)); f.write(le16(16))                // block align, bits
        f.write("data".toByteArray()); f.write(le32(data))
    }
}
