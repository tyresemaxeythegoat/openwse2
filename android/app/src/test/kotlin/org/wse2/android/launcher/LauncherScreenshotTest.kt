@file:OptIn(androidx.compose.animation.ExperimentalSharedTransitionApi::class)

package org.wse2.android.launcher

import android.graphics.Bitmap
import android.net.Uri
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalInspectionMode
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onRoot
import androidx.test.ext.junit.runners.AndroidJUnit4
import com.github.takahirom.roborazzi.captureRoboImage
import java.io.File
import kotlin.math.abs
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlin.math.sqrt
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.wse2.android.launcher.art.ModuleArt
import org.wse2.android.launcher.theme.DefaultSeedColorArgb
import org.wse2.android.launcher.theme.LocalWseColors
import org.wse2.android.launcher.theme.Wse2Theme
import org.wse2.android.launcher.theme.wseBackdrop

private const val OUTPUT_DIR = "build/outputs/roborazzi"
private const val PORTRAIT_QUALIFIERS = "w411dp-h891dp-port-xxhdpi"
private const val LANDSCAPE_QUALIFIERS = "w891dp-h411dp-land-xxhdpi"

@RunWith(AndroidJUnit4::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(sdk = [34])
class LauncherScreenshotTest {

    @get:Rule
    val composeRule = createComposeRule()

    @Test
    @Config(sdk = [34], qualifiers = PORTRAIT_QUALIFIERS)
    fun home_ready_portrait() {
        val art = createPlaceholderArt(variant = 0)
        captureHome(
            fileName = "home_ready_portrait.png",
            state = sampleHomeReadyState(heroArt = art),
            seedColorArgb = art.seedArgb
        )
    }

    @Test
    @Config(sdk = [34], qualifiers = LANDSCAPE_QUALIFIERS)
    fun home_ready_landscape() {
        val art = createPlaceholderArt(variant = 0)
        captureHome(
            fileName = "home_ready_landscape.png",
            state = sampleHomeReadyState(heroArt = art),
            seedColorArgb = art.seedArgb
        )
    }

    @Test
    @Config(sdk = [34], qualifiers = PORTRAIT_QUALIFIERS)
    fun home_no_art_portrait() {
        captureHome(
            fileName = "home_no_art_portrait.png",
            state = sampleHomeReadyState(heroArt = null),
            seedColorArgb = DefaultSeedColorArgb
        )
    }

    @Test
    @Config(sdk = [34], qualifiers = PORTRAIT_QUALIFIERS)
    fun home_missing_files_portrait() {
        val art = createPlaceholderArt(variant = 1)
        captureHome(
            fileName = "home_missing_files_portrait.png",
            state = sampleHomeMissingState(heroArt = art),
            seedColorArgb = art.seedArgb
        )
    }

    @Test
    @Config(sdk = [34], qualifiers = PORTRAIT_QUALIFIERS)
    fun modules_ready_portrait() {
        val artMap = sampleModuleArtMap()
        val seed = artMap["Modules/Native"]?.seedArgb ?: DefaultSeedColorArgb
        captureModules(
            fileName = "modules_ready_portrait.png",
            state = sampleModulesReadyState(),
            moduleArt = artMap,
            seedColorArgb = seed
        )
    }

    @Test
    @Config(sdk = [34], qualifiers = LANDSCAPE_QUALIFIERS)
    fun modules_ready_landscape() {
        val artMap = sampleModuleArtMap()
        val seed = artMap["Modules/Native"]?.seedArgb ?: DefaultSeedColorArgb
        captureModules(
            fileName = "modules_ready_landscape.png",
            state = sampleModulesReadyState(),
            moduleArt = artMap,
            seedColorArgb = seed
        )
    }

    @Test
    @Config(sdk = [34], qualifiers = PORTRAIT_QUALIFIERS)
    fun modules_no_art_portrait() {
        captureModules(
            fileName = "modules_no_art_portrait.png",
            state = sampleModulesReadyState(),
            moduleArt = emptyMap(),
            seedColorArgb = DefaultSeedColorArgb
        )
    }

    @Test
    @Config(sdk = [34], qualifiers = PORTRAIT_QUALIFIERS)
    fun modules_missing_files_portrait() {
        val artMap = sampleModuleArtMap()
        val seed = artMap["Modules/Custom_Realm"]?.seedArgb ?: DefaultSeedColorArgb
        captureModules(
            fileName = "modules_missing_files_portrait.png",
            state = sampleModulesMissingState(),
            moduleArt = artMap,
            seedColorArgb = seed
        )
    }

    @Test
    @Config(sdk = [34], qualifiers = PORTRAIT_QUALIFIERS)
    fun settings_default_portrait() {
        captureSettings(fileName = "settings_default_portrait.png")
    }

    @Test
    @Config(sdk = [34], qualifiers = LANDSCAPE_QUALIFIERS)
    fun settings_default_landscape() {
        captureSettings(fileName = "settings_default_landscape.png")
    }

    @Test
    @Config(sdk = [34], qualifiers = PORTRAIT_QUALIFIERS)
    fun settings_rgl_graphics_portrait() {
        captureSettings(
            fileName = "settings_rgl_graphics_portrait.png",
            category = SettingsCategory.GRAPHICS
        )
    }

    @Test
    @Config(sdk = [34], qualifiers = PORTRAIT_QUALIFIERS)
    fun settings_rgl_gameplay_portrait() {
        captureSettings(
            fileName = "settings_rgl_gameplay_portrait.png",
            category = SettingsCategory.GAMEPLAY
        )
    }

    @Test
    @Config(sdk = [34], qualifiers = PORTRAIT_QUALIFIERS)
    fun advanced_default_portrait() {
        captureAdvanced(fileName = "advanced_default_portrait.png")
    }

    @Test
    @Config(sdk = [34], qualifiers = LANDSCAPE_QUALIFIERS)
    fun advanced_default_landscape() {
        captureAdvanced(fileName = "advanced_default_landscape.png")
    }

    private fun captureHome(
        fileName: String,
        state: HomeUiState,
        seedColorArgb: Int
    ) {
        val modules = sampleModules()
        val selected = modules.indexOfFirst { it.modulePath == state.selectedModule?.modulePath }
        captureScreen(fileName = fileName, seedColorArgb = seedColorArgb) {
            HomeScreen(
                state = state,
                onChooseFolder = {},
                onChangeModule = {},
                onShowMissingFiles = { _, _ -> },
                onPlay = {},
                modules = modules,
                selectedIndex = selected,
                // The no-art case also leaves the strip on its generated gradients.
                moduleArt = if (state.heroArt != null) sampleModuleArtMap() else emptyMap(),
                onSelectModule = {}
            )
        }
    }

    private fun captureModules(
        fileName: String,
        state: ModulesUiState,
        moduleArt: Map<String, ModuleArt>,
        seedColorArgb: Int
    ) {
        captureScreen(fileName = fileName, seedColorArgb = seedColorArgb) {
            ModulesScreen(
                state = state,
                installFolder = sampleInstallFolder(),
                scanState = sampleScanState(),
                moduleArt = moduleArt,
                onChooseFolder = {},
                onRescan = {},
                onCancelScan = {},
                onSelectModule = {},
                onShowMissingFiles = { _, _ -> }
            )
        }
    }

    private fun captureSettings(
        fileName: String,
        category: SettingsCategory = SettingsCategory.PERFORMANCE
    ) {
        captureScreen(fileName = fileName) {
            SettingsScreen(
                state = sampleSettingsState(),
                onQualityPresetChange = {},
                onFrameRateCapChange = {},
                onPresentModeChange = {},
                onRenderScaleChange = {},
                onMaxTextureSizeChange = {},
                onThermalThrottlingChange = {},
                onDynamicResolutionChange = {},
                onLodBiasScalingChange = {},
                onUiAspectChange = {},
                onUiScaleChange = {},
                onGyroAimChange = {},
                onPinchZoomChange = {},
                onLanguageChange = {},
                initialCategory = category
            )
        }
    }

    private fun captureAdvanced(fileName: String) {
        captureScreen(fileName = fileName) {
            AdvancedScreen(
                state = sampleAdvancedState(),
                onVerifyScripts = {},
                onSelectDriver = {},
                onImportDriver = {},
                onAllowMockFallbackChange = {},
                onProbeVulkan = {},
                onShowFpsOverlayChange = {},
                onLaunchSelfTest = {}
            )
        }
    }

    private fun captureScreen(
        fileName: String,
        seedColorArgb: Int = DefaultSeedColorArgb,
        content: @Composable () -> Unit
    ) {
        val outputPath = "$OUTPUT_DIR/$fileName"
        File(outputPath).parentFile?.mkdirs()
        composeRule.setContent {
            Wse2Theme(
                darkTheme = true,
                dynamicColor = false,
                seedColorArgb = seedColorArgb
            ) {
                // The same root the app draws (gradient, accent glow, vignette, grain).
                Box(
                    modifier = Modifier
                        .fillMaxSize()
                        .wseBackdrop(LocalWseColors.current.accent)
                ) {
                    Surface(modifier = Modifier.fillMaxSize(), color = Color.Transparent) {
                        // Inspection mode turns the Ken Burns drift and the entrance
                        // animations off (`rememberWseAnimationsEnabled`), so every capture
                        // is the settled frame and waitForIdle never chases an infinite loop.
                        CompositionLocalProvider(LocalInspectionMode provides true) {
                            content()
                        }
                    }
                }
            }
        }
        composeRule.waitForIdle()
        composeRule.onRoot().captureRoboImage(outputPath)
    }

    private fun sampleInstallFolder(): InstallFolderState = InstallFolderState(
        treeUri = Uri.parse(
            "content://com.android.externalstorage.documents/tree/primary%3AGames%2FWarband"
        ),
        displayName = "Mount & Blade - Warband"
    )

    private fun sampleScanState(): ScanState = ScanState(
        inProgress = false,
        generation = 1L,
        folderLabel = "Mount & Blade - Warband",
        indexStatsText = "SafVfs index: 4210 files, 184 dirs in 380 ms"
    )

    private fun sampleModules(): List<ModuleChoice> = listOf(
        ModuleChoice(
            folderName = "Native",
            modulePath = "Modules/Native",
            displayName = "Native",
            hasScriptsTxt = true,
            loadResourceCount = 221,
            loadModResourceCount = 0
        ),
        ModuleChoice(
            folderName = "Custom_Realm",
            modulePath = "Modules/Custom_Realm",
            displayName = "Custom Realm",
            hasScriptsTxt = false,
            loadResourceCount = 221,
            loadModResourceCount = 48
        ),
        ModuleChoice(
            folderName = "Frontier_1257",
            modulePath = "Modules/Frontier_1257",
            displayName = "Frontier 1257",
            hasScriptsTxt = true,
            loadResourceCount = 221,
            loadModResourceCount = 64
        ),
        ModuleChoice(
            folderName = "Viking_Coast",
            modulePath = "Modules/Viking_Coast",
            displayName = "Viking Coast",
            hasScriptsTxt = true,
            loadResourceCount = 221,
            loadModResourceCount = 32
        )
    )

    private fun sampleModuleArtMap(): Map<String, ModuleArt> = mapOf(
        "Modules/Native" to createPlaceholderArt(variant = 0),
        "Modules/Custom_Realm" to createPlaceholderArt(variant = 1),
        "Modules/Frontier_1257" to createPlaceholderArt(variant = 2),
        "Modules/Viking_Coast" to createPlaceholderArt(variant = 3)
    )

    private fun sampleHomeReadyState(heroArt: ModuleArt?): HomeUiState = HomeUiState(
        installFolder = sampleInstallFolder(),
        scanState = sampleScanState(),
        selectedModule = sampleModules().first(),
        installStatus = ModuleInstallStatus(state = InstallCheckState.READY),
        heroArt = heroArt,
        statusLog = "Found 4 module(s) in 380 ms."
    )

    private fun sampleHomeMissingState(heroArt: ModuleArt?): HomeUiState = HomeUiState(
        installFolder = sampleInstallFolder(),
        scanState = sampleScanState(),
        selectedModule = sampleModules()[1],
        installStatus = ModuleInstallStatus(
            state = InstallCheckState.MISSING,
            missingFiles = listOf(
                "CommonRes/",
                "Data/font_data.xml",
                "Modules/Custom_Realm/scripts.txt"
            )
        ),
        heroArt = heroArt,
        statusLog = "Install check: 3 required file(s) missing."
    )

    private fun sampleModulesReadyState(): ModulesUiState {
        val modules = sampleModules()
        return ModulesUiState(
            modules = modules,
            selectedIndex = 0,
            installStatuses = mapOf(
                "Modules/Native" to ModuleInstallStatus(InstallCheckState.READY),
                "Modules/Custom_Realm" to ModuleInstallStatus(InstallCheckState.READY),
                "Modules/Frontier_1257" to ModuleInstallStatus(InstallCheckState.READY),
                "Modules/Viking_Coast" to ModuleInstallStatus(InstallCheckState.READY)
            )
        )
    }

    private fun sampleModulesMissingState(): ModulesUiState {
        val all = sampleModules()
        val reordered = listOf(all[1], all[0], all[2], all[3])
        return ModulesUiState(
            modules = reordered,
            selectedIndex = 0,
            installStatuses = mapOf(
                "Modules/Custom_Realm" to ModuleInstallStatus(
                    state = InstallCheckState.MISSING,
                    missingFiles = listOf(
                        "CommonRes/",
                        "Data/font_data.xml",
                        "Modules/Custom_Realm/scripts.txt"
                    )
                ),
                "Modules/Native" to ModuleInstallStatus(InstallCheckState.READY),
                "Modules/Frontier_1257" to ModuleInstallStatus(InstallCheckState.CHECKING),
                "Modules/Viking_Coast" to ModuleInstallStatus(InstallCheckState.READY)
            )
        )
    }

    private fun sampleSettingsState(): SettingsUiState = SettingsUiState(
        performance = PerformanceUiState(
            qualityPresetKey = "medium",
            frameRateCapHz = 60,
            presentModeKey = "fifo",
            renderScalePercent = 85,
            maxTextureSize = 1024,
            thermalThrottlingEnabled = true,
            dynamicResolutionEnabled = true,
            lodBiasScalingEnabled = true,
            uiAspectKey = "4:3",
            uiScalePercent = 110,
            gyroAimEnabled = true,
            pinchZoomEnabled = true,
            language = "en"
        ),
        isLoaded = true,
        languageOptions = listOf("en", "de", "fr", "pl"),
        hasPhoneGyro = true,
        rglConfig = RglConfigState(),
        rglConfigReady = true
    )

    private fun sampleAdvancedState(): AdvancedUiState = AdvancedUiState(
        installFolder = sampleInstallFolder(),
        scanState = sampleScanState(),
        selectedModule = sampleModules().first(),
        driverState = DriverUiState(
            selectedIndex = 0,
            statusText = "Uses the device's built-in Vulkan driver.",
            allowMockFallback = false,
            isProbeInProgress = false
        ),
        showFpsOverlay = true,
        statusLog = "OK: 1755 scripts, 131080 statements, 65/65 mapped (cached index, parse 42 ms)"
    )

    private data class ArtPalette(
        val topRgb: Triple<Int, Int, Int>,
        val midRgb: Triple<Int, Int, Int>,
        val bottomRgb: Triple<Int, Int, Int>,
        val accentRgb: Triple<Int, Int, Int>,
        val crestRgb: Triple<Int, Int, Int>,
        val seedArgb: Int
    )

    private fun createPlaceholderArt(
        width: Int = 320,
        height: Int = 240,
        variant: Int = 0
    ): ModuleArt {
        val palette = when (variant.mod(4)) {
            0 -> ArtPalette(
                topRgb = Triple(44, 28, 42),
                midRgb = Triple(176, 86, 46),
                bottomRgb = Triple(26, 18, 20),
                accentRgb = Triple(244, 196, 98),
                crestRgb = Triple(216, 134, 62),
                seedArgb = 0xFFD98236.toInt()
            )
            1 -> ArtPalette(
                topRgb = Triple(22, 46, 58),
                midRgb = Triple(62, 134, 140),
                bottomRgb = Triple(16, 26, 32),
                accentRgb = Triple(172, 232, 224),
                crestRgb = Triple(96, 184, 178),
                seedArgb = 0xFF4CA3A8.toInt()
            )
            2 -> ArtPalette(
                topRgb = Triple(24, 30, 68),
                midRgb = Triple(84, 102, 184),
                bottomRgb = Triple(14, 18, 38),
                accentRgb = Triple(236, 208, 128),
                crestRgb = Triple(156, 174, 240),
                seedArgb = 0xFF6B82D6.toInt()
            )
            else -> ArtPalette(
                topRgb = Triple(28, 44, 28),
                midRgb = Triple(108, 138, 72),
                bottomRgb = Triple(18, 26, 18),
                accentRgb = Triple(228, 182, 104),
                crestRgb = Triple(164, 186, 102),
                seedArgb = 0xFF7FA858.toInt()
            )
        }

        val pixels = IntArray(width * height)
        val wF = width.toFloat()
        val hF = height.toFloat()
        val sunX = 0.68f * wF
        val sunY = 0.30f * hF
        val sunRadius = 0.12f * hF
        val sunGlowRadius = 0.34f * hF
        val crestX = 0.30f * wF
        val crestY = 0.36f * hF
        val crestSize = 0.16f * hF

        for (y in 0 until height) {
            val ny = y / hF
            val baseColor = if (ny < 0.56f) {
                lerpRgb(palette.topRgb, palette.midRgb, ny / 0.56f)
            } else {
                lerpRgb(palette.midRgb, palette.bottomRgb, (ny - 0.56f) / 0.44f)
            }

            for (x in 0 until width) {
                val nx = x / wF
                var r = baseColor.first
                var g = baseColor.second
                var b = baseColor.third

                val diag = abs((nx * 0.7f + ny * 0.9f) - 0.85f)
                if (diag < 0.14f) {
                    val t = (1f - diag / 0.14f) * 0.28f
                    r = lerpChannel(r, palette.accentRgb.first, t)
                    g = lerpChannel(g, palette.accentRgb.second, t)
                    b = lerpChannel(b, palette.accentRgb.third, t)
                }

                val sdx = x - sunX
                val sdy = y - sunY
                val sDistSq = sdx * sdx + sdy * sdy
                if (sDistSq < sunGlowRadius * sunGlowRadius) {
                    val dist = sqrt(sDistSq)
                    if (dist <= sunRadius) {
                        r = palette.accentRgb.first
                        g = palette.accentRgb.second
                        b = palette.accentRgb.third
                    } else {
                        val glow = (1f - (dist - sunRadius) / (sunGlowRadius - sunRadius)) * 0.5f
                        r = lerpChannel(r, palette.accentRgb.first, glow)
                        g = lerpChannel(g, palette.accentRgb.second, glow)
                        b = lerpChannel(b, palette.accentRgb.third, glow)
                    }
                }

                val battlement = ((x / 18) % 2) * 0.03f
                val ridgeY = 0.56f - 0.08f * sin(nx * 5.5f + variant * 0.9f) +
                    if (nx in 0.16f..0.44f) -0.05f + battlement else 0f
                if (ny >= ridgeY) {
                    val depthT = ((ny - ridgeY) / 0.44f).coerceIn(0f, 1f)
                    val darkR = lerpChannel(palette.bottomRgb.first, 10, depthT)
                    val darkG = lerpChannel(palette.bottomRgb.second, 10, depthT)
                    val darkB = lerpChannel(palette.bottomRgb.third, 12, depthT)
                    r = lerpChannel(r, darkR, 0.86f)
                    g = lerpChannel(g, darkG, 0.86f)
                    b = lerpChannel(b, darkB, 0.86f)
                }

                val cdx = abs(x - crestX)
                val cdy = abs(y - crestY)
                val manhattan = cdx + cdy
                if (manhattan <= crestSize) {
                    val isBorder = manhattan >= crestSize * 0.78f
                    val target = if (isBorder) palette.accentRgb else palette.crestRgb
                    r = target.first
                    g = target.second
                    b = target.third
                }

                if (x < 4 || x >= width - 4 || y < 4 || y >= height - 4) {
                    r = lerpChannel(r, palette.accentRgb.first, 0.55f)
                    g = lerpChannel(g, palette.accentRgb.second, 0.55f)
                    b = lerpChannel(b, palette.accentRgb.third, 0.55f)
                }

                pixels[y * width + x] = packArgb(r, g, b)
            }
        }

        val bitmap = Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888)
        return ModuleArt(bitmap = bitmap.asImageBitmap(), seedArgb = palette.seedArgb)
    }

    private fun lerpChannel(a: Int, b: Int, t: Float): Int {
        val clamped = t.coerceIn(0f, 1f)
        return (a + (b - a) * clamped).roundToInt().coerceIn(0, 255)
    }

    private fun lerpRgb(
        a: Triple<Int, Int, Int>,
        b: Triple<Int, Int, Int>,
        t: Float
    ): Triple<Int, Int, Int> = Triple(
        lerpChannel(a.first, b.first, t),
        lerpChannel(a.second, b.second, t),
        lerpChannel(a.third, b.third, t)
    )

    private fun packArgb(r: Int, g: Int, b: Int, a: Int = 255): Int =
        ((a and 0xFF) shl 24) or
            ((r.coerceIn(0, 255) and 0xFF) shl 16) or
            ((g.coerceIn(0, 255) and 0xFF) shl 8) or
            (b.coerceIn(0, 255) and 0xFF)
}
