package org.wse2.android

import android.app.Activity
import android.app.AlertDialog
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.hardware.Sensor
import android.hardware.SensorManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.os.RemoteException
import android.provider.DocumentsContract
import android.view.Display
import android.view.View
import android.widget.Toast
import org.json.JSONObject
import org.wse2.android.launcher.AdvancedPage
import org.wse2.android.launcher.HomePage
import org.wse2.android.launcher.InstallCheckState
import org.wse2.android.launcher.LauncherTab
import org.wse2.android.launcher.LauncherUi
import org.wse2.android.launcher.ModuleChoice
import org.wse2.android.launcher.ModuleInstallStatus
import org.wse2.android.launcher.ModulesPage
import org.wse2.android.launcher.PerformanceUiState
import org.wse2.android.launcher.SettingsPage
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicLong
import java.util.zip.ZipInputStream

class WseLauncherActivity : Activity() {

    private data class ScanSnapshot(
        val treeUri: Uri?,
        val folderDisplayName: String?,
        val modules: List<ModuleChoice>,
        val selectedModuleIndex: Int,
        val installStates: Map<String, ModuleInstallStatus>,
        val languages: List<String>,
        val indexStatsText: String
    )

    private lateinit var homePage: HomePage
    private lateinit var modulesPage: ModulesPage
    private lateinit var settingsPage: SettingsPage
    private lateinit var advancedPage: AdvancedPage
    private lateinit var pageViews: List<View>
    private lateinit var tabBar: LauncherUi.TabBar

    private var selectedTabIndex = 0
    private val modules = ArrayList<ModuleChoice>()
    private var selectedModuleIndex = -1
    private val moduleInstallStates = HashMap<String, ModuleInstallStatus>()
    private var scannedLanguagesList: List<String> = listOf("en")
    private val driverOptions = listOf("System Vulkan", "Turnip (Adreno custom)", "PanVK (Mali direct ICD)")
    // Native `parse_driver_mode` keys, index-aligned with driverOptions.
    private val driverModeKeys = listOf("system", "turnip", "panvk")
    // Caps offered on this device: `allFrameRateValues` filtered to what the display can
    // refresh at (a 120 Hz cap on a 60 Hz panel only wastes power), index-aligned labels.
    private val allFrameRateValues = listOf(30, 60, 90, 120, 0)
    private var frameRateValues = allFrameRateValues
    private var frameRateLabels = allFrameRateValues.map { frameRateLabel(it) }
    private val executor: ExecutorService = Executors.newSingleThreadExecutor()
    private val settingsExecutor: ExecutorService = Executors.newSingleThreadExecutor()
    private val mainHandler = Handler(Looper.getMainLooper())
    private var probeConnection: ServiceConnection? = null
    private var settingsOpSeq = 0L
    private var settingsNeedsRefresh = false

    private var selectedTreeUri: Uri? = null
    private var selectedFolderDisplayName: String? = null
    private val scanGeneration = AtomicLong(0L)
    private var isScanInProgress = false
    private var preScanSnapshot: ScanSnapshot? = null
    private var lastIndexStatsText = "SafVfs index: idle"
    private var activeDialog: AlertDialog? = null

    /** Returns the language codes found by the last folder scan; read by the Settings page. */
    fun scannedLanguages(): List<String> = scannedLanguagesList

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        selectedTabIndex = savedInstanceState
            ?.getInt(STATE_SELECTED_TAB, 0)
            ?.coerceIn(0, LauncherTab.entries.lastIndex)
            ?: 0

        val maxRefreshHz = maxDisplayRefreshHz()
        frameRateValues = allFrameRateValues.filter { it == 0 || it <= maxRefreshHz + 1 }
        frameRateLabels = frameRateValues.map { frameRateLabel(it) }

        val sensorManager = getSystemService(Context.SENSOR_SERVICE) as? SensorManager
        val hasPhoneGyro = sensorManager?.getDefaultSensor(Sensor.TYPE_GYROSCOPE) != null

        homePage = HomePage(
            context = this,
            onChooseFolder = { openWarbandFolderPicker() },
            onChangeModule = { selectTab(LauncherTab.MODULES.ordinal) },
            onPlay = { launchSelfTestActivity(runModule = true) }
        )
        modulesPage = ModulesPage(
            context = this,
            onChooseFolder = { openWarbandFolderPicker() },
            onRescan = {
                selectedTreeUri?.let { uri ->
                    startScan(
                        uri = uri,
                        initialDisplayName = selectedFolderDisplayName ?: quickTreeDisplayName(uri),
                        isNewFolderPick = false
                    )
                }
            },
            onSelectModule = { position -> selectModule(position) },
            onCancelScan = { cancelScan() },
            onShowMissingFiles = { mod, missing -> showMissingFilesDialog(mod, missing) }
        )
        settingsPage = SettingsPage(
            context = this,
            frameRateValues = frameRateValues,
            frameRateLabels = frameRateLabels,
            presentModeKeys = presentModeKeys,
            presentModeLabels = presentModeLabels,
            hasPhoneGyro = hasPhoneGyro,
            onSaveChoices = { choicesJson, refreshGraphics ->
                savePerformanceSettings(choicesJson, refreshGraphics)
            }
        )
        advancedPage = AdvancedPage(
            context = this,
            driverOptions = driverOptions,
            onVerifyScripts = { verifySelectedModuleScripts() },
            onDriverSelected = { pos -> onDriverChanged(pos) },
            onImportDriver = { openDriverPackagePicker() },
            onProbeVulkan = { probeVulkanDriver() },
            onLaunchSelfTest = { launchSelfTestActivity(runModule = false) }
        )

