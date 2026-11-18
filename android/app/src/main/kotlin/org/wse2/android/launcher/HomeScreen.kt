@file:OptIn(androidx.compose.animation.ExperimentalSharedTransitionApi::class)

package org.wse2.android.launcher

import android.content.res.Configuration
import androidx.compose.animation.AnimatedVisibilityScope
import androidx.compose.animation.ExperimentalSharedTransitionApi
import androidx.compose.animation.SharedTransitionScope
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.State
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.path
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.wse2.android.launcher.art.ModuleArt
import org.wse2.android.launcher.art.ModuleArtGraphic
import org.wse2.android.launcher.theme.CinzelFontFamily
import org.wse2.android.launcher.theme.LocalWseColors
import org.wse2.android.launcher.theme.Wse2Theme
import org.wse2.android.launcher.theme.WseShapes
import org.wse2.android.launcher.theme.WseType
import org.wse2.android.launcher.theme.wseBackdrop
import org.wse2.android.launcher.ui.WsePrimaryButton
import org.wse2.android.launcher.ui.WseSecondaryButton
import org.wse2.android.launcher.ui.WseSectionHeader
import org.wse2.android.launcher.ui.WseStatusChip
import org.wse2.android.launcher.ui.WseStatusTone
import org.wse2.android.launcher.ui.WseTextButton
import org.wse2.android.launcher.ui.rememberWseAnimationsEnabled
import org.wse2.android.launcher.ui.wseStaggeredEntrance

