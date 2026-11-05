package com.amoledwatch.camerahelper

import android.accessibilityservice.AccessibilityService
import android.accessibilityservice.GestureDescription
import android.content.Context
import android.content.Intent
import android.graphics.Path
import android.graphics.Rect
import android.os.Build
import android.view.Surface
import android.view.WindowManager
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo

/** Presses the camera app's buttons for the watch (Settings > Accessibility > AmoledWatch Camera Helper). */
class HelperAccess : AccessibilityService() {
    companion object {
        @Volatile var instance: HelperAccess? = null
        // what the shutter and the flip button are called, in camera apps and a few languages
        private val SHUTTER = Regex("shutter|take (a )?(photo|picture)|capture|camera_button|shoot|scatta|otturatore|" +
            "disparador|obturador|tomar foto|déclencheur|prendre (une )?photo|auslöser|foto aufnehmen|aufnahme")
        private val FLIP = Regex("switch( to)?( front| rear| back)? camera|flip|front camera|rear camera|back camera|" +
            "selfie|toggle.?camera|switch_camera|camera_switch|cambia fotocamera|fotocamera (anteriore|posteriore)|" +
            "cambiar cámara|cámara (frontal|trasera)|changer de caméra|kamera wechseln")
        private val NOT = Regex("setting|timer|gallery|thumbnail|preview image|impostazioni|galleria|einstellung")
    }

    @Volatile var frontPackage: String? = null

    override fun onServiceConnected() {
        instance = this
    }

    override fun onUnbind(intent: Intent?): Boolean {
        instance = null
        return super.onUnbind(intent)
    }

    override fun onDestroy() {
        instance = null
        super.onDestroy()
    }

    override fun onAccessibilityEvent(e: AccessibilityEvent?) {
        if (e?.eventType == AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED) {
            e.packageName?.toString()?.let { if (it != "com.android.systemui") frontPackage = it }
        }
    }

    override fun onInterrupt() {}

    fun act(a: String): Boolean = when (a) {
        "shot" -> press(SHUTTER, true) || tapShutterSpot()
        "flip" -> press(FLIP, false)
        "zoom+" -> pinch(true)
        "zoom-" -> pinch(false)
        else -> false
    }

    private fun label(n: AccessibilityNodeInfo): String =
        listOfNotNull(n.contentDescription, n.text, n.viewIdResourceName).joinToString(" ").lowercase()

    // The visible button whose name matches (the biggest one for the shutter): clicked, or tapped.
    private fun press(re: Regex, biggest: Boolean): Boolean {
        val root = rootInActiveWindow ?: return false
        var best: AccessibilityNodeInfo? = null
        var bestArea = -1
        val queue = ArrayDeque<AccessibilityNodeInfo>()
        queue.add(root)
        var seen = 0
        while (queue.isNotEmpty() && seen < 1500) {
            val n = queue.removeFirst()
            seen++
            val l = label(n)
            if (n.isVisibleToUser && l.isNotEmpty() && re.containsMatchIn(l) && !NOT.containsMatchIn(l)) {
                val r = Rect()
                n.getBoundsInScreen(r)
                val area = r.width() * r.height()
                if (area > 0 && (best == null || (biggest && area > bestArea))) { best = n; bestArea = area }
                if (!biggest && best != null) break
            }
            for (i in 0 until n.childCount) n.getChild(i)?.let { queue.add(it) }
        }
        val b = best ?: return false
        var c: AccessibilityNodeInfo? = b
        while (c != null && !c.isClickable) c = c.parent
        if (c != null && c.performAction(AccessibilityNodeInfo.ACTION_CLICK)) return true
        val r = Rect()
        b.getBoundsInScreen(r)
        return tap(r.exactCenterX(), r.exactCenterY())
    }

    private fun screen(): Pair<Int, Int> {
        val wm = getSystemService(Context.WINDOW_SERVICE) as WindowManager
        return if (Build.VERSION.SDK_INT >= 30) {
            val b = wm.currentWindowMetrics.bounds
            b.width() to b.height()
        } else {
            val m = resources.displayMetrics
            m.widthPixels to m.heightPixels
        }
    }

    // No shutter by name: where camera apps put it (bottom centre, or the side in landscape).
    private fun tapShutterSpot(): Boolean {
        val (w, h) = screen()
        if (h >= w) return tap(w / 2f, h * 0.86f)
        @Suppress("DEPRECATION")
        val rot = (getSystemService(Context.WINDOW_SERVICE) as WindowManager).defaultDisplay.rotation
        return tap(if (rot == Surface.ROTATION_270) w * 0.1f else w * 0.9f, h / 2f)
    }

    private fun tap(x: Float, y: Float): Boolean {
        val p = Path().apply { moveTo(x, y) }
        return dispatchGesture(GestureDescription.Builder()
            .addStroke(GestureDescription.StrokeDescription(p, 0, 60)).build(), null, null)
    }

    // Two fingers apart (zoom in) or together (out), around the viewfinder's middle.
    private fun pinch(zoomIn: Boolean): Boolean {
        val (w, h) = screen()
        val cx = w / 2f
        val cy = if (h >= w) h * 0.42f else h / 2f
        val near = w * 0.06f
        val far = w * 0.28f
        val (from, to) = if (zoomIn) near to far else far to near
        val a = Path().apply { moveTo(cx - from, cy); lineTo(cx - to, cy) }
        val b = Path().apply { moveTo(cx + from, cy); lineTo(cx + to, cy) }
        return dispatchGesture(GestureDescription.Builder()
            .addStroke(GestureDescription.StrokeDescription(a, 0, 280))
            .addStroke(GestureDescription.StrokeDescription(b, 0, 280)).build(), null, null)
    }
}

