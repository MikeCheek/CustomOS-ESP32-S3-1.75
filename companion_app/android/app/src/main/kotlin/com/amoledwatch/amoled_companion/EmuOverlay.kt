package com.amoledwatch.amoled_companion

import android.annotation.SuppressLint
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PixelFormat
import android.graphics.Rect
import android.graphics.RectF
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.view.Choreographer
import android.view.Gravity
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.View
import android.view.WindowManager
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min

/**
 * The emulated watch floating over every app, round like the real one:
 *  - inside the circle: the watch's own touch (taps, swipes, holds)
 *  - the rim: drag it to move it; pinch anywhere to resize it
 *  - the little button on the rim: the side button (BOOT; hold it: PWR)
 *  - pushed past a side of the screen it tucks away, a sliver showing:
 *    tap the sliver to bring it back
 *  - dragged onto the ✕ that shows at the bottom while moving: closed
 * The firmware keeps running in the app (the emulator keeps stepping);
 * this only shows its frames (nativeFrame) and sends its touches.
 */
object EmuOverlay {
    @JvmStatic private external fun nativeFrame(buf: ByteBuffer, seq: Int): Int
    @JvmStatic private external fun nativeTouch(x: Int, y: Int, down: Int)
    @JvmStatic private external fun nativeButton(which: Int, event: Int)
    @JvmStatic private external fun nativeSize(): Int

    private var view: WatchView? = null
    private var closeTarget: CloseView? = null
    private var wm: WindowManager? = null
    private var params: WindowManager.LayoutParams? = null
    var onClosed: (() -> Unit)? = null

    fun canDraw(ctx: Context): Boolean = Build.VERSION.SDK_INT < 23 || Settings.canDrawOverlays(ctx)

    fun askPermission(ctx: Context) {
        val i = Intent(Settings.ACTION_MANAGE_OVERLAY_PERMISSION, Uri.parse("package:${ctx.packageName}"))
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        try { ctx.startActivity(i) } catch (_: Exception) {}
    }

    val showing: Boolean get() = view != null

    fun show(ctx: Context): Boolean {
        if (view != null) return true
        if (!canDraw(ctx)) return false
        try { System.loadLibrary("watchemu") } catch (_: Throwable) { return false }
        val app = ctx.applicationContext
        val w = app.getSystemService(Context.WINDOW_SERVICE) as WindowManager
        val dm = app.resources.displayMetrics
        val prefs = app.getSharedPreferences("emu_overlay", Context.MODE_PRIVATE)
        val size = prefs.getInt("size", (dm.density * 170).toInt())
        val type = if (Build.VERSION.SDK_INT >= 26) WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY
                   else @Suppress("DEPRECATION") WindowManager.LayoutParams.TYPE_PHONE
        val p = WindowManager.LayoutParams(
            size, size, type,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
            PixelFormat.TRANSLUCENT,
        )
        p.gravity = Gravity.TOP or Gravity.START
        p.x = prefs.getInt("x", dm.widthPixels - size - (dm.density * 16).toInt())
        p.y = prefs.getInt("y", (dm.heightPixels * 0.3f).toInt())
        val v = WatchView(app)
        wm = w
        params = p
        view = v
        try { w.addView(v, p) } catch (e: Exception) { view = null; return false }
        v.start()
        return true
    }

    fun hide() {
        val v = view ?: return
        v.stop()
        try { wm?.removeView(v) } catch (_: Exception) {}
        closeTarget?.let { try { wm?.removeView(it) } catch (_: Exception) {} }
        closeTarget = null
        view = null
    }

    private fun save() {
        val v = view ?: return
        val p = params ?: return
        v.context.getSharedPreferences("emu_overlay", Context.MODE_PRIVATE).edit()
            .putInt("x", p.x).putInt("y", p.y).putInt("size", p.width).apply()
    }

    // ---- the ✕ to drop it on -------------------------------------------------------
    private fun showClose(ctx: Context, show: Boolean) {
        val w = wm ?: return
        if (show && closeTarget == null) {
            val d = ctx.resources.displayMetrics.density
            val s = (64 * d).toInt()
            val t = CloseView(ctx)
            val type = params!!.type
            val p = WindowManager.LayoutParams(s, s, type,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE,
                PixelFormat.TRANSLUCENT)
            p.gravity = Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL
            p.y = (40 * d).toInt()
            try { w.addView(t, p); closeTarget = t } catch (_: Exception) {}
        } else if (!show) {
            closeTarget?.let { try { w.removeView(it) } catch (_: Exception) {} }
            closeTarget = null
        }
    }

