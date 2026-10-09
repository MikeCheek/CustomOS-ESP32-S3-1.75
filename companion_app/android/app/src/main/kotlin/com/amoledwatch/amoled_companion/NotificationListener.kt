package com.amoledwatch.amoled_companion

import android.app.Notification
import android.app.RemoteInput
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.os.Bundle
import android.service.notification.NotificationListenerService
import android.service.notification.StatusBarNotification

/**
 * Phone notifications -> watch, and the watch's actions back.
 *
 * Every forwarded notification gets a small numeric id (its key's hash) so
 * the watch can ask for it to be dismissed, replied to, or - for call
 * notifications - answered/declined. Those actions are done here through
 * the notification's own actions, so no extra phone permissions are needed.
 */
class NotificationListener : NotificationListenerService() {

    companion object {
        var instance: NotificationListener? = null
            private set
        @Volatile var notificationsEnabled = false
        @Volatile var callsEnabled = true
        val allowedPackages: MutableSet<String> = java.util.Collections.synchronizedSet(mutableSetOf<String>())

        // Turn-by-turn navigation (Google Maps & co.), independent of the
        // "forward notifications" switch.
        @Volatile var navigationEnabled = true
        private val navListeners = mutableListOf<NotificationCallback>()
        fun addNavListener(cb: NotificationCallback) { synchronized(navListeners) { if (cb !in navListeners) navListeners.add(cb) } }
        fun removeNavListener(cb: NotificationCallback) { synchronized(navListeners) { navListeners.remove(cb) } }

        private val NAV_PACKAGES = setOf(
            "com.google.android.apps.maps",
            "com.google.android.apps.mapslite",
            "com.waze",
            "net.osmand", "net.osmand.plus",
            "com.here.app.maps",
        )
        // [\s  ]: many locales (Italian too) format "200 m" with a no-break space.
        private val DISTANCE = Regex("""^[\s  ]*[\d.,]+[\s  ]?(m|km|mi|ft|yd)\b.*""", RegexOption.IGNORE_CASE)
        private val DIST_SPLIT = Regex("""^[\s  ]*([\d.,]+[\s  ]?(?:m|km|mi|ft|yd))\b[\s  ]*[·\-–]?[\s  ]*(.*)$""", RegexOption.IGNORE_CASE)

        private val listeners = mutableListOf<NotificationCallback>()
        fun addListener(cb: NotificationCallback) { synchronized(listeners) { if (cb !in listeners) listeners.add(cb) } }
        fun removeListener(cb: NotificationCallback) { synchronized(listeners) { listeners.remove(cb) } }

        private val active = java.util.concurrent.ConcurrentHashMap<Int, StatusBarNotification>()

        fun idOf(sbn: StatusBarNotification): Int = sbn.key.hashCode() and 0x7fffffff

        private val ANSWER_WORDS = listOf("answer", "accept", "rispondi", "accetta", "responder", "répondre", "annehmen")
        private val DECLINE_WORDS = listOf("decline", "reject", "hang up", "end", "rifiuta", "riaggancia", "termina",
            "rechazar", "colgar", "refuser", "raccrocher", "ablehnen", "auflegen")

        /** Runs [action] on the notification [id]. Returns false if it's gone or has no such action. */
        fun perform(id: Int, action: String, text: String?): Boolean {
            val svc = instance ?: return false
            val sbn = active[id] ?: return false
            return when (action) {
                "dismiss" -> {
                    try { svc.cancelNotification(sbn.key); true } catch (e: Exception) { false }
                }
                "reply" -> svc.reply(sbn, text ?: return false)
                "answer" -> svc.callAction(sbn, answer = true)
                "decline" -> svc.callAction(sbn, answer = false)
                else -> false
            }
        }
    }

    fun interface NotificationCallback {
        fun onEvent(event: Map<String, Any>)
    }

    override fun onListenerConnected() {
        super.onListenerConnected()
        instance = this
    }

    override fun onListenerDisconnected() {
        super.onListenerDisconnected()
        instance = null
    }

    private fun emit(event: Map<String, Any>) {
        val copy = synchronized(listeners) { listeners.toList() }
        for (cb in copy) cb.onEvent(event)
    }

