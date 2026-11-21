package org.wse2.android.launcher

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.wse2.android.launcher.theme.LocalWseColors
import org.wse2.android.launcher.theme.Wse2Theme
import org.wse2.android.launcher.theme.wseBackdrop
import org.wse2.android.launcher.ui.WseCategoryStrip
import org.wse2.android.launcher.ui.WseCategoryTab
import org.wse2.android.launcher.ui.WseDialog
import org.wse2.android.launcher.ui.WseDialogAction
import org.wse2.android.launcher.ui.WseKeyValueGrid
import org.wse2.android.launcher.ui.WseMetricItem
import org.wse2.android.launcher.ui.WseScreenHeader
import org.wse2.android.launcher.ui.WseSecondaryButton
import org.wse2.android.launcher.ui.WseStatusChip
import org.wse2.android.launcher.ui.WseStatusTone
import java.util.Locale
import kotlin.math.roundToInt

private val SettingsPlaceholderPerformance = PerformanceUiState.fromJson("{}")

enum class SettingsCategory(val label: String) {
    PERFORMANCE("Performance"),
    GRAPHICS("Graphics"),
    WORLD("World"),
    GAMEPLAY("Gameplay"),
    AUDIO_INPUT("Audio & Input"),
    SYSTEM("System")
}

@Composable
fun SettingsScreen(
    state: SettingsUiState,
    onQualityPresetChange: (String) -> Unit,
    onFrameRateCapChange: (Int) -> Unit,
    onPresentModeChange: (String) -> Unit,
    onRenderScaleChange: (Int) -> Unit,
    onMaxTextureSizeChange: (Int) -> Unit,
    onThermalThrottlingChange: (Boolean) -> Unit,
    onDynamicResolutionChange: (Boolean) -> Unit,
    onLodBiasScalingChange: (Boolean) -> Unit,
    onUiAspectChange: (String) -> Unit,
    onUiScaleChange: (Int) -> Unit,
    onGyroAimChange: (Boolean) -> Unit,
    onPinchZoomChange: (Boolean) -> Unit,
    onLanguageChange: (String) -> Unit,
    modifier: Modifier = Modifier,
    topContentPadding: Dp = 0.dp,
    onUpdateRglConfig: ((RglConfigState) -> RglConfigState) -> Unit = {},
    onResetRglConfig: () -> Unit = {},
    initialCategory: SettingsCategory = SettingsCategory.PERFORMANCE
) {
    val perf = state.performance ?: SettingsPlaceholderPerformance
    val rgl = state.rglConfig
    val enabled = state.isLoaded
    var selectedCategory by rememberSaveable { mutableStateOf(initialCategory) }
    var showResetDialog by remember { mutableStateOf(false) }

    val categories = remember { SettingsCategory.entries }
    val categoryTabs = remember(categories) {
        categories.map { WseCategoryTab(label = it.label) }
    }

    Box(
        modifier = modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
        contentAlignment = Alignment.TopCenter
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .widthIn(max = 720.dp)
                .padding(
                    start = 16.dp,
                    end = 16.dp,
                    top = topContentPadding + 16.dp,
                    bottom = 28.dp
                ),
            verticalArrangement = Arrangement.spacedBy(14.dp)
        ) {
            WseScreenHeader(
                eyebrow = "Configuration",
                title = "Settings",
                subtitle = "Tune Android performance, display scaling and every rgl_config.txt property.",
                trailing = {
                    WseStatusChip(
                        text = if (state.rglConfigReady) "rgl_config ready" else "rgl_config",
                        tone = if (state.rglConfigReady) WseStatusTone.Ready else WseStatusTone.Neutral
                    )
                }
            )

            WseCategoryStrip(
                tabs = categoryTabs,
                selectedIndex = categories.indexOf(selectedCategory).coerceAtLeast(0),
                onSelect = { idx -> selectedCategory = categories[idx] }
            )

            when (selectedCategory) {
                SettingsCategory.PERFORMANCE -> PerformanceSettingsSection(
                    state = state,
                    perf = perf,
                    enabled = enabled,
                    onQualityPresetChange = onQualityPresetChange,
                    onFrameRateCapChange = onFrameRateCapChange,
                    onPresentModeChange = onPresentModeChange,
                    onRenderScaleChange = onRenderScaleChange,
                    onMaxTextureSizeChange = onMaxTextureSizeChange,
                    onThermalThrottlingChange = onThermalThrottlingChange,
                    onDynamicResolutionChange = onDynamicResolutionChange,
                    onLodBiasScalingChange = onLodBiasScalingChange,
                    onUiAspectChange = onUiAspectChange,
                    onUiScaleChange = onUiScaleChange,
                    onLanguageChange = onLanguageChange
                )
                SettingsCategory.GRAPHICS -> RglGraphicsSettingsSection(
                    rgl = rgl,
                    onUpdate = onUpdateRglConfig
                )
                SettingsCategory.WORLD -> RglWorldSettingsSection(
                    rgl = rgl,
                    onUpdate = onUpdateRglConfig
                )
                SettingsCategory.GAMEPLAY -> RglGameplaySettingsSection(
                    rgl = rgl,
                    onUpdate = onUpdateRglConfig
                )
                SettingsCategory.AUDIO_INPUT -> AudioInputSettingsSection(
                    state = state,
                    perf = perf,
                    rgl = rgl,
                    perfEnabled = enabled,
                    onGyroAimChange = onGyroAimChange,
                    onPinchZoomChange = onPinchZoomChange,
                    onUpdateRgl = onUpdateRglConfig
                )
                SettingsCategory.SYSTEM -> RglSystemSettingsSection(
                    state = state,
                    rgl = rgl,
                    onUpdate = onUpdateRglConfig,
                    onRequestReset = { showResetDialog = true }
                )
            }
        }
    }

    if (showResetDialog) {
        WseDialog(
            title = "Reset rgl_config.txt?",
            text = "Restores all Warband graphics, world, gameplay, audio and system settings " +
                "in rgl_config.txt back to their default values.",
            confirm = WseDialogAction("Reset") {
                showResetDialog = false
                onResetRglConfig()
            },
            dismiss = WseDialogAction("Cancel") { showResetDialog = false },
            onDismissRequest = { showResetDialog = false }
        )
    }
}

