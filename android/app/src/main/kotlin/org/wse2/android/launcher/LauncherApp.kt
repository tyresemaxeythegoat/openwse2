package org.wse2.android.launcher

import android.content.res.Configuration
import android.widget.Toast
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedContent
import androidx.compose.animation.AnimatedVisibilityScope
import androidx.compose.animation.ContentTransform
import androidx.compose.animation.ExperimentalSharedTransitionApi
import androidx.compose.animation.SharedTransitionLayout
import androidx.compose.animation.SharedTransitionScope
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.scaleIn
import androidx.compose.animation.togetherWith
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.calculateEndPadding
import androidx.compose.foundation.layout.calculateStartPadding
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.material3.Scaffold
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
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.wse2.android.launcher.art.ModuleArt
import org.wse2.android.launcher.theme.LocalWseColors
import org.wse2.android.launcher.theme.wseBackdrop
import org.wse2.android.launcher.ui.WseDialog
import org.wse2.android.launcher.ui.WseDialogAction
import org.wse2.android.launcher.ui.WseNavItem
import org.wse2.android.launcher.ui.WseNavigationBar
import org.wse2.android.launcher.ui.WseNavigationRail

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
    val moduleArt by viewModel.moduleArt.collectAsStateWithLifecycle()
    val heroArt by viewModel.heroArt.collectAsStateWithLifecycle()
    val r0State by viewModel.r0State.collectAsStateWithLifecycle()
    val r4State by viewModel.r4State.collectAsStateWithLifecycle()

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
        moduleArt = moduleArt,
        heroArt = heroArt,
        onSelectTab = viewModel::selectTab,
        onChooseFolder = onChooseFolder,
        onRescan = viewModel::rescanSelectedFolder,
        onCancelScan = viewModel::cancelScan,
        onSelectModule = { index ->
            viewModel.selectModule(index)
            viewModel.selectTab(LauncherTab.HOME)
        },
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
        onLaunchSelfTest = viewModel::onLaunchSelfTestClicked,
        r0State = r0State,
        onRunR0Spikes = viewModel::runR0Spikes,
        onDismissR0Report = viewModel::dismissR0Report,
        r4State = r4State,
        onRunR4Startup = viewModel::runR4Startup,
        onR4UseCopyChange = viewModel::setR4UseCopy,
        onDismissR4Report = viewModel::dismissR4Report
    )

    when (val dialog = activeDialog) {
        is LauncherDialogState.ScanError -> {
            WseDialog(
                title = "Could not read this folder",
                text = dialog.reason,
                confirm = WseDialogAction("Choose a different folder") {
                    activeDialog = null
                    onChooseFolder()
                },
                dismiss = WseDialogAction("OK") { activeDialog = null },
                onDismissRequest = { activeDialog = null }
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
            WseDialog(
                title = "Missing files ($modName)",
                text = message,
                confirm = WseDialogAction("Choose folder") {
                    activeDialog = null
                    onChooseFolder()
                },
                dismiss = WseDialogAction("OK") { activeDialog = null },
                onDismissRequest = { activeDialog = null }
            )
        }
        is LauncherDialogState.WarbandFilesNotFound -> {
            WseDialog(
                title = "Warband files not found",
                text = "${dialog.reason}\n\nTap \"Choose folder\" and pick the Mount & Blade: " +
                    "Warband folder itself (the one that contains Modules, Data and CommonRes).",
                confirm = WseDialogAction(context.getString(android.R.string.ok)) {
                    activeDialog = null
                },
                onDismissRequest = { activeDialog = null }
            )
        }
        null -> Unit
    }
}

