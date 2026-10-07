package com.amoledwatch.amoled_companion

import android.content.Context
import android.media.AudioAttributes
import android.media.MediaPlayer
import android.media.PlaybackParams
import android.os.Build
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.MethodChannel

/**
 * Plays voice memos (WAV/MP3 files in the app's folder) with Android's
 * MediaPlayer. One file at a time; the Dart side polls "state" for the
 * position. No extra Flutter package needed.
 */
class MemoPlayer(private val context: Context, messenger: BinaryMessenger) {
    private var player: MediaPlayer? = null
    private var path: String? = null
    private var completed = false
    private var speed = 1.0f

    init {
        MethodChannel(messenger, "com.amoledwatch.player").setMethodCallHandler { call, result ->
            try {
                when (call.method) {
                    "play" -> result.success(play(call.argument<String>("path") ?: "",
                        call.argument<Int>("from") ?: 0))
                    "pause" -> { player?.takeIf { it.isPlaying }?.pause(); result.success(state()) }
                    "resume" -> {
                        player?.let {
                            if (completed) { it.seekTo(0); completed = false }
                            it.start()
                        }
                        result.success(state())
                    }
                    "seek" -> { player?.seekTo(call.argument<Int>("ms") ?: 0); completed = false; result.success(state()) }
                    "speed" -> { setSpeed((call.argument<Double>("v") ?: 1.0).toFloat()); result.success(state()) }
                    "stop" -> { release(); result.success(state()) }
                    "state" -> result.success(state())
                    else -> result.notImplemented()
                }
            } catch (e: Exception) {
                result.error("player", e.message, null)
            }
        }
    }

    private fun play(p: String, from: Int): Map<String, Any?> {
        release()
        val mp = MediaPlayer()
        mp.setAudioAttributes(
            AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_MEDIA)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                .build()
        )
        mp.setDataSource(p)
        mp.prepare()   // local file: fast
        mp.setOnCompletionListener { completed = true }
        if (from > 0) mp.seekTo(from)
        player = mp
        path = p
        completed = false
        applySpeed()
        mp.start()
        return state()
    }

    private fun setSpeed(v: Float) {
        speed = v.coerceIn(0.5f, 2.5f)
        applySpeed()
    }

    private fun applySpeed() {
        val mp = player ?: return
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return
        val wasPlaying = mp.isPlaying
        mp.playbackParams = PlaybackParams().setSpeed(speed)
        // Setting params on a paused player starts it - keep it paused.
        if (!wasPlaying && mp.isPlaying) mp.pause()
    }

    private fun release() {
        player?.let {
            try { it.stop() } catch (_: Exception) {}
            it.release()
        }
        player = null
        path = null
        completed = false
    }

    private fun state(): Map<String, Any?> {
        val mp = player
        return mapOf(
            "path" to path,
            "playing" to (mp?.isPlaying ?: false),
            "pos" to (if (mp == null) 0 else if (completed) mp.duration else mp.currentPosition),
            "dur" to (mp?.duration ?: 0),
            "done" to completed,
            "speed" to speed.toDouble(),
        )
    }
}