@Composable
private fun PerformanceSettingsSection(
    state: SettingsUiState,
    perf: PerformanceUiState,
    enabled: Boolean,
    onQualityPresetChange: (String) -> Unit,
    onFrameRateCapChange: (Int) -> Unit,
    onPresentModeChange: (String) -> Unit,
    onRenderScaleChange: (Int) -> Unit,
    onMaxTextureSizeChange: (Int) -> Unit,
    onThermalThrottlingChange: (Boolean) -> Unit,
    onDynamicResolutionChange: (Boolean) -> Unit,
    onLodBiasScalingChange: (Boolean) -> Unit,
    onUiAspectChange: (String) -> Unit,
    onUiScaleChange: (Int) -> Unit,
    onLanguageChange: (String) -> Unit
) {
    val colors = LocalWseColors.current
    LauncherSectionHeader(title = "Performance Presets", badge = "Launcher + rgl_config")
    LauncherGroupedCard {
        val presetIdx = LauncherDefaults.QUALITY_PRESET_KEYS
            .indexOf(perf.qualityPresetKey)
            .coerceAtLeast(0)
        TileGridListItem(
            headline = "Quality preset",
            supporting = "Applies balanced render scale, texture cap and rgl_config.txt quality.",
            options = LauncherDefaults.QUALITY_PRESET_LABELS,
            selectedIndex = presetIdx,
            columns = 4,
            valueReadout = LauncherDefaults.QUALITY_PRESET_LABELS[presetIdx],
            enabled = enabled,
            onSelected = { idx ->
                onQualityPresetChange(LauncherDefaults.QUALITY_PRESET_KEYS[idx])
            }
        )
        LauncherSectionDivider()

        val capIdx = state.frameRateValues
            .indexOf(perf.frameRateCapHz)
            .coerceAtLeast(0)
        val compactCapLabels = state.frameRateValues.map { hz ->
            if (hz <= 0) "Max" else "$hz"
        }
        val capReadout = if (perf.frameRateCapHz <= 0) "Uncapped" else "${perf.frameRateCapHz} FPS"
        TileGridListItem(
            headline = "Frame-rate cap",
            options = compactCapLabels,
            selectedIndex = capIdx,
            columns = compactCapLabels.size.coerceIn(1, 5),
            valueReadout = capReadout,
            enabled = enabled,
            onSelected = { idx ->
                onFrameRateCapChange(state.frameRateValues[idx])
            }
        )
        LauncherSectionDivider()

        SliderListItem(
            headline = "Render scale",
            supporting = "Internal 3D viewport scale relative to screen resolution.",
            persistedPercent = perf.renderScalePercent,
            minPercent = LauncherDefaults.RENDER_SCALE_MIN,
            maxPercent = LauncherDefaults.RENDER_SCALE_MAX,
            stepPercent = LauncherDefaults.SCALE_STEP,
            enabled = enabled,
            onCommitPercent = onRenderScaleChange
        )
        LauncherSectionDivider()

        val texIdx = LauncherDefaults.TEXTURE_SIZE_VALUES
            .indexOf(perf.maxTextureSize)
            .coerceAtLeast(0)
        val texReadout = LauncherDefaults.TEXTURE_SIZE_LABELS[texIdx]
        TileGridListItem(
            headline = "Texture size cap",
            supporting = "Clamps oversized DDS textures to save memory on mobile GPUs.",
            options = LauncherDefaults.TEXTURE_SIZE_LABELS,
            selectedIndex = texIdx,
            columns = 3,
            valueReadout = texReadout,
            enabled = enabled,
            onSelected = { idx ->
                onMaxTextureSizeChange(LauncherDefaults.TEXTURE_SIZE_VALUES[idx])
            }
        )
        LauncherSectionDivider()

        val modeIdx = state.presentModeKeys
            .indexOf(perf.presentModeKey)
            .coerceAtLeast(0)
        ExposedDropdownListItem(
            headline = "Present mode",
            options = state.presentModeLabels,
            selectedIndex = modeIdx,
            enabled = enabled,
            onSelected = { idx ->
                onPresentModeChange(state.presentModeKeys[idx])
            }
        )
    }

    LauncherSectionHeader(title = "Thermal Management")
    LauncherGroupedCard {
        SwitchListItem(
            headline = "Thermal headroom throttling",
            supporting = "Adjusts resolution, LOD bias and FPS when the phone warms up",
            checked = perf.thermalThrottlingEnabled,
            enabled = enabled,
            onCheckedChange = onThermalThrottlingChange
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Dynamic resolution under thermal load",
            checked = perf.dynamicResolutionEnabled,
            enabled = enabled,
            onCheckedChange = onDynamicResolutionChange
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "LOD bias scaling under thermal load",
            checked = perf.lodBiasScalingEnabled,
            enabled = enabled,
            onCheckedChange = onLodBiasScalingChange
        )
    }

    LauncherSectionHeader(title = "Display & Language")
    LauncherGroupedCard {
        val aspectIdx = LauncherDefaults.UI_ASPECT_KEYS
            .indexOf(perf.uiAspectKey)
            .coerceAtLeast(0)
        SegmentedListItem(
            headline = "UI aspect",
            supporting = "Keeps 2D menus undistorted on tall phone screens.",
            options = LauncherDefaults.UI_ASPECT_LABELS,
            selectedIndex = aspectIdx,
            enabled = enabled,
            showSelectedIcon = true,
            onSelected = { idx ->
                onUiAspectChange(LauncherDefaults.UI_ASPECT_KEYS[idx])
            }
        )
        LauncherSectionDivider()
        SliderListItem(
            headline = "UI text scale",
            persistedPercent = perf.uiScalePercent,
            minPercent = LauncherDefaults.UI_SCALE_MIN,
            maxPercent = LauncherDefaults.UI_SCALE_MAX,
            stepPercent = LauncherDefaults.SCALE_STEP,
            enabled = enabled,
            onCommitPercent = onUiScaleChange
        )
        LauncherSectionDivider()
        val langOptions = state.languageOptions.ifEmpty { listOf("en") }
        val langIdx = langOptions.indexOf(perf.language).coerceAtLeast(0)
        ExposedDropdownListItem(
            headline = "Game language",
            options = langOptions,
            selectedIndex = langIdx,
            enabled = enabled,
            onSelected = { idx ->
                onLanguageChange(langOptions[idx])
            },
            supporting = "Detected from the install's languages/ folder."
        )
    }

    Text(
        text = "Need per-feature Warband controls? Switch categories above to edit all 80 properties in rgl_config.txt.",
        style = MaterialTheme.typography.bodySmall,
        color = colors.onSurfaceVariant,
        modifier = Modifier.padding(horizontal = 4.dp)
    )
}