    private fun overClose(cx: Float, cy: Float, ctx: Context): Boolean {
        val dm = ctx.resources.displayMetrics
        val tx = dm.widthPixels / 2f
        val ty = dm.heightPixels - 72 * dm.density
        val hot = hypot(cx - tx, cy - ty) < 70 * dm.density
        closeTarget?.let { if (it.hot != hot) { it.hot = hot; it.invalidate() } }
        return hot
    }

    private class CloseView(ctx: Context) : View(ctx) {
        private val d = ctx.resources.displayMetrics.density
        private val bg = Paint(Paint.ANTI_ALIAS_FLAG)
        private val fg = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE; strokeWidth = 3 * d; strokeCap = Paint.Cap.ROUND }
        var hot = false
        override fun onDraw(c: Canvas) {
            bg.color = if (hot) 0xEEE53935.toInt() else 0xCC202020.toInt()
            c.drawCircle(width / 2f, height / 2f, width / 2f, bg)
            val k = width * 0.18f
            c.drawLine(width / 2f - k, height / 2f - k, width / 2f + k, height / 2f + k, fg)
            c.drawLine(width / 2f + k, height / 2f - k, width / 2f - k, height / 2f + k, fg)
        }
    }

    // ---- the watch -------------------------------------------------------------------
    @SuppressLint("ViewConstructor", "ClickableViewAccessibility")
    private class WatchView(ctx: Context) : View(ctx) {
        val n = try { nativeSize() } catch (_: Throwable) { 466 }
        val buf: ByteBuffer = ByteBuffer.allocateDirect(n * n * 4).order(ByteOrder.nativeOrder())
        val bmp: Bitmap = Bitmap.createBitmap(n, n, Bitmap.Config.ARGB_8888)
        var seq = -1
        val paint = Paint(Paint.FILTER_BITMAP_FLAG or Paint.ANTI_ALIAS_FLAG)
        val rim = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF2A2A2E.toInt() }
        val rimHi = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0x33FFFFFF; style = Paint.Style.STROKE }
        val btn = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = 0xFF8A8A90.toInt() }
        val clip = Path()
        val d = ctx.resources.displayMetrics.density
        var running = false
        val tick = object : Choreographer.FrameCallback {
            override fun doFrame(t: Long) {
                if (!running) return
                try {
                    val s = nativeFrame(buf, seq)
                    if (s != seq) {
                        seq = s
                        buf.rewind()
                        bmp.copyPixelsFromBuffer(buf)
                        buf.rewind()
                        invalidate()
                    }
                } catch (_: Throwable) {}
                Choreographer.getInstance().postFrameCallback(this)
            }
        }

        fun start() { running = true; Choreographer.getInstance().postFrameCallback(tick) }
        fun stop() { running = false }

        // the rim's width, and the side button's place (2 o'clock)
        fun rimW() = max(6 * d, width * 0.055f)
        fun btnX() = width / 2f + width * 0.47f * 0.866f
        fun btnY() = height / 2f - height * 0.47f * 0.5f

        override fun onDraw(c: Canvas) {
            val r = width / 2f
            c.drawCircle(r, r, r, rim)
            rimHi.strokeWidth = 1.2f * d
            c.drawCircle(r, r, r - rimHi.strokeWidth, rimHi)
            val inner = r - rimW()
            clip.reset()
            clip.addCircle(r, r, inner, Path.Direction.CW)
            c.save()
            c.clipPath(clip)
            c.drawBitmap(bmp, Rect(0, 0, n, n), RectF(r - inner, r - inner, r + inner, r + inner), paint)
            c.restore()
            c.drawCircle(btnX(), btnY(), max(4 * d, width * 0.035f), btn)
        }

        // ---- touch: the watch, the rim (move), pinch (size), the button --------------
        val scaler = ScaleGestureDetector(ctx, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(det: ScaleGestureDetector): Boolean {
                val p = params ?: return true
                val dm = resources.displayMetrics
                val ns = (p.width * det.scaleFactor).toInt().coerceIn((110 * d).toInt(), min(dm.widthPixels, (420 * d).toInt()))
                p.x -= (ns - p.width) / 2
                p.y -= (ns - p.height) / 2
                p.width = ns
                p.height = ns
                try { wm?.updateViewLayout(this@WatchView, p) } catch (_: Exception) {}
                return true
            }
        })
        var mode = 0           // 0 none, 1 watch touch, 2 moving, 3 button, 4 pinching
        var downRawX = 0f; var downRawY = 0f; var startX = 0; var startY = 0
        var downAt = 0L
        val uiHandler = Handler(Looper.getMainLooper())
        var buttonLong = false
        val longPress = Runnable { buttonLong = true; nativeButton(1, 1) }   // hold the button: PWR

        fun toWatch(x: Float, y: Float): Pair<Int, Int> {
            val r = width / 2f
            val inner = r - rimW()
            val wx = ((x - (r - inner)) / (2 * inner) * n).toInt()
            val wy = ((y - (r - inner)) / (2 * inner) * n).toInt()
            return Pair(wx, wy)
        }

        fun tucked(): Boolean {
            val p = params ?: return false
            val dm = resources.displayMetrics
            return p.x < -p.width / 3 || p.x > dm.widthPixels - p.width * 2 / 3
        }

        override fun onTouchEvent(e: MotionEvent): Boolean {
            scaler.onTouchEvent(e)
            if (e.pointerCount > 1) {
                if (mode == 1) { val (wx, wy) = toWatch(e.x, e.y); nativeTouch(wx, wy, 0) }
                mode = 4
                return true
            }
            val p = params ?: return true
            val r = width / 2f
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    downRawX = e.rawX; downRawY = e.rawY; startX = p.x; startY = p.y
                    downAt = System.currentTimeMillis()
                    val dist = hypot(e.x - r, e.y - r)
                    mode = when {
                        tucked() -> 2                                                   // a sliver: pull it back
                        hypot(e.x - btnX(), e.y - btnY()) < max(14 * d, width * 0.08f) -> 3
                        dist > r - rimW() * 1.6f -> 2                                   // the rim moves it
                        else -> 1
                    }
                    if (mode == 1) { val (wx, wy) = toWatch(e.x, e.y); nativeTouch(wx, wy, 1) }
                    if (mode == 3) { buttonLong = false; uiHandler.postDelayed(longPress, 600) }
                }
                MotionEvent.ACTION_MOVE -> when (mode) {
                    1 -> { val (wx, wy) = toWatch(e.x, e.y); nativeTouch(wx, wy, 1) }
                    2 -> {
                        p.x = startX + (e.rawX - downRawX).toInt()
                        p.y = startY + (e.rawY - downRawY).toInt()
                        try { wm?.updateViewLayout(this, p) } catch (_: Exception) {}
                        val moved = hypot(e.rawX - downRawX, e.rawY - downRawY) > 12 * d
                        if (moved) showClose(context, true)
                        overClose(p.x + p.width / 2f, p.y + p.height / 2f, context)
                    }
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    when (mode) {
                        1 -> { val (wx, wy) = toWatch(e.x, e.y); nativeTouch(wx, wy, 0) }
                        3 -> { uiHandler.removeCallbacks(longPress); if (!buttonLong) nativeButton(0, 1) }
                        2 -> {
                            val dropped = overClose(p.x + p.width / 2f, p.y + p.height / 2f, context)
                            showClose(context, false)
                            if (dropped) { hide(); onClosed?.invoke(); return true }
                            settle(p, hypot(e.rawX - downRawX, e.rawY - downRawY) < 8 * d)
                        }
                    }
                    mode = 0
                    save()
                }
            }
            return true
        }

        // After a move: past a side, it tucks in with a sliver showing; a tap on
        // the sliver brings it out; otherwise kept on screen.
        fun settle(p: WindowManager.LayoutParams, wasTap: Boolean) {
            val dm = resources.displayMetrics
            val peek = (p.width * 0.28f).toInt()
            val left = p.x < -p.width / 4
            val right = p.x > dm.widthPixels - p.width * 3 / 4
            when {
                wasTap && p.x < 0 -> p.x = (8 * d).toInt()
                wasTap && p.x > dm.widthPixels - p.width -> p.x = dm.widthPixels - p.width - (8 * d).toInt()
                left -> p.x = peek - p.width
                right -> p.x = dm.widthPixels - peek
            }
            p.y = p.y.coerceIn(0, dm.heightPixels - p.height / 2)
            try { wm?.updateViewLayout(this, p) } catch (_: Exception) {}
        }
    }
}
