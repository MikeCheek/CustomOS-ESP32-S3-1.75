package com.amoledwatch.amoled_companion

import android.app.PendingIntent
import android.appwidget.AppWidgetManager
import android.appwidget.AppWidgetProvider
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.view.View
import android.widget.RemoteViews

/**
 * Home-screen widget: watch connection, battery and today's steps, with a
 * "Find" button that rings the watch.
 *
 * The Dart side pushes the numbers (PhoneBridge "updateWidget" -> store());
 * they're kept in SharedPreferences so the widget can redraw while the app
 * isn't running.
 */
class WatchWidget : AppWidgetProvider() {

    companion object {
        private const val PREFS = "watch_widget"
        private const val ACTION_FIND = "com.amoledwatch.WIDGET_FIND"

        fun store(ctx: Context, data: Map<String, Any?>) {
            val e = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            fun int(k: String) = (data[k] as? Number)?.toInt()
            data["name"]?.let { e.putString("name", it.toString()) }
            int("battery")?.let { e.putInt("battery", it) }
            int("steps")?.let {
                e.putInt("steps", it)
                e.putString("stepsDay", java.text.SimpleDateFormat("yyyyMMdd", java.util.Locale.US).format(java.util.Date()))
            }
            int("goal")?.let { e.putInt("goal", it) }
            (data["connected"] as? Boolean)?.let { e.putBoolean("connected", it) }
            (data["charging"] as? Boolean)?.let { e.putBoolean("charging", it) }
            data["next"]?.let { e.putString("next", it.toString()) }
            e.putLong("updated", System.currentTimeMillis())
            e.apply()
            refresh(ctx)
        }

        fun refresh(ctx: Context) {
            val mgr = AppWidgetManager.getInstance(ctx)
            val ids = mgr.getAppWidgetIds(ComponentName(ctx, WatchWidget::class.java))
            if (ids.isEmpty()) return
            for (id in ids) mgr.updateAppWidget(id, build(ctx))
        }

        private fun build(ctx: Context): RemoteViews {
            val p = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            val v = RemoteViews(ctx.packageName, R.layout.watch_widget)
            // The app pushes updates at least every few minutes while it's
            // linked (watch battery reports); older data means it's gone.
            val fresh = System.currentTimeMillis() - p.getLong("updated", 0L) < 20 * 60_000L
            val connected = p.getBoolean("connected", false) && fresh
            val battery = p.getInt("battery", -1)
            val today = java.text.SimpleDateFormat("yyyyMMdd", java.util.Locale.US).format(java.util.Date())
            val steps = if (p.getString("stepsDay", "") == today) p.getInt("steps", -1) else -1
            val goal = p.getInt("goal", 8000).coerceAtLeast(1)
            val name = p.getString("name", null) ?: "AmoledWatch"
            val next = p.getString("next", "") ?: ""

            v.setTextViewText(R.id.w_name, name)
            v.setTextViewText(R.id.w_status, when {
                connected && p.getBoolean("charging", false) -> "Connected · charging"
                connected -> "Connected"
                else -> "Not connected"
            })
            v.setTextColor(R.id.w_status, if (connected) 0xFF2EE6A0.toInt() else 0xFF8A8A96.toInt())
            v.setTextViewText(R.id.w_battery, if (battery >= 0) "$battery%" else "--")
            v.setProgressBar(R.id.w_battery_bar, 100, battery.coerceIn(0, 100), false)
            v.setTextViewText(R.id.w_steps, if (steps >= 0) "%,d".format(steps) else "--")
            v.setProgressBar(R.id.w_steps_bar, goal, steps.coerceIn(0, goal), false)
            if (next.isNotEmpty()) {
                v.setViewVisibility(R.id.w_next, View.VISIBLE)
                v.setTextViewText(R.id.w_next, next)
            } else {
                v.setViewVisibility(R.id.w_next, View.GONE)
            }

            val open = Intent(ctx, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            v.setOnClickPendingIntent(R.id.w_root, PendingIntent.getActivity(
                ctx, 0, open, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT))
            val find = Intent(ctx, WatchWidget::class.java).setAction(ACTION_FIND)
            v.setOnClickPendingIntent(R.id.w_find, PendingIntent.getBroadcast(
                ctx, 1, find, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT))
            v.setViewVisibility(R.id.w_find, if (connected) View.VISIBLE else View.GONE)
            return v
        }
    }

    override fun onUpdate(ctx: Context, mgr: AppWidgetManager, ids: IntArray) {
        for (id in ids) mgr.updateAppWidget(id, build(ctx))
    }

    override fun onReceive(ctx: Context, intent: Intent) {
        super.onReceive(ctx, intent)
        if (intent.action == ACTION_FIND) {
            // The Flutter engine lives in CompanionApp; the Dart side rings the watch.
            CompanionApp.instance.bridge.onWidgetAction("find")
        }
    }
}
