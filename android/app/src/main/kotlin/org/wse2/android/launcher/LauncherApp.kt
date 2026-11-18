package org.wse2.android.launcher

import android.content.res.Configuration
import android.widget.Toast
import androidx.compose.animation.Crossfade
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationRail
import androidx.compose.material3.NavigationRailItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.path
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle

private sealed interface LauncherDialogState {
    data class ScanError(val reason: String) : LauncherDialogState

    data class MissingFiles(
        val module: ModuleChoice,
        val missingFiles: List<String>
    ) : LauncherDialogState

    data class WarbandFilesNotFound(val reason: String) : LauncherDialogState
}

@Composable
fun LauncherApp(
    viewModel: LauncherViewModel,
    onChooseFolder: () -> Unit,
    onImportDriver: () -> Unit,
    onLaunchGame: (LauncherEvent.LaunchGame) -> Unit
) {
    val selectedTab by viewModel.selectedTab.collectAsStateWithLifecycle()
    val installFolder by viewModel.installFolder.collectAsStateWithLifecycle()
    val scanState by viewModel.scanState.collectAsStateWithLifecycle()
    val modulesState by viewModel.modulesState.collectAsStateWithLifecycle()
    val settingsState by viewModel.settingsState.collectAsStateWithLifecycle()
    val driverState by viewModel.driverState.collectAsStateWithLifecycle()
    val showFpsOverlay by viewModel.showFpsOverlay.collectAsStateWithLifecycle()
    val statusLog by viewModel.statusLog.collectAsStateWithLifecycle()

    val context = LocalContext.current
    var activeDialog by remember { mutableStateOf<LauncherDialogState?>(null) }

    LaunchedEffect(viewModel) {
        viewModel.events.collect { event ->
            when (event) {
                is LauncherEvent.ShowScanError -> {
                    activeDialog = LauncherDialogState.ScanError(event.reason)
                }
                is LauncherEvent.ShowMissingFiles -> {
                    activeDialog = LauncherDialogState.MissingFiles(event.module, event.missingFiles)
                }
                is LauncherEvent.ShowWarbandFilesNotFound -> {
                    activeDialog = LauncherDialogState.WarbandFilesNotFound(event.reason)
                }
                is LauncherEvent.ShowToast -> {
                    Toast.makeText(context, event.message, Toast.LENGTH_SHORT).show()
                }
                is LauncherEvent.LaunchGame -> {
                    onLaunchGame(event)
                }
            }
        }
    }

    LauncherAppContent(
        selectedTab = selectedTab,
        installFolder = installFolder,
        scanState = scanState,
        modulesState = modulesState,
        settingsState = settingsState,
        driverState = driverState,
        showFpsOverlay = showFpsOverlay,
        statusLog = statusLog,
        onSelectTab = viewModel::selectTab,
        onChooseFolder = onChooseFolder,
        onRescan = viewModel::rescanSelectedFolder,
        onCancelScan = viewModel::cancelScan,
        onSelectModule = viewModel::selectModule,
        onShowMissingFiles = viewModel::showMissingFiles,
        onPlay = viewModel::onPlayClicked,
        onQualityPresetChange = viewModel::setQualityPreset,
        onFrameRateCapChange = viewModel::setFrameRateCap,
        onPresentModeChange = viewModel::setPresentMode,
        onRenderScaleChange = viewModel::setRenderScalePercent,
        onMaxTextureSizeChange = viewModel::setMaxTextureSize,
        onThermalThrottlingChange = viewModel::setThermalThrottling,
        onDynamicResolutionChange = viewModel::setDynamicResolution,
        onLodBiasScalingChange = viewModel::setLodBiasScaling,
        onUiAspectChange = viewModel::setUiAspect,
        onUiScaleChange = viewModel::setUiScalePercent,
        onGyroAimChange = viewModel::setGyroAim,
        onPinchZoomChange = viewModel::setPinchZoom,
        onLanguageChange = viewModel::setLanguage,
        onVerifyScripts = viewModel::verifySelectedModuleScripts,
        onSelectDriver = viewModel::selectDriver,
        onImportDriver = onImportDriver,
        onAllowMockFallbackChange = viewModel::setAllowMockFallback,
        onProbeVulkan = viewModel::probeVulkanDriver,
        onShowFpsOverlayChange = viewModel::setShowFpsOverlay,
        onLaunchSelfTest = viewModel::onLaunchSelfTestClicked
    )

    when (val dialog = activeDialog) {
        is LauncherDialogState.ScanError -> {
            AlertDialog(
                onDismissRequest = { activeDialog = null },
                title = { Text("Could not read this folder") },
                text = { Text(dialog.reason) },
                confirmButton = {
                    TextButton(
                        onClick = {
                            activeDialog = null
                            onChooseFolder()
                        }
                    ) {
                        Text("Choose a different folder")
                    }
                },
                dismissButton = {
                    TextButton(onClick = { activeDialog = null }) {
                        Text("OK")
                    }
                }
            )
        }
        is LauncherDialogState.MissingFiles -> {
            val modName = dialog.module.displayName.ifBlank { dialog.module.folderName }
            val message = if (dialog.missingFiles.isEmpty()) {
                "Required Warband files could not be verified."
            } else {
                buildString {
                    append("These Warband files were not found:\n")
                    for (path in dialog.missingFiles) {
                        append("  - ").append(path).append('\n')
                    }
                    append("\nChoose the folder that contains Modules/ and CommonRes/.")
                }
            }
            AlertDialog(
                onDismissRequest = { activeDialog = null },
                title = { Text("Missing files ($modName)") },
                text = {
                    Column(modifier = Modifier.verticalScroll(rememberScrollState())) {
                        Text(message)
                    }
                },
                confirmButton = {
                    TextButton(
                        onClick = {
                            activeDialog = null
                            onChooseFolder()
                        }
                    ) {
                        Text("Choose folder")
                    }
                },
                dismissButton = {
                    TextButton(onClick = { activeDialog = null }) {
                        Text("OK")
                    }
                }
            )
        }
        is LauncherDialogState.WarbandFilesNotFound -> {
            AlertDialog(
                onDismissRequest = { activeDialog = null },
                title = { Text("Warband files not found") },
                text = {
                    Text(
                        "${dialog.reason}\n\nTap \"Choose folder\" and pick the Mount & Blade: " +
                            "Warband folder itself (the one that contains Modules, Data and CommonRes)."
                    )
                },
                confirmButton = {
                    TextButton(onClick = { activeDialog = null }) {
                        Text(context.getString(android.R.string.ok))
                    }
                }
            )
        }
        null -> Unit
    }
}

