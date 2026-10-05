plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "dev.bearite.launcher"
    compileSdk = 34

    defaultConfig {
        applicationId = "dev.bearite.launcher"
        minSdk = 26
        targetSdk = 34
        versionCode = 3
        versionName = "0.3.0"
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
        isCoreLibraryDesugaringEnabled = true
    }
    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation("com.google.android.material:material:1.12.0")
    implementation("com.android.tools.build:apksig:8.5.2")
    coreLibraryDesugaring("com.android.tools:desugar_jdk_libs:2.0.4")
}