@OptIn(ExperimentalSharedTransitionApi::class)
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
    moduleArt: Map<String, ModuleArt>,
    heroArt: ModuleArt?,
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
    onLaunchSelfTest: () -> Unit,
    r0State: R0UiState = R0UiState(),
    onRunR0Spikes: () -> Unit = {},
    onDismissR0Report: () -> Unit = {},
    r4State: R4UiState = R4UiState(),
    onRunR4Startup: () -> Unit = {},
    onR4UseCopyChange: (Boolean) -> Unit = {},
    onDismissR4Report: () -> Unit = {}
) {
    val context = LocalContext.current
    val layoutDirection = LocalLayoutDirection.current
    val isLandscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    val colors = LocalWseColors.current

    BackHandler(enabled = selectedTab != LauncherTab.HOME) {
        onSelectTab(LauncherTab.HOME)
    }

    val selectedModule = modulesState.selectedModule
    val resolvedHeroArt = heroArt
        ?: selectedModule?.let { moduleArt[it.modulePath] }
        ?: moduleArt[""]

    val homeState = HomeUiState(
        installFolder = installFolder,
        scanState = scanState,
        selectedModule = selectedModule,
        installStatus = modulesState.selectedInstallStatus,
        heroArt = resolvedHeroArt,
        statusLog = statusLog
    )

    val advancedState = AdvancedUiState(
        installFolder = installFolder,
        scanState = scanState,
        selectedModule = selectedModule,
        driverState = driverState,
        showFpsOverlay = showFpsOverlay,
        statusLog = statusLog
    )

    val navTabs = LauncherTab.entries
    val navItems = remember(context, navTabs) {
        navTabs.map { tab ->
            WseNavItem(
                label = tab.resolvedTitle(context),
                icon = navIconFor(tab)
            )
        }
    }
    val selectedTabIndex = navTabs.indexOf(selectedTab).coerceAtLeast(0)

    val renderRoute: @Composable (
        LauncherTab,
        Dp,
        SharedTransitionScope,
        AnimatedVisibilityScope
    ) -> Unit = { tab, topPadding, sharedScope, animScope ->
        when (tab) {
            LauncherTab.HOME -> HomeScreen(
                state = homeState,
                onChooseFolder = onChooseFolder,
                onChangeModule = { onSelectTab(LauncherTab.MODULES) },
                onShowMissingFiles = onShowMissingFiles,
                onPlay = onPlay,
                topContentPadding = topPadding,
                sharedTransitionScope = sharedScope,
                animatedVisibilityScope = animScope,
                modules = modulesState.modules,
                selectedIndex = modulesState.selectedIndex,
                moduleArt = moduleArt,
                onSelectModule = onSelectModule
            )
            LauncherTab.MODULES -> ModulesScreen(
                state = modulesState,
                installFolder = installFolder,
                scanState = scanState,
                moduleArt = moduleArt,
                onChooseFolder = onChooseFolder,
                onRescan = onRescan,
                onCancelScan = onCancelScan,
                onSelectModule = onSelectModule,
                onShowMissingFiles = onShowMissingFiles,
                topContentPadding = topPadding,
                sharedTransitionScope = sharedScope,
                animatedVisibilityScope = animScope
            )
            LauncherTab.SETTINGS -> SettingsScreen(
                state = settingsState,
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
                onLanguageChange = onLanguageChange,
                topContentPadding = topPadding
            )
            LauncherTab.ADVANCED -> AdvancedScreen(
                state = advancedState,
                onVerifyScripts = onVerifyScripts,
                onSelectDriver = onSelectDriver,
                onImportDriver = onImportDriver,
                onAllowMockFallbackChange = onAllowMockFallbackChange,
                onProbeVulkan = onProbeVulkan,
                onShowFpsOverlayChange = onShowFpsOverlayChange,
                onLaunchSelfTest = onLaunchSelfTest,
                topContentPadding = topPadding,
                r0State = r0State,
                onRunR0Spikes = onRunR0Spikes,
                onDismissR0Report = onDismissR0Report,
                r4State = r4State,
                onRunR4Startup = onRunR4Startup,
                onR4UseCopyChange = onR4UseCopyChange,
                onDismissR4Report = onDismissR4Report
            )
        }
    }

    SharedTransitionLayout(
        modifier = Modifier
            .fillMaxSize()
            .wseBackdrop(colors.accent)
    ) {
        val sharedScope = this
        if (isLandscape) {
            Row(modifier = Modifier.fillMaxSize()) {
                WseNavigationRail(
                    items = navItems,
                    selectedIndex = selectedTabIndex,
                    onSelect = { index -> onSelectTab(navTabs[index]) },
                    windowInsets = WindowInsets.safeDrawing.only(
                        WindowInsetsSides.Vertical + WindowInsetsSides.Start
                    )
                )
                Scaffold(
                    modifier = Modifier
                        .weight(1f)
                        .fillMaxHeight(),
                    containerColor = Color.Transparent,
                    contentWindowInsets = WindowInsets.safeDrawing.only(
                        WindowInsetsSides.Vertical + WindowInsetsSides.End
                    )
                ) { innerPadding ->
                    AnimatedContent(
                        targetState = selectedTab,
                        transitionSpec = { fadeThroughSpec() },
                        label = "launcher-tab-landscape",
                        modifier = Modifier
                            .fillMaxSize()
                            .padding(
                                start = innerPadding.calculateStartPadding(layoutDirection),
                                end = innerPadding.calculateEndPadding(layoutDirection),
                                bottom = innerPadding.calculateBottomPadding()
                            )
                    ) { tab ->
                        Box(modifier = Modifier.fillMaxSize()) {
                            renderRoute(
                                tab,
                                innerPadding.calculateTopPadding(),
                                sharedScope,
                                this@AnimatedContent
                            )
                        }
                    }
                }
            }
        } else {
            Scaffold(
                containerColor = Color.Transparent,
                contentWindowInsets = WindowInsets.safeDrawing,
                bottomBar = {
                    WseNavigationBar(
                        items = navItems,
                        selectedIndex = selectedTabIndex,
                        onSelect = { index -> onSelectTab(navTabs[index]) },
                        windowInsets = WindowInsets.safeDrawing.only(
                            WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom
                        )
                    )
                }
            ) { innerPadding ->
                AnimatedContent(
                    targetState = selectedTab,
                    transitionSpec = { fadeThroughSpec() },
                    label = "launcher-tab-portrait",
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(
                            start = innerPadding.calculateStartPadding(layoutDirection),
                            end = innerPadding.calculateEndPadding(layoutDirection),
                            bottom = innerPadding.calculateBottomPadding()
                        )
                ) { tab ->
                    Box(modifier = Modifier.fillMaxSize()) {
                        renderRoute(
                            tab,
                            innerPadding.calculateTopPadding(),
                            sharedScope,
                            this@AnimatedContent
                        )
                    }
                }
            }
        }
    }
}

private fun fadeThroughSpec(): ContentTransform =
    (fadeIn(animationSpec = tween(durationMillis = 210, delayMillis = 90)) +
        scaleIn(
            initialScale = 0.94f,
            animationSpec = tween(durationMillis = 210, delayMillis = 90)
        )) togetherWith fadeOut(animationSpec = tween(durationMillis = 90))

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
