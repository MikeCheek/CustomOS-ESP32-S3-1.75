package com.amoledwatch.amoled_companion

import androidx.core.content.FileProvider

/**
 * Hands a downloaded app update (an .apk in cache/updates/) to the system
 * installer. Its own subclass so it can't clash with a plugin's FileProvider.
 */
class UpdateFileProvider : FileProvider()
