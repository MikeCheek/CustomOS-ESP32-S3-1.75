// AmoledWatch Camera Helper: the optional add-on that presses a camera app's
// buttons for the watch (an accessibility service), kept out of the main app
// so the main app installs without Play Protect's block. Built by CI with
// the main app's Gradle wrapper: ../android/gradlew -p . assembleRelease
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
    }
}
rootProject.name = "camera_helper"
