@file:OptIn(androidx.compose.animation.ExperimentalSharedTransitionApi::class)

package org.wse2.android.launcher

import androidx.compose.animation.AnimatedVisibilityScope
import androidx.compose.animation.ExperimentalSharedTransitionApi
import androidx.compose.animation.SharedTransitionScope
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.itemsIndexed
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.wse2.android.launcher.art.ModuleArt
import org.wse2.android.launcher.theme.LocalWseColors
import org.wse2.android.launcher.theme.Wse2Theme
import org.wse2.android.launcher.theme.WseShapes
import org.wse2.android.launcher.theme.wseBackdrop
import org.wse2.android.launcher.ui.WseBannerCard
import org.wse2.android.launcher.ui.WsePrimaryButton
import org.wse2.android.launcher.ui.WseSecondaryButton
import org.wse2.android.launcher.ui.WseSectionHeader
import org.wse2.android.launcher.ui.WseStatusChip
import org.wse2.android.launcher.ui.WseStatusTone

@OptIn(ExperimentalSharedTransitionApi::class)
@Composable
fun ModulesScreen(
    state: ModulesUiState,
    installFolder: InstallFolderState,
    scanState: ScanState,
    moduleArt: Map<String, ModuleArt>,
    onChooseFolder: () -> Unit,
    onRescan: () -> Unit,
    onCancelScan: () -> Unit,
    onSelectModule: (Int) -> Unit,
    onShowMissingFiles: (ModuleChoice, List<String>) -> Unit,
    modifier: Modifier = Modifier,
    topContentPadding: Dp = 0.dp,
    sharedTransitionScope: SharedTransitionScope? = null,
    animatedVisibilityScope: AnimatedVisibilityScope? = null
) {
    val colors = LocalWseColors.current
    val hasFolder = installFolder.treeUri != null ||
        !installFolder.displayName.isNullOrBlank()
    val folderLabel = installFolder.displayName?.takeIf { it.isNotBlank() }

    LazyVerticalGrid(
        columns = GridCells.Adaptive(180.dp),
        modifier = modifier.fillMaxSize(),
        contentPadding = PaddingValues(
            start = 16.dp,
            end = 16.dp,
            top = topContentPadding + 16.dp,
            bottom = 20.dp
        ),
        horizontalArrangement = Arrangement.spacedBy(12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        item(key = "modules-header", span = { GridItemSpan(maxLineSpan) }) {
            Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(
                    text = "Modules",
                    style = MaterialTheme.typography.headlineMedium,
                    color = colors.onSurface
                )

                Surface(
                    shape = WseShapes.large,
                    color = colors.surfaceContainerLow,
                    border = BorderStroke(1.dp, colors.hairline),
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Column(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(16.dp),
                        verticalArrangement = Arrangement.spacedBy(10.dp)
                    ) {
                        WseSectionHeader(title = "Warband folder")
                        Text(
                            text = folderLabel ?: "No Warband folder selected",
                            style = MaterialTheme.typography.bodyLarge,
                            color = colors.onSurface,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.spacedBy(8.dp)
                        ) {
                            WseSecondaryButton(
                                text = "Choose folder",
                                onClick = onChooseFolder,
                                enabled = !scanState.inProgress,
                                modifier = Modifier.weight(1f)
                            )
                            WseSecondaryButton(
                                text = "Rescan",
                                onClick = onRescan,
                                enabled = hasFolder && !scanState.inProgress,
                                modifier = Modifier.weight(1f)
                            )
                        }

                        if (scanState.inProgress) {
                            val label = scanState.folderLabel?.takeIf { it.isNotBlank() } ?: "folder"
                            LinearProgressIndicator(
                                color = colors.accent,
                                trackColor = colors.surfaceContainerHigh,
                                modifier = Modifier.fillMaxWidth()
                            )
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                verticalAlignment = Alignment.CenterVertically
                            ) {
                                CircularProgressIndicator(
                                    color = colors.accent,
                                    trackColor = colors.surfaceContainerHigh,
                                    modifier = Modifier.size(18.dp),
                                    strokeWidth = 2.dp
                                )
                                Spacer(Modifier.width(10.dp))
                                Text(
                                    text = "Scanning $label…",
                                    style = MaterialTheme.typography.bodyMedium,
                                    color = colors.onSurface,
                                    modifier = Modifier.weight(1f)
                                )
                            }
                            WseSecondaryButton(
                                text = "Cancel",
                                onClick = onCancelScan,
                                modifier = Modifier.fillMaxWidth()
                            )
                        }
                    }
                }
            }
        }

        if (scanState.inProgress) {
            item(key = "modules-scanning", span = { GridItemSpan(maxLineSpan) }) {
                val label = scanState.folderLabel?.takeIf { it.isNotBlank() } ?: "folder"
                Surface(
                    shape = WseShapes.large,
                    color = colors.surfaceContainerLow,
                    border = BorderStroke(1.dp, colors.hairline),
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Text(
                        text = "Scanning $label for Modules/*/module.ini…",
                        style = MaterialTheme.typography.bodyMedium,
                        color = colors.onSurfaceVariant,
                        modifier = Modifier.padding(16.dp)
                    )
                }
            }
        } else if (state.modules.isEmpty()) {
            item(key = "modules-empty", span = { GridItemSpan(maxLineSpan) }) {
                Surface(
                    shape = WseShapes.large,
                    color = colors.surfaceContainerLow,
                    border = BorderStroke(1.dp, colors.hairline),
                    modifier = Modifier.fillMaxWidth()
                ) {
                    Column(
                        modifier = Modifier
                            .fillMaxWidth()
                            .padding(20.dp),
                        verticalArrangement = Arrangement.spacedBy(12.dp)
                    ) {
                        Text(
                            text = if (hasFolder) "No modules found" else "Choose your Warband folder",
                            style = MaterialTheme.typography.headlineSmall,
                            color = colors.onSurface
                        )
                        Text(
                            text = state.emptyMessage,
                            style = MaterialTheme.typography.bodyMedium,
                            color = colors.onSurfaceVariant
                        )
                        WsePrimaryButton(
                            text = "Choose folder",
                            onClick = onChooseFolder
                        )
                    }
                }
            }
        } else {
            itemsIndexed(
                items = state.modules,
                key = { index, mod -> "${mod.modulePath}|$index" }
            ) { index, mod ->
                val isSelected = index == state.selectedIndex
                val status = state.installStatuses[mod.modulePath]
                val art = moduleArt[mod.modulePath]
                val sharedArtModifier = if (
                    sharedTransitionScope != null && animatedVisibilityScope != null
                ) {
                    with(sharedTransitionScope) {
                        Modifier.sharedElement(
                            rememberSharedContentState(key = "module-art-${mod.modulePath}"),
                            animatedVisibilityScope
                        )
                    }
                } else {
                    Modifier
                }

                ModuleGridCard(
                    module = mod,
                    art = art,
                    status = status,
                    selected = isSelected,
                    artModifier = sharedArtModifier,
                    onClick = { onSelectModule(index) },
                    onShowMissingFiles = {
                        if (status?.state == InstallCheckState.MISSING) {
                            onShowMissingFiles(mod, status.missingFiles)
                        }
                    }
                )
            }

            val selectedMod = state.selectedModule
            if (selectedMod != null) {
                item(key = "modules-details", span = { GridItemSpan(maxLineSpan) }) {
                    val scriptsLabel = if (selectedMod.hasScriptsTxt) {
                        "scripts.txt present"
                    } else {
                        "scripts.txt missing"
                    }
                    val installLabel = when (val st = state.installStatuses[selectedMod.modulePath]) {
                        null -> "Checking…"
                        else -> when (st.state) {
                            InstallCheckState.CHECKING -> "Checking…"
                            InstallCheckState.READY -> "Ready"
                            InstallCheckState.MISSING -> if (st.missingFiles.isNotEmpty()) {
                                "${st.missingFiles.size} file(s) missing"
                            } else {
                                "Install check failed"
                            }
                        }
                    }
                    Surface(
                        shape = WseShapes.large,
                        color = colors.surfaceContainerLow,
                        border = BorderStroke(1.dp, colors.hairline),
                        modifier = Modifier.fillMaxWidth()
                    ) {
                        Column(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(16.dp),
                            verticalArrangement = Arrangement.spacedBy(8.dp)
                        ) {
                            WseSectionHeader(title = "Selected module details")
                            Text(
                                text = "${selectedMod.modulePath} · $scriptsLabel · BRFs: " +
                                    "${selectedMod.loadResourceCount} common + " +
                                    "${selectedMod.loadModResourceCount} mod · $installLabel",
                                style = MaterialTheme.typography.bodySmall,
                                color = colors.onSurfaceVariant
                            )
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun ModuleGridCard(
    module: ModuleChoice,
    art: ModuleArt?,
    status: ModuleInstallStatus?,
    selected: Boolean,
    artModifier: Modifier,
    onClick: () -> Unit,
    onShowMissingFiles: () -> Unit
) {
    val displayTitle = module.displayName.ifBlank { module.folderName }
    val chipLabel: String
    val chipTone: WseStatusTone
    val isMissing = status?.state == InstallCheckState.MISSING
    when (status?.state) {
        InstallCheckState.READY -> {
            chipLabel = "Ready"
            chipTone = WseStatusTone.Ready
        }
        InstallCheckState.MISSING -> {
            val count = status.missingFiles.size
            chipLabel = when {
                count == 1 -> "1 file missing"
                count > 1 -> "$count files missing"
                else -> "Files missing"
            }
            chipTone = WseStatusTone.Error
        }
        null, InstallCheckState.CHECKING -> {
            chipLabel = "Checking…"
            chipTone = WseStatusTone.Neutral
        }
    }

    WseBannerCard(
        art = art?.bitmap,
        fallbackSeed = displayTitle,
        title = displayTitle,
        subtitle = module.folderName,
        chip = {
            WseStatusChip(
                text = chipLabel,
                tone = chipTone,
                enabled = true,
                onClick = if (isMissing) onShowMissingFiles else null
            )
        },
        selected = selected,
        onClick = onClick,
        artModifier = artModifier
    )
}

private fun previewModulesState(): ModulesUiState {
    val modules = listOf(
        ModuleChoice(
            folderName = "Native",
            modulePath = "Modules/Native",
            displayName = "Native",
            hasScriptsTxt = true,
            loadResourceCount = 221,
            loadModResourceCount = 0
        ),
        ModuleChoice(
            folderName = "Prophesy_of_Pendor",
            modulePath = "Modules/Prophesy_of_Pendor",
            displayName = "Prophesy of Pendor",
            hasScriptsTxt = true,
            loadResourceCount = 221,
            loadModResourceCount = 64
        ),
        ModuleChoice(
            folderName = "Broken_Mod",
            modulePath = "Modules/Broken_Mod",
            displayName = "Broken Mod",
            hasScriptsTxt = false,
            loadResourceCount = 221,
            loadModResourceCount = 12
        )
    )
    return ModulesUiState(
        modules = modules,
        selectedIndex = 0,
        installStatuses = mapOf(
            "Modules/Native" to ModuleInstallStatus(InstallCheckState.READY),
            "Modules/Prophesy_of_Pendor" to ModuleInstallStatus(InstallCheckState.CHECKING),
            "Modules/Broken_Mod" to ModuleInstallStatus(
                state = InstallCheckState.MISSING,
                missingFiles = listOf("Modules/Broken_Mod/scripts.txt", "CommonRes/")
            )
        )
    )
}

@Preview(name = "Modules Dark", showBackground = true, widthDp = 412, heightDp = 844)
@Composable
private fun PreviewModulesDark() {
    Wse2Theme(darkTheme = true, dynamicColor = false) {
        Box(modifier = Modifier.wseBackdrop(LocalWseColors.current.accent)) {
            ModulesScreen(
                state = previewModulesState(),
                installFolder = InstallFolderState(displayName = "Mount & Blade - Warband"),
                scanState = ScanState(),
                moduleArt = emptyMap(),
                onChooseFolder = {},
                onRescan = {},
                onCancelScan = {},
                onSelectModule = {},
                onShowMissingFiles = { _, _ -> }
            )
        }
    }
}

@Preview(name = "Modules Light", showBackground = true, widthDp = 412, heightDp = 844)
@Composable
private fun PreviewModulesLight() {
    Wse2Theme(darkTheme = false, dynamicColor = false) {
        Box(modifier = Modifier.wseBackdrop(LocalWseColors.current.accent)) {
            ModulesScreen(
                state = previewModulesState(),
                installFolder = InstallFolderState(displayName = "Mount & Blade - Warband"),
                scanState = ScanState(),
                moduleArt = emptyMap(),
                onChooseFolder = {},
                onRescan = {},
                onCancelScan = {},
                onSelectModule = {},
                onShowMissingFiles = { _, _ -> }
            )
        }
    }
}

@Preview(name = "Modules Empty Dark", showBackground = true, widthDp = 412, heightDp = 640)
@Composable
private fun PreviewModulesEmptyDark() {
    Wse2Theme(darkTheme = true, dynamicColor = false) {
        Box(modifier = Modifier.wseBackdrop(LocalWseColors.current.accent)) {
            ModulesScreen(
                state = ModulesUiState(),
                installFolder = InstallFolderState(),
                scanState = ScanState(),
                moduleArt = emptyMap(),
                onChooseFolder = {},
                onRescan = {},
                onCancelScan = {},
                onSelectModule = {},
                onShowMissingFiles = { _, _ -> }
            )
        }
    }
}
