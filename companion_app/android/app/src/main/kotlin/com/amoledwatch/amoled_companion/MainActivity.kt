package com.amoledwatch.amoled_companion

import io.flutter.embedding.android.FlutterActivity

/**
 * Thin UI host: attaches to the engine CompanionApp created at process
 * start and leaves it running when the Activity is destroyed, so the watch
 * link survives closing the app. All native channels live in PhoneBridge.
 */
class MainActivity : FlutterActivity() {
    override fun getCachedEngineId(): String = CompanionApp.ENGINE_ID

    override fun shouldDestroyEngineWithHost(): Boolean = false
}