@Composable
private fun RglGraphicsSettingsSection(
    rgl: RglConfigState,
    onUpdate: ((RglConfigState) -> RglConfigState) -> Unit
) {
    LauncherSectionHeader(title = "Textures & Shaders", badge = "rgl_config.txt")
    LauncherGroupedCard {
        IntSliderListItem(
            headline = "Texture detail",
            supporting = "texture_detail — Scales texture mip sharpness",
            persistedValue = rgl.textureDetail,
            minValue = 10,
            maxValue = 100,
            step = 5,
            enabled = true,
            formatValue = { "$it%" },
            onCommitValue = { v -> onUpdate { it.copy(textureDetail = v) } }
        )
        LauncherSectionDivider()
        TileGridListItem(
            headline = "Shader quality",
            supporting = "shader_quality",
            options = RglConfigOptions.SHADER_QUALITY_LABELS,
            selectedIndex = rgl.shaderQuality.coerceIn(0, 2),
            columns = 3,
            valueReadout = RglConfigOptions.SHADER_QUALITY_LABELS[rgl.shaderQuality.coerceIn(0, 2)],
            enabled = true,
            onSelected = { idx -> onUpdate { it.copy(shaderQuality = idx) } }
        )
        LauncherSectionDivider()
        val aaIdx = RglConfigOptions.ANTIALIASING_VALUES
            .indexOf(rgl.antialiasing)
            .coerceAtLeast(0)
        TileGridListItem(
            headline = "Antialiasing (MSAA)",
            supporting = "antialiasing",
            options = RglConfigOptions.ANTIALIASING_LABELS,
            selectedIndex = aaIdx,
            columns = 5,
            valueReadout = RglConfigOptions.ANTIALIASING_LABELS[aaIdx],
            enabled = true,
            onSelected = { idx ->
                onUpdate { it.copy(antialiasing = RglConfigOptions.ANTIALIASING_VALUES[idx]) }
            }
        )
        LauncherSectionDivider()
        IntSliderListItem(
            headline = "Sample quality",
            supporting = "sample_quality — Additional multisampling quality level",
            persistedValue = rgl.sampleQuality,
            minValue = 0,
            maxValue = 4,
            step = 1,
            enabled = true,
            formatValue = { "$it" },
            onCommitValue = { v -> onUpdate { it.copy(sampleQuality = v) } }
        )
        LauncherSectionDivider()
        IntSliderListItem(
            headline = "Render buffer size",
            supporting = "render_buffer_size — Vertex/texture staging buffer in MiB",
            persistedValue = rgl.renderBufferSize,
            minValue = 15,
            maxValue = 512,
            step = 5,
            enabled = true,
            formatValue = { "$it MB" },
            onCommitValue = { v -> onUpdate { it.copy(renderBufferSize = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Anisotropic filtering",
            supporting = "anisotropic_filtering — Sharper ground textures at oblique angles",
            checked = rgl.anisotropicFiltering,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(anisotropicFiltering = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Load textures on demand",
            supporting = "use_ondemand_textures_ — Streams textures to lower memory use",
            checked = rgl.useOnDemandTextures,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(useOnDemandTextures = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Multithreaded on-demand textures",
            supporting = "use_ondemand_textures_mt",
            checked = rgl.useOnDemandTexturesMt,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(useOnDemandTexturesMt = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Pixel shaders",
            supporting = "use_pixel_shaders",
            checked = rgl.usePixelShaders,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(usePixelShaders = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Vertex shaders",
            supporting = "use_vertex_shaders",
            checked = rgl.useVertexShaders,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(useVertexShaders = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Fake reflections",
            supporting = "fake_reflections — Lightweight water and metal reflections",
            checked = rgl.fakeReflections,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(fakeReflections = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Hardware instancing",
            supporting = "use_instancing — Batches repeated scene props and foliage",
            checked = rgl.useInstancing,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(useInstancing = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Alpha to coverage",
            supporting = "alpha2coverage — Smoother foliage edges with MSAA",
            checked = rgl.alpha2Coverage,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(alpha2Coverage = v) } }
        )
    }

    LauncherSectionHeader(title = "Post-Processing", badge = "rgl_config.txt")
    LauncherGroupedCard {
        TileGridListItem(
            headline = "HDR mode",
            supporting = "postfx_hdr",
            options = RglConfigOptions.POSTFX_HDR_LABELS,
            selectedIndex = rgl.postfxHdr.coerceIn(0, 2),
            columns = 3,
            valueReadout = RglConfigOptions.POSTFX_HDR_LABELS[rgl.postfxHdr.coerceIn(0, 2)],
            enabled = true,
            onSelected = { idx -> onUpdate { it.copy(postfxHdr = idx) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Auto-exposure",
            supporting = "postfx_autoexp — Dynamic brightness adaptation",
            checked = rgl.postfxAutoexp,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(postfxAutoexp = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Depth of field",
            supporting = "postfx_dof — Soft focus blur on distant scenery",
            checked = rgl.postfxDof,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(postfxDof = v) } }
        )
        LauncherSectionDivider()
        FloatSliderListItem(
            headline = "Gamma",
            supporting = "gamma",
            persistedValue = rgl.gamma,
            minValue = 1.0f,
            maxValue = 3.0f,
            step = 0.05f,
            enabled = true,
            formatValue = { String.format(Locale.US, "%.2f", it) },
            onCommitValue = { v -> onUpdate { it.copy(gamma = v) } }
        )
    }

    LauncherSectionHeader(title = "Video & Display", badge = "rgl_config.txt")
    LauncherGroupedCard {
        val currentResIdx = RglConfigOptions.RESOLUTION_PRESETS
            .indexOfFirst { it.first == rgl.displayWidth && it.second == rgl.displayHeight }
            .coerceAtLeast(0)
        val resReadout = if (rgl.displayWidth == 0 && rgl.displayHeight == 0) {
            "Auto"
        } else {
            "${rgl.displayWidth}x${rgl.displayHeight}"
        }
        TileGridListItem(
            headline = "Display resolution",
            supporting = "display_width / display_height (0 = match device)",
            options = RglConfigOptions.RESOLUTION_LABELS,
            selectedIndex = currentResIdx,
            columns = 5,
            valueReadout = resReadout,
            enabled = true,
            onSelected = { idx ->
                val (w, h) = RglConfigOptions.RESOLUTION_PRESETS[idx]
                onUpdate { it.copy(displayWidth = w, displayHeight = h) }
            }
        )
        LauncherSectionDivider()
        IntSliderListItem(
            headline = "Max framerate",
            supporting = "max_framerate",
            persistedValue = rgl.maxFramerate,
            minValue = 30,
            maxValue = 240,
            step = 10,
            enabled = true,
            formatValue = { "$it FPS" },
            onCommitValue = { v -> onUpdate { it.copy(maxFramerate = v) } }
        )
        LauncherSectionDivider()
        val bitsIdx = RglConfigOptions.DISPLAY_BITS_VALUES.indexOf(rgl.displayBits).coerceAtLeast(0)
        SegmentedListItem(
            headline = "Color depth",
            supporting = "display_bits",
            options = RglConfigOptions.DISPLAY_BITS_LABELS,
            selectedIndex = bitsIdx,
            enabled = true,
            showSelectedIcon = false,
            onSelected = { idx ->
                onUpdate { it.copy(displayBits = RglConfigOptions.DISPLAY_BITS_VALUES[idx]) }
            }
        )
        LauncherSectionDivider()
        SegmentedListItem(
            headline = "Adapter format",
            supporting = "adapter_format",
            options = RglConfigOptions.ADAPTER_FORMAT_LABELS,
            selectedIndex = rgl.adapterFormat.coerceIn(0, 2),
            enabled = true,
            showSelectedIcon = false,
            onSelected = { idx -> onUpdate { it.copy(adapterFormat = idx) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Force vertical sync",
            supporting = "force_vsync",
            checked = rgl.forceVsync,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(forceVsync = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Start windowed",
            supporting = "start_windowed",
            checked = rgl.startWindowed,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(startWindowed = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Aspect ratio control",
            supporting = "enable_aspect_ratio_control",
            checked = rgl.enableAspectRatioControl,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(enableAspectRatioControl = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Show in-game framerate",
            supporting = "show_framerate",
            checked = rgl.showFramerate,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(showFramerate = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Auto graphics quality",
            supporting = "auto_gfx_quality",
            checked = rgl.autoGfxQuality,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(autoGfxQuality = v) } }
        )
    }
}

@Composable
private fun RglWorldSettingsSection(
    rgl: RglConfigState,
    onUpdate: ((RglConfigState) -> RglConfigState) -> Unit
) {
    LauncherSectionHeader(title = "Shadows & Lighting", badge = "rgl_config.txt")
    LauncherGroupedCard {
        TileGridListItem(
            headline = "Shadow quality",
            supporting = "shadowmap_quality",
            options = RglConfigOptions.SHADOWMAP_QUALITY_LABELS,
            selectedIndex = rgl.shadowmapQuality.coerceIn(0, 2),
            columns = 3,
            valueReadout = RglConfigOptions.SHADOWMAP_QUALITY_LABELS[rgl.shadowmapQuality.coerceIn(0, 2)],
            enabled = true,
            onSelected = { idx -> onUpdate { it.copy(shadowmapQuality = idx) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Dynamic lighting",
            supporting = "enable_lighting",
            checked = rgl.enableLighting,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(enableLighting = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Character shadows",
            supporting = "enable_character_shadows",
            checked = rgl.enableCharacterShadows,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(enableCharacterShadows = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Accurate shadows",
            supporting = "enable_accurate_shadows",
            checked = rgl.enableAccurateShadows,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(enableAccurateShadows = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Environment shadows",
            supporting = "enable_environment_shadows",
            checked = rgl.enableEnvironmentShadows,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(enableEnvironmentShadows = v) } }
        )
        LauncherSectionDivider()
        TileGridListItem(
            headline = "Realistic shadows on plants",
            supporting = "realistic_shadows_on_plants",
            options = RglConfigOptions.PLANT_SHADOW_LABELS,
            selectedIndex = rgl.realisticShadowsOnPlants.coerceIn(0, 2),
            columns = 3,
            valueReadout = RglConfigOptions.PLANT_SHADOW_LABELS[rgl.realisticShadowsOnPlants.coerceIn(0, 2)],
            enabled = true,
            onSelected = { idx -> onUpdate { it.copy(realisticShadowsOnPlants = idx) } }
        )
        LauncherSectionDivider()
        FloatSliderListItem(
            headline = "Character detail",
            supporting = "character_detail",
            persistedValue = rgl.characterDetail,
            minValue = 0f,
            maxValue = 1f,
            step = 0.05f,
            enabled = true,
            formatValue = { "${(it * 100f).roundToInt()}%" },
            onCommitValue = { v -> onUpdate { it.copy(characterDetail = v) } }
        )
        LauncherSectionDivider()
        FloatSliderListItem(
            headline = "Character shadow detail",
            supporting = "character_shadow_detail",
            persistedValue = rgl.characterShadowDetail,
            minValue = 0f,
            maxValue = 1f,
            step = 0.05f,
            enabled = true,
            formatValue = { "${(it * 100f).roundToInt()}%" },
            onCommitValue = { v -> onUpdate { it.copy(characterShadowDetail = v) } }
        )
    }

    LauncherSectionHeader(title = "Foliage & Effects", badge = "rgl_config.txt")
    LauncherGroupedCard {
        IntSliderListItem(
            headline = "Grass density",
            supporting = "grass_density",
            persistedValue = rgl.grassDensity,
            minValue = 0,
            maxValue = 100,
            step = 5,
            enabled = true,
            formatValue = { "$it%" },
            onCommitValue = { v -> onUpdate { it.copy(grassDensity = v) } }
        )
        LauncherSectionDivider()
        TileGridListItem(
            headline = "Tree & flora detail",
            supporting = "flora_lod_detail",
            options = RglConfigOptions.FLORA_LOD_LABELS,
            selectedIndex = rgl.floraLodDetail.coerceIn(0, 2),
            columns = 3,
            valueReadout = RglConfigOptions.FLORA_LOD_LABELS[rgl.floraLodDetail.coerceIn(0, 2)],
            enabled = true,
            onSelected = { idx -> onUpdate { it.copy(floraLodDetail = idx) } }
        )
        LauncherSectionDivider()
        FloatSliderListItem(
            headline = "Tree degrade distance",
            supporting = "flora_degrade_distance",
            persistedValue = rgl.floraDegradeDistance,
            minValue = 40f,
            maxValue = 200f,
            step = 10f,
            enabled = true,
            formatValue = { "${it.roundToInt()} m" },
            onCommitValue = { v -> onUpdate { it.copy(floraDegradeDistance = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Particle systems",
            supporting = "enable_particles — Smoke, dust, sparks and weather",
            checked = rgl.enableParticles,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(enableParticles = v) } }
        )
    }

    LauncherSectionHeader(title = "Battlefield", badge = "rgl_config.txt")
    LauncherGroupedCard {
        SwitchListItem(
            headline = "Show blood",
            supporting = "enable_blood",
            checked = rgl.enableBlood,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(enableBlood = v) } }
        )
        LauncherSectionDivider()
        TileGridListItem(
            headline = "Blood stains",
            supporting = "blood_stains",
            options = RglConfigOptions.BLOOD_STAINS_LABELS,
            selectedIndex = rgl.bloodStains.coerceIn(0, 2),
            columns = 3,
            valueReadout = RglConfigOptions.BLOOD_STAINS_LABELS[rgl.bloodStains.coerceIn(0, 2)],
            enabled = true,
            onSelected = { idx -> onUpdate { it.copy(bloodStains = idx) } }
        )
        LauncherSectionDivider()
        TileGridListItem(
            headline = "Number of corpses",
            supporting = "number_of_corpses",
            options = RglConfigOptions.CORPSES_LABELS,
            selectedIndex = rgl.numberOfCorpses.coerceIn(0, 6),
            columns = 4,
            valueReadout = RglConfigOptions.CORPSES_LABELS[rgl.numberOfCorpses.coerceIn(0, 6)],
            enabled = true,
            onSelected = { idx -> onUpdate { it.copy(numberOfCorpses = idx) } }
        )
        LauncherSectionDivider()
        TileGridListItem(
            headline = "Number of ragdolls",
            supporting = "number_of_ragdolls",
            options = RglConfigOptions.RAGDOLLS_LABELS,
            selectedIndex = rgl.numberOfRagdolls.coerceIn(0, 4),
            columns = 5,
            valueReadout = RglConfigOptions.RAGDOLLS_LABELS[rgl.numberOfRagdolls.coerceIn(0, 4)],
            enabled = true,
            onSelected = { idx -> onUpdate { it.copy(numberOfRagdolls = idx) } }
        )
    }
}

@Composable
private fun RglGameplaySettingsSection(
    rgl: RglConfigState,
    onUpdate: ((RglConfigState) -> RglConfigState) -> Unit
) {
    LauncherSectionHeader(title = "Battle & Difficulty", badge = "rgl_config.txt")
    LauncherGroupedCard {
        FloatSliderListItem(
            headline = "Battle size",
            supporting = "battle_size — Maximum soldiers active in a battle at once",
            persistedValue = rgl.battleSize,
            minValue = 0f,
            maxValue = 1f,
            step = 0.025f,
            enabled = true,
            formatValue = { v ->
                val troops = (30 + (v.coerceIn(0f, 1f) * 120f).roundToInt()).coerceIn(30, 150)
                "$troops troops"
            },
            onCommitValue = { v -> onUpdate { it.copy(battleSize = v) } }
        )
        LauncherSectionDivider()
        TileGridListItem(
            headline = "Combat speed",
            supporting = "combat_speed",
            options = RglConfigOptions.COMBAT_SPEED_LABELS,
            selectedIndex = rgl.combatSpeed.coerceIn(0, 4),
            columns = 3,
            valueReadout = RglConfigOptions.COMBAT_SPEED_LABELS[rgl.combatSpeed.coerceIn(0, 4)],
            enabled = true,
            onSelected = { idx -> onUpdate { it.copy(combatSpeed = idx) } }
        )
        LauncherSectionDivider()
        TileGridListItem(
            headline = "Damage to player",
            supporting = "combat_difficulty",
            options = RglConfigOptions.DAMAGE_DIFFICULTY_LABELS,
            selectedIndex = rgl.combatDifficulty.coerceIn(0, 2),
            columns = 3,
            valueReadout = RglConfigOptions.DAMAGE_DIFFICULTY_LABELS[rgl.combatDifficulty.coerceIn(0, 2)],
            enabled = true,
            onSelected = { idx -> onUpdate { it.copy(combatDifficulty = idx) } }
        )
        LauncherSectionDivider()
        TileGridListItem(
            headline = "Damage to friends",
            supporting = "friend_combat_difficulty",
            options = RglConfigOptions.DAMAGE_DIFFICULTY_LABELS,
            selectedIndex = rgl.friendCombatDifficulty.coerceIn(0, 2),
            columns = 3,
            valueReadout = RglConfigOptions.DAMAGE_DIFFICULTY_LABELS[rgl.friendCombatDifficulty.coerceIn(0, 2)],
            enabled = true,
            onSelected = { idx -> onUpdate { it.copy(friendCombatDifficulty = idx) } }
        )
        LauncherSectionDivider()
        TileGridListItem(
            headline = "Combat AI",
            supporting = "reduce_combat_ai",
            options = RglConfigOptions.AI_LEVEL_LABELS,
            selectedIndex = rgl.reduceCombatAi.coerceIn(0, 2),
            columns = 3,
            valueReadout = RglConfigOptions.AI_LEVEL_LABELS[rgl.reduceCombatAi.coerceIn(0, 2)],
            enabled = true,
            onSelected = { idx -> onUpdate { it.copy(reduceCombatAi = idx) } }
        )
        LauncherSectionDivider()
        TileGridListItem(
            headline = "Campaign AI",
            supporting = "reduce_campaign_ai",
            options = RglConfigOptions.AI_LEVEL_LABELS,
            selectedIndex = rgl.reduceCampaignAi.coerceIn(0, 2),
            columns = 3,
            valueReadout = RglConfigOptions.AI_LEVEL_LABELS[rgl.reduceCampaignAi.coerceIn(0, 2)],
            enabled = true,
            onSelected = { idx -> onUpdate { it.copy(reduceCampaignAi = idx) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Realistic headshots",
            supporting = "realistic_headshots",
            checked = rgl.realisticHeadshots,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(realisticHeadshots = v) } }
        )
    }

    LauncherSectionHeader(title = "Combat Controls", badge = "rgl_config.txt")
    LauncherGroupedCard {
        ExposedDropdownListItem(
            headline = "Attack direction control",
            supporting = "attack_direction_control",
            options = RglConfigOptions.ATTACK_CONTROL_LABELS,
            selectedIndex = rgl.attackDirectionControl.coerceIn(0, 3),
            enabled = true,
            onSelected = { idx -> onUpdate { it.copy(attackDirectionControl = idx) } }
        )
        LauncherSectionDivider()
        ExposedDropdownListItem(
            headline = "Defend direction control",
            supporting = "defend_direction_control",
            options = RglConfigOptions.DEFEND_CONTROL_LABELS,
            selectedIndex = rgl.defendDirectionControl.coerceIn(0, 3),
            enabled = true,
            onSelected = { idx -> onUpdate { it.copy(defendDirectionControl = idx) } }
        )
        LauncherSectionDivider()
        TileGridListItem(
            headline = "Lance control",
            supporting = "lance_control",
            options = RglConfigOptions.LANCE_CONTROL_LABELS,
            selectedIndex = rgl.lanceControl.coerceIn(0, 2),
            columns = 3,
            enabled = true,
            onSelected = { idx -> onUpdate { it.copy(lanceControl = idx) } }
        )
        LauncherSectionDivider()
        TileGridListItem(
            headline = "Turn camera with horse (1st person)",
            supporting = "turn_camera_with_horse_in_first_person",
            options = RglConfigOptions.HORSE_CAMERA_LABELS,
            selectedIndex = rgl.turnCameraWithHorseInFirstPerson.coerceIn(0, 2),
            columns = 3,
            enabled = true,
            onSelected = { idx -> onUpdate { it.copy(turnCameraWithHorseInFirstPerson = idx) } }
        )
    }

    LauncherSectionHeader(title = "HUD & Combat Log", badge = "rgl_config.txt")
    LauncherGroupedCard {
        SwitchListItem(
            headline = "Show banners on friendly troops",
            supporting = "display_labels",
            checked = rgl.displayLabels,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(displayLabels = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Show targeting reticule",
            supporting = "display_targeting_reticule",
            checked = rgl.displayTargetingReticule,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(displayTargetingReticule = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Show attack direction indicator",
            supporting = "display_attack_direction",
            checked = rgl.displayAttackDirection,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(displayAttackDirection = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Report damage",
            supporting = "verbose_damage",
            checked = rgl.verboseDamage,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(verboseDamage = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Report shot difficulty",
            supporting = "verbose_shot_difficulty",
            checked = rgl.verboseShotDifficulty,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(verboseShotDifficulty = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Report casualties",
            supporting = "verbose_casualties",
            checked = rgl.verboseCasualties,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(verboseCasualties = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Report experience",
            supporting = "verbose_experience",
            checked = rgl.verboseExperience,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(verboseExperience = v) } }
        )
    }
}

@Composable
private fun AudioInputSettingsSection(
    state: SettingsUiState,
    perf: PerformanceUiState,
    rgl: RglConfigState,
    perfEnabled: Boolean,
    onGyroAimChange: (Boolean) -> Unit,
    onPinchZoomChange: (Boolean) -> Unit,
    onUpdateRgl: ((RglConfigState) -> RglConfigState) -> Unit
) {
    LauncherSectionHeader(title = "Audio", badge = "rgl_config.txt")
    LauncherGroupedCard {
        FloatSliderListItem(
            headline = "Music volume",
            supporting = "music_volume",
            persistedValue = rgl.musicVolume,
            minValue = 0f,
            maxValue = 1f,
            step = 0.05f,
            enabled = !rgl.disableMusic,
            formatValue = { "${(it * 100f).roundToInt()}%" },
            onCommitValue = { v -> onUpdateRgl { it.copy(musicVolume = v) } }
        )
        LauncherSectionDivider()
        FloatSliderListItem(
            headline = "Sound effects volume",
            supporting = "sound_volume",
            persistedValue = rgl.soundVolume,
            minValue = 0f,
            maxValue = 1f,
            step = 0.05f,
            enabled = !rgl.disableSound,
            formatValue = { "${(it * 100f).roundToInt()}%" },
            onCommitValue = { v -> onUpdateRgl { it.copy(soundVolume = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Disable music",
            supporting = "disable_music",
            checked = rgl.disableMusic,
            enabled = true,
            onCheckedChange = { v -> onUpdateRgl { it.copy(disableMusic = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Disable sound",
            supporting = "disable_sound",
            checked = rgl.disableSound,
            enabled = true,
            onCheckedChange = { v -> onUpdateRgl { it.copy(disableSound = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Disable sound pitch variation",
            supporting = "disable_frequency_variation",
            checked = rgl.disableFrequencyVariation,
            enabled = true,
            onCheckedChange = { v -> onUpdateRgl { it.copy(disableFrequencyVariation = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Use WinMM audio backend",
            supporting = "use_winmm_audio",
            checked = rgl.useWinmmAudio,
            enabled = true,
            onCheckedChange = { v -> onUpdateRgl { it.copy(useWinmmAudio = v) } }
        )
    }

    LauncherSectionHeader(title = "Touch, Sensors & Controls", badge = "Android + rgl_config")
    LauncherGroupedCard {
        SwitchListItem(
            headline = "Opt-in gyro aiming (Mission)",
            supporting = if (state.hasPhoneGyro) {
                "Tilt device or controller for fine ranged aiming"
            } else {
                "No phone gyroscope — controller gyro only"
            },
            checked = perf.gyroAimEnabled,
            enabled = perfEnabled,
            onCheckedChange = onGyroAimChange
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Pinch-to-zoom (Campaign map)",
            checked = perf.pinchZoomEnabled,
            enabled = perfEnabled,
            onCheckedChange = onPinchZoomChange
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Gamepad vibration",
            supporting = "enable_gamepad_vibration",
            checked = rgl.enableGamepadVibration,
            enabled = true,
            onCheckedChange = { v -> onUpdateRgl { it.copy(enableGamepadVibration = v) } }
        )
        LauncherSectionDivider()
        FloatSliderListItem(
            headline = "Mouse & look sensitivity",
            supporting = "mouse_sensitivity",
            persistedValue = rgl.mouseSensitivity,
            minValue = 0.1f,
            maxValue = 2.0f,
            step = 0.05f,
            enabled = true,
            formatValue = { String.format(Locale.US, "%.2f", it) },
            onCommitValue = { v -> onUpdateRgl { it.copy(mouseSensitivity = v) } }
        )
        LauncherSectionDivider()
        FloatSliderListItem(
            headline = "Mouse vertical movement scale",
            supporting = "control_mouse_movement_y_scale",
            persistedValue = rgl.controlMouseMovementYScale,
            minValue = 0.5f,
            maxValue = 3.0f,
            step = 0.1f,
            enabled = true,
            formatValue = { String.format(Locale.US, "%.1fx", it) },
            onCommitValue = { v -> onUpdateRgl { it.copy(controlMouseMovementYScale = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Invert mouse Y-axis",
            supporting = "invert_mouse",
            checked = rgl.invertMouse,
            enabled = true,
            onCheckedChange = { v -> onUpdateRgl { it.copy(invertMouse = v) } }
        )
    }
}

@Composable
private fun RglSystemSettingsSection(
    state: SettingsUiState,
    rgl: RglConfigState,
    onUpdate: ((RglConfigState) -> RglConfigState) -> Unit,
    onRequestReset: () -> Unit
) {
    val colors = LocalWseColors.current
    LauncherSectionHeader(title = "Engine & Cheats", badge = "rgl_config.txt")
    LauncherGroupedCard {
        SwitchListItem(
            headline = "Enable cheats",
            supporting = "cheat_mode — Unlocks Warband cheat shortcuts",
            checked = rgl.cheatMode,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(cheatMode = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Enable edit mode",
            supporting = "enable_edit_mode — Scene editing tools",
            checked = rgl.enableEditMode,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(enableEditMode = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Debug mode",
            supporting = "debug_mode — Verbose engine logs",
            checked = rgl.debugMode,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(debugMode = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Force single threading",
            supporting = "force_single_threading — Compatibility fallback",
            checked = rgl.forceSingleThreading,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(forceSingleThreading = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Check for game updates",
            supporting = "enable_version_check",
            checked = rgl.enableVersionCheck,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(enableVersionCheck = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "First-time startup flag",
            supporting = "first_time — Re-runs initial Warband setup when enabled",
            checked = rgl.firstTime,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(firstTime = v) } }
        )
    }

    LauncherSectionHeader(title = "Network", badge = "rgl_config.txt")
    LauncherGroupedCard {
        IntSliderListItem(
            headline = "Max connections",
            supporting = "max_number_of_connections",
            persistedValue = rgl.maxNumberOfConnections,
            minValue = 2,
            maxValue = 200,
            step = 2,
            enabled = true,
            formatValue = { "$it" },
            onCommitValue = { v -> onUpdate { it.copy(maxNumberOfConnections = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Use secure connection",
            supporting = "use_secure_connection",
            checked = rgl.useSecureConnection,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(useSecureConnection = v) } }
        )
        LauncherSectionDivider()
        SwitchListItem(
            headline = "Look for server on this machine",
            supporting = "look_for_server_on_this_machine",
            checked = rgl.lookForServerOnThisMachine,
            enabled = true,
            onCheckedChange = { v -> onUpdate { it.copy(lookForServerOnThisMachine = v) } }
        )
    }

    LauncherSectionHeader(title = "Config File", badge = "rgl_config.txt")
    LauncherGroupedCard {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            WseKeyValueGrid(
                items = listOf(
                    WseMetricItem(
                        label = "Config Status",
                        value = if (state.rglConfigReady) "Ready" else "Default",
                        accentValue = state.rglConfigReady
                    ),
                    WseMetricItem(
                        label = "Properties",
                        value = "${RglConfigCodec.KNOWN_KEYS.size + rgl.extraEntries.size} keys"
                    )
                ),
                columns = 2
            )
            Text(
                text = state.rglConfigPath,
                style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                color = colors.onSurfaceVariant
            )
            WseSecondaryButton(
                text = "Reset rgl_config.txt to defaults",
                onClick = onRequestReset,
                modifier = Modifier.fillMaxWidth()
            )
        }
    }
}

private fun previewSettingsState(): SettingsUiState = SettingsUiState(
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
        uiScalePercent = 100,
        gyroAimEnabled = false,
        pinchZoomEnabled = true,
        language = "en"
    ),
    isLoaded = true,
    languageOptions = listOf("en", "de", "fr", "pl"),
    hasPhoneGyro = true,
    rglConfig = RglConfigState(),
    rglConfigReady = true
)

@Preview(name = "Settings Performance Dark", showBackground = true, widthDp = 412, heightDp = 1280)
@Composable
private fun PreviewSettingsDark() {
    Wse2Theme(darkTheme = true, dynamicColor = false) {
        Box(modifier = Modifier.wseBackdrop(LocalWseColors.current.accent)) {
            SettingsScreen(
                state = previewSettingsState(),
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
                onLanguageChange = {}
            )
        }
    }
}

@Preview(name = "Settings Graphics rgl_config", showBackground = true, widthDp = 412, heightDp = 1280)
@Composable
private fun PreviewSettingsRglGraphics() {
    Wse2Theme(darkTheme = true, dynamicColor = false) {
        Box(modifier = Modifier.wseBackdrop(LocalWseColors.current.accent)) {
            SettingsScreen(
                state = previewSettingsState(),
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
                initialCategory = SettingsCategory.GRAPHICS
            )
        }
    }
}