        val root = LauncherUi.rootLayout(this)
        val contentHost = LauncherUi.pageHost(this)
        pageViews = listOf(homePage.view, modulesPage.view, settingsPage.view, advancedPage.view)
        for (pageView in pageViews) {
            contentHost.addView(pageView)
        }
        tabBar = LauncherUi.tabBar(this, LauncherTab.entries, selectedTabIndex) { idx ->
            selectTab(idx)
        }
        root.addView(contentHost)
        root.addView(tabBar.root)

        setContentView(root)
        selectTab(selectedTabIndex)
        restorePreferences()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        outState.putInt(STATE_SELECTED_TAB, selectedTabIndex)
    }

    private fun selectTab(index: Int) {
        val clamped = index.coerceIn(0, pageViews.lastIndex)
        selectedTabIndex = clamped
        for (i in pageViews.indices) {
            pageViews[i].visibility = if (i == clamped) View.VISIBLE else View.GONE
        }
        tabBar.setSelected(clamped)
    }

    private fun setStatus(message: String) {
        homePage.setLastStatus(message)
        advancedPage.setOutputStatus(message)
    }

    private val configRoot: String
        get() = filesDir.absolutePath

    // config/performance.json is the one saved copy of these settings (native serializer via
    // JNI). Builds that kept them in SharedPreferences are migrated once.
    private fun restorePerformanceSettings() {
        val prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        val jsonFile = File(configRoot, PERFORMANCE_JSON)
        val migratedJson = if (!jsonFile.isFile && prefs.contains(KEY_FRAME_RATE_CAP)) {
            val rawCap = prefs.getInt(KEY_FRAME_RATE_CAP, 60)
            val capKey = when {
                rawCap <= 0 -> "uncapped"
                rawCap <= 30 -> "30"
                rawCap <= 60 -> "60"
                rawCap <= 90 -> "90"
                else -> "120"
            }
            JSONObject()
                .put("frame_rate_cap", capKey)
                .put("present_mode", "fifo")
                .put(
                    "thermal_throttling_enabled",
                    prefs.getBoolean(KEY_THERMAL_THROTTLE, true).toString()
                )
                .put("dynamic_resolution_enabled", "true")
                .put("lod_bias_scaling_enabled", "true")
                .put("gyro_aim_enabled", prefs.getBoolean(KEY_GYRO_AIM, false).toString())
                .put("pinch_zoom_enabled", prefs.getBoolean(KEY_PINCH_ZOOM, true).toString())
                .toString()
        } else {
            null
        }
        prefs.edit()
            .remove(KEY_FRAME_RATE_CAP)
            .remove(KEY_GYRO_AIM)
            .remove(KEY_PINCH_ZOOM)
            .remove(KEY_THERMAL_THROTTLE)
            .apply()

        val seq = ++settingsOpSeq
        settingsNeedsRefresh = false
        val root = configRoot
        settingsExecutor.execute {
            if (migratedJson != null && !File(root, PERFORMANCE_JSON).isFile) {
                WseSafBridge.nativeSaveLauncherSettings(root, migratedJson)
            }
            val loaded = PerformanceUiState.fromJson(
                WseSafBridge.nativeEnsureFirstLaunchSettings(root)
            )
            runOnUiThread {
                if (isFinishing || isDestroyed || seq != settingsOpSeq) {
                    return@runOnUiThread
                }
                settingsPage.applyState(loaded)
            }
        }
    }

    private fun savePerformanceSettings(choicesJson: String, refreshGraphics: Boolean) {
        if (refreshGraphics) {
            settingsNeedsRefresh = true
        }
        val seq = ++settingsOpSeq
        val reloadGraphics = settingsNeedsRefresh
        val root = configRoot
        settingsExecutor.execute {
            val saved = WseSafBridge.nativeSaveLauncherSettings(root, choicesJson)
            val reloaded = if (!saved || reloadGraphics) {
                PerformanceUiState.fromJson(WseSafBridge.nativeEnsureFirstLaunchSettings(root))
            } else {
                null
            }
            runOnUiThread {
                if (isFinishing || isDestroyed) {
                    return@runOnUiThread
                }
                if (!saved) {
                    Toast.makeText(
                        this,
                        "Could not save settings.",
                        Toast.LENGTH_SHORT
                    ).show()
                    if (seq != settingsOpSeq) {
                        restorePerformanceSettings()
                    } else if (reloaded != null) {
                        settingsNeedsRefresh = false
                        settingsPage.applyState(reloaded)
                    }
                } else if (reloaded != null && seq == settingsOpSeq) {
                    settingsNeedsRefresh = false
                    settingsPage.applyGraphicsState(reloaded)
                }
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        scanGeneration.incrementAndGet()
        activeDialog?.dismiss()
        activeDialog = null
        finishProbe(null)
        executor.shutdownNow()
        settingsExecutor.shutdown()
    }

    private fun restorePreferences() {
        val prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        // Stored as the stable mode id; older builds stored the display label.
        val savedDriver = prefs.getString(KEY_GPU_DRIVER, driverModeKeys.first()) ?: ""
        val driverIdx = driverModeKeys.indexOf(savedDriver).takeIf { it >= 0 }
            ?: driverOptions.indexOf(savedDriver).coerceAtLeast(0)
        advancedPage.setDriverSelection(driverIdx)
        prefs.edit().putString(KEY_GPU_DRIVER, driverModeKeys[driverIdx]).apply()
        updateDriverStatus()

        restorePerformanceSettings()

        val savedTree = prefs.getString(KEY_TREE_URI, null)
        if (!savedTree.isNullOrEmpty()) {
            val uri = Uri.parse(savedTree)
            val quickName = quickTreeDisplayName(uri)
            selectedTreeUri = uri
            selectedFolderDisplayName = quickName
            startScan(uri, quickName, isNewFolderPick = false)
        } else {
            selectedTreeUri = null
            selectedFolderDisplayName = null
            homePage.setInstallFolder(null)
            homePage.setSelectedModule(null, hasFolder = false)
            modulesPage.setFolderState(null, hasFolder = false)
            modulesPage.showModules(emptyList(), -1)
            updateHomePlayState()
        }
    }

    private fun onDriverChanged(pos: Int) {
        val idx = pos.coerceIn(0, driverModeKeys.lastIndex)
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
            .edit()
            .putString(KEY_GPU_DRIVER, driverModeKeys[idx])
            .apply()
        updateDriverStatus()
    }

    private fun updateFolderUi(uri: Uri, displayName: String) {
        homePage.setInstallFolder(displayName)
        modulesPage.setFolderState(displayName, hasFolder = true)
        advancedPage.setFolderStatus("Tree URI: $uri")
    }

    private fun quickTreeDisplayName(uri: Uri): String {
        val docId = try {
            DocumentsContract.getTreeDocumentId(uri)
        } catch (_: Exception) {
            ""
        }
        if (docId.isNotEmpty()) {
            val segment = docId.substringAfterLast('/').substringAfterLast(':')
            if (segment.isNotBlank()) {
                return segment
            }
            return docId
        }
        return uri.lastPathSegment?.substringAfterLast(':')?.ifBlank { null } ?: uri.toString()
    }

    private fun resolveTreeDisplayName(uri: Uri): String {
        val docId = try {
            DocumentsContract.getTreeDocumentId(uri)
        } catch (_: Exception) {
            ""
        }
        if (docId.isNotEmpty()) {
            try {
                val docUri = DocumentsContract.buildDocumentUriUsingTree(uri, docId)
                contentResolver.query(
                    docUri,
                    arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME),
                    null,
                    null,
                    null
                )?.use { cursor ->
                    if (cursor.moveToFirst() && !cursor.isNull(0)) {
                        val name = cursor.getString(0)
                        if (!name.isNullOrBlank()) {
                            return name
                        }
                    }
                }
            } catch (_: Exception) {
                // Fall back to parsing the document ID below.
            }
        }
        return quickTreeDisplayName(uri)
    }

    @Suppress("DEPRECATION")
    private fun openWarbandFolderPicker() {
        if (isScanInProgress) {
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
        startActivityForResult(intent, REQ_PICK_TREE)
    }

    @Suppress("DEPRECATION")
    private fun openDriverPackagePicker() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
            putExtra(
                Intent.EXTRA_MIME_TYPES,
                arrayOf("application/zip", "application/x-zip-compressed", "application/octet-stream")
            )
        }
        startActivityForResult(intent, REQ_PICK_DRIVER)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        if (resultCode != RESULT_OK) {
            return
        }
        if (requestCode == REQ_PICK_DRIVER) {
            val driverUri = data?.data ?: return
            importDriverPackage(driverUri)
            return
        }
        if (requestCode != REQ_PICK_TREE) {
            return
        }
        val uri = data?.data ?: return
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

        startScan(uri, quickTreeDisplayName(uri), isNewFolderPick = true)
    }

    private fun startScan(uri: Uri, initialDisplayName: String, isNewFolderPick: Boolean) {
        if (!isScanInProgress) {
            preScanSnapshot = ScanSnapshot(
                treeUri = if (isNewFolderPick) selectedTreeUri else uri,
                folderDisplayName = if (isNewFolderPick) selectedFolderDisplayName else initialDisplayName,
                modules = modules.toList(),
                selectedModuleIndex = selectedModuleIndex,
                installStates = HashMap(moduleInstallStates),
                languages = scannedLanguagesList.toList(),
                indexStatsText = lastIndexStatsText
            )
        }

        val gen = scanGeneration.incrementAndGet()
        isScanInProgress = true
        selectedTreeUri = uri
        selectedFolderDisplayName = initialDisplayName

        updateFolderUi(uri, initialDisplayName)
        homePage.setScanInProgress(true)
        homePage.setSelectedModule(null, hasFolder = true)
        modulesPage.setScanInProgress(true, initialDisplayName)
        advancedPage.setVerifyScriptsEnabled(false)
        advancedPage.setOutputStatus("Indexing SAF tree and scanning Modules/*/module.ini...")
        updateHomePlayState()

        executor.execute {
            if (gen != scanGeneration.get()) {
                return@execute
            }
            val displayName: String
            val rawJson: String
            try {
                displayName = resolveTreeDisplayName(uri)
                val bridge = WseSafBridge(applicationContext, uri)
                rawJson = bridge.nativeScanTree()
            } catch (t: Throwable) {
                val reason = t.message?.takeIf { it.isNotBlank() } ?: t.javaClass.simpleName
                runOnUiThread {
                    if (isFinishing || isDestroyed || gen != scanGeneration.get()) {
                        return@runOnUiThread
                    }
                    finishScanWithError(uri, initialDisplayName, reason)
                }
                return@execute
            }
            runOnUiThread {
                if (isFinishing || isDestroyed || gen != scanGeneration.get()) {
                    return@runOnUiThread
                }
                completeScan(gen, uri, displayName, rawJson)
            }
        }
    }

    private fun cancelScan() {
        if (!isScanInProgress) {
            return
        }
        val restoredGen = scanGeneration.incrementAndGet()
        isScanInProgress = false
        val snapshot = preScanSnapshot
        preScanSnapshot = null

        if (snapshot != null) {
            selectedTreeUri = snapshot.treeUri
            selectedFolderDisplayName = snapshot.folderDisplayName
            modules.clear()
            modules.addAll(snapshot.modules)
            selectedModuleIndex = snapshot.selectedModuleIndex
            moduleInstallStates.clear()
            moduleInstallStates.putAll(snapshot.installStates)
            scannedLanguagesList = snapshot.languages
            settingsPage.setAvailableLanguages(scannedLanguagesList)
            lastIndexStatsText = snapshot.indexStatsText
            advancedPage.setIndexStats(snapshot.indexStatsText)
        }

        val uri = selectedTreeUri
        val folderName = selectedFolderDisplayName
        if (uri != null && folderName != null) {
            updateFolderUi(uri, folderName)
        } else {
            homePage.setInstallFolder(null)
            modulesPage.setFolderState(null, hasFolder = false)
            advancedPage.setFolderStatus("No Warband folder selected.")
        }

        homePage.setScanInProgress(false)
        modulesPage.setScanInProgress(false)

        val emptyMsg = if (uri == null) {
            "Select a Warband folder to discover modules."
        } else {
            "No modules loaded. Tap Rescan to scan this folder."
        }
        modulesPage.showModules(modules, selectedModuleIndex, emptyMsg, moduleInstallStates)
        applySelectionUi(persistModulePref = false)
        updateHomePlayState()
        advancedPage.setOutputStatus("Scan cancelled.")

        if (uri != null && modules.any {
                moduleInstallStates[it.modulePath]?.state == InstallCheckState.CHECKING
            }
        ) {
            startPendingInstallChecks(uri, restoredGen)
        }
    }

    private fun completeScan(gen: Long, uri: Uri, displayName: String, rawJson: String) {
        val trimmed = rawJson.trim()
        if (trimmed.startsWith("ERROR:", ignoreCase = true)) {
            val reason = trimmed.substring(6).trim().ifEmpty { trimmed }
            finishScanWithError(uri, displayName, reason)
            return
        }

        val obj = try {
            JSONObject(trimmed)
        } catch (e: Exception) {
            finishScanWithError(
                uri,
                displayName,
                "Invalid scan response: ${e.message ?: e.javaClass.simpleName}"
            )
            return
        }

        if (!obj.optBoolean("ok", false)) {
            val rawErr = obj.optString("error", "").trim()
            val reason = when {
                rawErr.startsWith("ERROR:", ignoreCase = true) ->
                    rawErr.substring(6).trim().ifEmpty { rawErr }
                rawErr.isNotEmpty() -> rawErr
                else -> "Unknown scan failure."
            }
            finishScanWithError(uri, displayName, reason)
            return
        }

        isScanInProgress = false
        preScanSnapshot = null
        selectedTreeUri = uri
        selectedFolderDisplayName = displayName

        val prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        prefs.edit().putString(KEY_TREE_URI, uri.toString()).apply()

        updateFolderUi(uri, displayName)
        homePage.setScanInProgress(false)
        modulesPage.setScanInProgress(false)

        val indexMs = obj.optLong("index_ms", 0L)
        val fileCount = obj.optLong("files", 0L)
        val dirCount = obj.optLong("dirs", 0L)
        lastIndexStatsText = "SafVfs index: $fileCount files, $dirCount dirs in $indexMs ms"
        advancedPage.setIndexStats(lastIndexStatsText)

        val parsedLanguages = ArrayList<String>()
        val langArr = obj.optJSONArray("languages")
        if (langArr != null) {
            for (i in 0 until langArr.length()) {
                val code = langArr.optString(i, "").trim()
                if (code.isNotEmpty() && parsedLanguages.none { it.equals(code, ignoreCase = true) }) {
                    parsedLanguages.add(code)
                }
            }
        }
        scannedLanguagesList = if (parsedLanguages.isNotEmpty()) parsedLanguages else listOf("en")
        settingsPage.setAvailableLanguages(scannedLanguagesList)

        modules.clear()
        moduleInstallStates.clear()
        val arr = obj.optJSONArray("modules")
        if (arr != null) {
            for (i in 0 until arr.length()) {
                val m = arr.getJSONObject(i)
                val mod = ModuleChoice(
                    folderName = m.optString("folder_name", ""),
                    modulePath = m.optString("module_path", ""),
                    displayName = m.optString("display_name", ""),
                    hasScriptsTxt = m.optBoolean("has_scripts_txt", false),
                    loadResourceCount = m.optInt("load_resource_count", 0),
                    loadModResourceCount = m.optInt("load_mod_resource_count", 0)
                )
                modules.add(mod)
                moduleInstallStates[mod.modulePath] = ModuleInstallStatus(InstallCheckState.CHECKING)
            }
        }

        val savedModule = prefs.getString(KEY_SELECTED_MODULE, null)
        if (modules.isNotEmpty()) {
            val savedIdx = if (!savedModule.isNullOrBlank()) {
                modules.indexOfFirst { it.folderName.equals(savedModule, ignoreCase = true) }
            } else {
                -1
            }
            val idx = if (savedIdx >= 0) {
                savedIdx
            } else {
                modules.indexOfFirst { it.folderName.equals("Native", ignoreCase = true) }
                    .coerceAtLeast(0)
            }
            selectedModuleIndex = idx
            modulesPage.showModules(modules, idx, installStatuses = moduleInstallStates)
            applySelectionUi(persistModulePref = savedModule.isNullOrBlank())
            updateHomePlayState()
            setStatus("Found ${modules.size} module(s) in $indexMs ms.")
            startPendingInstallChecks(uri, gen)
        } else {
            selectedModuleIndex = -1
            modulesPage.showModules(
                emptyList(),
                -1,
                "No Modules/*/module.ini folders found in selected tree."
            )
            applySelectionUi(persistModulePref = false)
            updateHomePlayState()
            setStatus("Indexed $fileCount file(s), 0 Warband modules.")
        }
    }

    private fun finishScanWithError(uri: Uri, displayName: String, reason: String) {
        isScanInProgress = false
        preScanSnapshot = null
        selectedTreeUri = uri
        selectedFolderDisplayName = displayName

        updateFolderUi(uri, displayName)
        homePage.setScanInProgress(false)
        modulesPage.setScanInProgress(false)

        modules.clear()
        moduleInstallStates.clear()
        selectedModuleIndex = -1
        scannedLanguagesList = listOf("en")
        settingsPage.setAvailableLanguages(scannedLanguagesList)

        lastIndexStatsText = "SafVfs index: error"
        advancedPage.setIndexStats(lastIndexStatsText)
        advancedPage.setVerifyScriptsEnabled(false)
        advancedPage.setOutputStatus("ERROR: $reason")

        modulesPage.showModules(emptyList(), -1, "No modules available.")
        homePage.setSelectedModule(null, hasFolder = true)
        updateHomePlayState()
        showScanErrorDialog(reason)
    }

    private fun startPendingInstallChecks(uri: Uri, gen: Long) {
        val pendingPaths = modules
            .filter {
                val state = moduleInstallStates[it.modulePath]?.state
                state == null || state == InstallCheckState.CHECKING
            }
            .map { it.modulePath }
        for (modulePath in pendingPaths) {
            executor.execute {
                if (gen != scanGeneration.get()) {
                    return@execute
                }
                val result = runInstallCheck(uri, modulePath)
                runOnUiThread {
                    if (isFinishing || isDestroyed || gen != scanGeneration.get()) {
                        return@runOnUiThread
                    }
                    moduleInstallStates[modulePath] = result
                    modulesPage.updateModuleStatus(modulePath, result)
                    if (modules.getOrNull(selectedModuleIndex)?.modulePath == modulePath) {
                        updateHomePlayState()
                    }
                }
            }
        }
    }

    private fun runInstallCheck(uri: Uri, modulePath: String): ModuleInstallStatus {
        return try {
            val bridge = WseSafBridge(applicationContext, uri)
            val raw = bridge.nativeCheckModuleInstall(modulePath)
            parseInstallCheckJson(raw)
        } catch (t: Throwable) {
            ModuleInstallStatus(
                state = InstallCheckState.MISSING,
                missingFiles = listOf(t.message ?: t.javaClass.simpleName)
            )
        }
    }

    private fun parseInstallCheckJson(raw: String): ModuleInstallStatus {
        return try {
            val obj = JSONObject(raw)
            if (obj.optBoolean("ok", false)) {
                ModuleInstallStatus(InstallCheckState.READY, emptyList())
            } else {
                val arr = obj.optJSONArray("missing")
                val missing = ArrayList<String>()
                if (arr != null) {
                    for (i in 0 until arr.length()) {
                        val item = arr.optString(i, "").trim()
                        if (item.isNotEmpty()) {
                            missing.add(item)
                        }
                    }
                }
                if (missing.isEmpty()) {
                    missing.add("Required Warband files could not be verified.")
                }
                ModuleInstallStatus(InstallCheckState.MISSING, missing)
            }
        } catch (e: Exception) {
            ModuleInstallStatus(
                state = InstallCheckState.MISSING,
                missingFiles = listOf(
                    "Invalid install check response: ${e.message ?: e.javaClass.simpleName}"
                )
            )
        }
    }

    private fun selectModule(position: Int) {
        if (position !in modules.indices) {
            selectedModuleIndex = -1
            applySelectionUi(persistModulePref = false)
            updateHomePlayState()
            return
        }
        selectedModuleIndex = position
        applySelectionUi(persistModulePref = true)
        updateHomePlayState()
    }

    private fun applySelectionUi(persistModulePref: Boolean) {
        val pos = selectedModuleIndex
        if (pos !in modules.indices) {
            modulesPage.updateSelectedIndex(-1)
            homePage.setSelectedModule(null, hasFolder = selectedTreeUri != null)
            advancedPage.setVerifyScriptsEnabled(false)
            return
        }
        val m = modules[pos]
        if (persistModulePref) {
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                .edit()
                .putString(KEY_SELECTED_MODULE, m.folderName)
                .apply()
        }
        modulesPage.updateSelectedIndex(pos)
        homePage.setSelectedModule(m, hasFolder = selectedTreeUri != null)
        advancedPage.setVerifyScriptsEnabled(m.hasScriptsTxt && selectedTreeUri != null && !isScanInProgress)
    }

    private fun updateHomePlayState() {
        if (isScanInProgress) {
            val label = selectedFolderDisplayName?.takeIf { it.isNotBlank() } ?: "folder"
            homePage.setInstallStatus("Scanning $label…", isReady = false)
            return
        }
        if (selectedTreeUri == null) {
            homePage.setInstallStatus("Choose your Warband folder", isReady = false)
            return
        }
        val mod = modules.getOrNull(selectedModuleIndex)
        if (mod == null) {
            homePage.setInstallStatus("Choose a module", isReady = false)
            return
        }
        val status = moduleInstallStates[mod.modulePath]
        when (status?.state) {
            null, InstallCheckState.CHECKING -> {
                homePage.setInstallStatus("Checking files…", isReady = false)
            }
            InstallCheckState.READY -> {
                homePage.setInstallStatus(null, isReady = true)
            }
            InstallCheckState.MISSING -> {
                val msg = if (status.missingFiles.isNotEmpty()) {
                    "Missing ${status.missingFiles.size} file(s) — tap for details"
                } else {
                    "Missing files — tap for details"
                }
                homePage.setInstallStatus(msg, isReady = false) {
                    showMissingFilesDialog(mod, status.missingFiles)
                }
            }
        }
    }

    private fun showScanErrorDialog(reason: String) {
        if (isFinishing || isDestroyed) {
            return
        }
        activeDialog?.dismiss()
        activeDialog = AlertDialog.Builder(this)
            .setTitle("Could not read this folder")
            .setMessage(reason)
            .setPositiveButton("Choose a different folder") { _, _ ->
                openWarbandFolderPicker()
            }
            .setNegativeButton("OK", null)
            .show()
    }

    private fun showMissingFilesDialog(module: ModuleChoice, missingFiles: List<String>) {
        if (isFinishing || isDestroyed) {
            return
        }
        val modName = module.displayName.ifBlank { module.folderName }
        val message = if (missingFiles.isEmpty()) {
            "Required Warband files could not be verified."
        } else {
            buildString {
                append("These Warband files were not found:\n")
                for (path in missingFiles) {
                    append("  - ").append(path).append('\n')
                }
                append("\nChoose the folder that contains Modules/ and CommonRes/.")
            }
        }
        activeDialog?.dismiss()
        activeDialog = AlertDialog.Builder(this)
            .setTitle("Missing files ($modName)")
            .setMessage(message)
            .setPositiveButton("Choose folder") { _, _ ->
                openWarbandFolderPicker()
            }
            .setNegativeButton("OK", null)
            .show()
    }

    private fun verifySelectedModuleScripts() {
        val uri = selectedTreeUri ?: return
        val pos = selectedModuleIndex
        if (pos !in modules.indices) {
            return
        }
        val mod = modules[pos]
        setStatus("Verifying ${mod.modulePath}/scripts.txt via SafVfs...")
        executor.execute {
            val bridge = WseSafBridge(applicationContext, uri)
            val result = bridge.nativeVerifyModule(mod.modulePath)
            runOnUiThread {
                if (!isFinishing && !isDestroyed) {
                    setStatus(result)
                }
            }
        }
    }

    private fun frameRateLabel(hz: Int): String = when (hz) {
        0 -> "Uncapped"
        30 -> "30 Hz (battery saver)"
        else -> "$hz Hz"
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

    private fun selectedDriverModeKey(): String =
        driverModeKeys[advancedPage.selectedDriverIndex().coerceIn(0, driverModeKeys.size - 1)]

    private fun updateDriverStatus() {
        val mode = selectedDriverModeKey()
        if (mode == "system") {
            advancedPage.setDriverStatus("Uses the device's built-in Vulkan driver.")
            return
        }
        val prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        val path = prefs.getString(KEY_DRIVER_PATH_PREFIX + mode, "") ?: ""
        val name = prefs.getString(KEY_DRIVER_NAME_PREFIX + mode, "") ?: ""
        advancedPage.setDriverStatus(
            if (path.isNotEmpty() && File(path).isFile) {
                "Imported: $name"
            } else {
                "No $mode driver imported yet."
            }
        )
    }

    private fun importDriverPackage(uri: Uri) {
        val selectedMode = selectedDriverModeKey()
        setStatus("Importing GPU driver package...")
        executor.execute {
            val result = try {
                stageDriverPackage(uri, selectedMode)
            } catch (e: Exception) {
                "ERROR: ${e.message ?: e.javaClass.simpleName}"
            }
            runOnUiThread {
                if (isFinishing || isDestroyed) {
                    return@runOnUiThread
                }
                applyDriverImportResult(result)
            }
        }
    }

    // Reads meta.json and the library it names out of an AdrenoTools-style zip,
    // stages them natively (ELF check + libhardware.so patch) under app storage,
    // and marks the ICD read-only. dlopen needs a real file path, not a SAF URI.
    private fun stageDriverPackage(uri: Uri, selectedMode: String): String {
        var metaJson: String? = null
        val libraries = HashMap<String, ByteArray>()
        var total = 0L
        contentResolver.openInputStream(uri)?.use { raw ->
            ZipInputStream(raw.buffered()).use { zip ->
                while (true) {
                    val entry = zip.nextEntry ?: break
                    if (entry.isDirectory) {
                        continue
                    }
                    val base = entry.name.substringAfterLast('/')
                    val isMeta = base.equals("meta.json", ignoreCase = true)
                    if (!isMeta && !base.endsWith(".so", ignoreCase = true)) {
                        continue
                    }
                    val bytes = readLimited(zip, MAX_DRIVER_PACKAGE_BYTES - total)
                        ?: return "ERROR: driver package exceeds ${MAX_DRIVER_PACKAGE_BYTES / (1024 * 1024)} MiB"
                    total += bytes.size
                    if (isMeta) {
                        metaJson = String(bytes, Charsets.UTF_8)
                    } else {
                        libraries[base] = bytes
                    }
                }
            }
        } ?: return "ERROR: cannot open the selected file"

        val meta = metaJson ?: return "ERROR: package has no meta.json"
        val libName = try {
            JSONObject(meta).optString("libraryName", "")
        } catch (_: Exception) {
            ""
        }
        val icd = libraries[libName]
            ?: return "ERROR: package has no '$libName' (libraryName in meta.json)"

        val root = File(noBackupFilesDir, "gpu_drivers")
        val staging = File(root, "import")
        staging.deleteRecursively()
        if (!staging.mkdirs()) {
            return "ERROR: cannot create ${staging.path}"
        }
        val bridge = WseSafBridge(applicationContext, Uri.EMPTY)
        val res = bridge.nativeStageDriverPackage(meta, icd, staging.absolutePath, selectedMode)
        val parts = res.split('\t')
        if (parts.size < 5 || parts[0] != "OK") {
            staging.deleteRecursively()
            return res
        }
        val mode = when {
            parts[1] == "turnip" || parts[1] == "panvk" -> parts[1]
            selectedMode != "system" -> selectedMode
            else -> {
                staging.deleteRecursively()
                return "ERROR: cannot tell whether '${parts[3]}' is Turnip or PanVK; select one first"
            }
        }
        val icdName = File(parts[2]).name
        val stagedIcd = File(staging, icdName)
        // Android 14+ refuses to load dynamically loaded code from a writable file;
        // enforce this inside staging before swapping out any working driver directory.
        if (!stagedIcd.setReadOnly()) {
            staging.deleteRecursively()
            return "ERROR: cannot mark $icdName read-only"
        }
        val finalDir = File(root, mode)
        val backupDir = File(root, "$mode.bak")
        backupDir.deleteRecursively()
        val hadExisting = finalDir.exists()
        if (hadExisting && !finalDir.renameTo(backupDir)) {
            staging.deleteRecursively()
            return "ERROR: cannot back up ${finalDir.path}"
        }
        if (!staging.renameTo(finalDir)) {
            staging.deleteRecursively()
            if (hadExisting) {
                backupDir.renameTo(finalDir)
            }
            return "ERROR: cannot move driver into ${finalDir.path}"
        }
        backupDir.deleteRecursively()
        val icdFile = File(finalDir, icdName)
        return "OK\t$mode\t${icdFile.absolutePath}\t${parts[3]}\t${parts[4]}"
    }

    private fun readLimited(input: InputStream, limit: Long): ByteArray? {
        val out = ByteArrayOutputStream()
        val buf = ByteArray(64 * 1024)
        var total = 0L
        while (true) {
            val n = input.read(buf)
            if (n < 0) {
                break
            }
            total += n
            if (total > limit) {
                return null
            }
            out.write(buf, 0, n)
        }
        return out.toByteArray()
    }

    private fun applyDriverImportResult(result: String) {
        val parts = result.split('\t')
        if (parts.size < 5 || parts[0] != "OK") {
            setStatus(result)
            return
        }
        val mode = parts[1]
        val editor = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
            .edit()
            .putString(KEY_DRIVER_PATH_PREFIX + mode, parts[2])
            .putString(KEY_DRIVER_NAME_PREFIX + mode, parts[3])
        val idx = driverModeKeys.indexOf(mode)
        if (idx >= 0) {
            editor.putString(KEY_GPU_DRIVER, driverModeKeys[idx])
        }
        editor.apply()
        if (idx >= 0) {
            advancedPage.setDriverSelection(idx)
        }
        updateDriverStatus()
        setStatus("Imported ${parts[3]} as $mode (libhardware.so references patched: ${parts[4]}).")
    }

    // Runs the probe in WseVulkanProbeService (separate ":vkprobe" process), so a
    // crashing driver cannot take the launcher down. One probe at a time.
    private fun probeVulkanDriver() {
        if (probeConnection != null) {
            return
        }
        val mode = selectedDriverModeKey()
        val driver = driverOptions[driverModeKeys.indexOf(mode)]
        val icdPath = if (mode == "system") {
            ""
        } else {
            getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
                .getString(KEY_DRIVER_PATH_PREFIX + mode, "") ?: ""
        }
        val shaderCache = File(cacheDir, "mesa_shader_cache")
        shaderCache.mkdirs()
        val args = Bundle().apply {
            putString(WseVulkanProbeService.KEY_MODE, mode)
            putString(WseVulkanProbeService.KEY_ICD_PATH, icdPath)
            putString(WseVulkanProbeService.KEY_NATIVE_LIB_DIR, applicationInfo.nativeLibraryDir ?: "")
            putString(WseVulkanProbeService.KEY_SHADER_CACHE_DIR, shaderCache.absolutePath)
            putBoolean(WseVulkanProbeService.KEY_ALLOW_MOCK, advancedPage.isMockFallbackChecked())
        }

        val replies = Messenger(object : Handler(Looper.getMainLooper()) {
            override fun handleMessage(msg: Message) {
                if (msg.what == WseVulkanProbeService.MSG_RESULT) {
                    finishProbe(msg.data.getString(WseVulkanProbeService.KEY_RESULT) ?: "ERROR: empty probe result")
                } else {
                    super.handleMessage(msg)
                }
            }
        })
        val connection = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
                val request = Message.obtain(null, WseVulkanProbeService.MSG_PROBE).apply {
                    data = args
                    replyTo = replies
                }
                try {
                    Messenger(service).send(request)
                } catch (_: RemoteException) {
                    finishProbe("ERROR: Vulkan probe process died before the request was sent")
                }
            }

            override fun onServiceDisconnected(name: ComponentName?) {
                finishProbe("ERROR: the $driver driver crashed the probe process " +
                    "(native fault while loading or initializing it); the launcher is unaffected")
            }

            override fun onBindingDied(name: ComponentName?) {
                onServiceDisconnected(name)
            }
        }

        setStatus("Probing $driver Vulkan driver in an isolated process...")
        advancedPage.setProbeEnabled(false)
        probeConnection = connection
        val bound = bindService(
            Intent(this, WseVulkanProbeService::class.java),
            connection,
            Context.BIND_AUTO_CREATE
        )
        if (!bound) {
            finishProbe("ERROR: cannot start the Vulkan probe service")
            return
        }
        mainHandler.postDelayed(probeTimeout, PROBE_TIMEOUT_MS)
    }

    private val probeTimeout = Runnable {
        finishProbe("ERROR: Vulkan probe timed out after ${PROBE_TIMEOUT_MS / 1000} s (driver hung); probe process stopped")
    }

    // Unbinding ends the probe process (WseVulkanProbeService.onUnbind exits it).
    // `result == null` means the activity is going away: clean up silently.
    private fun finishProbe(result: String?) {
        val connection = probeConnection ?: return
        probeConnection = null
        mainHandler.removeCallbacks(probeTimeout)
        try {
            unbindService(connection)
        } catch (_: IllegalArgumentException) {
            // Already unbound.
        }
        if (result != null && !isFinishing && !isDestroyed) {
            setStatus(result)
            advancedPage.setProbeEnabled(true)
        }
    }

    // `runModule`: WseActivity runs the selected module's UI from the picked tree
    // (`--saf-module`) instead of the self-test.
    private fun launchSelfTestActivity(runModule: Boolean = false) {
        val selectedPos = selectedModuleIndex
        val selectedMod = if (selectedPos in modules.indices) modules[selectedPos].folderName else "Native"
        if (runModule) {
            if (isScanInProgress) {
                return
            }
            if (selectedTreeUri == null || selectedPos !in modules.indices) {
                val why = if (selectedTreeUri == null) {
                    "No Warband folder is selected."
                } else {
                    "No Warband modules were found in the selected folder."
                }
                activeDialog?.dismiss()
                activeDialog = AlertDialog.Builder(this)
                    .setTitle("Warband files not found")
                    .setMessage(
                        "$why\n\nTap \"Choose folder\" and pick the Mount & Blade: Warband " +
                            "folder itself (the one that contains Modules, Data and CommonRes)."
                    )
                    .setPositiveButton(android.R.string.ok, null)
                    .show()
                return
            }
            val currentMod = modules[selectedPos]
            val checkStatus = moduleInstallStates[currentMod.modulePath]
            if (checkStatus?.state == InstallCheckState.MISSING) {
                showMissingFilesDialog(currentMod, checkStatus.missingFiles)
                return
            }
            if (checkStatus?.state != InstallCheckState.READY) {
                return
            }
        }
        val intent = Intent(this, WseActivity::class.java).apply {
            putExtra(EXTRA_RUN_MODULE, runModule)
            if (runModule) putExtra(WseActivity.EXTRA_SHOW_FPS,
                this@WseLauncherActivity.getSharedPreferences(PREFS_NAME, MODE_PRIVATE).getBoolean("show_fps_overlay", false))
            putExtra(EXTRA_TREE_URI, selectedTreeUri?.toString() ?: "")
            putExtra(EXTRA_MODULE_NAME, selectedMod)
            // Stable mode id ("system" / "turnip" / "panvk"), as parse_driver_mode expects.
            putExtra(EXTRA_GPU_DRIVER, selectedDriverModeKey())
        }
        startActivity(intent)
    }

    companion object {
        private const val STATE_SELECTED_TAB = "wse2.state.SELECTED_TAB"
        private const val REQ_PICK_TREE = 2001
        private const val REQ_PICK_DRIVER = 2002
        private const val MAX_DRIVER_PACKAGE_BYTES = 256L * 1024L * 1024L
        private const val PROBE_TIMEOUT_MS = 60_000L
        private const val KEY_DRIVER_PATH_PREFIX = "driver_path_"
        private const val KEY_DRIVER_NAME_PREFIX = "driver_name_"
        private const val PREFS_NAME = "wse2_launcher"
        private const val KEY_TREE_URI = "tree_uri"
        private const val KEY_SELECTED_MODULE = "selected_module"
        private const val KEY_GPU_DRIVER = "gpu_driver"
        private const val KEY_FRAME_RATE_CAP = "frame_rate_cap_hz"
        private const val KEY_GYRO_AIM = "gyro_aim_enabled"
        private const val KEY_PINCH_ZOOM = "pinch_zoom_enabled"
        private const val KEY_THERMAL_THROTTLE = "thermal_throttling_enabled"
        private const val PERFORMANCE_JSON = "config/performance.json"
        private val presentModeKeys = listOf("fifo", "mailbox", "immediate", "fifo_relaxed")
        private val presentModeLabels = listOf(
            "Present: FIFO (vsync)", "Present: mailbox", "Present: immediate (tearing)",
            "Present: FIFO relaxed"
        )

        const val EXTRA_TREE_URI = "wse2.extra.TREE_URI"
        const val EXTRA_MODULE_NAME = "wse2.extra.MODULE_NAME"
        const val EXTRA_GPU_DRIVER = "wse2.extra.GPU_DRIVER"
        const val EXTRA_RUN_MODULE = "wse2.extra.RUN_MODULE"
    }
}
