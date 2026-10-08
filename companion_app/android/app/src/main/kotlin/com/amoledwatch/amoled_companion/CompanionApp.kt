package com.amoledwatch.amoled_companion

import android.app.Application
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.embedding.engine.FlutterEngineCache
import io.flutter.embedding.engine.dart.DartExecutor

/**
 * Owns the one Flutter engine for the whole process.
 *
 * The engine is created here (not by MainActivity) and kept in
 * FlutterEngineCache, so the Dart side - the BLE connection to the watch,
 * notification forwarding, the phone link - keeps running after the UI is
 * closed. LinkService (a foreground service) keeps the process alive while
 * the watch is connected; MainActivity just attaches to this engine.
 */
class CompanionApp : Application() {

    companion object {
        const val ENGINE_ID = "companion_engine"
        lateinit var instance: CompanionApp
            private set
    }

    lateinit var bridge: PhoneBridge
        private set

    lateinit var player: MemoPlayer
        private set

    lateinit var recorder: MemoRecorder
        private set

    override fun onCreate() {
        super.onCreate()
        instance = this
        val engine = FlutterEngine(this)
        // Channels are registered once, against the application context, so
        // they keep working when there is no Activity.
        bridge = PhoneBridge(this, engine.dartExecutor.binaryMessenger)
        player = MemoPlayer(this, engine.dartExecutor.binaryMessenger)
        recorder = MemoRecorder(this, engine.dartExecutor.binaryMessenger)
        engine.dartExecutor.executeDartEntrypoint(DartExecutor.DartEntrypoint.createDefault())
        FlutterEngineCache.getInstance().put(ENGINE_ID, engine)
    }
}
