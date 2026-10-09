package com.amoledwatch.amoled_companion

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Rect
import java.io.ByteArrayOutputStream

/**
 * App icons and album covers for the watch, as small JPEGs (the watch
 * decodes them with JPEGDEC). Transparent parts go on black, the watch's
 * background.
 */
object WatchImages {
    const val ICON_SIZE = 40
    const val ART_SIZE = 128

    /** Stable id of an app's icon: the watch asks for it by this. Never 0. */
    fun iconHash(pkg: String): Int = (pkg.hashCode() and 0x7fffffff) or 1

    fun appIconJpeg(ctx: Context, pkg: String): ByteArray? = try {
        val d = ctx.packageManager.getApplicationIcon(pkg)
        val bmp = Bitmap.createBitmap(ICON_SIZE, ICON_SIZE, Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        c.drawColor(Color.BLACK)
        d.setBounds(0, 0, ICON_SIZE, ICON_SIZE)
        d.draw(c)
        jpeg(bmp, 92)
    } catch (e: Exception) {
        null // uninstalled since
    }

    /** Centre-cropped square cover. */
    fun artJpeg(src: Bitmap): ByteArray {
        val side = minOf(src.width, src.height)
        val sx = (src.width - side) / 2
        val sy = (src.height - side) / 2
        val bmp = Bitmap.createBitmap(ART_SIZE, ART_SIZE, Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        c.drawColor(Color.BLACK)
        c.drawBitmap(src, Rect(sx, sy, sx + side, sy + side), Rect(0, 0, ART_SIZE, ART_SIZE), null)
        return jpeg(bmp, 82)
    }

    private fun jpeg(bmp: Bitmap, quality: Int): ByteArray {
        val out = ByteArrayOutputStream()
        bmp.compress(Bitmap.CompressFormat.JPEG, quality, out)
        bmp.recycle()
        return out.toByteArray()
    }
}
