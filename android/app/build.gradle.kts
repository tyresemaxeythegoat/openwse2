import java.io.File
import java.net.URI
import java.net.http.HttpClient
import java.net.http.HttpRequest
import java.net.http.HttpResponse
import java.security.MessageDigest
import java.time.Duration
import java.util.zip.ZipInputStream

plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

val sdl3Version = "3.4.20"
val sdl3ZipUrl =
    "https://github.com/libsdl-org/SDL/releases/download/release-$sdl3Version/SDL3-devel-$sdl3Version-android.zip"
val sdl3ZipSha256 = "535320e58f3bd7235b3e15f940595cdeeb9b8c0aa6b3810ff127345e3567d4da"
// SHA-256 of SDL3-$sdl3Version.aar inside that archive.
val sdl3AarSha256 = "e5eec5e3d0293cbe9ce5899d451eaaf00a5c9ed70c1e444b751932118714e170"
val sdl3AarFile = layout.projectDirectory.file("libs/SDL3-$sdl3Version.aar").asFile
val sdl3ZipFile = layout.buildDirectory.file("downloads/SDL3-devel-$sdl3Version-android.zip").get().asFile

fun sha256Hex(file: File): String {
    val digest = MessageDigest.getInstance("SHA-256")
    file.inputStream().use { input ->
        val buf = ByteArray(65536)
        while (true) {
            val read = input.read(buf)
            if (read <= 0) break
            digest.update(buf, 0, read)
        }
    }
    return digest.digest().joinToString("") { "%02x".format(it) }
}

fun compareNumericVersions(a: String, b: String): Int {
    val pa = a.split(".").map { it.toIntOrNull() ?: 0 }
    val pb = b.split(".").map { it.toIntOrNull() ?: 0 }
    val maxLen = maxOf(pa.size, pb.size)
    for (i in 0 until maxLen) {
        val diff = (pa.getOrElse(i) { 0 }) - (pb.getOrElse(i) { 0 })
        if (diff != 0) return diff
    }
    return a.compareTo(b)
}

fun detectSdkSubdir(subdir: String): String? {
    val sdkRoot = System.getenv("ANDROID_HOME")
        ?: System.getenv("ANDROID_SDK_ROOT")
        ?: return null
    val dir = File(sdkRoot, subdir)
    if (!dir.isDirectory) return null
    return dir.listFiles()
        ?.filter { it.isDirectory }
        ?.map { it.name }
        ?.maxWithOrNull(::compareNumericVersions)
}

