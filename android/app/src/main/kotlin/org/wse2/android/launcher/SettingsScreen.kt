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
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.wse2.android.launcher.theme.Wse2Theme

private val SettingsPlaceholderPerformance = PerformanceUiState.fromJson("{}")

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
    topContentPadding: Dp = 0.dp
) {
    val perf = state.performance ?: SettingsPlaceholderPerformance
    val enabled = state.isLoaded

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
                    bottom = 24.dp
                ),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            Text(
                text = "Settings",
                style = MaterialTheme.typography.headlineMedium,
                color = MaterialTheme.colorScheme.onBackground
            )

            LauncherSectionHeader(title = "Graphics")
            LauncherGroupedCard {
                val presetIdx = LauncherDefaults.QUALITY_PRESET_KEYS
                    .indexOf(perf.qualityPresetKey)
                    .coerceAtLeast(0)
                SegmentedListItem(
                    headline = "Quality preset",
                    options = LauncherDefaults.QUALITY_PRESET_LABELS,
                    selectedIndex = presetIdx,
                    enabled = enabled,
                    showSelectedIcon = false,
                    onSelected = { idx ->
                        onQualityPresetChange(LauncherDefaults.QUALITY_PRESET_KEYS[idx])
                    }
                )
                LauncherSectionDivider()

                val capIdx = state.frameRateValues
                    .indexOf(perf.frameRateCapHz)
                    .coerceAtLeast(0)
                ExposedDropdownListItem(
                    headline = "Frame-rate cap",
                    options = state.frameRateLabels,
                    selectedIndex = capIdx,
                    enabled = enabled,
                    onSelected = { idx ->
                        onFrameRateCapChange(state.frameRateValues[idx])
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
                LauncherSectionDivider()

                SliderListItem(
                    headline = "Render scale",
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
                ExposedDropdownListItem(
                    headline = "Texture size cap",
                    options = LauncherDefaults.TEXTURE_SIZE_LABELS,
                    selectedIndex = texIdx,
                    enabled = enabled,
                    onSelected = { idx ->
                        onMaxTextureSizeChange(LauncherDefaults.TEXTURE_SIZE_VALUES[idx])
                    }
                )
                LauncherSectionDivider()

                SwitchListItem(
                    headline = "Thermal headroom throttling",
                    supporting = "Adjusts resolution, LOD bias and FPS",
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

            LauncherSectionHeader(title = "Display")
            LauncherGroupedCard {
                val aspectIdx = LauncherDefaults.UI_ASPECT_KEYS
                    .indexOf(perf.uiAspectKey)
                    .coerceAtLeast(0)
                SegmentedListItem(
                    headline = "UI aspect",
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
                    headline = "UI scale",
                    persistedPercent = perf.uiScalePercent,
                    minPercent = LauncherDefaults.UI_SCALE_MIN,
                    maxPercent = LauncherDefaults.UI_SCALE_MAX,
                    stepPercent = LauncherDefaults.SCALE_STEP,
                    enabled = enabled,
                    onCommitPercent = onUiScaleChange
                )
                LauncherSectionDivider()

                ListItem(
                    headlineContent = {
                        Text(
                            text = "Applies to the in-game interface in a later update.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    },
                    colors = ListItemDefaults.colors(containerColor = Color.Transparent)
                )
            }

            LauncherSectionHeader(title = "Controls")
            LauncherGroupedCard {
                SwitchListItem(
                    headline = "Opt-in gyro aiming (Mission)",
                    supporting = if (state.hasPhoneGyro) {
                        null
                    } else {
                        "No phone gyroscope — controller gyro only"
                    },
                    checked = perf.gyroAimEnabled,
                    enabled = enabled,
                    onCheckedChange = onGyroAimChange
                )
                LauncherSectionDivider()

                SwitchListItem(
                    headline = "Pinch-to-zoom (Campaign map)",
                    checked = perf.pinchZoomEnabled,
                    enabled = enabled,
                    onCheckedChange = onPinchZoomChange
                )
            }

            LauncherSectionHeader(title = "Language")
            LauncherGroupedCard {
                val langOptions = state.languageOptions.ifEmpty { listOf("en") }
                val langIdx = langOptions.indexOf(perf.language).coerceAtLeast(0)
                ExposedDropdownListItem(
                    headline = "Language",
                    options = langOptions,
                    selectedIndex = langIdx,
                    enabled = enabled,
                    onSelected = { idx ->
                        onLanguageChange(langOptions[idx])
                    }
                )
            }
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
    hasPhoneGyro = true
)

@Preview(name = "Settings Dark", showBackground = true, widthDp = 412, heightDp = 1280)
@Composable
private fun PreviewSettingsDark() {
    Wse2Theme(darkTheme = true, dynamicColor = false) {
        Surface {
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

@Preview(name = "Settings Light", showBackground = true, widthDp = 412, heightDp = 1280)
@Composable
private fun PreviewSettingsLight() {
    Wse2Theme(darkTheme = false, dynamicColor = false) {
        Surface {
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
