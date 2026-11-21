package org.wse2.android.launcher

import android.content.Context
import android.net.Uri
import org.json.JSONObject
import org.wse2.android.R
import org.wse2.android.launcher.art.ModuleArt
import java.util.Locale
import kotlin.math.roundToInt

enum class LauncherTab(val title: String, private val labelRes: Int) {
    HOME("Home", R.string.tab_home),
    MODULES("Modules", R.string.tab_modules),
    SETTINGS("Settings", R.string.tab_settings),
    ADVANCED("Advanced", R.string.tab_advanced);

    internal fun resolvedTitle(ctx: Context): String = ctx.getString(labelRes)
}

data class ModuleChoice(
    val folderName: String,
    val modulePath: String,
    val displayName: String,
    val hasScriptsTxt: Boolean,
    val loadResourceCount: Int,
    val loadModResourceCount: Int
) {
    override fun toString(): String =
        if (displayName == folderName) {
            folderName
        } else {
            "$displayName ($folderName)"
        }
}

enum class InstallCheckState {
    CHECKING,
    READY,
    MISSING
}

data class ModuleInstallStatus(
    val state: InstallCheckState = InstallCheckState.CHECKING,
    val missingFiles: List<String> = emptyList()
)

data class PerformanceUiState(
    val qualityPresetKey: String,
    val frameRateCapHz: Int,
    val presentModeKey: String,
    val renderScalePercent: Int,
    val maxTextureSize: Int,
    val thermalThrottlingEnabled: Boolean,
    val dynamicResolutionEnabled: Boolean,
    val lodBiasScalingEnabled: Boolean,
    val uiAspectKey: String,
    val uiScalePercent: Int,
    val gyroAimEnabled: Boolean,
    val pinchZoomEnabled: Boolean,
    val language: String
) {
    companion object {
        fun fromJson(rawJson: String): PerformanceUiState {
            val obj = try {
                JSONObject(rawJson)
            } catch (_: Exception) {
                JSONObject()
            }
            val preset = optStringClean(obj, "quality_preset", "custom")
                .lowercase(Locale.US)
                .takeIf { it in VALID_QUALITY_PRESETS }
                ?: "custom"
            val capHz = parseFrameRateCap(obj)
            val presentMode = optStringClean(obj, "present_mode", "fifo")
                .lowercase(Locale.US)
                .takeIf { it in VALID_PRESENT_MODES }
                ?: "fifo"
            val renderScalePct = parseSteppedPercent(
                obj = obj,
                key = "render_scale",
                defaultScale = 1.0,
                minPct = 50,
                maxPct = 100,
                stepPct = 5
            )
            val maxTex = parseIntSetting(obj, "max_texture_size", 0).coerceAtLeast(0)
            val uiAspect = optStringClean(obj, "ui_aspect", "4:3")
                .lowercase(Locale.US)
                .takeIf { it in VALID_UI_ASPECTS }
                ?: "4:3"
            val uiScalePct = parseSteppedPercent(
                obj = obj,
                key = "ui_scale",
                defaultScale = 1.0,
                minPct = 75,
                maxPct = 200,
                stepPct = 5
            )
            val language = optStringClean(obj, "language", "en").ifEmpty { "en" }

            return PerformanceUiState(
                qualityPresetKey = preset,
                frameRateCapHz = capHz,
                presentModeKey = presentMode,
                renderScalePercent = renderScalePct,
                maxTextureSize = maxTex,
                thermalThrottlingEnabled = parseBoolSetting(obj, "thermal_throttling_enabled", true),
                dynamicResolutionEnabled = parseBoolSetting(obj, "dynamic_resolution_enabled", true),
                lodBiasScalingEnabled = parseBoolSetting(obj, "lod_bias_scaling_enabled", true),
                uiAspectKey = uiAspect,
                uiScalePercent = uiScalePct,
                gyroAimEnabled = parseBoolSetting(obj, "gyro_aim_enabled", false),
                pinchZoomEnabled = parseBoolSetting(obj, "pinch_zoom_enabled", true),
                language = language
            )
        }

        private val VALID_QUALITY_PRESETS = setOf("low", "medium", "high", "custom")
        private val VALID_PRESENT_MODES = setOf("fifo", "mailbox", "immediate", "fifo_relaxed")
        private val VALID_UI_ASPECTS = setOf("4:3", "16:9", "stretch")

        private fun optStringClean(obj: JSONObject, key: String, fallback: String): String {
            if (!obj.has(key) || obj.isNull(key)) {
                return fallback
            }
            return obj.optString(key, fallback).trim()
        }

        private fun parseFrameRateCap(obj: JSONObject): Int {
            val raw = if (obj.has("frame_rate_cap") && !obj.isNull("frame_rate_cap")) {
                obj.opt("frame_rate_cap")
            } else {
                null
            }
            if (raw is Number) {
                val hz = raw.toInt()
                return if (hz <= 0) 0 else hz
            }
            val text = (raw as? String)?.trim()?.lowercase(Locale.US) ?: ""
            return when {
                text == "uncapped" || text == "unlimited" || text == "0" -> 0
                text.endsWith("hz") -> text.removeSuffix("hz").toIntOrNull()?.let { if (it <= 0) 0 else it } ?: 60
                text.isNotEmpty() -> text.toIntOrNull()?.let { if (it <= 0) 0 else it } ?: 60
                else -> 60
            }
        }

        private fun parseIntSetting(obj: JSONObject, key: String, fallback: Int): Int {
            if (!obj.has(key) || obj.isNull(key)) {
                return fallback
            }
            return when (val raw = obj.opt(key)) {
                is Number -> raw.toInt()
                is String -> raw.trim().toIntOrNull() ?: fallback
                else -> fallback
            }
        }

        private fun parseSteppedPercent(
            obj: JSONObject,
            key: String,
            defaultScale: Double,
            minPct: Int,
            maxPct: Int,
            stepPct: Int
        ): Int {
            val raw = if (obj.has(key) && !obj.isNull(key)) obj.opt(key) else null
            val scale = when (raw) {
                is Number -> raw.toDouble()
                is String -> raw.trim().toDoubleOrNull()
                else -> null
            }
            val valid = if (scale != null && scale.isFinite()) scale else defaultScale
            val clampedPct = (valid * 100.0).roundToInt().coerceIn(minPct, maxPct)
            val steps = ((clampedPct - minPct + (stepPct / 2)) / stepPct)
            return (minPct + steps * stepPct).coerceIn(minPct, maxPct)
        }

        private fun parseBoolSetting(obj: JSONObject, key: String, fallback: Boolean): Boolean {
            if (!obj.has(key) || obj.isNull(key)) {
                return fallback
            }
            return when (val raw = obj.opt(key)) {
                is Boolean -> raw
                is Number -> raw.toInt() != 0
                is String -> when (raw.trim().lowercase(Locale.US)) {
                    "true", "1" -> true
                    "false", "0" -> false
                    else -> fallback
                }
                else -> fallback
            }
        }
    }
}