@Composable
fun LauncherAppContent(
    selectedTab: LauncherTab,
    installFolder: InstallFolderState,
    scanState: ScanState,
    modulesState: ModulesUiState,
    settingsState: SettingsUiState,
    driverState: DriverUiState,
    showFpsOverlay: Boolean,
    statusLog: String,
    onSelectTab: (LauncherTab) -> Unit,
    onChooseFolder: () -> Unit,
    onRescan: () -> Unit,
    onCancelScan: () -> Unit,
    onSelectModule: (Int) -> Unit,
    onShowMissingFiles: (ModuleChoice, List<String>) -> Unit,
    onPlay: () -> Unit,
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
    onVerifyScripts: () -> Unit,
    onSelectDriver: (Int) -> Unit,
    onImportDriver: () -> Unit,
    onAllowMockFallbackChange: (Boolean) -> Unit,
    onProbeVulkan: () -> Unit,
    onShowFpsOverlayChange: (Boolean) -> Unit,
    onLaunchSelfTest: () -> Unit
) {
    val context = LocalContext.current
    val isLandscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE

    val renderRoute: @Composable (LauncherTab) -> Unit = { tab ->
        when (tab) {
            LauncherTab.HOME -> InterimHomeScreen(
                installFolder = installFolder,
                scanState = scanState,
                modulesState = modulesState,
                statusLog = statusLog,
                onChooseFolder = onChooseFolder,
                onChangeModule = { onSelectTab(LauncherTab.MODULES) },
                onShowMissingFiles = onShowMissingFiles,
                onPlay = onPlay
            )
            LauncherTab.MODULES -> InterimModulesScreen(
                installFolder = installFolder,
                scanState = scanState,
                modulesState = modulesState,
                onChooseFolder = onChooseFolder,
                onRescan = onRescan,
                onCancelScan = onCancelScan,
                onSelectModule = onSelectModule,
                onShowMissingFiles = onShowMissingFiles
            )
            LauncherTab.SETTINGS -> InterimSettingsScreen(
                settingsState = settingsState,
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
                onGyroAimChange = onGyroAimChange,
                onPinchZoomChange = onPinchZoomChange,
                onLanguageChange = onLanguageChange
            )
            LauncherTab.ADVANCED -> InterimAdvancedScreen(
                installFolder = installFolder,
                scanState = scanState,
                modulesState = modulesState,
                driverState = driverState,
                showFpsOverlay = showFpsOverlay,
                statusLog = statusLog,
                onVerifyScripts = onVerifyScripts,
                onSelectDriver = onSelectDriver,
                onImportDriver = onImportDriver,
                onAllowMockFallbackChange = onAllowMockFallbackChange,
                onProbeVulkan = onProbeVulkan,
                onShowFpsOverlayChange = onShowFpsOverlayChange,
                onLaunchSelfTest = onLaunchSelfTest
            )
        }
    }

    if (isLandscape) {
        Row(modifier = Modifier.fillMaxSize()) {
            NavigationRail(
                windowInsets = WindowInsets.safeDrawing.only(
                    WindowInsetsSides.Vertical + WindowInsetsSides.Start
                )
            ) {
                Spacer(Modifier.weight(1f))
                LauncherTab.entries.forEach { tab ->
                    val label = tab.resolvedTitle(context)
                    NavigationRailItem(
                        selected = tab == selectedTab,
                        onClick = { onSelectTab(tab) },
                        icon = { Icon(navIconFor(tab), contentDescription = label) },
                        label = {
                            Text(
                                text = label,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis
                            )
                        }
                    )
                }
                Spacer(Modifier.weight(1f))
            }
            Scaffold(
                modifier = Modifier
                    .weight(1f)
                    .fillMaxHeight(),
                contentWindowInsets = WindowInsets.safeDrawing.only(
                    WindowInsetsSides.Vertical + WindowInsetsSides.End
                )
            ) { innerPadding ->
                Crossfade(
                    targetState = selectedTab,
                    label = "launcher-tab-landscape",
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(innerPadding)
                ) { tab ->
                    Box(modifier = Modifier.fillMaxSize()) {
                        renderRoute(tab)
                    }
                }
            }
        }
    } else {
        Scaffold(
            contentWindowInsets = WindowInsets.safeDrawing,
            bottomBar = {
                NavigationBar(
                    windowInsets = WindowInsets.safeDrawing.only(
                        WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom
                    )
                ) {
                    LauncherTab.entries.forEach { tab ->
                        val label = tab.resolvedTitle(context)
                        NavigationBarItem(
                            selected = tab == selectedTab,
                            onClick = { onSelectTab(tab) },
                            icon = { Icon(navIconFor(tab), contentDescription = label) },
                            label = {
                                Text(
                                    text = label,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis
                                )
                            }
                        )
                    }
                }
            }
        ) { innerPadding ->
            Crossfade(
                targetState = selectedTab,
                label = "launcher-tab-portrait",
                modifier = Modifier
                    .fillMaxSize()
                    .padding(innerPadding)
            ) { tab ->
                Box(modifier = Modifier.fillMaxSize()) {
                    renderRoute(tab)
                }
            }
        }
    }
}

