plugins {
    id("com.android.application")
}

android {
    namespace = "dev.kage.vrstream"
    compileSdk = 37  // build-time API level only; device requirements are minSdk/targetSdk
    // Must be installed through the Android SDK Manager (SDK Tools > NDK (Side by side)).
    ndkVersion = "27.2.12479018"

    defaultConfig {
        applicationId = "dev.kage.vrstream"
        minSdk = 29     // Horizon OS minimum for OpenXR apps
        targetSdk = 32  // value recommended by Meta for Quest apps
        versionCode = 1
        versionName = "0.1.0"
        ndk { abiFilters += "arm64-v8a" }  // Quest 3 is arm64 only
        externalNativeBuild {
            cmake {
                arguments += listOf("-DANDROID_STL=c++_shared")
            }
        }
    }

    buildFeatures {
        prefab = true  // consumes the Khronos OpenXR loader AAR
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    buildTypes {
        release {
            isMinifyEnabled = false
        }
    }
}

dependencies {
    // Official Khronos OpenXR loader for Android (Apache-2.0).
    implementation("org.khronos.openxr:openxr_loader_for_android:1.1.65")
}
