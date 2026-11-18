package org.wse2.android.launcher

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedCard
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlin.math.roundToInt

private val PlaceholderPerformanceState = PerformanceUiState.fromJson("{}")

@Composable
fun InterimHomeScreen(
    installFolder: InstallFolderState,
    scanState: ScanState,
    modulesState: ModulesUiState,
    statusLog: String,
    onChooseFolder: () -> Unit,
    onChangeModule: () -> Unit,
    onShowMissingFiles: (ModuleChoice, List<String>) -> Unit,
    onPlay: () -> Unit
) {
    val hasFolder = installFolder.treeUri != null
    val selectedModule = modulesState.selectedModule
    val installStatus = modulesState.selectedInstallStatus
    val isReady = !scanState.inProgress &&
        hasFolder &&
        selectedModule != null &&
        installStatus?.state == InstallCheckState.READY

    val statusText: String
    val statusClickable: (() -> Unit)?
    when {
        scanState.inProgress -> {
            val label = scanState.folderLabel?.takeIf { it.isNotBlank() }
                ?: installFolder.displayName?.takeIf { it.isNotBlank() }
                ?: "folder"
            statusText = "Scanning $label…"
            statusClickable = null
        }
        !hasFolder -> {
            statusText = "Choose your Warband folder"
            statusClickable = null
        }
        selectedModule == null -> {
            statusText = "Choose a module"
            statusClickable = null
        }
        installStatus?.state == null || installStatus.state == InstallCheckState.CHECKING -> {
            statusText = "Checking files…"
            statusClickable = null
        }
        installStatus.state == InstallCheckState.READY -> {
            statusText = "Ready"
            statusClickable = null
        }
        else -> {
            val count = installStatus.missingFiles.size
            statusText = if (count > 0) {
                "Missing $count file(s) — tap for details"
            } else {
                "Missing files — tap for details"
            }
            statusClickable = { onShowMissingFiles(selectedModule, installStatus.missingFiles) }
        }
    }

    LauncherPageColumn {
        Text(
            text = "WSE2 Android",
            style = MaterialTheme.typography.displaySmall,
            color = MaterialTheme.colorScheme.onBackground
        )

        LauncherSectionCard(title = "Warband Install") {
            val validName = installFolder.displayName?.takeIf { it.isNotBlank() }
            Text(
                text = validName ?: "No Warband folder selected",
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurface
            )
            if (validName == null) {
                Spacer(Modifier.height(4.dp))
                Text(
                    text = "Choose the folder that contains Modules/ and CommonRes/",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
            Spacer(Modifier.height(10.dp))
            OutlinedButton(
                onClick = onChooseFolder,
                enabled = !scanState.inProgress,
                modifier = Modifier
                    .fillMaxWidth()
                    .defaultMinSize(minHeight = 48.dp)
            ) {
                Text("Choose folder")
            }
        }

        LauncherSectionCard(title = "Selected Module") {
            val title = selectedModule?.let { it.displayName.ifBlank { it.folderName } }
                ?: "No module selected"
            val subtitle = when {
                selectedModule != null -> "Folder: ${selectedModule.folderName}"
                hasFolder && scanState.inProgress -> "Scanning folder for modules…"
                hasFolder -> "No Warband modules found in the selected folder."
                else -> "Select a Warband folder to discover modules."
            }
            Text(
                text = title,
                style = MaterialTheme.typography.titleMedium,
                color = MaterialTheme.colorScheme.onSurface
            )
            Spacer(Modifier.height(4.dp))
            Text(
                text = subtitle,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(Modifier.height(10.dp))
            OutlinedButton(
                onClick = onChangeModule,
                modifier = Modifier
                    .fillMaxWidth()
                    .defaultMinSize(minHeight = 48.dp)
            ) {
                Text("Change")
            }
        }

        AssistChip(
            onClick = { statusClickable?.invoke() },
            enabled = statusClickable != null || isReady,
            label = { Text(statusText) },
            modifier = Modifier.defaultMinSize(minHeight = 48.dp)
        )

        Button(
            onClick = onPlay,
            enabled = isReady,
            modifier = Modifier
                .fillMaxWidth()
                .heightIn(min = 56.dp)
        ) {
            Text(
                text = "Play",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold
            )
        }

        val singleLineStatus = statusLog
            .lineSequence()
            .map { it.trim() }
            .filter { it.isNotEmpty() }
            .joinToString(" · ")
            .ifEmpty { "Ready." }
        Text(
            text = singleLineStatus,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis
        )
    }
}

@Composable
fun InterimModulesScreen(
    installFolder: InstallFolderState,
    scanState: ScanState,
    modulesState: ModulesUiState,
    onChooseFolder: () -> Unit,
    onRescan: () -> Unit,
    onCancelScan: () -> Unit,
    onSelectModule: (Int) -> Unit,
    onShowMissingFiles: (ModuleChoice, List<String>) -> Unit
) {
    val hasFolder = installFolder.treeUri != null
    val folderLabel = installFolder.displayName?.takeIf { it.isNotBlank() }

    LauncherPageColumn {
        Text(
            text = "Modules",
            style = MaterialTheme.typography.headlineMedium,
            color = MaterialTheme.colorScheme.onBackground
        )

        LauncherSectionCard(title = "Warband Folder") {
            Text(
                text = folderLabel ?: "No Warband folder selected",
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurface
            )
            Spacer(Modifier.height(10.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                OutlinedButton(
                    onClick = onChooseFolder,
                    enabled = !scanState.inProgress,
                    modifier = Modifier
                        .weight(1f)
                        .defaultMinSize(minHeight = 48.dp)
                ) {
                    Text("Choose folder")
                }
                OutlinedButton(
                    onClick = onRescan,
                    enabled = hasFolder && !scanState.inProgress,
                    modifier = Modifier
                        .weight(1f)
                        .defaultMinSize(minHeight = 48.dp)
                ) {
                    Text("Rescan")
                }
            }
            if (scanState.inProgress) {
                Spacer(Modifier.height(12.dp))
                val label = scanState.folderLabel?.takeIf { it.isNotBlank() } ?: "folder"
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(22.dp),
                        strokeWidth = 2.5.dp
                    )
                    Spacer(Modifier.width(10.dp))
                    Text(
                        text = "Scanning $label…",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurface
                    )
                }
                Spacer(Modifier.height(8.dp))
                OutlinedButton(
                    onClick = onCancelScan,
                    modifier = Modifier
                        .fillMaxWidth()
                        .defaultMinSize(minHeight = 48.dp)
                ) {
                    Text("Cancel")
                }
            }
        }

        LauncherSectionCard(title = "Available Modules") {
            if (scanState.inProgress) {
                val label = scanState.folderLabel?.takeIf { it.isNotBlank() } ?: "folder"
                Text(
                    text = "Scanning $label…",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            } else if (modulesState.modules.isEmpty()) {
                Text(
                    text = modulesState.emptyMessage,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            } else {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    modulesState.modules.forEachIndexed { index, mod ->
                        val selected = index == modulesState.selectedIndex
                        val status = modulesState.installStatuses[mod.modulePath]
                        val scriptsLabel = if (mod.hasScriptsTxt) {
                            "scripts.txt present"
                        } else {
                            "scripts.txt missing"
                        }
                        val statusLabel = when (status?.state) {
                            null, InstallCheckState.CHECKING -> "Checking…"
                            InstallCheckState.READY -> "Ready"
                            InstallCheckState.MISSING -> if (status.missingFiles.isNotEmpty()) {
                                "Missing ${status.missingFiles.size} file(s)"
                            } else {
                                "Install check failed"
                            }
                        }

                        OutlinedCard(
                            modifier = Modifier
                                .fillMaxWidth()
                                .defaultMinSize(minHeight = 48.dp)
                                .selectable(
                                    selected = selected,
                                    role = Role.RadioButton,
                                    onClick = {
                                        onSelectModule(index)
                                        if (status?.state == InstallCheckState.MISSING) {
                                            onShowMissingFiles(mod, status.missingFiles)
                                        }
                                    }
                                ),
                            border = BorderStroke(
                                width = if (selected) 2.dp else 1.dp,
                                color = if (selected) {
                                    MaterialTheme.colorScheme.primary
                                } else {
                                    MaterialTheme.colorScheme.outlineVariant
                                }
                            ),
                            colors = CardDefaults.outlinedCardColors(
                                containerColor = if (selected) {
                                    MaterialTheme.colorScheme.primaryContainer.copy(alpha = 0.35f)
                                } else {
                                    MaterialTheme.colorScheme.surfaceContainerLow
                                }
                            )
                        ) {
                            Column(modifier = Modifier.padding(12.dp)) {
                                Text(
                                    text = mod.toString(),
                                    style = MaterialTheme.typography.titleSmall,
                                    fontWeight = if (selected) FontWeight.Bold else FontWeight.Medium,
                                    color = if (selected) {
                                        MaterialTheme.colorScheme.primary
                                    } else {
                                        MaterialTheme.colorScheme.onSurface
                                    }
                                )
                                Spacer(Modifier.height(2.dp))
                                Text(
                                    text = "${mod.modulePath} | $scriptsLabel",
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                                Spacer(Modifier.height(4.dp))
                                Text(
                                    text = statusLabel,
                                    style = MaterialTheme.typography.bodySmall,
                                    color = if (status?.state == InstallCheckState.MISSING) {
                                        MaterialTheme.colorScheme.error
                                    } else {
                                        MaterialTheme.colorScheme.onSurfaceVariant
                                    }
                                )
                                if (status?.state == InstallCheckState.MISSING) {
                                    Spacer(Modifier.height(8.dp))
                                    OutlinedButton(
                                        onClick = { onShowMissingFiles(mod, status.missingFiles) },
                                        modifier = Modifier
                                            .fillMaxWidth()
                                            .defaultMinSize(minHeight = 48.dp)
                                    ) {
                                        Text("Details")
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        LauncherSectionCard(title = "Module Details") {
            val selectedMod = modulesState.selectedModule
            val detailText = when {
                scanState.inProgress -> {
                    val label = scanState.folderLabel?.takeIf { it.isNotBlank() } ?: "folder"
                    "Scanning $label…"
                }
                selectedMod != null -> {
                    val scriptsLabel = if (selectedMod.hasScriptsTxt) {
                        "scripts.txt present"
                    } else {
                        "scripts.txt missing"
                    }
                    val installLabel = when (val st = modulesState.installStatuses[selectedMod.modulePath]) {
                        null -> "Checking…"
                        else -> when (st.state) {
                            InstallCheckState.CHECKING -> "Checking…"
                            InstallCheckState.READY -> "Ready"
                            InstallCheckState.MISSING -> if (st.missingFiles.isNotEmpty()) {
                                "Missing ${st.missingFiles.size} file(s)"
                            } else {
                                "Install check failed"
                            }
                        }
                    }
                    "${selectedMod.modulePath} | $scriptsLabel | BRFs: ${selectedMod.loadResourceCount} common + " +
                        "${selectedMod.loadModResourceCount} mod | $installLabel"
                }
                modulesState.modules.isNotEmpty() -> "Tap a module above to select it."
                else -> modulesState.emptyMessage
            }
            Text(
                text = detailText,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

@Composable
fun InterimSettingsScreen(
    settingsState: SettingsUiState,
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
    onLanguageChange: (String) -> Unit
) {
    val perf = settingsState.performance ?: PlaceholderPerformanceState
    val enabled = settingsState.isLoaded

    LauncherPageColumn {
        Text(
            text = "Settings",
            style = MaterialTheme.typography.headlineMedium,
            color = MaterialTheme.colorScheme.onBackground
        )

        LauncherSectionCard(title = "Graphics") {
            val presetIdx = LauncherViewModel.QUALITY_PRESET_KEYS
                .indexOf(perf.qualityPresetKey)
                .coerceAtLeast(0)
            DropdownSelectorRow(
                label = "Quality preset",
                options = LauncherViewModel.QUALITY_PRESET_LABELS,
                selectedIndex = presetIdx,
                enabled = enabled,
                onSelected = { idx ->
                    onQualityPresetChange(LauncherViewModel.QUALITY_PRESET_KEYS[idx])
                }
            )

            val capIdx = settingsState.frameRateValues
                .indexOf(perf.frameRateCapHz)
                .coerceAtLeast(0)
            DropdownSelectorRow(
                label = "Frame-rate cap",
                options = settingsState.frameRateLabels,
                selectedIndex = capIdx,
                enabled = enabled,
                onSelected = { idx ->
                    onFrameRateCapChange(settingsState.frameRateValues[idx])
                }
            )

            val modeIdx = settingsState.presentModeKeys
                .indexOf(perf.presentModeKey)
                .coerceAtLeast(0)
            DropdownSelectorRow(
                label = "Present mode",
                options = settingsState.presentModeLabels,
                selectedIndex = modeIdx,
                enabled = enabled,
                onSelected = { idx ->
                    onPresentModeChange(settingsState.presentModeKeys[idx])
                }
            )

            SteppedPercentSliderRow(
                label = "Render scale",
                persistedPercent = perf.renderScalePercent,
                minPercent = LauncherViewModel.RENDER_SCALE_MIN,
                maxPercent = LauncherViewModel.RENDER_SCALE_MAX,
                stepPercent = LauncherViewModel.SCALE_STEP,
                enabled = enabled,
                onCommitPercent = onRenderScaleChange
            )

            val texIdx = LauncherViewModel.TEXTURE_SIZE_VALUES
                .indexOf(perf.maxTextureSize)
                .coerceAtLeast(0)
            DropdownSelectorRow(
                label = "Texture size cap",
                options = LauncherViewModel.TEXTURE_SIZE_LABELS,
                selectedIndex = texIdx,
                enabled = enabled,
                onSelected = { idx ->
                    onMaxTextureSizeChange(LauncherViewModel.TEXTURE_SIZE_VALUES[idx])
                }
            )

            SwitchSettingRow(
                label = "Thermal headroom throttling (res / LOD / FPS)",
                checked = perf.thermalThrottlingEnabled,
                enabled = enabled,
                onCheckedChange = onThermalThrottlingChange
            )
            SwitchSettingRow(
                label = "Dynamic resolution under thermal load",
                checked = perf.dynamicResolutionEnabled,
                enabled = enabled,
                onCheckedChange = onDynamicResolutionChange
            )
            SwitchSettingRow(
                label = "LOD bias scaling under thermal load",
                checked = perf.lodBiasScalingEnabled,
                enabled = enabled,
                onCheckedChange = onLodBiasScalingChange
            )
        }

        LauncherSectionCard(title = "Display") {
            val aspectIdx = LauncherViewModel.UI_ASPECT_KEYS
                .indexOf(perf.uiAspectKey)
                .coerceAtLeast(0)
            DropdownSelectorRow(
                label = "UI aspect",
                options = LauncherViewModel.UI_ASPECT_LABELS,
                selectedIndex = aspectIdx,
                enabled = enabled,
                onSelected = { idx ->
                    onUiAspectChange(LauncherViewModel.UI_ASPECT_KEYS[idx])
                }
            )

            SteppedPercentSliderRow(
                label = "UI scale",
                persistedPercent = perf.uiScalePercent,
                minPercent = LauncherViewModel.UI_SCALE_MIN,
                maxPercent = LauncherViewModel.UI_SCALE_MAX,
                stepPercent = LauncherViewModel.SCALE_STEP,
                enabled = enabled,
                onCommitPercent = onUiScaleChange
            )

            Text(
                text = "Applies to the in-game interface in a later update.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }

        LauncherSectionCard(title = "Controls") {
            val gyroLabel = if (settingsState.hasPhoneGyro) {
                "Opt-in gyro aiming (Mission)"
            } else {
                "Opt-in gyro aiming (Mission) - no phone gyroscope, controller gyro only"
            }
            SwitchSettingRow(
                label = gyroLabel,
                checked = perf.gyroAimEnabled,
                enabled = enabled,
                onCheckedChange = onGyroAimChange
            )
            SwitchSettingRow(
                label = "Pinch-to-zoom (Campaign map)",
                checked = perf.pinchZoomEnabled,
                enabled = enabled,
                onCheckedChange = onPinchZoomChange
            )
        }

        LauncherSectionCard(title = "Language") {
            val langOptions = settingsState.languageOptions.ifEmpty { listOf("en") }
            val langIdx = langOptions.indexOf(perf.language).coerceAtLeast(0)
            DropdownSelectorRow(
                label = "Language",
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

@Composable
fun InterimAdvancedScreen(
    installFolder: InstallFolderState,
    scanState: ScanState,
    modulesState: ModulesUiState,
    driverState: DriverUiState,
    showFpsOverlay: Boolean,
    statusLog: String,
    onVerifyScripts: () -> Unit,
    onSelectDriver: (Int) -> Unit,
    onImportDriver: () -> Unit,
    onAllowMockFallbackChange: (Boolean) -> Unit,
    onProbeVulkan: () -> Unit,
    onShowFpsOverlayChange: (Boolean) -> Unit,
    onLaunchSelfTest: () -> Unit
) {
    val treeUri = installFolder.treeUri
    val selectedModule = modulesState.selectedModule
    val verifyEnabled = selectedModule?.hasScriptsTxt == true &&
        treeUri != null &&
        !scanState.inProgress

    LauncherPageColumn {
        Text(
            text = "Advanced",
            style = MaterialTheme.typography.headlineMedium,
            color = MaterialTheme.colorScheme.onBackground
        )

        LauncherSectionCard(title = "Warband Folder (SAF)") {
            Text(
                text = if (treeUri != null) {
                    "Tree URI: $treeUri"
                } else {
                    "No Warband folder selected."
                },
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(Modifier.height(4.dp))
            Text(
                text = scanState.indexStatsText,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(Modifier.height(10.dp))
            OutlinedButton(
                onClick = onVerifyScripts,
                enabled = verifyEnabled,
                modifier = Modifier
                    .fillMaxWidth()
                    .defaultMinSize(minHeight = 48.dp)
            ) {
                Text("Verify scripts.txt via SafVfs")
            }
        }

        LauncherSectionCard(title = "GPU Driver") {
            DropdownSelectorRow(
                label = "Vulkan driver",
                options = driverState.options,
                selectedIndex = driverState.selectedIndex,
                enabled = true,
                onSelected = onSelectDriver
            )
            Text(
                text = driverState.statusText,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(Modifier.height(10.dp))
            OutlinedButton(
                onClick = onImportDriver,
                modifier = Modifier
                    .fillMaxWidth()
                    .defaultMinSize(minHeight = 48.dp)
            ) {
                Text("Import Turnip / PanVK Driver Package (.zip)")
            }
            Spacer(Modifier.height(6.dp))
            SwitchSettingRow(
                label = "Allow mock ICD fallback (testing only)",
                checked = driverState.allowMockFallback,
                enabled = true,
                onCheckedChange = onAllowMockFallbackChange
            )
            Spacer(Modifier.height(6.dp))
            OutlinedButton(
                onClick = onProbeVulkan,
                enabled = !driverState.isProbeInProgress,
                modifier = Modifier
                    .fillMaxWidth()
                    .defaultMinSize(minHeight = 48.dp)
            ) {
                Text("Probe Vulkan Driver (CPU reference frame)")
            }
        }

        LauncherSectionCard(title = "Runtime") {
            SwitchSettingRow(
                label = "Show FPS overlay",
                checked = showFpsOverlay,
                enabled = true,
                onCheckedChange = onShowFpsOverlayChange
            )
            Spacer(Modifier.height(8.dp))
            OutlinedButton(
                onClick = onLaunchSelfTest,
                modifier = Modifier
                    .fillMaxWidth()
                    .defaultMinSize(minHeight = 48.dp)
            ) {
                Text("Launch WSE2 SDL3 Self-Test (WseActivity)")
            }
        }

        LauncherSectionCard(title = "Status Log") {
            Text(
                text = statusLog,
                style = MaterialTheme.typography.bodySmall.copy(fontFamily = FontFamily.Monospace),
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

@Composable
private fun LauncherPageColumn(content: @Composable ColumnScope.() -> Unit) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState()),
        contentAlignment = Alignment.TopCenter
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .widthIn(max = 720.dp)
                .padding(horizontal = 16.dp, vertical = 16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
            content = content
        )
    }
}

@Composable
private fun LauncherSectionCard(
    title: String,
    content: @Composable ColumnScope.() -> Unit
) {
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainerLow
        )
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp)
        ) {
            Text(
                text = title.uppercase(),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(Modifier.height(10.dp))
            content()
        }
    }
}

@Composable
private fun DropdownSelectorRow(
    label: String,
    options: List<String>,
    selectedIndex: Int,
    enabled: Boolean,
    onSelected: (Int) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    val safeIdx = if (options.isNotEmpty()) selectedIndex.coerceIn(0, options.lastIndex) else -1
    val selectedText = if (safeIdx >= 0) options[safeIdx] else ""

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(bottom = 10.dp)
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Spacer(Modifier.height(4.dp))
        Box(modifier = Modifier.fillMaxWidth()) {
            OutlinedButton(
                onClick = { expanded = true },
                enabled = enabled && options.isNotEmpty(),
                modifier = Modifier
                    .fillMaxWidth()
                    .defaultMinSize(minHeight = 48.dp)
            ) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = selectedText,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier.weight(1f)
                    )
                    Spacer(Modifier.width(8.dp))
                    Text(text = "▾")
                }
            }
            DropdownMenu(
                expanded = expanded,
                onDismissRequest = { expanded = false }
            ) {
                options.forEachIndexed { index, option ->
                    DropdownMenuItem(
                        text = {
                            Text(
                                text = option,
                                fontWeight = if (index == safeIdx) {
                                    FontWeight.Bold
                                } else {
                                    FontWeight.Normal
                                }
                            )
                        },
                        onClick = {
                            expanded = false
                            onSelected(index)
                        }
                    )
                    if (index < options.lastIndex) {
                        HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                    }
                }
            }
        }
    }
}

@Composable
private fun SteppedPercentSliderRow(
    label: String,
    persistedPercent: Int,
    minPercent: Int,
    maxPercent: Int,
    stepPercent: Int,
    enabled: Boolean,
    onCommitPercent: (Int) -> Unit
) {
    var draftPercent by remember(persistedPercent) { mutableIntStateOf(persistedPercent) }
    val steps = ((maxPercent - minPercent) / stepPercent - 1).coerceAtLeast(0)

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(bottom = 8.dp)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = label,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Text(
                text = "$draftPercent%",
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.Bold,
                color = MaterialTheme.colorScheme.onSurface
            )
        }
        Slider(
            value = draftPercent.toFloat(),
            onValueChange = { raw ->
                draftPercent = LauncherViewModel.snapPercent(raw.roundToInt(), minPercent, maxPercent)
            },
            onValueChangeFinished = {
                onCommitPercent(draftPercent)
            },
            valueRange = minPercent.toFloat()..maxPercent.toFloat(),
            steps = steps,
            enabled = enabled,
            modifier = Modifier.fillMaxWidth()
        )
    }
}

@Composable
private fun SwitchSettingRow(
    label: String,
    checked: Boolean,
    enabled: Boolean,
    onCheckedChange: (Boolean) -> Unit
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .defaultMinSize(minHeight = 48.dp)
            .toggleable(
                value = checked,
                enabled = enabled,
                role = Role.Switch,
                onValueChange = onCheckedChange
            )
            .padding(vertical = 6.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            color = if (enabled) {
                MaterialTheme.colorScheme.onSurface
            } else {
                MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
            },
            modifier = Modifier
                .weight(1f)
                .padding(end = 12.dp)
        )
        Switch(
            checked = checked,
            onCheckedChange = null,
            enabled = enabled
        )
    }
}