@OptIn(ExperimentalSharedTransitionApi::class)
@Composable
fun HomeScreen(
    state: HomeUiState,
    onChooseFolder: () -> Unit,
    onChangeModule: () -> Unit,
    onShowMissingFiles: (ModuleChoice, List<String>) -> Unit,
    onPlay: () -> Unit,
    modifier: Modifier = Modifier,
    topContentPadding: Dp = 0.dp,
    sharedTransitionScope: SharedTransitionScope? = null,
    animatedVisibilityScope: AnimatedVisibilityScope? = null,
    modules: List<ModuleChoice> = emptyList(),
    selectedIndex: Int = -1,
    moduleArt: Map<String, ModuleArt> = emptyMap(),
    onSelectModule: (Int) -> Unit = {}
) {
    val hasFolder = state.installFolder.treeUri != null ||
        !state.installFolder.displayName.isNullOrBlank()
    val selectedModule = state.selectedModule
    val installStatus = state.installStatus
    val isReady = !state.scanState.inProgress &&
        hasFolder &&
        selectedModule != null &&
        installStatus?.state == InstallCheckState.READY
    val isMissing = !state.scanState.inProgress &&
        hasFolder &&
        selectedModule != null &&
        installStatus?.state == InstallCheckState.MISSING

    val statusInfo = resolveHomeStatus(
        state = state,
        hasFolder = hasFolder,
        selectedModule = selectedModule,
        installStatus = installStatus
    )

    val heroTitle = selectedModule?.let { it.displayName.ifBlank { it.folderName } }
        ?: "WSE2 Android"
    val folderDisplay = state.installFolder.displayName?.takeIf { it.isNotBlank() }
    val heroSubtitle = when {
        selectedModule != null && folderDisplay != null ->
            "${selectedModule.modulePath} · $folderDisplay"
        selectedModule != null -> selectedModule.modulePath
        hasFolder && state.scanState.inProgress -> "Scanning folder for modules…"
        hasFolder -> "No Warband modules found in ${folderDisplay ?: "this folder"}"
        else -> "Pick your Mount & Blade: Warband folder"
    }

    val artSharedModifier = if (
        selectedModule != null &&
        sharedTransitionScope != null &&
        animatedVisibilityScope != null
    ) {
        with(sharedTransitionScope) {
            Modifier.sharedElement(
                rememberSharedContentState(key = "module-art-${selectedModule.modulePath}"),
                animatedVisibilityScope
            )
        }
    } else {
        Modifier
    }

    // Each Home entry reveals its blocks once (staggered); the set resets when Home leaves.
    val revealedKeys = remember { mutableSetOf<Any>() }
    val animationsEnabled = rememberWseAnimationsEnabled()
    val showModules = modules.size > 1

    BoxWithConstraints(
        modifier = modifier.fillMaxSize()
    ) {
        val isLandscape =
            LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE ||
                (maxWidth > maxHeight && maxWidth >= 560.dp)

        if (isLandscape) {
            Row(modifier = Modifier.fillMaxSize()) {
                HomeHeroBanner(
                    art = state.heroArt,
                    heroTitle = heroTitle,
                    heroSubtitle = heroSubtitle,
                    overline = if (selectedModule != null) "Selected module" else "Warband launcher",
                    fadeRightEdge = true,
                    topContentPadding = topContentPadding,
                    artModifier = artSharedModifier,
                    animationsEnabled = animationsEnabled,
                    modifier = Modifier
                        .weight(0.58f)
                        .fillMaxHeight()
                )
                Column(
                    modifier = Modifier
                        .weight(0.42f)
                        .fillMaxHeight()
                        .verticalScroll(rememberScrollState())
                        .padding(
                            start = 16.dp,
                            end = 20.dp,
                            top = topContentPadding + 16.dp,
                            bottom = 16.dp
                        ),
                    verticalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    HomeActionsPanel(
                        statusInfo = statusInfo,
                        isReady = isReady,
                        isMissing = isMissing,
                        scanInProgress = state.scanState.inProgress,
                        selectedModule = selectedModule,
                        installStatus = installStatus,
                        folderDisplay = folderDisplay,
                        statusLog = state.statusLog,
                        compactButtonsStacked = true,
                        moduleSectionBeforeButtons = false,
                        revealedKeys = revealedKeys,
                        animationsEnabled = animationsEnabled,
                        moduleSection = if (showModules) {
                            {
                                HomeModuleGrid(
                                    modules = modules,
                                    selectedIndex = selectedIndex,
                                    moduleArt = moduleArt,
                                    onSelectModule = onSelectModule
                                )
                            }
                        } else {
                            null
                        },
                        onChooseFolder = onChooseFolder,
                        onChangeModule = onChangeModule,
                        onShowMissingFiles = onShowMissingFiles,
                        onPlay = onPlay
                    )
                }
            }
        } else {
            val heroHeight = (maxHeight * 0.55f).coerceAtLeast(300.dp) + topContentPadding
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .verticalScroll(rememberScrollState())
            ) {
                HomeHeroBanner(
                    art = state.heroArt,
                    heroTitle = heroTitle,
                    heroSubtitle = heroSubtitle,
                    overline = if (selectedModule != null) "Selected module" else "Warband launcher",
                    fadeRightEdge = false,
                    topContentPadding = topContentPadding,
                    artModifier = artSharedModifier,
                    animationsEnabled = animationsEnabled,
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(heroHeight)
                )
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(start = 20.dp, end = 20.dp, top = 8.dp, bottom = 20.dp),
                    verticalArrangement = Arrangement.spacedBy(14.dp)
                ) {
                    HomeActionsPanel(
                        statusInfo = statusInfo,
                        isReady = isReady,
                        isMissing = isMissing,
                        scanInProgress = state.scanState.inProgress,
                        selectedModule = selectedModule,
                        installStatus = installStatus,
                        folderDisplay = folderDisplay,
                        statusLog = state.statusLog,
                        compactButtonsStacked = false,
                        moduleSectionBeforeButtons = true,
                        revealedKeys = revealedKeys,
                        animationsEnabled = animationsEnabled,
                        moduleSection = if (showModules) {
                            {
                                HomeModuleStrip(
                                    modules = modules,
                                    selectedIndex = selectedIndex,
                                    moduleArt = moduleArt,
                                    onSelectModule = onSelectModule
                                )
                            }
                        } else {
                            null
                        },
                        onChooseFolder = onChooseFolder,
                        onChangeModule = onChangeModule,
                        onShowMissingFiles = onShowMissingFiles,
                        onPlay = onPlay
                    )
                }
            }
        }
    }
}

private enum class HomeChipTone {
    READY,
    MISSING,
    PENDING
}

