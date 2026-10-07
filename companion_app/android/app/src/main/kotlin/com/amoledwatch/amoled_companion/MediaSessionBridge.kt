package com.amoledwatch.amoled_companion

import android.content.ComponentName
import android.content.Context
import android.media.AudioManager
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Handler
import android.os.Looper

/**
 * Reads what the phone is playing (any app with a media session) and
 * controls it. Needs notification access - that's what lets
 * getActiveSessions() see other apps' sessions.
 */
class MediaSessionBridge(private val context: Context) {

    fun interface MediaCallback {
        fun onMediaUpdate(info: Map<String, Any>)
    }

    private val handler = Handler(Looper.getMainLooper())
    private var listener: MediaCallback? = null
    private var lastKey = ""
    private var ticksSinceSend = 0

    private val audio get() = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager

    fun startListening(cb: MediaCallback) {
        listener = cb
        lastKey = ""
        poll()
    }

    fun stopListening() {
        listener = null
        handler.removeCallbacksAndMessages(null)
    }

    private val pollRunnable: Runnable = Runnable {
        read()
        if (listener != null) poll()
    }
    private val kickRunnable: Runnable = Runnable { read() }

    private fun poll() {
        handler.removeCallbacks(pollRunnable)
        handler.postDelayed(pollRunnable, 1000)
    }

    private fun controller(): MediaController? = try {
        val msm = context.getSystemService(Context.MEDIA_SESSION_SERVICE) as MediaSessionManager
        val list = msm.getActiveSessions(ComponentName(context, NotificationListener::class.java))
        list.firstOrNull { it.playbackState?.state == PlaybackState.STATE_PLAYING } ?: list.firstOrNull()
    } catch (e: Exception) {
        null // no notification access yet
    }

    private fun read() {
        val ctrl = controller()
        val md = ctrl?.metadata
        val st = ctrl?.playbackState
        val title = md?.getString(MediaMetadata.METADATA_KEY_TITLE) ?: ""
        val artist = md?.getString(MediaMetadata.METADATA_KEY_ARTIST)
            ?: md?.getString(MediaMetadata.METADATA_KEY_ALBUM_ARTIST) ?: ""
        val album = md?.getString(MediaMetadata.METADATA_KEY_ALBUM) ?: ""
        val durMs = md?.getLong(MediaMetadata.METADATA_KEY_DURATION) ?: 0L
        val playing = st?.state == PlaybackState.STATE_PLAYING
        val posMs = st?.position ?: 0L
        val vol = audio.getStreamVolume(AudioManager.STREAM_MUSIC)
        val vmax = audio.getStreamMaxVolume(AudioManager.STREAM_MUSIC)

        val key = "$title|$artist|$playing|$vol"
        val periodic = playing && ++ticksSinceSend >= 10 // keep the watch's progress bar honest
        if (key == lastKey && !periodic) return
        lastKey = key
        ticksSinceSend = 0
        listener?.onMediaUpdate(mapOf(
            "title" to title,
            "artist" to artist,
            "album" to album,
            "playing" to playing,
            "position" to (posMs / 1000).toInt(),
            "duration" to (durMs / 1000).toInt(),
            "volume" to vol,
            "volumeMax" to vmax,
            "app" to (ctrl?.packageName ?: ""),
        ))
    }

    /** toggle | play | pause | next | prev | volUp | volDown */
    fun command(cmd: String): Boolean {
        when (cmd) {
            "volUp" -> { audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_RAISE, 0); kick(); return true }
            "volDown" -> { audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, AudioManager.ADJUST_LOWER, 0); kick(); return true }
        }
        val c = controller() ?: return false
        val tc = c.transportControls
        when (cmd) {
            "toggle" -> if (c.playbackState?.state == PlaybackState.STATE_PLAYING) tc.pause() else tc.play()
            "play" -> tc.play()
            "pause" -> tc.pause()
            "next" -> tc.skipToNext()
            "prev" -> tc.skipToPrevious()
            else -> return false
        }
        kick()
        return true
    }

    // Re-read soon after a command so the watch updates without waiting a full poll.
    private fun kick() {
        lastKey = ""
        handler.removeCallbacks(kickRunnable)
        handler.postDelayed(kickRunnable, 300)
    }
}
