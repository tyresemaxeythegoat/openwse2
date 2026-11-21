package org.wse2.android

import android.Manifest
import android.annotation.SuppressLint
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import android.view.Display
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.getValue
import androidx.lifecycle.ViewModelProvider
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import org.wse2.android.launcher.LauncherApp
import org.wse2.android.launcher.LauncherEvent
import org.wse2.android.launcher.LauncherTab
import org.wse2.android.launcher.LauncherViewModel
import org.wse2.android.launcher.R4StartupRunner
import org.wse2.android.launcher.theme.Wse2Theme

class WseLauncherActivity : ComponentActivity() {

    private val viewModel: LauncherViewModel by lazy {
        ViewModelProvider(this)[LauncherViewModel::class.java]
    }

    private val pickTreeLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode != RESULT_OK) {
            return@registerForActivityResult
        }
        val data = result.data ?: return@registerForActivityResult
        val uri = data.data ?: return@registerForActivityResult
        val grantFlags = data.flags and
            (Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
        try {
            contentResolver.takePersistableUriPermission(
                uri,
                Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION
            )
        } catch (_: SecurityException) {
            if ((grantFlags and Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0) {
                try {
                    contentResolver.takePersistableUriPermission(
                        uri,
                        Intent.FLAG_GRANT_READ_URI_PERMISSION
                    )
                } catch (_: SecurityException) {
                    // Continue with temporary grant if provider disallows persistence.
                }
            }
        }
        viewModel.onFolderPicked(uri)
    }

    private val pickDriverLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { result ->
        if (result.resultCode != RESULT_OK) {
            return@registerForActivityResult
        }
        val driverUri = result.data?.data ?: return@registerForActivityResult
        viewModel.importDriverPackage(driverUri)
    }

    // Android 13+: the R4 run's progress and result notifications need this; the run starts
    // whatever the answer (denied, the run still goes on in the background, silently).
    private val notificationPermission = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) {
        viewModel.runR4Startup()
        askToSkipBatteryOptimization()
    }

    private fun runR4Startup() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            notificationPermission.launch(Manifest.permission.POST_NOTIFICATIONS)
        } else {
            viewModel.runR4Startup()
            askToSkipBatteryOptimization()
        }
    }

    // Battery optimization (and vendor "app freezers" that follow it) can still stop the run in the
    // background; Android's own dialog lets the user exempt the app. Asked only while not exempt.
    @SuppressLint("BatteryLife")
    private fun askToSkipBatteryOptimization() {
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        if (pm.isIgnoringBatteryOptimizations(packageName)) return
        try {
            startActivity(
                Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName"))
            )
        } catch (_: Throwable) {
            // No such screen on this device: the run goes on without the exemption.
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        showR4ReportIfAsked(intent)
    }

    private fun showR4ReportIfAsked(intent: Intent?) {
        if (intent?.getBooleanExtra(EXTRA_SHOW_R4_REPORT, false) != true) return
        intent.removeExtra(EXTRA_SHOW_R4_REPORT)
        viewModel.showR4Report()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.dark(Color.TRANSPARENT),
            navigationBarStyle = SystemBarStyle.dark(Color.TRANSPARENT)
        )
        super.onCreate(savedInstanceState)

        if (savedInstanceState != null) {
            val savedTab = savedInstanceState
                .getInt(STATE_SELECTED_TAB, 0)
                .coerceIn(0, LauncherTab.entries.lastIndex)
            viewModel.selectTab(LauncherTab.entries[savedTab])
        }
        viewModel.updateMaxDisplayRefreshHz(maxDisplayRefreshHz())
        if (savedInstanceState == null) showR4ReportIfAsked(intent)
        // A run the system ended with the whole app goes on by itself.
        R4StartupRunner.get(application).resumeIfInterrupted()

        setContent {
            val seedColor by viewModel.seedColorArgb.collectAsStateWithLifecycle()
            Wse2Theme(seedColorArgb = seedColor) {
                LauncherApp(
                    viewModel = viewModel,
                    onChooseFolder = { openWarbandFolderPicker() },
                    onImportDriver = { openDriverPackagePicker() },
                    onLaunchGame = { event -> launchWseActivity(event) },
                    onRunR4Startup = { runR4Startup() }
                )
            }
        }
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putInt(STATE_SELECTED_TAB, viewModel.selectedTab.value.ordinal)
    }

    private fun openWarbandFolderPicker() {
        if (viewModel.scanState.value.inProgress) {
            return
        }
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT_TREE).apply {
            addFlags(
                Intent.FLAG_GRANT_READ_URI_PERMISSION or
                    Intent.FLAG_GRANT_WRITE_URI_PERMISSION or
                    Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION or
                    Intent.FLAG_GRANT_PREFIX_URI_PERMISSION
            )
        }
        pickTreeLauncher.launch(intent)
    }

    private fun openDriverPackagePicker() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(
                Intent.EXTRA_MIME_TYPES,
                arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream")
            )
        }
        pickDriverLauncher.launch(intent)
    }

    private fun launchWseActivity(event: LauncherEvent.LaunchGame) {
        val intent = Intent(this, WseActivity::class.java).apply {
            putExtra(EXTRA_RUN_MODULE, event.runModule)
            if (event.runModule) {
                putExtra(WseActivity.EXTRA_SHOW_FPS, event.showFpsOverlay)
            }
            putExtra(EXTRA_TREE_URI, event.treeUri)
            putExtra(EXTRA_MODULE_NAME, event.moduleName)
            // Stable mode id ("system" / "turnip" / "panvk"), as parse_driver_mode expects.
            putExtra(EXTRA_GPU_DRIVER, event.gpuDriver)
        }
        startActivity(intent)
    }

    // Highest refresh rate among the display's modes (60 when unknown).
    @Suppress("DEPRECATION")
    private fun maxDisplayRefreshHz(): Int {
        val d: Display? = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            display
        } else {
            windowManager.defaultDisplay
        }
        val max = d?.supportedModes?.maxOfOrNull { it.refreshRate } ?: return 60
        return Math.round(max)
    }

    companion object {
        private const val STATE_SELECTED_TAB = "wse2.state.SELECTED_TAB"

        const val EXTRA_TREE_URI = "wse2.extra.TREE_URI"
        const val EXTRA_MODULE_NAME = "wse2.extra.MODULE_NAME"
        const val EXTRA_GPU_DRIVER = "wse2.extra.GPU_DRIVER"
        const val EXTRA_RUN_MODULE = "wse2.extra.RUN_MODULE"
        // Set by R4ForegroundService's result notification.
        const val EXTRA_SHOW_R4_REPORT = "wse2.extra.SHOW_R4_REPORT"
    }
}