object LauncherDefaults {
    val DRIVER_OPTIONS = listOf(
        "System Vulkan",
        "Turnip (Adreno custom)",
        "PanVK (Mali direct ICD)"
    )
    val DRIVER_MODE_KEYS = listOf("system", "turnip", "panvk")
    val ALL_FRAME_RATE_VALUES = listOf(30, 60, 90, 120, 0)

    const val QUALITY_PRESET_CUSTOM = "custom"
    val QUALITY_PRESET_LABELS = listOf("Low", "Medium", "High", "Custom")
    val QUALITY_PRESET_KEYS = listOf("low", "medium", "high", "custom")

    val PRESENT_MODE_KEYS = listOf("fifo", "mailbox", "immediate", "fifo_relaxed")
    val PRESENT_MODE_LABELS = listOf(
        "Present: FIFO (vsync)",
        "Present: mailbox",
        "Present: immediate (tearing)",
        "Present: FIFO relaxed"
    )

    val TEXTURE_SIZE_LABELS = listOf("No cap", "256", "512", "1024", "2048", "4096")
    val TEXTURE_SIZE_VALUES = listOf(0, 256, 512, 1024, 2048, 4096)

    val UI_ASPECT_LABELS = listOf("4:3", "16:9", "Stretch")
    val UI_ASPECT_KEYS = listOf("4:3", "16:9", "stretch")

    const val RENDER_SCALE_MIN = 50
    const val RENDER_SCALE_MAX = 100
    const val UI_SCALE_MIN = 75
    const val UI_SCALE_MAX = 200
    const val SCALE_STEP = 5

    fun frameRateLabel(hz: Int): String = when (hz) {
        0 -> "Uncapped"
        30 -> "30 Hz (battery saver)"
        else -> "$hz Hz"
    }

