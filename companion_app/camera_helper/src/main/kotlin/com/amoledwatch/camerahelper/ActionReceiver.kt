package com.amoledwatch.camerahelper

import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/**
 * AmoledWatch's requests (an ordered broadcast, only from an app holding its
 * signature permission): extra "a" = shot / flip / zoom+ / zoom- presses that
 * button in the camera app on screen; "front" answers with the app in front.
 * The result code is RESULT_OK when it was done, RESULT_CANCELED when it wasn't
 * (the service is off, or no such button).
 */
class ActionReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val svc = HelperAccess.instance
        val a = intent.getStringExtra("a") ?: ""
        if (svc == null) { setResult(Activity.RESULT_CANCELED, "off", null); return }
        if (a == "front") { setResult(Activity.RESULT_OK, svc.frontPackage ?: "", null); return }
        setResult(if (svc.act(a)) Activity.RESULT_OK else Activity.RESULT_CANCELED, null, null)
    }
}
