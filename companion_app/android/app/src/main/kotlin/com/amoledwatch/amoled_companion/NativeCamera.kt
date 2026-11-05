package com.amoledwatch.amoled_companion

import android.app.Activity
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.graphics.Bitmap
import android.graphics.PixelFormat
import android.hardware.display.DisplayManager
import android.hardware.display.VirtualDisplay
import android.media.ImageReader
import android.media.projection.MediaProjection
import android.media.projection.MediaProjectionManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.Looper
import android.provider.MediaStore
import android.provider.Settings
import android.view.WindowManager
import android.widget.Toast
import io.flutter.plugin.common.EventChannel
import java.io.ByteArrayOutputStream

/**
 * The watch as a remote for the phone's own camera app (or any camera app):
 *  - another app opens the camera -> a notification asks "use the watch?"
 *    (prompt); tapping it opens CameraLinkActivity, a see-through screen over
 *    the camera app that asks to share the screen (for the live view);
 *  - the AmoledWatch Camera Helper, a separate small app (camera_helper/, in
 *    each release) with an accessibility service the user turns on once,
 *    presses the camera app's shutter and flip buttons and pinches to zoom.
 *    It's not in this app: Play Protect blocks installing an app with an
 *    accessibility service from a browser or file manager. Asked through an
 *    ordered broadcast only apps signed like this one may send
 *    (com.amoledwatch.permission.CAMERA_HELPER, defined here);
 *  - ScreenView captures the screen (MediaProjection) a few times a second,
 *    crops the viewfinder and gives the JPEG to Dart, which sends it to the
 *    watch like the app's own camera remote does.
 * Dart gets {"k":"state",...} and {"k":"frame","jpeg":...} on
 * com.amoledwatch.native/camnative (PhoneBridge).
 */
object NativeCamera {
    private val main = Handler(Looper.getMainLooper())
    var sink: EventChannel.EventSink? = null
    @Volatile var active = false
    @Volatile var view = false
    private const val CH = "camera_remote"
    private const val NOTIF = 4108
    private const val PREFS = "native_camera"

    const val HELPER = "com.amoledwatch.camerahelper"
    private const val HELPER_URL = "https://github.com/MikeCheek/CustomOS-ESP32-S3-Amoled/releases/latest"

    fun helperInstalled(ctx: Context): Boolean = try {
        ctx.packageManager.getPackageInfo(HELPER, 0)
        true
    } catch (_: Exception) {
        false
    }

    /** The helper is installed and its accessibility service is on. */
    fun accessOn(ctx: Context): Boolean {
        if (!helperInstalled(ctx)) return false
        val list = Settings.Secure.getString(ctx.contentResolver, Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES) ?: return false
        return list.split(':').any { it.startsWith("$HELPER/", ignoreCase = true) && it.contains("HelperAccess") }
    }

