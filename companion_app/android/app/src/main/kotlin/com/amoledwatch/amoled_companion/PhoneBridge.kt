package com.amoledwatch.amoled_companion

import android.Manifest
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.location.Location
import android.location.LocationManager
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.Ringtone
import android.media.RingtoneManager
import android.os.BatteryManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.VibrationEffect
import android.os.Vibrator
import android.database.ContentObserver
import android.net.Uri
import android.provider.CalendarContract
import android.provider.ContactsContract
import android.provider.Settings
import android.app.NotificationManager
import androidx.core.content.FileProvider
import java.io.File
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodChannel

/**
 * Every phone-side capability the Dart code uses, on channels registered
 * once against the application context (see CompanionApp), so they work
 * with or without an Activity.
 *
 * MethodChannel  com.amoledwatch.native            one-off calls
 * EventChannel   com.amoledwatch.native/notifications  posted/removed notifications
 * EventChannel   com.amoledwatch.native/media          now playing
 * EventChannel   com.amoledwatch.native/battery        phone battery
 * EventChannel   com.amoledwatch.native/navigation     turn-by-turn from navigation apps
 * EventChannel   com.amoledwatch.native/calendar       "changed" when the calendar changes
 * EventChannel   com.amoledwatch.native/widget         taps on the home-screen widget
 * EventChannel   com.amoledwatch.native/dnd            Do Not Disturb on/off changes
 */
class PhoneBridge(private val app: Context, messenger: BinaryMessenger) {

    private val main = Handler(Looper.getMainLooper())
    private val media = MediaSessionBridge(app)

    private var notifSink: EventChannel.EventSink? = null
    private var mediaSink: EventChannel.EventSink? = null
    private var batterySink: EventChannel.EventSink? = null
    private var navSink: EventChannel.EventSink? = null
    private var calendarSink: EventChannel.EventSink? = null
    private var widgetSink: EventChannel.EventSink? = null
    private var calendarObserver: ContentObserver? = null
    private var dndSink: EventChannel.EventSink? = null
    private var dndReceiver: BroadcastReceiver? = null

    private val navCallback = NotificationListener.NotificationCallback { ev ->
        main.post { navSink?.success(ev) }
    }

    /** Called by WatchWidget when its Find button is tapped. */
    fun onWidgetAction(action: String) {
        main.post {
            val sink = widgetSink
            if (sink != null) sink.success(action) else pendingWidgetAction = action
        }
    }
    private var pendingWidgetAction: String? = null

    private val notifCallback = NotificationListener.NotificationCallback { ev ->
        main.post { notifSink?.success(ev) }
    }

