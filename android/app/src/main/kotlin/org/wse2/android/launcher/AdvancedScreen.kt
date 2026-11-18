package org.wse2.android.launcher

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.defaultMinSize
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.wse2.android.launcher.theme.Wse2Theme

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
    val treeUri = state.installFolder.treeUri
    val selectedModule = state.selectedModule
    val verifyEnabled = selectedModule?.hasScriptsTxt == true &&
        treeUri != null &&
        !state.scanState.inProgress

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
                color = MaterialTheme.colorScheme.onBackground
            )

            LauncherSectionHeader(title = "Warband Folder (SAF)")
            LauncherGroupedCard {
                ListItem(
                    headlineContent = { Text("Tree URI") },
                    supportingContent = {
                        Text(
                            text = treeUri?.toString() ?: "No Warband folder selected.",
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    },
                    colors = ListItemDefaults.colors(containerColor = Color.Transparent)
                )
                LauncherSectionDivider()

                ListItem(
                    headlineContent = { Text("SAF tree index") },
                    supportingContent = {
                        Text(
                            text = state.scanState.indexStatsText,
                            color = MaterialTheme.colorScheme.onSurfaceVariant
                        )
                    },
                    colors = ListItemDefaults.colors(containerColor = Color.Transparent)
                )
                LauncherSectionDivider()

                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp, vertical = 12.dp)
                ) {
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
                    OutlinedButton(
                        onClick = onImportDriver,
                        modifier = Modifier
                            .fillMaxWidth()
                            .defaultMinSize(minHeight = 48.dp)
                    ) {
                        Text("Import Turnip / PanVK Driver Package (.zip)")
                    }
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
                    OutlinedButton(
                        onClick = onProbeVulkan,
                        enabled = !state.driverState.isProbeInProgress,
                        modifier = Modifier
                            .fillMaxWidth()
                            .defaultMinSize(minHeight = 48.dp)
                    ) {
                        if (state.driverState.isProbeInProgress) {
                            CircularProgressIndicator(
                                modifier = Modifier.size(18.dp),
                                strokeWidth = 2.dp
                            )
                            Spacer(Modifier.width(8.dp))
                        }
                        Text("Probe Vulkan Driver (CPU reference frame)")
                    }
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
                    OutlinedButton(
                        onClick = onLaunchSelfTest,
                        modifier = Modifier
                            .fillMaxWidth()
                            .defaultMinSize(minHeight = 48.dp)
                    ) {
                        Text("Launch WSE2 SDL3 Self-Test (WseActivity)")
                    }
                }
            }

            LauncherSectionHeader(title = "Status Log")
            LauncherGroupedCard {
                Text(
                    text = state.statusLog,
                    style = MaterialTheme.typography.bodySmall.copy(
                        fontFamily = FontFamily.Monospace
                    ),
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(16.dp)
                )
            }
        }
    }
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
        Surface {
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
        Surface {
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