private data class HomeStatusInfo(
    val chipLabel: String,
    val tone: HomeChipTone,
    val disabledReason: String?
)

private fun resolveHomeStatus(
    state: HomeUiState,
    hasFolder: Boolean,
    selectedModule: ModuleChoice?,
    installStatus: ModuleInstallStatus?
): HomeStatusInfo {
    return when {
        state.scanState.inProgress -> {
            val label = state.scanState.folderLabel?.takeIf { it.isNotBlank() }
                ?: state.installFolder.displayName?.takeIf { it.isNotBlank() }
                ?: "folder"
            HomeStatusInfo(
                chipLabel = "Checking…",
                tone = HomeChipTone.PENDING,
                disabledReason = "Scanning $label for Warband modules…"
            )
        }
        !hasFolder -> HomeStatusInfo(
            chipLabel = "Choose folder",
            tone = HomeChipTone.PENDING,
            disabledReason = "Choose the Warband folder that contains Modules/ and CommonRes/."
        )
        selectedModule == null -> HomeStatusInfo(
            chipLabel = "No module",
            tone = HomeChipTone.PENDING,
            disabledReason = "No Warband module selected."
        )
        installStatus == null || installStatus.state == InstallCheckState.CHECKING -> HomeStatusInfo(
            chipLabel = "Checking…",
            tone = HomeChipTone.PENDING,
            disabledReason = "Checking required Warband files…"
        )
        installStatus.state == InstallCheckState.READY -> HomeStatusInfo(
            chipLabel = "Ready",
            tone = HomeChipTone.READY,
            disabledReason = null
        )
        else -> {
            val count = installStatus.missingFiles.size
            val chipText = when {
                count == 1 -> "1 file missing"
                count > 1 -> "$count files missing"
                else -> "Files missing"
            }
            val reasonText = if (count > 0) {
                "$count required Warband file(s) missing."
            } else {
                "Required Warband files could not be verified."
            }
            HomeStatusInfo(
                chipLabel = chipText,
                tone = HomeChipTone.MISSING,
                disabledReason = reasonText
            )
        }
    }
}

@Composable
private fun HomeHeroBanner(
    art: ModuleArt?,
    heroTitle: String,
    heroSubtitle: String,
    overline: String,
    fadeRightEdge: Boolean,
    topContentPadding: Dp,
    artModifier: Modifier,
    animationsEnabled: Boolean,
    modifier: Modifier = Modifier
) {
    val colors = LocalWseColors.current
    val fadeTarget = colors.background
    // Ends fully opaque so the art never shows through as a band at the bottom edge.
    val verticalScrim = Brush.verticalGradient(
        0.0f to fadeTarget.copy(alpha = 0.30f),
        0.35f to fadeTarget.copy(alpha = 0.10f),
        0.70f to fadeTarget.copy(alpha = 0.70f),
        1.0f to fadeTarget.copy(alpha = 1.0f)
    )
    // Slow Ken Burns drift (18 s each way); read in the draw phase only, so it never
    // recomposes. Off when the system animator scale is 0 and in screenshot tests.
    val kenBurns: State<Float>? = if (animationsEnabled) {
        val transition = rememberInfiniteTransition(label = "home-hero")
        transition.animateFloat(
            initialValue = 0f,
            targetValue = 1f,
            animationSpec = infiniteRepeatable(
                animation = tween(durationMillis = 18_000, easing = LinearEasing),
                repeatMode = RepeatMode.Reverse
            ),
            label = "home-hero-ken-burns"
        )
    } else {
        null
    }
    Box(modifier = modifier) {
        ModuleArtGraphic(
            art = art,
            fallbackTitle = heroTitle,
            contentDescription = heroTitle,
            blurRadius = 3.dp,
            modifier = artModifier.fillMaxSize(),
            imageModifier = Modifier.graphicsLayer {
                val t = kenBurns?.value ?: 0f
                // 5 % overscan even at rest: crops the art's outermost pixels, so a light
                // border in a module's main.bmp (or the blur's clamped edge) never shows as
                // a line along the hero's top edge.
                val s = 1.05f + 0.08f * t
                scaleX = s
                scaleY = s
                translationX = -size.width * 0.03f * t
                translationY = -size.height * 0.015f * t
            }
        )
        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(verticalScrim)
        )
        if (fadeRightEdge) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .background(
                        Brush.horizontalGradient(
                            0.70f to Color.Transparent,
                            1.0f to fadeTarget
                        )
                    )
            )
        }
        Column(
            modifier = Modifier
                .align(Alignment.BottomStart)
                .padding(
                    start = 20.dp,
                    end = 20.dp,
                    top = topContentPadding + 16.dp,
                    bottom = 12.dp
                )
        ) {
            Text(
                text = overline.uppercase(),
                style = WseType.overline,
                color = colors.primary
            )
            Spacer(Modifier.height(4.dp))
            Text(
                text = heroTitle,
                style = MaterialTheme.typography.displayMedium,
                color = colors.onSurface,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis
            )
            Spacer(Modifier.height(4.dp))
            Text(
                text = heroSubtitle,
                style = MaterialTheme.typography.bodyMedium,
                color = colors.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
    }
}

