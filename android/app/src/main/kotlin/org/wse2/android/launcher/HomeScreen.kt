@file:OptIn(androidx.compose.animation.ExperimentalSharedTransitionApi::class)

package org.wse2.android.launcher

import android.content.res.Configuration
import androidx.compose.animation.AnimatedVisibilityScope
import androidx.compose.animation.ExperimentalSharedTransitionApi
import androidx.compose.animation.SharedTransitionScope
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.path
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.wse2.android.launcher.art.ModuleArt
import org.wse2.android.launcher.art.ModuleArtGraphic
import org.wse2.android.launcher.theme.LocalWseColors
import org.wse2.android.launcher.theme.Wse2Theme
import org.wse2.android.launcher.theme.WseType
import org.wse2.android.launcher.theme.wseBackdrop
import org.wse2.android.launcher.ui.WsePrimaryButton
import org.wse2.android.launcher.ui.WseSecondaryButton
import org.wse2.android.launcher.ui.WseStatusChip
import org.wse2.android.launcher.ui.WseStatusTone
import org.wse2.android.launcher.ui.WseTextButton

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
    animatedVisibilityScope: AnimatedVisibilityScope? = null
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
                    modifier = Modifier
                        .weight(2f)
                        .fillMaxHeight()
                )
                Column(
                    modifier = Modifier
                        .weight(1f)
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
                        onChooseFolder = onChooseFolder,
                        onChangeModule = onChangeModule,
                        onShowMissingFiles = onShowMissingFiles,
                        onPlay = onPlay
                    )
                }
            }
        } else {
            val heroHeight = (maxHeight * 0.48f).coerceIn(250.dp, 420.dp) + topContentPadding
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
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(heroHeight)
                )
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 20.dp, vertical = 16.dp),
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
                        compactButtonsStacked = false,
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
    modifier: Modifier = Modifier
) {
    val colors = LocalWseColors.current
    val fadeTarget = colors.background
    val verticalScrim = Brush.verticalGradient(
        0.0f to fadeTarget.copy(alpha = 0.28f),
        0.38f to fadeTarget.copy(alpha = 0.14f),
        0.72f to fadeTarget.copy(alpha = 0.76f),
        0.94f to fadeTarget.copy(alpha = 0.94f),
        1.0f to fadeTarget.copy(alpha = 0.0f)
    )
    Box(modifier = modifier) {
        ModuleArtGraphic(
            art = art,
            fallbackTitle = heroTitle,
            contentDescription = heroTitle,
            blurRadius = 6.dp,
            modifier = artModifier.fillMaxSize()
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
                            0.72f to Color.Transparent,
                            1.0f to fadeTarget.copy(alpha = 0.92f)
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
                    bottom = 16.dp
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
        modifier = Modifier.fillMaxWidth()
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

    if (compactButtonsStacked) {
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
    } else {
        Row(
            modifier = Modifier.fillMaxWidth(),
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
        overflow = TextOverflow.Ellipsis
    )
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
