plugins {
    id("com.android.application") version "8.11.1"
    id("org.jetbrains.kotlin.android") version "2.2.20"
}

android {
    namespace = "com.amoledwatch.camerahelper"
    compileSdk = 36

    defaultConfig {
        applicationId = "com.amoledwatch.camerahelper"
        minSdk = 26
        targetSdk = 36
        versionCode = (project.findProperty("helperVersionCode") as String?)?.toIntOrNull() ?: 1
        versionName = (project.findProperty("helperVersionName") as String?) ?: "1.0"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }

    // The same key as the main app (build.yml's secrets): the main app's
    // signature permission lets only it ask the helper to press buttons.
    val keystore = System.getenv("ANDROID_KEYSTORE_PATH")
    signingConfigs {
        if (keystore != null && file(keystore).exists()) {
            create("stable") {
                storeFile = file(keystore)
                storePassword = System.getenv("ANDROID_KEYSTORE_PASSWORD")?.trim()
                keyAlias = System.getenv("ANDROID_KEY_ALIAS")?.trim()
                keyPassword = System.getenv("ANDROID_KEY_PASSWORD")?.trim()
            }
        }
    }
    buildTypes {
        val signing = signingConfigs.findByName("stable") ?: signingConfigs.getByName("debug")
        debug { signingConfig = signing }
        release {
            signingConfig = signing
            isMinifyEnabled = false
        }
    }
    sourceSets["main"].java.srcDirs("src/main/kotlin")
}