@Composable
private fun HomeActionsPanel(
    statusInfo: HomeStatusInfo,
    isReady: Boolean,
    isMissing: Boolean,
    scanInProgress: Boolean,
    selectedModule: ModuleChoice?,
    installStatus: ModuleInstallStatus?,
    folderDisplay: String?,
    statusLog: String,
    compactButtonsStacked: Boolean,
    moduleSectionBeforeButtons: Boolean,
    revealedKeys: MutableSet<Any>,
    animationsEnabled: Boolean,
    moduleSection: (@Composable () -> Unit)?,
    onChooseFolder: () -> Unit,
    onChangeModule: () -> Unit,
    onShowMissingFiles: (ModuleChoice, List<String>) -> Unit,
    onPlay: () -> Unit
) {
    val colors = LocalWseColors.current
    val haptic = LocalHapticFeedback.current
    val chipTone = when (statusInfo.tone) {
        HomeChipTone.READY -> WseStatusTone.Ready
        HomeChipTone.MISSING -> WseStatusTone.Error
        HomeChipTone.PENDING -> WseStatusTone.Neutral
    }
    val chipClickable = isMissing && selectedModule != null
    Box(modifier = Modifier.wseStaggeredEntrance("chip", 0, revealedKeys, animationsEnabled)) {
        WseStatusChip(
            text = statusInfo.chipLabel,
            tone = chipTone,
            enabled = true,
            onClick = if (chipClickable) {
                {
                    if (selectedModule != null && installStatus != null) {
                        onShowMissingFiles(selectedModule, installStatus.missingFiles)
                    }
                }
            } else {
                null
            }
        )
    }

    WsePrimaryButton(
        text = "Play",
        onClick = {
            haptic.performHapticFeedback(HapticFeedbackType.LongPress)
            onPlay()
        },
        enabled = isReady,
        leadingIcon = {
            Icon(
                imageVector = HomeIcons.Play,
                contentDescription = null,
                modifier = Modifier.size(18.dp)
            )
        },
        modifier = Modifier
            .fillMaxWidth()
            .height(60.dp)
            .then(Modifier.wseStaggeredEntrance("play", 1, revealedKeys, animationsEnabled))
    )

    if (statusInfo.disabledReason != null) {
        Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(
                text = statusInfo.disabledReason,
                style = MaterialTheme.typography.bodySmall,
                color = if (isMissing) {
                    colors.error
                } else {
                    colors.onSurfaceVariant
                }
            )
            if (isMissing && selectedModule != null && installStatus != null) {
                WseTextButton(
                    text = "View missing files",
                    onClick = { onShowMissingFiles(selectedModule, installStatus.missingFiles) }
                )
            }
        }
    }

    if (moduleSection != null && moduleSectionBeforeButtons) {
        Box(modifier = Modifier.wseStaggeredEntrance("modules", 2, revealedKeys, animationsEnabled)) { moduleSection() }
    }

    if (compactButtonsStacked) {
        Column(
            modifier = Modifier.wseStaggeredEntrance("buttons", 2, revealedKeys, animationsEnabled),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            WseSecondaryButton(
                text = "Change module",
                onClick = onChangeModule,
                modifier = Modifier.fillMaxWidth()
            )
            WseSecondaryButton(
                text = "Choose folder",
                onClick = onChooseFolder,
                enabled = !scanInProgress,
                modifier = Modifier.fillMaxWidth()
            )
        }
    } else {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .then(Modifier.wseStaggeredEntrance("buttons", 3, revealedKeys, animationsEnabled)),
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            WseSecondaryButton(
                text = "Choose folder",
                onClick = onChooseFolder,
                enabled = !scanInProgress,
                modifier = Modifier.weight(1f)
            )
            WseSecondaryButton(
                text = "Modules",
                onClick = onChangeModule,
                modifier = Modifier.weight(1f)
            )
        }
    }

    if (moduleSection != null && !moduleSectionBeforeButtons) {
        Box(modifier = Modifier.wseStaggeredEntrance("modules", 3, revealedKeys, animationsEnabled)) { moduleSection() }
    }

    val footerText = buildString {
        if (folderDisplay != null) {
            append("Install: ").append(folderDisplay).append(" · ")
        }
        val singleLineStatus = statusLog
            .lineSequence()
            .map { it.trim() }
            .filter { it.isNotEmpty() }
            .joinToString(" · ")
            .ifEmpty { "Ready." }
        append(singleLineStatus)
    }
    Text(
        text = footerText,
        style = MaterialTheme.typography.bodySmall,
        color = colors.onSurfaceVariant,
        maxLines = 2,
        overflow = TextOverflow.Ellipsis,
        modifier = Modifier.wseStaggeredEntrance("footer", 4, revealedKeys, animationsEnabled)
    )
}

