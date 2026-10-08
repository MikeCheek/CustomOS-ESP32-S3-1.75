package com.amoledwatch.amoled_companion

import android.view.WindowManager
import io.flutter.embedding.android.FlutterActivity
import java.lang.ref.WeakReference

/**
 * Thin UI host: attaches to the engine CompanionApp created at process
 * start and leaves it running when the Activity is destroyed, so the watch
 * link survives closing the app. All native channels live in PhoneBridge.
 */
class MainActivity : FlutterActivity() {
    companion object {
        private var current: WeakReference<MainActivity>? = null

        /** Keeps the screen on while true (MemoRecorder: Android silences
         *  the microphone for apps in the background, e.g. a locked phone). */
        fun keepScreenOn(on: Boolean) {
            val a = current?.get() ?: return
            a.runOnUiThread {
                if (on) a.window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                else a.window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
            }
        }
    }

    override fun getCachedEngineId(): String = CompanionApp.ENGINE_ID

    override fun shouldDestroyEngineWithHost(): Boolean = false

    override fun onResume() {
        super.onResume()
        current = WeakReference(this)
    }
}