    private fun emitNav(event: Map<String, Any>) {
        val copy = synchronized(navListeners) { navListeners.toList() }
        for (cb in copy) cb.onEvent(event)
    }

    private val navKeys = java.util.Collections.synchronizedSet(mutableSetOf<String>())
    private var lastNav = ""

    /** Directions, not just any ongoing notification of a maps app (location
     *  sharing, offline map downloads...). Apps that don't set the
     *  navigation category must at least show a distance or a maneuver icon. */
    private fun isNavigation(sbn: StatusBarNotification, n: Notification): Boolean {
        if ((n.flags and Notification.FLAG_ONGOING_EVENT) == 0) return false
        if (n.category == Notification.CATEGORY_NAVIGATION) return true
        if (sbn.packageName !in NAV_PACKAGES) return false
        val title = n.extras?.getCharSequence(Notification.EXTRA_TITLE)?.toString() ?: ""
        return DISTANCE.matches(title) || n.getLargeIcon() != null
    }

    /** Navigation notification -> {type: nav, dist, instr, extra, icon (48x48, 1 bit), iconHash}. */
    private fun handleNavigation(sbn: StatusBarNotification, n: Notification) {
        val extras = n.extras ?: return
        val title = extras.getCharSequence(Notification.EXTRA_TITLE)?.toString()?.trim() ?: ""
        val text = (extras.getCharSequence(Notification.EXTRA_TEXT)
            ?: extras.getCharSequence(Notification.EXTRA_BIG_TEXT))?.toString()?.trim() ?: ""
        val sub = extras.getCharSequence(Notification.EXTRA_SUB_TEXT)?.toString()?.trim() ?: ""
        if (title.isEmpty() && text.isEmpty()) return
        // Maps puts the distance to the next turn in the title ("200 m") and
        // the maneuver in the text; other apps put everything in the title.
        val dist: String
        val instr: String
        val m = if (DISTANCE.matches(title)) DIST_SPLIT.find(title) else null
        if (m != null) {
            dist = m.groupValues[1]
            instr = listOf(m.groupValues[2], text).filter { it.isNotEmpty() }.joinToString(" - ")
        } else {
            dist = ""
            instr = if (text.isNotEmpty()) "$title - $text" else title
        }
        val icon = maneuverIcon(n)
        val hash = icon?.contentHashCode()?.and(0x7fffffff) ?: 0
        val key = "$dist|$instr|$sub|$hash"
        navKeys.add(sbn.key)
        if (key == lastNav) return
        lastNav = key
        val ev = hashMapOf<String, Any>(
            "type" to "nav",
            "app" to appLabel(sbn.packageName),
            "dist" to dist,
            "instr" to instr,
            "extra" to sub,
            "iconHash" to hash,
        )
        if (icon != null) ev["icon"] = icon
        emitNav(ev)
    }

    /** The maneuver arrow (the large icon) as a 48x48 1-bit mask, rows MSB first. */
    private fun maneuverIcon(n: Notification): ByteArray? {
        val ic = n.getLargeIcon() ?: return null
        return try {
            val d = ic.loadDrawable(this) ?: return null
            val size = 48
            val bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
            d.setBounds(0, 0, size, size)
            d.draw(Canvas(bmp))
            val px = IntArray(size * size)
            bmp.getPixels(px, 0, size, 0, 0, size, size)
            bmp.recycle()
            // Arrow = bright opaque pixels. An opaque light tile with a dark
            // arrow gets inverted; a dark glyph on transparent uses alpha.
            var bright = 0
            val on = BooleanArray(px.size) { i ->
                val p = px[i]
                val lum = (Color.red(p) * 3 + Color.green(p) * 6 + Color.blue(p)) / 10
                val b = Color.alpha(p) > 128 && lum > 140
                if (b) bright++
                b
            }
            if (bright > px.size * 6 / 10) {
                for (i in on.indices) on[i] = !on[i] && Color.alpha(px[i]) > 128
            } else if (bright == 0) {
                for (i in on.indices) on[i] = Color.alpha(px[i]) > 128
            }
            if (on.none { it }) return null
            val out = ByteArray(size * size / 8)
            for (i in on.indices) if (on[i]) out[i / 8] = (out[i / 8].toInt() or (0x80 shr (i % 8))).toByte()
            out
        } catch (e: Exception) {
            null
        }
    }

