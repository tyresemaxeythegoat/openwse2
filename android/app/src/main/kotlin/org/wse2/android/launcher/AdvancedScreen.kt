package org.wse2.android.launcher

import android.content.Context
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.wse2.android.R
import org.wse2.android.launcher.theme.LocalWseColors
import org.wse2.android.launcher.theme.Wse2Theme
import org.wse2.android.launcher.theme.wseBackdrop
import org.wse2.android.launcher.ui.WseDialog
import org.wse2.android.launcher.ui.WseDialogAction
import org.wse2.android.launcher.ui.WseListRow
import org.wse2.android.launcher.ui.WseSecondaryButton

@Composable
fun AdvancedScreen(
    state: AdvancedUiState,
    onVerifyScripts: () -> Unit,
    onSelectDriver: (Int) -> Unit,
    onImportDriver: () -> Unit,
    onAllowMockFallbackChange: (Boolean) -> Unit,
    onProbeVulkan: () -> Unit,
    onShowFpsOverlayChange: (Boolean) -> Unit,
    onLaunchSelfTest: () -> Unit,
    modifier: Modifier = Modifier,
    topContentPadding: Dp = 0.dp
) {
    val colors = LocalWseColors.current
    val treeUri = state.installFolder.treeUri
    val selectedModule = state.selectedModule
    val verifyEnabled = selectedModule?.hasScriptsTxt == true &&
        treeUri != null &&
        !state.scanState.inProgress
    var showLicencesDialog by remember { mutableStateOf(false) }

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
                text = "Advanced",
                style = MaterialTheme.typography.headlineMedium,
                color = colors.onSurface
            )

            LauncherSectionHeader(title = "Warband Folder (SAF)")
            LauncherGroupedCard {
                WseListRow(
                    title = "Tree URI",
                    supporting = treeUri?.toString() ?: "No Warband folder selected."
                )
                WseListRow(
                    title = "SAF tree index",
                    supporting = state.scanState.indexStatsText
                )

                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 12.dp)
                ) {
                    WseSecondaryButton(
                        text = "Verify scripts.txt via SafVfs",
                        onClick = onVerifyScripts,
                        enabled = verifyEnabled,
                        modifier = Modifier.fillMaxWidth()
                    )
                }
            }

            LauncherSectionHeader(title = "GPU Driver")
            LauncherGroupedCard {
                ExposedDropdownListItem(
                    headline = "Vulkan driver",
                    options = state.driverState.options,
                    selectedIndex = state.driverState.selectedIndex,
                    enabled = true,
                    onSelected = onSelectDriver,
                    supporting = state.driverState.statusText
                )
                LauncherSectionDivider()

                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 12.dp)
                ) {
                    WseSecondaryButton(
                        text = "Import Turnip / PanVK Driver Package (.zip)",
                        onClick = onImportDriver,
                        modifier = Modifier.fillMaxWidth()
                    )
                }
                LauncherSectionDivider()

                SwitchListItem(
                    headline = "Allow mock ICD fallback",
                    supporting = "Testing only",
                    checked = state.driverState.allowMockFallback,
                    enabled = true,
                    onCheckedChange = onAllowMockFallbackChange
                )
                LauncherSectionDivider()

                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 12.dp)
                ) {
                    WseSecondaryButton(
                        text = "Probe Vulkan Driver (CPU reference frame)",
                        onClick = onProbeVulkan,
                        enabled = !state.driverState.isProbeInProgress,
                        leadingIcon = if (state.driverState.isProbeInProgress) {
                            {
                                CircularProgressIndicator(
                                    color = colors.accent,
                                    modifier = Modifier.size(16.dp),
                                    strokeWidth = 2.dp
                                )
                            }
                        } else {
                            null
                        },
                        modifier = Modifier.fillMaxWidth()
                    )
                }
            }

            LauncherSectionHeader(title = "Runtime")
            LauncherGroupedCard {
                SwitchListItem(
                    headline = "Show FPS overlay",
                    checked = state.showFpsOverlay,
                    enabled = true,
                    onCheckedChange = onShowFpsOverlayChange
                )
                LauncherSectionDivider()

                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 12.dp)
                ) {
                    WseSecondaryButton(
                        text = "Launch WSE2 SDL3 Self-Test (WseActivity)",
                        onClick = onLaunchSelfTest,
                        modifier = Modifier.fillMaxWidth()
                    )
                }
            }

            LauncherSectionHeader(title = "Status Log")
            LauncherGroupedCard {
                Text(
                    text = state.statusLog,
                    style = MaterialTheme.typography.bodySmall.copy(
                        fontFamily = FontFamily.Monospace
                    ),
                    color = colors.onSurfaceVariant,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(16.dp)
                )
            }

            LauncherSectionHeader(title = "About")
            LauncherGroupedCard {
                WseListRow(
                    title = "Licences",
                    supporting = "SIL Open Font License 1.1 (Cinzel, Manrope)",
                    showDivider = false,
                    onClick = { showLicencesDialog = true }
                )
            }
        }
    }

    if (showLicencesDialog) {
        val licenceText = rememberFontLicencesText()
        WseDialog(
            title = "Licences",
            text = licenceText,
            confirm = WseDialogAction("Close") { showLicencesDialog = false },
            onDismissRequest = { showLicencesDialog = false }
        )
    }
}

@Composable
private fun rememberFontLicencesText(): String {
    val context = LocalContext.current
    return remember(context) {
        val cinzel = readRawLicence(context, R.raw.ofl_cinzel)
        val manrope = readRawLicence(context, R.raw.ofl_manrope)
        buildString {
            append("CINZEL\n\n")
            append(cinzel)
            append("\n\n-----------------------------------------------------------\n\n")
            append("MANROPE\n\n")
            append(manrope)
        }
    }
}

private fun readRawLicence(context: Context, resId: Int): String = try {
    context.resources.openRawResource(resId).bufferedReader(Charsets.UTF_8).use {
        it.readText().trim()
    }
} catch (_: Throwable) {
    "SIL Open Font License 1.1"
}

private fun previewAdvancedState(): AdvancedUiState = AdvancedUiState(
    installFolder = InstallFolderState(displayName = "Mount & Blade - Warband"),
    scanState = ScanState(indexStatsText = "SafVfs index: 4210 files, 184 dirs in 380 ms"),
    selectedModule = ModuleChoice(
        folderName = "Native",
        modulePath = "Modules/Native",
        displayName = "Native",
        hasScriptsTxt = true,
        loadResourceCount = 221,
        loadModResourceCount = 0
    ),
    driverState = DriverUiState(
        selectedIndex = 0,
        statusText = "Uses the device's built-in Vulkan driver.",
        allowMockFallback = false,
        isProbeInProgress = false
    ),
    showFpsOverlay = true,
    statusLog = "OK: 1755 scripts, 131080 statements, 65/65 mapped (cached index, parse 42 ms)"
)

@Preview(name = "Advanced Dark", showBackground = true, widthDp = 412, heightDp = 1120)
@Composable
private fun PreviewAdvancedDark() {
    Wse2Theme(darkTheme = true, dynamicColor = false) {
        Box(modifier = Modifier.wseBackdrop(LocalWseColors.current.accent)) {
            AdvancedScreen(
                state = previewAdvancedState(),
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
}

@Preview(name = "Advanced Light", showBackground = true, widthDp = 412, heightDp = 1120)
@Composable
private fun PreviewAdvancedLight() {
    Wse2Theme(darkTheme = false, dynamicColor = false) {
        Box(modifier = Modifier.wseBackdrop(LocalWseColors.current.accent)) {
            AdvancedScreen(
                state = previewAdvancedState(),
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
}