val ensureSdl3Aar by tasks.registering {
    description = "Downloads and SHA-256 verifies the pinned SDL3 Android AAR"
    outputs.file(sdl3AarFile)

    doLast {
        // A cached AAR is only trusted if it still matches the pinned hash.
        if (sdl3AarFile.isFile) {
            if (sha256Hex(sdl3AarFile) == sdl3AarSha256) {
                return@doLast
            }
            logger.warn("Discarding ${sdl3AarFile.name}: SHA-256 does not match the pinned value")
            sdl3AarFile.delete()
        }
        sdl3ZipFile.parentFile.mkdirs()
        if (!sdl3ZipFile.isFile || sha256Hex(sdl3ZipFile) != sdl3ZipSha256) {
            logger.lifecycle("Downloading SDL3 $sdl3Version Android Prefab AAR from $sdl3ZipUrl")
            val client = HttpClient.newBuilder()
                .followRedirects(HttpClient.Redirect.NORMAL)
                .connectTimeout(Duration.ofSeconds(30))
                .build()
            val request = HttpRequest.newBuilder(URI.create(sdl3ZipUrl))
                .timeout(Duration.ofMinutes(3))
                .GET()
                .build()
            val tmpZip = File(sdl3ZipFile.parentFile, "${sdl3ZipFile.name}.tmp")
            val response = client.send(request, HttpResponse.BodyHandlers.ofFile(tmpZip.toPath()))
            if (response.statusCode() !in 200..299) {
                tmpZip.delete()
                throw GradleException("Failed to download $sdl3ZipUrl (HTTP ${response.statusCode()})")
            }
            val actualHash = sha256Hex(tmpZip)
            if (actualHash != sdl3ZipSha256) {
                tmpZip.delete()
                throw GradleException(
                    "SHA-256 mismatch for SDL3 archive: expected $sdl3ZipSha256, got $actualHash"
                )
            }
            if (sdl3ZipFile.exists()) sdl3ZipFile.delete()
            if (!tmpZip.renameTo(sdl3ZipFile)) {
                tmpZip.copyTo(sdl3ZipFile, overwrite = true)
                tmpZip.delete()
            }
        }

        val targetEntry = "SDL3-$sdl3Version.aar"
        sdl3AarFile.parentFile.mkdirs()
        val tmpAar = File(sdl3AarFile.parentFile, "${sdl3AarFile.name}.tmp")
        var extracted = false
        ZipInputStream(sdl3ZipFile.inputStream().buffered()).use { zis ->
            while (true) {
                val entry = zis.nextEntry ?: break
                if (!entry.isDirectory && File(entry.name).name == targetEntry) {
                    tmpAar.outputStream().buffered().use { out ->
                        zis.copyTo(out)
                    }
                    extracted = true
                    break
                }
            }
        }
        if (!extracted || tmpAar.length() == 0L) {
            tmpAar.delete()
            throw GradleException("Missing $targetEntry inside $sdl3ZipFile")
        }
        val aarHash = sha256Hex(tmpAar)
        if (aarHash != sdl3AarSha256) {
            tmpAar.delete()
            throw GradleException(
                "SHA-256 mismatch for $targetEntry: expected $sdl3AarSha256, got $aarHash"
            )
        }
        if (sdl3AarFile.exists()) sdl3AarFile.delete()
        if (!tmpAar.renameTo(sdl3AarFile)) {
            tmpAar.copyTo(sdl3AarFile, overwrite = true)
            tmpAar.delete()
        }
        logger.lifecycle("Verified and staged ${sdl3AarFile.absolutePath}")
    }
}

// Only the self-test fixture is packaged. Pointing the asset source set at
// engine/fixtures itself would ship every file later added there.
val selfTestAssetsDir = layout.buildDirectory.dir("generated/wse_selftest_assets")
val stageSelfTestAssets by tasks.registering(Sync::class) {
    description = "Stages the self-test fixture as the APK's only asset"
    from(layout.projectDirectory.dir("../../engine/fixtures")) {
        include("scripts.txt")
    }
    into(selfTestAssetsDir)
}
tasks.named("preBuild") {
    dependsOn(stageSelfTestAssets)
}

val resolvedNdkVersion = providers.gradleProperty("wse.ndkVersion").orNull
    ?: detectSdkSubdir("ndk")
val resolvedCmakeVersion = providers.gradleProperty("wse.cmakeVersion").orNull
    ?: detectSdkSubdir("cmake")

android {
    namespace = "org.wse2.android"
    compileSdk = 35
    if (resolvedNdkVersion != null) {
        ndkVersion = resolvedNdkVersion
    }

    defaultConfig {
        applicationId = "org.wse2.android"
        minSdk = 29
        targetSdk = 35
        versionCode = 1
        versionName = "0.1.0"

        ndk {
            abiFilters += listOf("arm64-v8a")
        }

        externalNativeBuild {
            cmake {
                targets += listOf("wse2", "panvk_hw")
                arguments += listOf(
                    "-DANDROID_STL=c++_static",
                    "-DANDROID_PLATFORM=android-29",
                    "-DWSE_BUILD_ANDROID_APP=ON",
                    "-DWSE_BUILD_TESTS=OFF",
                    "-DWSE_BUILD_TOOLS=OFF",
                    "-DWSE_BUILD_HEADER_CHECKS=OFF"
                )
            }
        }
    }

    buildTypes {
        debug {
            isMinifyEnabled = false
        }
        release {
            isMinifyEnabled = false
        }
    }

    buildFeatures {
        prefab = true
    }

    externalNativeBuild {
        cmake {
            path = file("../../CMakeLists.txt")
            if (resolvedCmakeVersion != null) {
                version = resolvedCmakeVersion
            }
        }
    }

    sourceSets {
        getByName("main") {
            assets.srcDirs(selfTestAssetsDir.get().asFile)
        }
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    kotlinOptions {
        jvmTarget = "17"
    }
}

dependencies {
    implementation(files(sdl3AarFile) {
        builtBy(ensureSdl3Aar)
    })
}
