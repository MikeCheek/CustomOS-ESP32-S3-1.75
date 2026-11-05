plugins {
    id("com.android.application")
    id("kotlin-android")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

android {
    namespace = "com.amoledwatch.amoled_companion"
    compileSdk = flutter.compileSdkVersion
    ndkVersion = "29.0.13113456"

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = JavaVersion.VERSION_17.toString()
    }

    // The watch emulator: the firmware's screen code as a native library
    // (src/main/cpp/CMakeLists.txt).
    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    // Several native plugins (whisper.cpp, LiteRT, sherpa-onnx) may each
    // bring the same C++ runtime: keep one copy instead of failing the merge.
    packaging {
        jniLibs {
            pickFirsts += listOf("**/libc++_shared.so", "**/libonnxruntime.so")
        }
    }

    defaultConfig {
        // TODO: Specify your own unique Application ID (https://developer.android.com/studio/build/application-id.html).
        applicationId = "com.amoledwatch.amoled_companion"
        // You can update the following values to match your application needs.
        // For more information, see: https://flutter.dev/to/review-gradle-config.
        minSdk = maxOf(26, flutter.minSdkVersion)   // Health Connect (health plugin) needs 26
        targetSdk = flutter.targetSdkVersion
        versionCode = flutter.versionCode
        versionName = flutter.versionName
    }

    // Android only installs an update signed with the same key as the
    // installed app. CI passes a fixed keystore (repository secrets, see
    // build.yml) so every release APK can update the previous one; without
    // it the build uses the machine's own debug key.
    val keystore = System.getenv("ANDROID_KEYSTORE_PATH")
    signingConfigs {
        if (keystore != null && file(keystore).exists()) {
            create("stable") {
                storeFile = file(keystore)
                // trimmed: a secret pasted with a trailing newline is a common slip
                storePassword = System.getenv("ANDROID_KEYSTORE_PASSWORD")?.trim()
                keyAlias = System.getenv("ANDROID_KEY_ALIAS")?.trim()
                keyPassword = System.getenv("ANDROID_KEY_PASSWORD")?.trim()
            }
        }
    }

    buildTypes {
        val signing = signingConfigs.findByName("stable") ?: signingConfigs.getByName("debug")
        debug {
            signingConfig = signing
        }
        release {
            signingConfig = signing
            // No R8: the watch emulator's JNI bridge (EmuOverlay's native methods, looked
            // up by name) and plugins that reach classes by reflection must keep their
            // names. The size is in the native libraries anyway, not the Java code.
            isMinifyEnabled = false
            isShrinkResources = false
        }
    }
}

flutter {
    source = "../.."
}

dependencies {
    implementation("androidx.core:core-ktx:1.13.1")   // FileProvider for app updates
}