    /** The Accessibility setting, or - without the helper - where to get it. */
    fun openAccessSettings(ctx: Context) {
        val i = if (helperInstalled(ctx)) Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS)
                else Intent(Intent.ACTION_VIEW, android.net.Uri.parse(HELPER_URL))
        try { ctx.startActivity(i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)) } catch (_: Exception) {}
    }

    /** Asks the helper; [done] gets whether it did it (and its answer, for "front"), on the main thread. */
    private fun askHelper(ctx: Context, a: String, done: (Boolean, String?) -> Unit) {
        if (!helperInstalled(ctx)) { done(false, null); return }
        val i = Intent("com.amoledwatch.camerahelper.ACT")
            .setClassName(HELPER, "$HELPER.ActionReceiver")
            .putExtra("a", a)
        val back = object : android.content.BroadcastReceiver() {
            override fun onReceive(c: Context, r: Intent) { done(resultCode == Activity.RESULT_OK, resultData) }
        }
        try {
            ctx.sendOrderedBroadcast(i, null, back, main, Activity.RESULT_CANCELED, null, null)
        } catch (_: Exception) {
            done(false, null)
        }
    }

    fun promptEnabled(ctx: Context) = ctx.getSharedPreferences(PREFS, 0).getBoolean("ask", true)
    fun setPromptEnabled(ctx: Context, on: Boolean) {
        ctx.getSharedPreferences(PREFS, 0).edit().putBoolean("ask", on).apply()
        if (!on) cancelPrompt(ctx)
    }

    /** The apps that answer "take a photo": a video call or face unlock using the camera isn't asked about. */
    private fun cameraApps(ctx: Context): Set<String> {
        val pm = ctx.packageManager
        val out = mutableSetOf<String>()
        for (a in listOf(MediaStore.INTENT_ACTION_STILL_IMAGE_CAMERA, MediaStore.ACTION_IMAGE_CAPTURE,
                         MediaStore.INTENT_ACTION_VIDEO_CAMERA)) {
            try {
                @Suppress("DEPRECATION")
                pm.queryIntentActivities(Intent(a), 0).forEach { out.add(it.activityInfo.packageName) }
            } catch (_: Exception) {}
        }
        return out
    }

    /** Another app has the camera: ask (heads-up) whether the watch should run it. */
    fun prompt(ctx: Context, force: Boolean = false) {
        if (active || (!force && !promptEnabled(ctx))) return
        if (force || !accessOn(ctx)) { notifyPrompt(ctx); return }
        // with the helper on, it knows the app in front: a video call isn't asked about
        askHelper(ctx, "front") { ok, front ->
            if (!ok || front.isNullOrEmpty() || front == ctx.packageName || front in cameraApps(ctx)) notifyPrompt(ctx)
        }
    }

    private fun notifyPrompt(ctx: Context) {
        if (active) return
        val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (nm.getNotificationChannel(CH) == null) {
            nm.createNotificationChannel(NotificationChannel(CH, "Camera remote", NotificationManager.IMPORTANCE_HIGH))
        }
        fun link(code: Int, extra: String) = PendingIntent.getActivity(ctx, code,
            Intent(ctx, CameraLinkActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_NO_ANIMATION)
                .putExtra("do", extra),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val use = link(11, "use")
        val n = Notification.Builder(ctx, CH)
            .setSmallIcon(ctx.applicationInfo.icon)
            .setContentTitle("Camera open")
            .setContentText("Use the watch as its remote, with the view on the watch?")
            .setContentIntent(use)
            .addAction(Notification.Action.Builder(null as android.graphics.drawable.Icon?, "Use the watch", use).build())
            .addAction(Notification.Action.Builder(null as android.graphics.drawable.Icon?, "Don't ask", link(12, "never")).build())
            .setOnlyAlertOnce(true)
            .setAutoCancel(true)
            .setCategory(Notification.CATEGORY_RECOMMENDATION)
            .build()
        try { nm.notify(NOTIF, n) } catch (_: SecurityException) {}
    }

    fun cancelPrompt(ctx: Context) {
        (ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).cancel(NOTIF)
    }

    fun start(ctx: Context, withView: Boolean) {
        active = true
        view = withView
        cancelPrompt(ctx)
        emit(ctx)
    }

    fun stop(ctx: Context) {
        val was = active
        active = false
        view = false
        cancelPrompt(ctx)
        ctx.stopService(Intent(ctx, ScreenView::class.java))
        if (was) emit(ctx)
    }

    fun viewEnded(ctx: Context) {
        if (!view) return
        view = false
        if (active) emit(ctx)
    }

    fun emit(ctx: Context) {
        val m = mapOf("k" to "state", "on" to active, "view" to view, "acc" to accessOn(ctx))
        main.post { sink?.success(m) }
    }

    fun frame(jpeg: ByteArray) { main.post { sink?.success(mapOf("k" to "frame", "jpeg" to jpeg)) } }

    /** A button from the watch; [done] gets false when it couldn't be pressed (no helper, or no such button). */
    fun action(ctx: Context, a: String, done: (Boolean) -> Unit) = askHelper(ctx, a) { ok, _ -> done(ok) }
}

/**
 * See-through, over the camera app: from the notification. Asks to share the
 * screen for the live view (if the accessibility service is on), then goes.
 */
class CameraLinkActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (intent.getStringExtra("do") == "never") {
            NativeCamera.setPromptEnabled(this, false)
            Toast.makeText(this, "The watch won't ask again (turn it back on in the app: Camera remote)",
                Toast.LENGTH_LONG).show()
            finish()
            return
        }
        if (!NativeCamera.accessOn(this)) {
            // the view works without it; the buttons need the Camera Helper
            Toast.makeText(this, if (NativeCamera.helperInstalled(this))
                "For the watch's buttons: turn on \"AmoledWatch camera remote\" in Accessibility"
            else "The view goes to the watch. For its buttons, install the AmoledWatch Camera Helper", Toast.LENGTH_LONG).show()
        }
        val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
        try {
            @Suppress("DEPRECATION")
            startActivityForResult(mpm.createScreenCaptureIntent(), 1)
        } catch (_: Exception) {
            NativeCamera.start(this, false)
            finish()
        }
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        var view = false
        if (resultCode == RESULT_OK && data != null) {
            try {
                startForegroundService(Intent(this, ScreenView::class.java)
                    .putExtra("code", resultCode).putExtra("data", data))
                view = true
            } catch (_: Exception) {}
        }
        NativeCamera.start(this, view)   // the buttons work without the view too
        finish()
    }
}