@Composable
private fun HomeModuleStrip(
    modules: List<ModuleChoice>,
    selectedIndex: Int,
    moduleArt: Map<String, ModuleArt>,
    onSelectModule: (Int) -> Unit
) {
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        WseSectionHeader(title = "Your modules")
        LazyRow(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            items(modules.size) { index ->
                val module = modules[index]
                HomeModuleCard(
                    module = module,
                    art = moduleArt[module.modulePath],
                    selected = index == selectedIndex,
                    onClick = { onSelectModule(index) },
                    modifier = Modifier.size(width = 140.dp, height = 92.dp)
                )
            }
        }
    }
}

@Composable
private fun HomeModuleGrid(
    modules: List<ModuleChoice>,
    selectedIndex: Int,
    moduleArt: Map<String, ModuleArt>,
    onSelectModule: (Int) -> Unit
) {
    // A plain two-column grid: it sits inside a scrolling column, so it must not be lazy.
    Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
        WseSectionHeader(title = "Your modules")
        modules.indices.chunked(2).forEach { rowIndices ->
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                rowIndices.forEach { index ->
                    val module = modules[index]
                    HomeModuleCard(
                        module = module,
                        art = moduleArt[module.modulePath],
                        selected = index == selectedIndex,
                        onClick = { onSelectModule(index) },
                        modifier = Modifier
                            .weight(1f)
                            .aspectRatio(140f / 92f)
                    )
                }
                if (rowIndices.size == 1) {
                    Spacer(Modifier.weight(1f))
                }
            }
        }
    }
}

