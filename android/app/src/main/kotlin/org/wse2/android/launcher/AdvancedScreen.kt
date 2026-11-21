package org.wse2.android.launcher

import android.app.Application
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.widget.Toast
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import org.wse2.android.launcher.ui.WseScreenHeader
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
    topContentPadding: Dp = 0.dp,
    r0State: R0UiState = R0UiState(),
    onRunR0Spikes: () -> Unit = {},
    onDismissR0Report: () -> Unit = {},
    r4State: R4UiState = R4UiState(),
    onRunR4Startup: () -> Unit = {},
    onR4UseCopyChange: (Boolean) -> Unit = {},
    onDismissR4Report: () -> Unit = {}
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
            WseScreenHeader(
                eyebrow = "Diagnostics & Tools",
                title = "Advanced",
                subtitle = "SAF storage inspection, custom Vulkan drivers, self-tests and Phase 5 tools."
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
                        text = "Import Driver Package (.zip)",
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
                        text = "Probe Vulkan Driver",
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
                        text = "Launch SDL3 Self-Test",
                        onClick = onLaunchSelfTest,
                        modifier = Modifier.fillMaxWidth()
                    )
                }
            }

            LauncherSectionHeader(title = "Running the game (experimental)")
            LauncherGroupedCard {
                WseListRow(
                    title = "Phone check",
                    supporting = if (r0State.running) {
                        "Checking. This can take a few minutes."
                    } else {
                        "Checks that this phone can run game code translated from your " +
                            "mb_warband_wse2_x64.exe. Takes a few minutes."
                    },
                    trailing = if (r0State.running) {
                        {
                            CircularProgressIndicator(
                                color = colors.accent,
                                modifier = Modifier.size(20.dp),
                                strokeWidth = 2.dp
                            )
                        }
                    } else {
                        null
                    },
                    showDivider = true,
                    enabled = !r0State.running && !r4State.running,
                    onClick = onRunR0Spikes
                )
                WseListRow(
                    title = "Game test",
                    supporting = if (r4State.running) {
                        r4State.status.ifEmpty { "Working…" } +
                            "\nYou can leave the app; it keeps working in the background. To stop it, " +
                            "use the notification."
                    } else {
                        "Prepares the game for your phone (needed only once; it takes a few hours " +
                            "and progress is saved), then starts Native briefly to check that it " +
                            "works. Nothing is shown on screen yet."
                    },
                    trailing = if (r4State.running) {
                        {
                            CircularProgressIndicator(
                                color = colors.accent,
                                modifier = Modifier.size(20.dp),
                                strokeWidth = 2.dp
                            )
                        }
                    } else {
                        null
                    },
                    showDivider = true,
                    enabled = !r4State.running && !r0State.running,
                    onClick = onRunR4Startup
                )
                // Saves the report as a file the player picks, during a run too (the user's
                // request, 2026-11-21: keep the phone's report when finishing on the PC).
                val saveContext = LocalContext.current
                val saveReport = rememberLauncherForActivityResult(
                    ActivityResultContracts.CreateDocument("text/plain")
                ) { uri ->
                    if (uri != null) {
                        val runner = R4StartupRunner.get(saveContext.applicationContext as Application)
                        val text = runner.reportToSave()
                        val saved = text != null && runner.saveReport(uri, text)
                        Toast.makeText(
                            saveContext,
                            if (saved) "Report saved." else "There is no report to save yet.",
                            Toast.LENGTH_SHORT
                        ).show()
                    }
                }
                WseListRow(
                    title = "Save the game test report",
                    supporting = "Saves what the game test has written so far as a text file, even " +
                        "while it is running.",
                    showDivider = true,
                    onClick = { saveReport.launch("wse2-game-test-report.txt") }
                )
                SwitchListItem(
                    headline = "Work from a copy of the game folder",
                    supporting = "Copies the game folder (without other modules) into the app " +
                        "first. Needs that much free space.",
                    checked = r4State.useCopy,
                    enabled = !r4State.running,
                    onCheckedChange = onR4UseCopyChange
                )
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

    r0State.report?.let { report ->
        val context = LocalContext.current
        WseDialog(
            title = "Phone check",
            text = report,
            confirm = WseDialogAction("Copy") {
                val clipboard =
                    context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
                clipboard?.setPrimaryClip(ClipData.newPlainText("WSE2 phone check", report))
                onDismissR0Report()
            },
            dismiss = WseDialogAction("Share") {
                val send = Intent(Intent.ACTION_SEND).apply {
                    type = "text/plain"
                    putExtra(Intent.EXTRA_SUBJECT, "WSE2 phone check")
                    putExtra(Intent.EXTRA_TEXT, report)
                }
                context.startActivity(Intent.createChooser(send, "Share the phone check report"))
                onDismissR0Report()
            },
            onDismissRequest = onDismissR0Report
        )
    }

    r4State.report?.let { report ->
        val context = LocalContext.current
        WseDialog(
            title = "Game test",
            text = report,
            confirm = WseDialogAction("Copy") {
                val clipboard =
                    context.getSystemService(Context.CLIPBOARD_SERVICE) as? ClipboardManager
                clipboard?.setPrimaryClip(ClipData.newPlainText("WSE2 game test", report))
                onDismissR4Report()
            },
            dismiss = WseDialogAction("Share") {
                val send = Intent(Intent.ACTION_SEND).apply {
                    type = "text/plain"
                    putExtra(Intent.EXTRA_SUBJECT, "WSE2 game test")
                    putExtra(Intent.EXTRA_TEXT, report)
                }
                context.startActivity(Intent.createChooser(send, "Share the game test report"))
                onDismissR4Report()
            },
            onDismissRequest = onDismissR4Report
        )
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
