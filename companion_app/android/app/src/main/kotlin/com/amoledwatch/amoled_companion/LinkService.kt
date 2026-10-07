package com.amoledwatch.amoled_companion

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder

/**
 * Foreground service that keeps the app process (and with it the Flutter
 * engine and its BLE connection) alive while the watch is connected.
 * It does no work itself - it only shows the persistent notification.
 */
class LinkService : Service() {

    companion object {
        private const val CHANNEL_ID = "watch_link"
        private const val NOTIF_ID = 4201
        const val EXTRA_TITLE = "title"
        const val EXTRA_TEXT = "text"

        fun start(ctx: Context, title: String, text: String): Boolean {
            // A connectedDevice foreground service needs Bluetooth permission
            // on Android 12+; starting without it would crash the app.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S &&
                ctx.checkSelfPermission(android.Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED) {
                return false
            }
            val i = Intent(ctx, LinkService::class.java)
                .putExtra(EXTRA_TITLE, title)
                .putExtra(EXTRA_TEXT, text)
            return try {
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) ctx.startForegroundService(i)
                else ctx.startService(i)
                true
            } catch (e: Exception) {
                // Android 12+ refuses to start a foreground service while the
                // app is in the background; the next start from the UI works.
                false
            }
        }

        fun stop(ctx: Context) {
            ctx.stopService(Intent(ctx, LinkService::class.java))
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val title = intent?.getStringExtra(EXTRA_TITLE) ?: "AmoledWatch connected"
        val text = intent?.getStringExtra(EXTRA_TEXT) ?: "Notifications and controls are linked"
        val notif = buildNotification(title, text)
        try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(NOTIF_ID, notif, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
            } else {
                startForeground(NOTIF_ID, notif)
            }
        } catch (e: Exception) {
            stopSelf()
        }
        return START_STICKY
    }

    @Suppress("DEPRECATION")
    private fun newBuilder(): Notification.Builder =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) Notification.Builder(this, CHANNEL_ID)
        else Notification.Builder(this)

    private fun buildNotification(title: String, text: String): Notification {
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(CHANNEL_ID, "Watch link", NotificationManager.IMPORTANCE_LOW)
            ch.description = "Shown while the watch is connected"
            ch.setShowBadge(false)
            nm.createNotificationChannel(ch)
        }
        val open = PendingIntent.getActivity(
            this, 0,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        )
        return newBuilder().setContentTitle(title)
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentIntent(open)
            .setOngoing(true)
            .build()
    }
}