@Composable
private fun HomeModuleCard(
    module: ModuleChoice,
    art: ModuleArt?,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier
) {
    val colors = LocalWseColors.current
    val name = module.displayName.ifBlank { module.folderName }
    val interactionSource = remember { MutableInteractionSource() }
    val pressed by interactionSource.collectIsPressedAsState()
    val scale by animateFloatAsState(
        targetValue = if (pressed) 0.97f else 1f,
        animationSpec = tween(durationMillis = 120),
        label = "home-module-card-scale"
    )
    Box(
        modifier = modifier
            .graphicsLayer {
                scaleX = scale
                scaleY = scale
            }
            .clip(WseShapes.medium)
            .border(
                width = if (selected) 1.5.dp else 1.dp,
                color = if (selected) colors.accent else colors.hairline,
                shape = WseShapes.medium
            )
            .clickable(
                interactionSource = interactionSource,
                indication = null,
                role = Role.Button,
                onClick = onClick
            )
    ) {
        ModuleArtGraphic(
            art = art,
            fallbackTitle = name,
            contentDescription = name,
            modifier = Modifier.fillMaxSize()
        )
        Box(
            modifier = Modifier
                .fillMaxSize()
                .background(
                    Brush.verticalGradient(
                        0.40f to Color.Transparent,
                        1.0f to colors.background.copy(alpha = 0.90f)
                    )
                )
        )
        Text(
            text = name,
            style = MaterialTheme.typography.titleSmall.copy(
                fontFamily = CinzelFontFamily,
                fontWeight = FontWeight.Bold
            ),
            color = colors.onSurface,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier
                .align(Alignment.BottomStart)
                .padding(horizontal = 10.dp, vertical = 8.dp)
        )
    }
}

private object HomeIcons {
    val Play: ImageVector by lazy {
        ImageVector.Builder(
            name = "Play",
            defaultWidth = 24.dp,
            defaultHeight = 24.dp,
            viewportWidth = 24f,
            viewportHeight = 24f
        ).apply {
            path(fill = SolidColor(Color.White)) {
                moveTo(8f, 5f)
                verticalLineTo(19f)
                lineTo(19f, 12f)
                close()
            }
        }.build()
    }
}

private fun previewHomeState(missing: Boolean = false): HomeUiState {
    val mod = ModuleChoice(
        folderName = "Native",
        modulePath = "Modules/Native",
        displayName = "Native",
        hasScriptsTxt = true,
        loadResourceCount = 221,
        loadModResourceCount = 0
    )
    return HomeUiState(
        installFolder = InstallFolderState(displayName = "Mount & Blade - Warband"),
        scanState = ScanState(inProgress = false, indexStatsText = "SafVfs index: 4210 files"),
        selectedModule = mod,
        installStatus = if (missing) {
            ModuleInstallStatus(
                state = InstallCheckState.MISSING,
                missingFiles = listOf("CommonRes/", "Data/font_data.xml", "Modules/Native/scripts.txt")
            )
        } else {
            ModuleInstallStatus(state = InstallCheckState.READY)
        },
        heroArt = null,
        statusLog = "Found 3 module(s) in 420 ms."
    )
}

@Preview(name = "Home Portrait Dark", showBackground = true, widthDp = 390, heightDp = 844)
@Composable
private fun PreviewHomePortraitDark() {
    Wse2Theme(darkTheme = true, dynamicColor = false) {
        Box(modifier = Modifier.wseBackdrop(LocalWseColors.current.accent)) {
            HomeScreen(
                state = previewHomeState(),
                onChooseFolder = {},
                onChangeModule = {},
                onShowMissingFiles = { _, _ -> },
                onPlay = {}
            )
        }
    }
}

@Preview(name = "Home Portrait Missing", showBackground = true, widthDp = 390, heightDp = 844)
@Composable
private fun PreviewHomePortraitLight() {
    Wse2Theme(darkTheme = false, dynamicColor = false) {
        Box(modifier = Modifier.wseBackdrop(LocalWseColors.current.accent)) {
            HomeScreen(
                state = previewHomeState(missing = true),
                onChooseFolder = {},
                onChangeModule = {},
                onShowMissingFiles = { _, _ -> },
                onPlay = {}
            )
        }
    }
}

@Preview(name = "Home Landscape Dark", showBackground = true, widthDp = 844, heightDp = 390)
@Composable
private fun PreviewHomeLandscapeDark() {
    Wse2Theme(darkTheme = true, dynamicColor = false) {
        Box(modifier = Modifier.wseBackdrop(LocalWseColors.current.accent)) {
            HomeScreen(
                state = previewHomeState(),
                onChooseFolder = {},
                onChangeModule = {},
                onShowMissingFiles = { _, _ -> },
                onPlay = {}
            )
        }
    }
}
