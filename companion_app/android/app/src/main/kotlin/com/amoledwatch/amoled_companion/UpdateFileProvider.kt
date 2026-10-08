package com.amoledwatch.amoled_companion

import androidx.core.content.FileProvider

/**
 * Hands a downloaded app update (cache/updates/*.apk) to the system
 * installer. Its own subclass so it can't clash with a plugin's FileProvider.
 */
class UpdateFileProvider : FileProvider()