private fun navIconFor(tab: LauncherTab): ImageVector = when (tab) {
    LauncherTab.HOME -> NavIcons.Home
    LauncherTab.MODULES -> NavIcons.Modules
    LauncherTab.SETTINGS -> NavIcons.Settings
    LauncherTab.ADVANCED -> NavIcons.Advanced
}

private object NavIcons {
    val Home: ImageVector by lazy {
        ImageVector.Builder(
            name = "Home",
            defaultWidth = 24.dp,
            defaultHeight = 24.dp,
            viewportWidth = 24f,
            viewportHeight = 24f
        ).apply {
            path(fill = SolidColor(Color.White)) {
                moveTo(10f, 20f)
                verticalLineTo(14f)
                horizontalLineTo(14f)
                verticalLineTo(20f)
                horizontalLineTo(19f)
                verticalLineTo(12f)
                horizontalLineTo(22f)
                lineTo(12f, 3f)
                lineTo(2f, 12f)
                horizontalLineTo(5f)
                verticalLineTo(20f)
                close()
            }
        }.build()
    }

    val Modules: ImageVector by lazy {
        ImageVector.Builder(
            name = "Modules",
            defaultWidth = 24.dp,
            defaultHeight = 24.dp,
            viewportWidth = 24f,
            viewportHeight = 24f
        ).apply {
            path(fill = SolidColor(Color.White)) {
                moveTo(3f, 3f)
                horizontalLineTo(10f)
                verticalLineTo(10f)
                horizontalLineTo(3f)
                close()
                moveTo(14f, 3f)
                horizontalLineTo(21f)
                verticalLineTo(10f)
                horizontalLineTo(14f)
                close()
                moveTo(3f, 14f)
                horizontalLineTo(10f)
                verticalLineTo(21f)
                horizontalLineTo(3f)
                close()
                moveTo(14f, 14f)
                horizontalLineTo(21f)
                verticalLineTo(21f)
                horizontalLineTo(14f)
                close()
            }
        }.build()
    }