    init {
        MethodChannel(messenger, "com.amoledwatch.native").setMethodCallHandler { call, result ->
            try {
                when (call.method) {
                    "isNotificationListenerEnabled" -> result.success(isNotificationListenerEnabled())
                    "openNotificationListenerSettings" -> { openNotificationSettings(); result.success(true) }
                    "setAutoForward" -> {
                        val enabled = call.argument<Boolean>("enabled") ?: false
                        val packages = call.argument<List<String>>("packages") ?: emptyList()
                        NotificationListener.notificationsEnabled = enabled
                        NotificationListener.allowedPackages.clear()
                        NotificationListener.allowedPackages.addAll(packages)
                        result.success(true)
                    }
                    "setCallAlerts" -> {
                        NotificationListener.callsEnabled = call.argument<Boolean>("enabled") ?: true
                        result.success(true)
                    }
                    "notifAction" -> {
                        val id = call.argument<Int>("id") ?: 0
                        val action = call.argument<String>("action") ?: ""
                        val text = call.argument<String>("text")
                        result.success(NotificationListener.perform(id, action, text))
                    }
                    "mediaCommand" -> result.success(media.command(call.argument<String>("cmd") ?: ""))
                    "findPhone" -> { findPhone(call.argument<Boolean>("on") ?: false); result.success(true) }
                    "getBattery" -> result.success(batteryNow())
                    "getLocation" -> result.success(lastLocation())
                    "getContacts" -> {
                        if (app.checkSelfPermission(Manifest.permission.READ_CONTACTS) != PackageManager.PERMISSION_GRANTED) {
                            result.error("permission", "READ_CONTACTS not granted", null)
                        } else {
                            Thread {
                                val list = try { readContacts() } catch (e: Exception) { null }
                                main.post {
                                    if (list != null) result.success(list)
                                    else result.error("query", "Contacts query failed", null)
                                }
                            }.start()
                        }
                    }
                    "startLinkService" -> result.success(
                        LinkService.start(app,
                            call.argument<String>("title") ?: "AmoledWatch connected",
                            call.argument<String>("text") ?: "Notifications and controls are linked"))
                    "stopLinkService" -> { LinkService.stop(app); result.success(true) }
                    "setNavigation" -> {
                        NotificationListener.navigationEnabled = call.argument<Boolean>("enabled") ?: true
                        result.success(true)
                    }
                    "getEvents" -> {
                        if (app.checkSelfPermission(Manifest.permission.READ_CALENDAR) != PackageManager.PERMISSION_GRANTED) {
                            result.error("permission", "READ_CALENDAR not granted", null)
                        } else {
                            val hours = call.argument<Int>("hours") ?: 48
                            Thread {
                                val list = try { readEvents(hours) } catch (e: Exception) { null }
                                main.post {
                                    if (list != null) result.success(list)
                                    else result.error("query", "Calendar query failed", null)
                                }
                            }.start()
                        }
                    }
                    "watchCalendar" -> { watchCalendar(); result.success(true) }
                    "updateWidget" -> {
                        @Suppress("UNCHECKED_CAST")
                        WatchWidget.store(app, (call.arguments as? Map<String, Any?>) ?: emptyMap())
                        result.success(true)
                    }
                    // Kept for older Dart code; media is driven by the event stream.
                    "startMediaListening", "stopMediaListening" -> result.success(true)
                    "getDnd" -> result.success(mapOf("on" to dndOn(), "access" to dndAccess()))
                    "setDnd" -> result.success(setDnd(call.argument<Boolean>("on") ?: false))
                    "openDndAccess" -> {
                        val i = Intent(Settings.ACTION_NOTIFICATION_POLICY_ACCESS_SETTINGS)
                        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                        app.startActivity(i)
                        result.success(true)
                    }
                    "openUrl" -> {
                        val i = Intent(Intent.ACTION_VIEW, Uri.parse(call.argument<String>("url") ?: ""))
                        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                        try {
                            app.startActivity(i)
                            result.success(true)
                        } catch (e: android.content.ActivityNotFoundException) {
                            result.success(false)   // no browser
                        }
                    }
                    // ---- images for the watch (WatchImages) ----
                    "appIconJpeg" -> result.success(WatchImages.appIconJpeg(app, call.argument<String>("package") ?: ""))
                    "albumArtJpeg" -> {
                        val h = call.argument<Int>("hash") ?: 0
                        result.success(if (h != 0 && h == media.artHash) media.artJpeg else null)
                    }
                    // ---- app updates (UpdateService.dart) ----
                    "getAppVersion" -> result.success(
                        app.packageManager.getPackageInfo(app.packageName, 0).versionName ?: ""
                    )
                    "canInstallApks" -> result.success(
                        Build.VERSION.SDK_INT < Build.VERSION_CODES.O || app.packageManager.canRequestPackageInstalls()
                    )
                    "openInstallPermission" -> {
                        val i = Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:${app.packageName}"))
                        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                        app.startActivity(i)
                        result.success(true)
                    }
                    "installApk" -> {
                        val file = File(call.argument<String>("path") ?: "")
                        val uri = FileProvider.getUriForFile(app, "${app.packageName}.updates", file)
                        val i = Intent(Intent.ACTION_VIEW)
                        i.setDataAndType(uri, "application/vnd.android.package-archive")
                        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_ACTIVITY_NEW_TASK)
                        app.startActivity(i)
                        result.success(true)
                    }
                    else -> result.notImplemented()
                }
            } catch (e: Exception) {
                result.error("native", e.message, null)
            }
        }

