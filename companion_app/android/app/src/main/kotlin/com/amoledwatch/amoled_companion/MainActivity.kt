package com.amoledwatch.amoled_companion

import android.content.Intent
import android.os.Bundle
import android.view.WindowManager
import io.flutter.embedding.android.FlutterFragmentActivity
import java.lang.ref.WeakReference

/**
 * Thin UI host: attaches to the engine CompanionApp created at process
 * start and leaves it running when the Activity is destroyed, so the watch
 * link survives closing the app. All native channels live in PhoneBridge.
 */
// A FlutterFragmentActivity (a ComponentActivity): Health Connect's permission
// screen is opened through registerForActivityResult (the health plugin).
class MainActivity : FlutterFragmentActivity() {
    companion object {
        const val EXTRA_ACTION = "amoled_action"
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

    // Opened from one of the app's notifications with something to do
    // ("camera": the watch asked for the camera remote) - handed to Dart
    // the way the home-screen widget's taps are.
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        takeAction(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        takeAction(intent)
    }

    private fun takeAction(i: Intent?) {
        val a = i?.getStringExtra(EXTRA_ACTION) ?: return
        i.removeExtra(EXTRA_ACTION)
        CompanionApp.instance.bridge.onWidgetAction(a)
    }
}