    override fun onNotificationPosted(sbn: StatusBarNotification) {
        if (sbn.packageName == packageName) return
        val n = sbn.notification ?: return
        if (navigationEnabled && isNavigation(sbn, n)) {
            handleNavigation(sbn, n)
            return
        }
        val isCall = n.category == Notification.CATEGORY_CALL

        if (isCall) {
            if (!callsEnabled) return
        } else {
            if (!notificationsEnabled) return
            if (allowedPackages.isNotEmpty() && sbn.packageName !in allowedPackages) return
            // Group summaries duplicate their children; ongoing ones are
            // music players, downloads, navigation - not messages.
            if ((n.flags and Notification.FLAG_GROUP_SUMMARY) != 0) return
            if ((n.flags and Notification.FLAG_ONGOING_EVENT) != 0) return
        }

        val extras = n.extras ?: return
        val title = extras.getCharSequence(Notification.EXTRA_TITLE)?.toString() ?: ""
        val text = (extras.getCharSequence(Notification.EXTRA_TEXT)
            ?: extras.getCharSequence(Notification.EXTRA_BIG_TEXT))?.toString() ?: ""
        if (title.isEmpty() && text.isEmpty()) return

        val id = idOf(sbn)
        active[id] = sbn
        val canReply = n.actions?.any { a -> a.remoteInputs?.any { it.allowFreeFormInput } == true } == true

        emit(mapOf(
            "type" to "posted",
            "id" to id,
            "package" to sbn.packageName,
            "app" to appLabel(sbn.packageName),
            "icon" to WatchImages.iconHash(sbn.packageName),
            "title" to title,
            "text" to text,
            "isCall" to isCall,
            "ongoing" to ((n.flags and Notification.FLAG_ONGOING_EVENT) != 0),
            "canReply" to canReply,
            // Incoming calls offer Answer + Decline; an ongoing one only Hang up.
            "callRinging" to (isCall && (n.actions?.size ?: 0) >= 2),
        ))
    }

    override fun onNotificationRemoved(sbn: StatusBarNotification) {
        if (navKeys.remove(sbn.key)) {
            if (navKeys.isEmpty()) {
                lastNav = ""
                emitNav(mapOf("type" to "navEnd"))
            }
            return
        }
        val id = idOf(sbn)
        if (active.remove(id) != null) {
            emit(mapOf(
                "type" to "removed",
                "id" to id,
                "isCall" to (sbn.notification?.category == Notification.CATEGORY_CALL),
            ))
        }
    }

    private fun appLabel(pkg: String): String = try {
        val ai = packageManager.getApplicationInfo(pkg, 0)
        packageManager.getApplicationLabel(ai).toString()
    } catch (e: Exception) {
        pkg.substringAfterLast('.').replaceFirstChar { it.uppercase() }
    }

    private fun reply(sbn: StatusBarNotification, text: String): Boolean {
        val actions = sbn.notification?.actions ?: return false
        val action = actions.firstOrNull { a -> a.remoteInputs?.any { it.allowFreeFormInput } == true } ?: return false
        val inputs = action.remoteInputs ?: return false
        return try {
            val intent = Intent()
            val results = Bundle()
            for (ri in inputs) results.putCharSequence(ri.resultKey, text)
            RemoteInput.addResultsToIntent(inputs, intent, results)
            action.actionIntent.send(this, 0, intent)
            true
        } catch (e: Exception) {
            false
        }
    }

    private fun callAction(sbn: StatusBarNotification, answer: Boolean): Boolean {
        val actions = sbn.notification?.actions ?: return false
        if (actions.isEmpty()) return false
        val words = if (answer) ANSWER_WORDS else DECLINE_WORDS
        var pick = actions.firstOrNull { a ->
            val t = a.title?.toString()?.lowercase() ?: ""
            words.any { t.contains(it) }
        }
        if (pick == null) {
            // Dialers put Decline first and Answer last (CallStyle order).
            pick = if (answer) { if (actions.size >= 2) actions.last() else null } else actions.first()
        }
        if (pick == null) return false
        return try { pick.actionIntent.send(); true } catch (e: Exception) { false }
    }
}