        EventChannel(messenger, "com.amoledwatch.native/notifications").setStreamHandler(
            object : EventChannel.StreamHandler {
                override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
                    notifSink = events
                    NotificationListener.addListener(notifCallback)
                }
                override fun onCancel(arguments: Any?) {
                    notifSink = null
                    NotificationListener.removeListener(notifCallback)
                }
            })

        EventChannel(messenger, "com.amoledwatch.native/media").setStreamHandler(
            object : EventChannel.StreamHandler {
                override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
                    mediaSink = events
                    media.startListening { info -> main.post { mediaSink?.success(info) } }
                }
                override fun onCancel(arguments: Any?) {
                    mediaSink = null
                    media.stopListening()
                }
            })

        EventChannel(messenger, "com.amoledwatch.native/navigation").setStreamHandler(
            object : EventChannel.StreamHandler {
                override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
                    navSink = events
                    NotificationListener.addNavListener(navCallback)
                }
                override fun onCancel(arguments: Any?) {
                    navSink = null
                    NotificationListener.removeNavListener(navCallback)
                }
            })

        EventChannel(messenger, "com.amoledwatch.native/calendar").setStreamHandler(
            object : EventChannel.StreamHandler {
                override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
                    calendarSink = events
                    watchCalendar()
                }
                override fun onCancel(arguments: Any?) {
                    calendarSink = null
                    calendarObserver?.let { try { app.contentResolver.unregisterContentObserver(it) } catch (_: Exception) {} }
                    calendarObserver = null
                }
            })

        EventChannel(messenger, "com.amoledwatch.native/widget").setStreamHandler(
            object : EventChannel.StreamHandler {
                override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
                    widgetSink = events
                    pendingWidgetAction?.let { events?.success(it) }
                    pendingWidgetAction = null
                }
                override fun onCancel(arguments: Any?) { widgetSink = null }
            })

        EventChannel(messenger, "com.amoledwatch.native/dnd").setStreamHandler(
            object : EventChannel.StreamHandler {
                override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
                    dndSink = events
                    if (dndReceiver == null) {
                        val r = object : BroadcastReceiver() {
                            override fun onReceive(context: Context?, intent: Intent?) {
                                main.post { dndSink?.success(dndOn()) }
                            }
                        }
                        app.registerReceiver(r, IntentFilter(NotificationManager.ACTION_INTERRUPTION_FILTER_CHANGED))
                        dndReceiver = r
                    }
                }
                override fun onCancel(arguments: Any?) {
                    dndSink = null
                    dndReceiver?.let { try { app.unregisterReceiver(it) } catch (_: Exception) {} }
                    dndReceiver = null
                }
            })

        EventChannel(messenger, "com.amoledwatch.native/battery").setStreamHandler(
            object : EventChannel.StreamHandler {
                override fun onListen(arguments: Any?, events: EventChannel.EventSink?) {
                    batterySink = events
                    registerBattery()
                    events?.success(batteryNow())
                }
                override fun onCancel(arguments: Any?) {
                    batterySink = null
                    unregisterBattery()
                }
            })
    }

    // ---- Do Not Disturb ------------------------------------------------------------

    private val notificationManager: NotificationManager
        get() = app.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager

    private fun dndOn(): Boolean =
        notificationManager.currentInterruptionFilter != NotificationManager.INTERRUPTION_FILTER_ALL &&
            notificationManager.currentInterruptionFilter != NotificationManager.INTERRUPTION_FILTER_UNKNOWN

    private fun dndAccess(): Boolean = notificationManager.isNotificationPolicyAccessGranted

    /** Needs "Do Not Disturb access" for this app (openDndAccess). */
    private fun setDnd(on: Boolean): Boolean {
        if (!dndAccess()) return false
        notificationManager.setInterruptionFilter(
            if (on) NotificationManager.INTERRUPTION_FILTER_PRIORITY else NotificationManager.INTERRUPTION_FILTER_ALL)
        return true
    }

    // ---- notification access ----------------------------------------------

    private fun isNotificationListenerEnabled(): Boolean {
        val cn = ComponentName(app, NotificationListener::class.java)
        val flat = Settings.Secure.getString(app.contentResolver, "enabled_notification_listeners")
        return flat?.contains(cn.flattenToString()) == true
    }

    private fun openNotificationSettings() {
        val intent = Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        app.startActivity(intent)
    }

    // ---- battery ------------------------------------------------------------

    private var batteryReceiver: BroadcastReceiver? = null
    private var lastBatteryKey = ""

    private fun batteryFrom(i: Intent?): Map<String, Any> {
        val level = i?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = i?.getIntExtra(BatteryManager.EXTRA_SCALE, 100) ?: 100
        val status = i?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
        val pct = if (level >= 0 && scale > 0) level * 100 / scale else -1
        val charging = status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL
        return mapOf("level" to pct, "charging" to charging)
    }

    private fun batteryNow(): Map<String, Any> =
        batteryFrom(app.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED)))

    private fun registerBattery() {
        if (batteryReceiver != null) return
        val r = object : BroadcastReceiver() {
            override fun onReceive(context: Context?, intent: Intent?) {
                val b = batteryFrom(intent)
                val key = "${b["level"]}|${b["charging"]}"
                if (key == lastBatteryKey) return
                lastBatteryKey = key
                batterySink?.success(b)
            }
        }
        batteryReceiver = r
        app.registerReceiver(r, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
    }

    private fun unregisterBattery() {
        batteryReceiver?.let { try { app.unregisterReceiver(it) } catch (_: Exception) {} }
        batteryReceiver = null
    }

    // ---- find my phone -------------------------------------------------------

    private var ringtone: Ringtone? = null
    private var savedAlarmVolume = -1
    private val stopFind = Runnable { findPhone(false) }

    private fun findPhone(on: Boolean) {
        val audio = app.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val vib = app.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
        main.removeCallbacks(stopFind)
        if (on) {
            if (ringtone?.isPlaying == true) return
            savedAlarmVolume = audio.getStreamVolume(AudioManager.STREAM_ALARM)
            audio.setStreamVolume(AudioManager.STREAM_ALARM, audio.getStreamMaxVolume(AudioManager.STREAM_ALARM), 0)
            val uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM)
                ?: RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE)
            val rt = RingtoneManager.getRingtone(app, uri)
            if (rt != null) {
                rt.audioAttributes = AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_ALARM)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build()
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) rt.isLooping = true
                rt.play()
            }
            ringtone = rt
            if (vib != null) vibrateRepeating(vib, longArrayOf(0, 600, 400))
            main.postDelayed(stopFind, 60_000) // never ring forever
        } else {
            ringtone?.stop()
            ringtone = null
            vib?.cancel()
            if (savedAlarmVolume >= 0) {
                audio.setStreamVolume(AudioManager.STREAM_ALARM, savedAlarmVolume, 0)
                savedAlarmVolume = -1
            }
        }
    }

    @Suppress("DEPRECATION")
    private fun vibrateRepeating(vib: Vibrator, pattern: LongArray) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) vib.vibrate(VibrationEffect.createWaveform(pattern, 0))
        else vib.vibrate(pattern, 0)
    }

    // ---- location (for weather) ------------------------------------------------

    private fun lastLocation(): Map<String, Double>? {
        val fine = app.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
        val coarse = app.checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION) == PackageManager.PERMISSION_GRANTED
        if (!fine && !coarse) return null
        val lm = app.getSystemService(Context.LOCATION_SERVICE) as LocationManager
        var best: Location? = null
        for (p in lm.getProviders(true)) {
            val l = try { lm.getLastKnownLocation(p) } catch (e: SecurityException) { null } ?: continue
            if (best == null || l.time > best.time) best = l
        }
        val b = best ?: return null
        return mapOf("lat" to b.latitude, "lon" to b.longitude)
    }

    // ---- contacts ----------------------------------------------------------------

    /** One entry per contact with a phone number: name, first number, first email. */
    private fun readContacts(): List<Map<String, String>> {
        val byId = LinkedHashMap<Long, HashMap<String, String>>()
        app.contentResolver.query(
            ContactsContract.CommonDataKinds.Phone.CONTENT_URI,
            arrayOf(
                ContactsContract.CommonDataKinds.Phone.CONTACT_ID,
                ContactsContract.CommonDataKinds.Phone.DISPLAY_NAME,
                ContactsContract.CommonDataKinds.Phone.NUMBER,
            ),
            null, null,
            ContactsContract.CommonDataKinds.Phone.DISPLAY_NAME + " COLLATE NOCASE ASC"
        )?.use { c ->
            while (c.moveToNext()) {
                val id = c.getLong(0)
                if (byId.containsKey(id)) continue
                byId[id] = hashMapOf(
                    "name" to (c.getString(1) ?: ""),
                    "phone" to (c.getString(2) ?: ""),
                    "email" to "",
                )
            }
        }
        app.contentResolver.query(
            ContactsContract.CommonDataKinds.Email.CONTENT_URI,
            arrayOf(
                ContactsContract.CommonDataKinds.Email.CONTACT_ID,
                ContactsContract.CommonDataKinds.Email.ADDRESS,
            ),
            null, null, null
        )?.use { c ->
            while (c.moveToNext()) {
                val entry = byId[c.getLong(0)] ?: continue
                if (entry["email"].isNullOrEmpty()) entry["email"] = c.getString(1) ?: ""
            }
        }
        return byId.values.toList()
    }

    // ---- calendar ----------------------------------------------------------------

    /** Event instances from a day ago to now + [hours], visible calendars only. */
    private fun readEvents(hours: Int): List<Map<String, Any>> {
        val now = System.currentTimeMillis()
        // A day back: all-day events are stored UTC midnight to UTC midnight,
        // so west of UTC "today" would drop out early. Ended ones are
        // filtered out on the Dart side.
        val begin = now - 24 * 3600_000L
        val end = now + hours * 3600_000L
        val uri = CalendarContract.Instances.CONTENT_URI.buildUpon().also {
            android.content.ContentUris.appendId(it, begin)
            android.content.ContentUris.appendId(it, end)
        }.build()
        val out = ArrayList<Map<String, Any>>()
        app.contentResolver.query(
            uri,
            arrayOf(
                CalendarContract.Instances.EVENT_ID,
                CalendarContract.Instances.TITLE,
                CalendarContract.Instances.BEGIN,
                CalendarContract.Instances.END,
                CalendarContract.Instances.ALL_DAY,
                CalendarContract.Instances.EVENT_LOCATION,
                CalendarContract.Instances.DISPLAY_COLOR,
                CalendarContract.Instances.VISIBLE,
                CalendarContract.Instances.SELF_ATTENDEE_STATUS,
            ),
            null, null,
            CalendarContract.Instances.BEGIN + " ASC"
        )?.use { c ->
            while (c.moveToNext()) {
                if (c.getInt(7) == 0) continue                       // hidden calendar
                if (c.getInt(8) == CalendarContract.Attendees.ATTENDEE_STATUS_DECLINED) continue
                out.add(mapOf(
                    "id" to c.getLong(0),
                    "title" to (c.getString(1) ?: ""),
                    "begin" to c.getLong(2),
                    "end" to c.getLong(3),
                    "allDay" to (c.getInt(4) != 0),
                    "location" to (c.getString(5) ?: ""),
                    "color" to (c.getInt(6) and 0xFFFFFF),
                ))
            }
        }
        return out
    }

    private fun watchCalendar() {
        if (calendarObserver != null) return
        if (app.checkSelfPermission(Manifest.permission.READ_CALENDAR) != PackageManager.PERMISSION_GRANTED) return
        val notify = Runnable { calendarSink?.success("changed") }
        val obs = object : ContentObserver(main) {
            override fun onChange(selfChange: Boolean, uri: Uri?) {
                // calendar syncs come in bursts: report once they settle
                main.removeCallbacks(notify)
                main.postDelayed(notify, 3000)
            }
        }
        try {
            app.contentResolver.registerContentObserver(CalendarContract.Events.CONTENT_URI, true, obs)
            calendarObserver = obs
        } catch (_: Exception) {}
    }
}