    val Settings: ImageVector by lazy {
        ImageVector.Builder(
            name = "Settings",
            defaultWidth = 24.dp,
            defaultHeight = 24.dp,
            viewportWidth = 24f,
            viewportHeight = 24f
        ).apply {
            path(fill = SolidColor(Color.White)) {
                moveTo(3f, 5f)
                horizontalLineTo(11f)
                verticalLineTo(7f)
                horizontalLineTo(3f)
                close()
                moveTo(12f, 3f)
                horizontalLineTo(14f)
                verticalLineTo(9f)
                horizontalLineTo(12f)
                close()
                moveTo(15f, 5f)
                horizontalLineTo(21f)
                verticalLineTo(7f)
                horizontalLineTo(15f)
                close()
                moveTo(3f, 11f)
                horizontalLineTo(7f)
                verticalLineTo(13f)
                horizontalLineTo(3f)
                close()
                moveTo(8f, 9f)
                horizontalLineTo(10f)
                verticalLineTo(15f)
                horizontalLineTo(8f)
                close()
                moveTo(11f, 11f)
                horizontalLineTo(21f)
                verticalLineTo(13f)
                horizontalLineTo(11f)
                close()
                moveTo(3f, 17f)
                horizontalLineTo(15f)
                verticalLineTo(19f)
                horizontalLineTo(3f)
                close()
                moveTo(16f, 15f)
                horizontalLineTo(18f)
                verticalLineTo(21f)
                horizontalLineTo(16f)
                close()
                moveTo(19f, 17f)
                horizontalLineTo(21f)
                verticalLineTo(19f)
                horizontalLineTo(19f)
                close()
            }
        }.build()
    }

    val Advanced: ImageVector by lazy {
        ImageVector.Builder(
            name = "Advanced",
            defaultWidth = 24.dp,
            defaultHeight = 24.dp,
            viewportWidth = 24f,
            viewportHeight = 24f
        ).apply {
            path(fill = SolidColor(Color.White)) {
                moveTo(4f, 7f)
                lineTo(9f, 12f)
                lineTo(4f, 17f)
                lineTo(5.5f, 18.5f)
                lineTo(12f, 12f)
                lineTo(5.5f, 5.5f)
                close()
                moveTo(12f, 17f)
                horizontalLineTo(20f)
                verticalLineTo(19f)
                horizontalLineTo(12f)
                close()
            }
        }.build()
    }
}