    fun snapPercent(percent: Int, minPct: Int, maxPct: Int): Int {
        val clamped = percent.coerceIn(minPct, maxPct)
        val steps = (clamped - minPct + (SCALE_STEP / 2)) / SCALE_STEP
        return (minPct + steps * SCALE_STEP).coerceIn(minPct, maxPct)
    }

    fun isValidLanguageTag(tag: String): Boolean {
        if (tag.isEmpty() || tag.length > 16) {
            return false
        }
        return tag.all { c ->
            c in 'A'..'Z' || c in 'a'..'z' || c in '0'..'9' || c == '_' || c == '-'
        }
    }
}

data class InstallFolderState(
    val treeUri: Uri? = null,
    val displayName: String? = null
)

data class ScanState(
    val inProgress: Boolean = false,
    val generation: Long = 0L,
    val folderLabel: String? = null,
    val indexStatsText: String = "SafVfs index: idle"
)

data class HomeUiState(
    val installFolder: InstallFolderState = InstallFolderState(),
    val scanState: ScanState = ScanState(),
    val selectedModule: ModuleChoice? = null,
    val installStatus: ModuleInstallStatus? = null,
    val heroArt: ModuleArt? = null,
    val statusLog: String = "Ready."
)

data class ModulesUiState(
    val modules: List<ModuleChoice> = emptyList(),
    val selectedIndex: Int = -1,
    val installStatuses: Map<String, ModuleInstallStatus> = emptyMap(),
    val emptyMessage: String = "Select a Warband folder to discover modules."
) {
    val selectedModule: ModuleChoice?
        get() = modules.getOrNull(selectedIndex)

    val selectedInstallStatus: ModuleInstallStatus?
        get() = selectedModule?.let { installStatuses[it.modulePath] }
}

data class SettingsUiState(
    val performance: PerformanceUiState? = null,
    val isLoaded: Boolean = false,
    val languageOptions: List<String> = listOf("en"),
    val frameRateValues: List<Int> = LauncherDefaults.ALL_FRAME_RATE_VALUES,
    val frameRateLabels: List<String> = LauncherDefaults.ALL_FRAME_RATE_VALUES.map {
        LauncherDefaults.frameRateLabel(it)
    },
    val presentModeKeys: List<String> = LauncherDefaults.PRESENT_MODE_KEYS,
    val presentModeLabels: List<String> = LauncherDefaults.PRESENT_MODE_LABELS,
    val hasPhoneGyro: Boolean = false,
    val rglConfig: RglConfigState = RglConfigState(),
    val rglConfigReady: Boolean = false,
    val rglConfigPath: String = "Documents/Mount&Blade Warband/rgl_config.txt"
)

data class DriverUiState(
    val options: List<String> = LauncherDefaults.DRIVER_OPTIONS,
    val modeKeys: List<String> = LauncherDefaults.DRIVER_MODE_KEYS,
    val selectedIndex: Int = 0,
    val statusText: String = "Uses the device's built-in Vulkan driver.",
    val allowMockFallback: Boolean = false,
    val isProbeInProgress: Boolean = false
) {
    val selectedModeKey: String
        get() = modeKeys[selectedIndex.coerceIn(0, modeKeys.lastIndex)]
}

// Phase 5 R0 feasibility spikes (`R0Spikes.kt`): `report` is shown in a dialog when set.
data class R0UiState(
    val running: Boolean = false,
    val report: String? = null
)

// Phase 5 R4 startup run (`R4StartupRunner`): `status` while it runs, `report` in a dialog when
// set. `useCopy` reads the install from a copy in app storage instead of the folder.
data class R4UiState(
    val running: Boolean = false,
    val useCopy: Boolean = false,
    val status: String = "",
    val report: String? = null
)

data class AdvancedUiState(
    val installFolder: InstallFolderState = InstallFolderState(),
    val scanState: ScanState = ScanState(),
    val selectedModule: ModuleChoice? = null,
    val driverState: DriverUiState = DriverUiState(),
    val showFpsOverlay: Boolean = false,
    val statusLog: String = "Ready."
)

sealed interface LauncherEvent {
    data class ShowScanError(val reason: String) : LauncherEvent

    data class ShowMissingFiles(
        val module: ModuleChoice,
        val missingFiles: List<String>
    ) : LauncherEvent

    data class ShowWarbandFilesNotFound(val reason: String) : LauncherEvent

    data class ShowToast(val message: String) : LauncherEvent

    data class LaunchGame(
        val runModule: Boolean,
        val treeUri: String,
        val moduleName: String,
        val gpuDriver: String,
        val showFpsOverlay: Boolean
    ) : LauncherEvent
}
