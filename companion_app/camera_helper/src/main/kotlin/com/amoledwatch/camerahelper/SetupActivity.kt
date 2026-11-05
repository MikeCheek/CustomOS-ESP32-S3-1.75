package com.amoledwatch.camerahelper

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.provider.Settings
import android.widget.Toast

/** From the launcher: says what to turn on and opens Accessibility. */
class SetupActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val on = HelperAccess.instance != null
        Toast.makeText(
            this,
            if (on) "The camera remote is on - the watch can press the camera app's buttons"
            else "Turn on \"AmoledWatch camera remote\" (Installed apps) so the watch can press the camera app's buttons",
            Toast.LENGTH_LONG,
        ).show()
        if (!on) {
            try {
                startActivity(Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            } catch (_: Exception) {}
        }
        finish()
    }
}