/** The screen, a few times a second, cropped to the viewfinder: the watch's live view. */
class ScreenView : Service() {
    private var proj: MediaProjection? = null
    private var display: VirtualDisplay? = null
    private var reader: ImageReader? = null
    private var thread: HandlerThread? = null
    private var handler: Handler? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (nm.getNotificationChannel("camera_view") == null) {
            nm.createNotificationChannel(NotificationChannel("camera_view", "Camera on the watch", NotificationManager.IMPORTANCE_LOW))
        }
        val n = Notification.Builder(this, "camera_view")
            .setSmallIcon(applicationInfo.icon)
            .setContentTitle("Camera view on the watch")
            .setContentText("Closing the camera stops it")
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= 29) startForeground(4109, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION)
        else startForeground(4109, n)

        val code = intent?.getIntExtra("code", 0) ?: 0
        val data: Intent? = if (Build.VERSION.SDK_INT >= 33) intent?.getParcelableExtra("data", Intent::class.java)
                            else @Suppress("DEPRECATION") intent?.getParcelableExtra("data")
        if (data == null || proj != null) {
            if (proj == null) stopSelf()
            return START_NOT_STICKY
        }
        try {
            val mpm = getSystemService(Context.MEDIA_PROJECTION_SERVICE) as MediaProjectionManager
            val p = mpm.getMediaProjection(code, data) ?: run { stopSelf(); return START_NOT_STICKY }
            proj = p
            val t = HandlerThread("watch-view").apply { start() }
            thread = t
            val h = Handler(t.looper)
            handler = h
            p.registerCallback(object : MediaProjection.Callback() {
                override fun onStop() { stopSelf() }
            }, h)
            val wm = getSystemService(Context.WINDOW_SERVICE) as WindowManager
            val (sw, sh) = if (Build.VERSION.SDK_INT >= 30) wm.currentWindowMetrics.bounds.let { it.width() to it.height() }
                           else resources.displayMetrics.let { it.widthPixels to it.heightPixels }
            // small: the watch shows 144 px of it
            val w = 360
            val hgt = (360L * sh / sw).toInt() and 1.inv()
            val r = ImageReader.newInstance(w, hgt, PixelFormat.RGBA_8888, 2)
            reader = r
            display = p.createVirtualDisplay("watch-view", w, hgt, resources.displayMetrics.densityDpi / 3,
                DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, r.surface, null, h)
            h.postDelayed(tick, 300)
        } catch (e: Exception) {
            stopSelf()
        }
        return START_NOT_STICKY
    }

    private val tick: Runnable = object : Runnable {
        override fun run() {
            if (!NativeCamera.active) { stopSelf(); return }
            grab()
            handler?.postDelayed(this, 450)
        }
    }

    private fun grab() {
        val img = try { reader?.acquireLatestImage() } catch (_: Exception) { null } ?: return
        try {
            val plane = img.planes[0]
            val pxStride = plane.pixelStride
            val rowPad = plane.rowStride - pxStride * img.width
            val full = Bitmap.createBitmap(img.width + rowPad / pxStride, img.height, Bitmap.Config.ARGB_8888)
            full.copyPixelsFromBuffer(plane.buffer)
            val w = img.width
            val h = img.height
            // the viewfinder: a square across the width, a bit above the middle (portrait),
            // or the full height in the middle (landscape)
            val side: Int
            val x0: Int
            val y0: Int
            if (h >= w) {
                side = w
                x0 = 0
                y0 = ((h * 0.42f) - side / 2f).toInt().coerceIn(0, h - side)
            } else {
                side = h
                x0 = (w - side) / 2
                y0 = 0
            }
            val sq = Bitmap.createBitmap(full, x0, y0, side, side)
            val small = Bitmap.createScaledBitmap(sq, 144, 144, true)
            val out = ByteArrayOutputStream()
            small.compress(Bitmap.CompressFormat.JPEG, 60, out)
            NativeCamera.frame(out.toByteArray())
            if (small !== sq) small.recycle()
            if (sq !== full) sq.recycle()
            full.recycle()
        } catch (_: Exception) {
        } finally {
            img.close()
        }
    }

    override fun onDestroy() {
        handler?.removeCallbacksAndMessages(null)
        try { display?.release() } catch (_: Exception) {}
        try { reader?.close() } catch (_: Exception) {}
        try { proj?.stop() } catch (_: Exception) {}
        thread?.quitSafely()
        proj = null
        NativeCamera.viewEnded(this)
        super.onDestroy()
    }
}
