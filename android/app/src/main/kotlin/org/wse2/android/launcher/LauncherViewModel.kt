package org.wse2.android.launcher

import android.app.Application
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.hardware.Sensor
import android.hardware.SensorManager
import android.hardware.display.DisplayManager
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.os.RemoteException
import android.provider.DocumentsContract
import android.util.LruCache
import android.view.Display
import androidx.lifecycle.AndroidViewModel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.receiveAsFlow
import org.json.JSONObject
import org.wse2.android.WseSafBridge
import org.wse2.android.WseVulkanProbeService
import org.wse2.android.launcher.art.CARD_ART_MAX_SIDE
import org.wse2.android.launcher.art.HERO_ART_MAX_SIDE
import org.wse2.android.launcher.art.ModuleArt
import org.wse2.android.launcher.art.decodeModuleArtIntArray
import org.wse2.android.launcher.theme.DefaultSeedColorArgb
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream
import java.util.Locale
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import java.util.zip.ZipInputStream

class LauncherViewModel(application: Application) : AndroidViewModel(application) {

    private data class ArtCacheKey(
        val treeUri: String,
        val moduleDir: String,
        val maxSide: Int
    )

    private data class ScanSnapshot(
        val treeUri: Uri?,
        val folderDisplayName: String?,
        val modules: List<ModuleChoice>,
        val selectedModuleIndex: Int,
        val installStates: Map<String, ModuleInstallStatus>,
        val languages: List<String>,
        val indexStatsText: String
    )

    private val app: Application = application
    private val executor: ExecutorService = Executors.newSingleThreadExecutor()
    private val settingsExecutor: ExecutorService = Executors.newSingleThreadExecutor()
    // rgl_config.txt changes run on settingsExecutor against `rglOnDisk`, the state last read from
    // or written to the file (executor thread only), so a change made before the file was read
    // never writes defaults over it. The screen updates at once when the file is already read.
    // Declared before `init`, which queues the file's first read.
    private var rglOnDisk: RglConfigState? = null
    @Volatile
    private var rglLoaded = false
    private val mainHandler = Handler(Looper.getMainLooper())
    private val scanGeneration = AtomicLong(0L)
    private val isCleared = AtomicBoolean(false)

    private var probeConnection: ServiceConnection? = null
    private var settingsOpSeq = 0L
    private var settingsNeedsRefresh = false
    private var lastCommittedSettings: PerformanceUiState? = null
    private var preScanSnapshot: ScanSnapshot? = null
    private val artCache = LruCache<ArtCacheKey, ModuleArt>(ART_CACHE_MAX_ENTRIES)
    private val inFlightArtKeys = HashSet<ArtCacheKey>()

    private val _selectedTab = MutableStateFlow(LauncherTab.HOME)
    val selectedTab: StateFlow<LauncherTab> = _selectedTab.asStateFlow()

    private val _installFolder = MutableStateFlow(InstallFolderState())
    val installFolder: StateFlow<InstallFolderState> = _installFolder.asStateFlow()

    private val _scanState = MutableStateFlow(ScanState())
    val scanState: StateFlow<ScanState> = _scanState.asStateFlow()

    private val _modulesState = MutableStateFlow(ModulesUiState())
    val modulesState: StateFlow<ModulesUiState> = _modulesState.asStateFlow()

    private val _scannedLanguages = MutableStateFlow<List<String>>(emptyList())
    val scannedLanguages: StateFlow<List<String>> = _scannedLanguages.asStateFlow()

    private val _settingsState = MutableStateFlow(SettingsUiState())
    val settingsState: StateFlow<SettingsUiState> = _settingsState.asStateFlow()

    private val _driverState = MutableStateFlow(DriverUiState())
    val driverState: StateFlow<DriverUiState> = _driverState.asStateFlow()

    private val _showFpsOverlay = MutableStateFlow(false)
    val showFpsOverlay: StateFlow<Boolean> = _showFpsOverlay.asStateFlow()

    private val _statusLog = MutableStateFlow("Ready.")
    val statusLog: StateFlow<String> = _statusLog.asStateFlow()

    private val _seedColorArgb = MutableStateFlow(DefaultSeedColorArgb)
    val seedColorArgb: StateFlow<Int> = _seedColorArgb.asStateFlow()

    private val _moduleArt = MutableStateFlow<Map<String, ModuleArt>>(emptyMap())
    val moduleArt: StateFlow<Map<String, ModuleArt>> = _moduleArt.asStateFlow()

    private val _heroArt = MutableStateFlow<ModuleArt?>(null)
    val heroArt: StateFlow<ModuleArt?> = _heroArt.asStateFlow()

    private val _r0State = MutableStateFlow(R0UiState())
    val r0State: StateFlow<R0UiState> = _r0State.asStateFlow()

    private val _r4State = MutableStateFlow(R4UiState())
    val r4State: StateFlow<R4UiState> = _r4State.asStateFlow()
    // Shared by every screen of this process: the run outlives the activity (R4ForegroundService).
    private val r4Runner = R4StartupRunner.get(application)
    private val r4Listener: (R4UiState) -> Unit = { s -> if (!isCleared.get()) _r4State.value = s }

    private val _events = Channel<LauncherEvent>(Channel.UNLIMITED)
    val events: Flow<LauncherEvent> = _events.receiveAsFlow()

    private val probeTimeout = Runnable {
        finishProbe(
            "ERROR: Vulkan probe timed out after ${PROBE_TIMEOUT_MS / 1000} s (driver hung); probe process stopped"
        )
    }

    init {
        val sensorManager = app.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
        val hasPhoneGyro = sensorManager?.getDefaultSensor(Sensor.TYPE_GYROSCOPE) != null
        val displayManager = app.getSystemService(Context.DISPLAY_SERVICE) as? DisplayManager
        val maxRefreshHz = maxDisplayRefreshHz(displayManager)
        val initialRates = ALL_FRAME_RATE_VALUES.filter { it == 0 || it <= maxRefreshHz + 1 }
        _settingsState.value = _settingsState.value.copy(
            frameRateValues = initialRates,
            frameRateLabels = initialRates.map { frameRateLabel(it) },
            hasPhoneGyro = hasPhoneGyro
        )

        restorePreferences()
        r4Runner.addListener(r4Listener)
    }

    override fun onCleared() {
        isCleared.set(true)
        r4Runner.removeListener(r4Listener)
        scanGeneration.incrementAndGet()
        finishProbe(null)
        executor.shutdownNow()
        settingsExecutor.shutdown()
        super.onCleared()
    }

    fun setSeedColor(argb: Int) {
        _seedColorArgb.value = argb
    }

    fun updateMaxDisplayRefreshHz(maxRefreshHz: Int) {
        val filtered = ALL_FRAME_RATE_VALUES.filter { it == 0 || it <= maxRefreshHz + 1 }
        val current = _settingsState.value
        if (filtered == current.frameRateValues) {
            return
        }
        val labels = filtered.map { frameRateLabel(it) }
        val committed = lastCommittedSettings
        if (committed != null) {
            val snapped = snapToOffered(committed, filtered, current.languageOptions)
            lastCommittedSettings = snapped
            _settingsState.value = current.copy(
                performance = snapped,
                frameRateValues = filtered,
                frameRateLabels = labels
            )
        } else {
            _settingsState.value = current.copy(
                frameRateValues = filtered,
                frameRateLabels = labels
            )
        }
    }

    fun selectTab(tab: LauncherTab) {
        _selectedTab.value = tab
    }

    fun onFolderPicked(uri: Uri) {
        startScan(
            uri = uri,
            initialDisplayName = quickTreeDisplayName(uri),
            isNewFolderPick = true
        )
    }

    fun rescanSelectedFolder() {
        val folder = _installFolder.value
        val uri = folder.treeUri ?: return
        if (_scanState.value.inProgress) {
            return
        }
        startScan(
            uri = uri,
            initialDisplayName = folder.displayName ?: quickTreeDisplayName(uri),
            isNewFolderPick = false
        )
    }

    fun cancelScan() {
        if (!_scanState.value.inProgress) {
            return
        }
        val restoredGen = scanGeneration.incrementAndGet()
        val snapshot = preScanSnapshot
        preScanSnapshot = null

        if (snapshot != null) {
            _installFolder.value = InstallFolderState(
                treeUri = snapshot.treeUri,
                displayName = snapshot.folderDisplayName
            )
            _scannedLanguages.value = snapshot.languages
            val emptyMsg = if (snapshot.treeUri == null) {
                "Select a Warband folder to discover modules."
            } else {
                "No modules loaded. Tap Rescan to scan this folder."
            }
            _modulesState.value = ModulesUiState(
                modules = snapshot.modules,
                selectedIndex = snapshot.selectedModuleIndex,
                installStatuses = snapshot.installStates,
                emptyMessage = emptyMsg
            )
            refreshLanguageOptions()
            _scanState.value = ScanState(
                inProgress = false,
                generation = restoredGen,
                folderLabel = null,
                indexStatsText = snapshot.indexStatsText
            )
        } else {
            _scanState.value = _scanState.value.copy(
                inProgress = false,
                generation = restoredGen,
                folderLabel = null
            )
        }

        _statusLog.value = "Scan cancelled."

        val uri = _installFolder.value.treeUri
        val currentModules = _modulesState.value
        if (uri != null) {
            if (currentModules.modules.any {
                    currentModules.installStatuses[it.modulePath]?.state == InstallCheckState.CHECKING
                }
            ) {
                startPendingInstallChecks(uri, restoredGen)
            }
            scheduleArtLoads(uri, restoredGen, currentModules.modules, currentModules.selectedIndex)
        } else {
            _moduleArt.value = emptyMap()
            _heroArt.value = null
            setSeedColor(DefaultSeedColorArgb)
        }
    }

    fun selectModule(position: Int) {
        val current = _modulesState.value
        val uri = _installFolder.value.treeUri
        val gen = _scanState.value.generation
        if (position !in current.modules.indices) {
            _modulesState.value = current.copy(selectedIndex = -1)
            if (uri != null) {
                scheduleHeroFallback(uri, gen)
            } else {
                _heroArt.value = null
                setSeedColor(DefaultSeedColorArgb)
            }
            return
        }
        val mod = current.modules[position]
        prefs().edit().putString(KEY_SELECTED_MODULE, mod.folderName).apply()
        _modulesState.value = current.copy(selectedIndex = position)
        if (uri != null) {
            selectHeroArtForModule(uri, gen, mod.modulePath)
        }
    }

    fun showMissingFiles(module: ModuleChoice, missingFiles: List<String>) {
        _events.trySend(LauncherEvent.ShowMissingFiles(module, missingFiles))
    }

    fun verifySelectedModuleScripts() {
        if (_scanState.value.inProgress) {
            return
        }
        val uri = _installFolder.value.treeUri ?: return
        val mod = _modulesState.value.selectedModule ?: return
        _statusLog.value = "Verifying ${mod.modulePath}/scripts.txt via SafVfs..."
        executor.execute {
            val result = try {
                val bridge = WseSafBridge(app, uri)
                bridge.nativeVerifyModule(mod.modulePath)
            } catch (t: Throwable) {
                "ERROR: ${t.message ?: t.javaClass.simpleName}"
            }
            mainHandler.post {
                if (!isCleared.get()) {
                    _statusLog.value = result
                }
            }
        }
    }

    // Runs on its own thread: translation takes seconds to minutes and must not hold up
    // folder scans on `executor`.
    fun runR0Spikes() {
        if (_r0State.value.running) {
            return
        }
        _r0State.value = R0UiState(running = true)
        val treeUri = _installFolder.value.treeUri
        Thread({
            val report = try {
                R0Spikes.run(app, treeUri)
            } catch (t: Throwable) {
                "The phone check failed: ${t.message ?: t.javaClass.simpleName}"
            }
            mainHandler.post {
                if (!isCleared.get()) {
                    _r0State.value = R0UiState(running = false, report = report)
                }
            }
        }, "wse-r0-spikes").start()
    }

    fun dismissR0Report() {
        _r0State.value = _r0State.value.copy(report = null)
    }

    // Phase 5 R4: translate and start the exe in R4StartupService's own process (R4StartupRunner).
    fun runR4Startup() = r4Runner.start(_installFolder.value.treeUri)

    fun setR4UseCopy(useCopy: Boolean) = r4Runner.setUseCopy(useCopy)

    fun dismissR4Report() = r4Runner.dismissReport()

    // The result notification was tapped: open Advanced with the last report.
    fun showR4Report() {
        selectTab(LauncherTab.ADVANCED)
        r4Runner.showLastReport()
    }

    fun selectDriver(index: Int) {
        val current = _driverState.value
        val clamped = index.coerceIn(0, current.modeKeys.lastIndex)
        if (clamped == current.selectedIndex) {
            return
        }
        prefs().edit().putString(KEY_GPU_DRIVER, current.modeKeys[clamped]).apply()
        _driverState.value = current.copy(
            selectedIndex = clamped,
            statusText = computeDriverStatusText(current.modeKeys[clamped])
        )
    }

    fun setAllowMockFallback(allow: Boolean) {
        _driverState.value = _driverState.value.copy(allowMockFallback = allow)
    }

    fun importDriverPackage(uri: Uri) {
        val selectedMode = _driverState.value.selectedModeKey
        _statusLog.value = "Importing GPU driver package..."
        executor.execute {
            val result = try {
                stageDriverPackage(uri, selectedMode)
            } catch (e: Exception) {
                "ERROR: ${e.message ?: e.javaClass.simpleName}"
            }
            mainHandler.post {
                if (!isCleared.get()) {
                    applyDriverImportResult(result)
                }
            }
        }
    }

    // Runs the probe in WseVulkanProbeService (separate ":vkprobe" process), so a
    // crashing driver cannot take the launcher down. One probe at a time.
    fun probeVulkanDriver() {
        if (probeConnection != null) {
            return
        }
        val driver = _driverState.value
        val mode = driver.selectedModeKey
        val driverLabel = driver.options[driver.selectedIndex.coerceIn(0, driver.options.lastIndex)]
        val icdPath = if (mode == "system") {
            ""
        } else {
            prefs().getString(KEY_DRIVER_PATH_PREFIX + mode, "") ?: ""
        }
        val shaderCache = File(app.cacheDir, "mesa_shader_cache")
        shaderCache.mkdirs()
        val args = Bundle().apply {
            putString(WseVulkanProbeService.KEY_MODE, mode)
            putString(WseVulkanProbeService.KEY_ICD_PATH, icdPath)
            putString(
                WseVulkanProbeService.KEY_NATIVE_LIB_DIR,
                app.applicationInfo.nativeLibraryDir ?: ""
            )
            putString(WseVulkanProbeService.KEY_SHADER_CACHE_DIR, shaderCache.absolutePath)
            putBoolean(WseVulkanProbeService.KEY_ALLOW_MOCK, driver.allowMockFallback)
        }

        val replies = Messenger(object : Handler(Looper.getMainLooper()) {
            override fun handleMessage(msg: Message) {
                if (msg.what == WseVulkanProbeService.MSG_RESULT) {
                    finishProbe(
                        msg.data.getString(WseVulkanProbeService.KEY_RESULT)
                            ?: "ERROR: empty probe result"
                    )
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
                finishProbe(
                    "ERROR: the $driverLabel driver crashed the probe process " +
                        "(native fault while loading or initializing it); the launcher is unaffected"
                )
            }

            override fun onBindingDied(name: ComponentName?) {
                onServiceDisconnected(name)
            }
        }

        _statusLog.value = "Probing $driverLabel Vulkan driver in an isolated process..."
        _driverState.value = _driverState.value.copy(isProbeInProgress = true)
        probeConnection = connection
        val bound = app.bindService(
            Intent(app, WseVulkanProbeService::class.java),
            connection,
            Context.BIND_AUTO_CREATE
        )
        if (!bound) {
            finishProbe("ERROR: cannot start the Vulkan probe service")
            return
        }
        mainHandler.postDelayed(probeTimeout, PROBE_TIMEOUT_MS)
    }

    fun setShowFpsOverlay(enabled: Boolean) {
        if (_showFpsOverlay.value == enabled) {
            return
        }
        prefs().edit().putBoolean(KEY_SHOW_FPS_OVERLAY, enabled).apply()
        _showFpsOverlay.value = enabled
    }

    fun onPlayClicked() {
        if (_scanState.value.inProgress) {
            return
        }
        val treeUri = _installFolder.value.treeUri
        val currentMod = _modulesState.value.selectedModule
        if (treeUri == null || currentMod == null) {
            val why = if (treeUri == null) {
                "No Warband folder is selected."
            } else {
                "No Warband modules were found in the selected folder."
            }
            _events.trySend(LauncherEvent.ShowWarbandFilesNotFound(why))
            return
        }
        val checkStatus = _modulesState.value.installStatuses[currentMod.modulePath]
        if (checkStatus?.state == InstallCheckState.MISSING) {
            _events.trySend(LauncherEvent.ShowMissingFiles(currentMod, checkStatus.missingFiles))
            return
        }
        if (checkStatus?.state != InstallCheckState.READY) {
            return
        }
        _events.trySend(
            LauncherEvent.LaunchGame(
                runModule = true,
                treeUri = treeUri.toString(),
                moduleName = currentMod.folderName,
                gpuDriver = _driverState.value.selectedModeKey,
                showFpsOverlay = _showFpsOverlay.value
            )
        )
    }

    fun onLaunchSelfTestClicked() {
        val selectedMod = _modulesState.value.selectedModule?.folderName ?: "Native"
        _events.trySend(
            LauncherEvent.LaunchGame(
                runModule = false,
                treeUri = _installFolder.value.treeUri?.toString() ?: "",
                moduleName = selectedMod,
                gpuDriver = _driverState.value.selectedModeKey,
                showFpsOverlay = false
            )
        )
    }

    fun setQualityPreset(presetKey: String) {
        val prev = lastCommittedSettings ?: return
        if (presetKey !in QUALITY_PRESET_KEYS || presetKey == prev.qualityPresetKey) {
            return
        }
        val next = prev.copy(qualityPresetKey = presetKey)
        commitSettingsChange(
            nextState = next,
            choicesJson = JSONObject().put("quality_preset", presetKey).toString(),
            refreshGraphics = presetKey != QUALITY_PRESET_CUSTOM
        )
        if (presetKey != QUALITY_PRESET_CUSTOM) {
            updateRglConfig { it.withQualityPreset(presetKey) }
        }
    }

    fun setFrameRateCap(capHz: Int) {
        val prev = lastCommittedSettings ?: return
        val allowed = _settingsState.value.frameRateValues
        if (capHz !in allowed || capHz == prev.frameRateCapHz) {
            return
        }
        val next = prev.copy(
            qualityPresetKey = QUALITY_PRESET_CUSTOM,
            frameRateCapHz = capHz
        )
        val json = JSONObject()
            .put("frame_rate_cap", frameRateCapKey(capHz))
            .put("quality_preset", QUALITY_PRESET_CUSTOM)
            .toString()
        commitSettingsChange(next, json, refreshGraphics = false)
        val mirroredFps = if (capHz <= 0) 240 else capHz.coerceIn(30, 240)
        updateRglConfig { it.copy(maxFramerate = mirroredFps) }
    }

    fun setPresentMode(presentModeKey: String) {
        val prev = lastCommittedSettings ?: return
        if (presentModeKey !in PRESENT_MODE_KEYS || presentModeKey == prev.presentModeKey) {
            return
        }
        val next = prev.copy(
            qualityPresetKey = QUALITY_PRESET_CUSTOM,
            presentModeKey = presentModeKey
        )
        val json = JSONObject()
            .put("present_mode", presentModeKey)
            .put("quality_preset", QUALITY_PRESET_CUSTOM)
            .toString()
        commitSettingsChange(next, json, refreshGraphics = false)
        val vsync = presentModeKey == "fifo" || presentModeKey == "fifo_relaxed"
        updateRglConfig { it.copy(forceVsync = vsync) }
    }

    fun setRenderScalePercent(percent: Int) {
        val prev = lastCommittedSettings ?: return
        val snapped = snapPercent(percent, RENDER_SCALE_MIN, RENDER_SCALE_MAX)
        if (snapped == prev.renderScalePercent) {
            return
        }
        val next = prev.copy(
            qualityPresetKey = QUALITY_PRESET_CUSTOM,
            renderScalePercent = snapped
        )
        val json = JSONObject()
            .put("render_scale", formatScaleDecimal(snapped))
            .put("quality_preset", QUALITY_PRESET_CUSTOM)
            .toString()
        commitSettingsChange(next, json, refreshGraphics = false)
    }

    fun setMaxTextureSize(maxTextureSize: Int) {
        val prev = lastCommittedSettings ?: return
        if (maxTextureSize !in TEXTURE_SIZE_VALUES || maxTextureSize == prev.maxTextureSize) {
            return
        }
        val next = prev.copy(
            qualityPresetKey = QUALITY_PRESET_CUSTOM,
            maxTextureSize = maxTextureSize
        )
        val json = JSONObject()
            .put("max_texture_size", maxTextureSize.toString())
            .put("quality_preset", QUALITY_PRESET_CUSTOM)
            .toString()
        commitSettingsChange(next, json, refreshGraphics = false)
    }

    fun setThermalThrottling(enabled: Boolean) {
        val prev = lastCommittedSettings ?: return
        if (enabled == prev.thermalThrottlingEnabled) {
            return
        }
        val next = prev.copy(
            qualityPresetKey = QUALITY_PRESET_CUSTOM,
            thermalThrottlingEnabled = enabled
        )
        val json = JSONObject()
            .put("thermal_throttling_enabled", enabled.toString())
            .put("quality_preset", QUALITY_PRESET_CUSTOM)
            .toString()
        commitSettingsChange(next, json, refreshGraphics = false)
    }

    fun setDynamicResolution(enabled: Boolean) {
        val prev = lastCommittedSettings ?: return
        if (enabled == prev.dynamicResolutionEnabled) {
            return
        }
        val next = prev.copy(
            qualityPresetKey = QUALITY_PRESET_CUSTOM,
            dynamicResolutionEnabled = enabled
        )
        val json = JSONObject()
            .put("dynamic_resolution_enabled", enabled.toString())
            .put("quality_preset", QUALITY_PRESET_CUSTOM)
            .toString()
        commitSettingsChange(next, json, refreshGraphics = false)
    }

    fun setLodBiasScaling(enabled: Boolean) {
        val prev = lastCommittedSettings ?: return
        if (enabled == prev.lodBiasScalingEnabled) {
            return
        }
        val next = prev.copy(
            qualityPresetKey = QUALITY_PRESET_CUSTOM,
            lodBiasScalingEnabled = enabled
        )
        val json = JSONObject()
            .put("lod_bias_scaling_enabled", enabled.toString())
            .put("quality_preset", QUALITY_PRESET_CUSTOM)
            .toString()
        commitSettingsChange(next, json, refreshGraphics = false)
    }

    fun setUiAspect(uiAspectKey: String) {
        val prev = lastCommittedSettings ?: return
        if (uiAspectKey !in UI_ASPECT_KEYS || uiAspectKey == prev.uiAspectKey) {
            return
        }
        val next = prev.copy(uiAspectKey = uiAspectKey)
        val json = JSONObject().put("ui_aspect", uiAspectKey).toString()
        commitSettingsChange(next, json, refreshGraphics = false)
    }

    fun setUiScalePercent(percent: Int) {
        val prev = lastCommittedSettings ?: return
        val snapped = snapPercent(percent, UI_SCALE_MIN, UI_SCALE_MAX)
        if (snapped == prev.uiScalePercent) {
            return
        }
        val next = prev.copy(uiScalePercent = snapped)
        val json = JSONObject().put("ui_scale", formatScaleDecimal(snapped)).toString()
        commitSettingsChange(next, json, refreshGraphics = false)
    }

    fun setGyroAim(enabled: Boolean) {
        val prev = lastCommittedSettings ?: return
        if (enabled == prev.gyroAimEnabled) {
            return
        }
        val next = prev.copy(gyroAimEnabled = enabled)
        val json = JSONObject().put("gyro_aim_enabled", enabled.toString()).toString()
        commitSettingsChange(next, json, refreshGraphics = false)
    }

    fun setPinchZoom(enabled: Boolean) {
        val prev = lastCommittedSettings ?: return
        if (enabled == prev.pinchZoomEnabled) {
            return
        }
        val next = prev.copy(pinchZoomEnabled = enabled)
        val json = JSONObject().put("pinch_zoom_enabled", enabled.toString()).toString()
        commitSettingsChange(next, json, refreshGraphics = false)
    }

    fun setLanguage(language: String) {
        val prev = lastCommittedSettings ?: return
        if (language !in _settingsState.value.languageOptions || language == prev.language) {
            return
        }
        val next = prev.copy(language = language)
        val json = JSONObject().put("language", language).toString()
        commitSettingsChange(next, json, refreshGraphics = false)
    }

    fun updateRglConfig(transform: (RglConfigState) -> RglConfigState) {
        if (rglLoaded) {
            val current = _settingsState.value
            _settingsState.value = current.copy(rglConfig = transform(current.rglConfig))
        }
        val filesDir = app.filesDir
        settingsExecutor.execute {
            val base = rglOnDisk ?: RglConfigStore.load(filesDir).first
            val updated = transform(base)
            if (updated == base) return@execute
            writeRglConfig(filesDir, updated, okToast = null, failToast = "Could not save rgl_config.txt.")
        }
    }

    fun resetRglConfigToDefaults() {
        val filesDir = app.filesDir
        settingsExecutor.execute {
            val base = rglOnDisk ?: RglConfigStore.load(filesDir).first
            writeRglConfig(
                filesDir,
                RglConfigState(extraEntries = base.extraEntries),
                okToast = "rgl_config.txt restored to defaults.",
                failToast = "Could not reset rgl_config.txt."
            )
        }
    }

    // Executor thread only.
    private fun writeRglConfig(filesDir: File, next: RglConfigState, okToast: String?, failToast: String) {
        val saved = RglConfigStore.save(filesDir, next)
        if (saved) rglOnDisk = next
        val shown = rglOnDisk ?: RglConfigState()
        mainHandler.post {
            if (isCleared.get()) return@post
            val now = _settingsState.value
            // A saved change is already on screen when the file had been read (a slider moved on
            // meanwhile must not jump back); otherwise, or after a failure, show the file's state.
            _settingsState.value = if (saved && rglLoaded) {
                now.copy(rglConfigReady = true)
            } else {
                now.copy(rglConfig = shown, rglConfigReady = saved || now.rglConfigReady)
            }
            val toast = if (saved) okToast else failToast
            if (toast != null) _events.trySend(LauncherEvent.ShowToast(toast))
        }
    }

    private fun restorePreferences() {
        val prefs = prefs()
        _showFpsOverlay.value = prefs.getBoolean(KEY_SHOW_FPS_OVERLAY, false)

        // Stored as the stable mode id; older builds stored the display label.
        val savedDriver = prefs.getString(KEY_GPU_DRIVER, DRIVER_MODE_KEYS.first()) ?: ""
        val driverIdx = DRIVER_MODE_KEYS.indexOf(savedDriver).takeIf { it >= 0 }
            ?: DRIVER_OPTIONS.indexOf(savedDriver).coerceAtLeast(0)
        val modeKey = DRIVER_MODE_KEYS[driverIdx]
        prefs.edit().putString(KEY_GPU_DRIVER, modeKey).apply()
        _driverState.value = _driverState.value.copy(
            selectedIndex = driverIdx,
            statusText = computeDriverStatusText(modeKey)
        )

        restorePerformanceSettings()
        restoreRglConfig()

        val savedTree = prefs.getString(KEY_TREE_URI, null)
        if (!savedTree.isNullOrEmpty()) {
            val uri = Uri.parse(savedTree)
            val quickName = quickTreeDisplayName(uri)
            startScan(uri, quickName, isNewFolderPick = false)
        }
    }

    // config/performance.json is the one saved copy of these settings (native serializer via
    // JNI). Builds that kept them in SharedPreferences are migrated once.
    private fun restorePerformanceSettings() {
        val prefs = prefs()
        val root = configRoot
        val jsonFile = File(root, PERFORMANCE_JSON)
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
        settingsExecutor.execute {
            if (migratedJson != null && !File(root, PERFORMANCE_JSON).isFile) {
                WseSafBridge.nativeSaveLauncherSettings(root, migratedJson)
            }
            val loaded = PerformanceUiState.fromJson(
                WseSafBridge.nativeEnsureFirstLaunchSettings(root)
            )
            mainHandler.post {
                if (isCleared.get() || seq != settingsOpSeq) {
                    return@post
                }
                applyFullSettingsState(loaded)
            }
        }
    }

    private fun restoreRglConfig() {
        val filesDir = app.filesDir
        val path = RglConfigStore.configFile(filesDir).absolutePath
        settingsExecutor.execute {
            val (cfg, source) = RglConfigStore.load(filesDir)
            // Defaults for a missing file are what the exe would write; an unreadable file is kept
            // (save moves it aside) and is not taken as the state on disk.
            if (source == RglConfigSource.LOADED) rglOnDisk = cfg
            mainHandler.post {
                if (isCleared.get()) return@post
                rglLoaded = true
                _settingsState.value = _settingsState.value.copy(
                    rglConfig = cfg,
                    rglConfigReady = source == RglConfigSource.LOADED,
                    rglConfigPath = path
                )
            }
        }
    }

    private fun commitSettingsChange(
        nextState: PerformanceUiState,
        choicesJson: String,
        refreshGraphics: Boolean
    ) {
        lastCommittedSettings = nextState
        _settingsState.value = _settingsState.value.copy(performance = nextState)
        savePerformanceSettings(choicesJson, refreshGraphics)
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
            mainHandler.post {
                if (isCleared.get()) {
                    return@post
                }
                if (!saved) {
                    _events.trySend(LauncherEvent.ShowToast("Could not save settings."))
                    if (seq != settingsOpSeq) {
                        restorePerformanceSettings()
                    } else if (reloaded != null) {
                        settingsNeedsRefresh = false
                        applyFullSettingsState(reloaded)
                    }
                } else if (reloaded != null && seq == settingsOpSeq) {
                    settingsNeedsRefresh = false
                    applyGraphicsSettingsState(reloaded)
                }
            }
        }
    }

    private fun applyFullSettingsState(loaded: PerformanceUiState) {
        val current = _settingsState.value
        val langOptions = buildLanguageOptions(_scannedLanguages.value, loaded.language)
        val snapped = snapToOffered(loaded, current.frameRateValues, langOptions)
        lastCommittedSettings = snapped
        _settingsState.value = current.copy(
            performance = snapped,
            isLoaded = true,
            languageOptions = langOptions
        )
    }

    private fun applyGraphicsSettingsState(reloaded: PerformanceUiState) {
        val current = _settingsState.value
        val prev = lastCommittedSettings ?: run {
            applyFullSettingsState(reloaded)
            return
        }
        val snappedReloaded = snapToOffered(reloaded, current.frameRateValues, current.languageOptions)
        val merged = prev.copy(
            qualityPresetKey = snappedReloaded.qualityPresetKey,
            frameRateCapHz = snappedReloaded.frameRateCapHz,
            presentModeKey = snappedReloaded.presentModeKey,
            renderScalePercent = snappedReloaded.renderScalePercent,
            maxTextureSize = snappedReloaded.maxTextureSize,
            thermalThrottlingEnabled = snappedReloaded.thermalThrottlingEnabled,
            dynamicResolutionEnabled = snappedReloaded.dynamicResolutionEnabled,
            lodBiasScalingEnabled = snappedReloaded.lodBiasScalingEnabled
        )
        lastCommittedSettings = merged
        _settingsState.value = current.copy(
            performance = merged,
            isLoaded = true
        )
    }

    private fun refreshLanguageOptions() {
        val current = _settingsState.value
        val activeLang = lastCommittedSettings?.language ?: "en"
        val options = buildLanguageOptions(_scannedLanguages.value, activeLang)
        val committed = lastCommittedSettings
        if (committed != null) {
            val resolved = if (committed.language in options) committed.language else options.first()
            val updatedPerf = if (resolved != committed.language) {
                committed.copy(language = resolved)
            } else {
                committed
            }
            lastCommittedSettings = updatedPerf
            _settingsState.value = current.copy(
                performance = updatedPerf,
                languageOptions = options
            )
        } else {
            _settingsState.value = current.copy(languageOptions = options)
        }
    }

    private fun startScan(uri: Uri, initialDisplayName: String, isNewFolderPick: Boolean) {
        val currentlyScanning = _scanState.value.inProgress
        if (!currentlyScanning) {
            val prevFolder = _installFolder.value
            val prevModules = _modulesState.value
            preScanSnapshot = ScanSnapshot(
                treeUri = if (isNewFolderPick) prevFolder.treeUri else uri,
                folderDisplayName = if (isNewFolderPick) prevFolder.displayName else initialDisplayName,
                modules = prevModules.modules,
                selectedModuleIndex = prevModules.selectedIndex,
                installStates = HashMap(prevModules.installStatuses),
                languages = _scannedLanguages.value,
                indexStatsText = _scanState.value.indexStatsText
            )
        }

        val gen = scanGeneration.incrementAndGet()
        _installFolder.value = InstallFolderState(treeUri = uri, displayName = initialDisplayName)
        _scanState.value = _scanState.value.copy(
            inProgress = true,
            generation = gen,
            folderLabel = initialDisplayName
        )
        _statusLog.value = "Indexing SAF tree and scanning Modules/*/module.ini..."

        executor.execute {
            if (gen != scanGeneration.get()) {
                return@execute
            }
            val displayName: String
            val rawJson: String
            try {
                displayName = resolveTreeDisplayName(uri)
                val bridge = WseSafBridge(app, uri)
                rawJson = bridge.nativeScanTree()
            } catch (t: Throwable) {
                val reason = t.message?.takeIf { it.isNotBlank() } ?: t.javaClass.simpleName
                mainHandler.post {
                    if (isCleared.get() || gen != scanGeneration.get()) {
                        return@post
                    }
                    finishScanWithError(gen, uri, initialDisplayName, reason)
                }
                return@execute
            }
            mainHandler.post {
                if (isCleared.get() || gen != scanGeneration.get()) {
                    return@post
                }
                completeScan(gen, uri, displayName, rawJson)
            }
        }
    }

    private fun completeScan(gen: Long, uri: Uri, displayName: String, rawJson: String) {
        val trimmed = rawJson.trim()
        if (trimmed.startsWith("ERROR:", ignoreCase = true)) {
            val reason = trimmed.substring(6).trim().ifEmpty { trimmed }
            finishScanWithError(gen, uri, displayName, reason)
            return
        }

        val obj = try {
            JSONObject(trimmed)
        } catch (e: Exception) {
            finishScanWithError(
                gen,
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
            finishScanWithError(gen, uri, displayName, reason)
            return
        }

        preScanSnapshot = null
        prefs().edit().putString(KEY_TREE_URI, uri.toString()).apply()
        _installFolder.value = InstallFolderState(treeUri = uri, displayName = displayName)

        val indexMs = obj.optLong("index_ms", 0L)
        val fileCount = obj.optLong("files", 0L)
        val dirCount = obj.optLong("dirs", 0L)
        _scanState.value = ScanState(
            inProgress = false,
            generation = gen,
            folderLabel = null,
            indexStatsText = "SafVfs index: $fileCount files, $dirCount dirs in $indexMs ms"
        )

        val parsedLanguages = ArrayList<String>()
        val langArr = obj.optJSONArray("languages")
        if (langArr != null) {
            for (i in 0 until langArr.length()) {
                val code = langArr.optString(i, "").trim()
                if (isValidLanguageTag(code) && parsedLanguages.none { it.equals(code, ignoreCase = true) }) {
                    parsedLanguages.add(code)
                }
            }
        }
        _scannedLanguages.value = if (parsedLanguages.isNotEmpty()) parsedLanguages else listOf("en")
        refreshLanguageOptions()

        val parsedModules = ArrayList<ModuleChoice>()
        val installStatuses = HashMap<String, ModuleInstallStatus>()
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
                parsedModules.add(mod)
                installStatuses[mod.modulePath] = ModuleInstallStatus(InstallCheckState.CHECKING)
            }
        }

        val savedModule = prefs().getString(KEY_SELECTED_MODULE, null)
        evictArtForTree(uri)
        if (parsedModules.isNotEmpty()) {
            val savedIdx = if (!savedModule.isNullOrBlank()) {
                parsedModules.indexOfFirst { it.folderName.equals(savedModule, ignoreCase = true) }
            } else {
                -1
            }
            val idx = if (savedIdx >= 0) {
                savedIdx
            } else {
                parsedModules.indexOfFirst { it.folderName.equals("Native", ignoreCase = true) }
                    .coerceAtLeast(0)
            }
            if (savedModule.isNullOrBlank()) {
                prefs().edit().putString(KEY_SELECTED_MODULE, parsedModules[idx].folderName).apply()
            }
            _modulesState.value = ModulesUiState(
                modules = parsedModules,
                selectedIndex = idx,
                installStatuses = installStatuses
            )
            _statusLog.value = "Found ${parsedModules.size} module(s) in $indexMs ms."
            startPendingInstallChecks(uri, gen)
            scheduleArtLoads(uri, gen, parsedModules, idx)
        } else {
            _modulesState.value = ModulesUiState(
                modules = emptyList(),
                selectedIndex = -1,
                installStatuses = emptyMap(),
                emptyMessage = "No Modules/*/module.ini folders found in selected tree."
            )
            _statusLog.value = "Indexed $fileCount file(s), 0 Warband modules."
            scheduleArtLoads(uri, gen, emptyList(), -1)
        }
    }

    private fun finishScanWithError(gen: Long, uri: Uri, displayName: String, reason: String) {
        preScanSnapshot = null
        _installFolder.value = InstallFolderState(treeUri = uri, displayName = displayName)
        _scanState.value = ScanState(
            inProgress = false,
            generation = gen,
            folderLabel = null,
            indexStatsText = "SafVfs index: error"
        )
        _scannedLanguages.value = listOf("en")
        refreshLanguageOptions()
        _modulesState.value = ModulesUiState(
            modules = emptyList(),
            selectedIndex = -1,
            installStatuses = emptyMap(),
            emptyMessage = "No modules available."
        )
        _moduleArt.value = emptyMap()
        _heroArt.value = null
        setSeedColor(DefaultSeedColorArgb)
        _statusLog.value = "ERROR: $reason"
        _events.trySend(LauncherEvent.ShowScanError(reason))
    }

    private fun evictArtForTree(uri: Uri) {
        val prefix = uri.toString()
        for (key in artCache.snapshot().keys) {
            if (key.treeUri == prefix) {
                artCache.remove(key)
            }
        }
        inFlightArtKeys.removeAll { it.treeUri == prefix }
    }

    private fun scheduleArtLoads(
        uri: Uri,
        gen: Long,
        modules: List<ModuleChoice>,
        selectedIndex: Int
    ) {
        _moduleArt.value = emptyMap()
        val selectedMod = modules.getOrNull(selectedIndex)
        if (selectedMod != null) {
            selectHeroArtForModule(uri, gen, selectedMod.modulePath)
            loadModuleArtIfNeeded(uri, gen, "", HERO_ART_MAX_SIDE)
            for (mod in modules) {
                if (mod.modulePath != selectedMod.modulePath) {
                    loadModuleArtIfNeeded(uri, gen, mod.modulePath, CARD_ART_MAX_SIDE)
                }
            }
        } else {
            scheduleHeroFallback(uri, gen)
        }
    }

    private fun scheduleHeroFallback(uri: Uri, gen: Long) {
        val cachedRoot = artCache.get(ArtCacheKey(uri.toString(), "", HERO_ART_MAX_SIDE))
        if (cachedRoot != null) {
            _heroArt.value = cachedRoot
            _moduleArt.value = _moduleArt.value + ("" to cachedRoot)
            setSeedColor(cachedRoot.seedArgb)
        } else {
            _heroArt.value = null
            setSeedColor(DefaultSeedColorArgb)
            loadModuleArtIfNeeded(uri, gen, "", HERO_ART_MAX_SIDE)
        }
    }

    private fun selectHeroArtForModule(uri: Uri, gen: Long, moduleDir: String) {
        val uriStr = uri.toString()
        val cachedHero = artCache.get(ArtCacheKey(uriStr, moduleDir, HERO_ART_MAX_SIDE))
        if (cachedHero != null) {
            _heroArt.value = cachedHero
            _moduleArt.value = _moduleArt.value + (moduleDir to cachedHero)
            setSeedColor(cachedHero.seedArgb)
            return
        }
        val cardArt = artCache.get(ArtCacheKey(uriStr, moduleDir, CARD_ART_MAX_SIDE))
            ?: _moduleArt.value[moduleDir]
        if (cardArt != null) {
            _heroArt.value = cardArt
            _moduleArt.value = _moduleArt.value + (moduleDir to cardArt)
            setSeedColor(cardArt.seedArgb)
        } else {
            _heroArt.value = null
            setSeedColor(DefaultSeedColorArgb)
        }
        loadModuleArtIfNeeded(uri, gen, moduleDir, HERO_ART_MAX_SIDE)
    }

    private fun loadModuleArtIfNeeded(
        uri: Uri,
        gen: Long,
        moduleDir: String,
        maxSide: Int
    ) {
        val key = ArtCacheKey(uri.toString(), moduleDir, maxSide)
        val cached = artCache.get(key)
        if (cached != null) {
            applyLoadedArt(moduleDir, maxSide, cached)
            return
        }
        if (!inFlightArtKeys.add(key)) {
            return
        }
        val relPath = moduleDir  // the JNI side appends "/main.bmp" ("" = install root)
        executor.execute {
            val art = try {
                val bridge = WseSafBridge(app, uri)
                val raw = bridge.nativeLoadModuleArt(relPath, maxSide)
                decodeModuleArtIntArray(raw, DefaultSeedColorArgb)
            } catch (_: Throwable) {
                ModuleArt(bitmap = null, seedArgb = DefaultSeedColorArgb)
            }
            mainHandler.post {
                inFlightArtKeys.remove(key)
                if (isCleared.get() || gen != scanGeneration.get() || _installFolder.value.treeUri != uri) {
                    return@post
                }
                artCache.put(key, art)
                applyLoadedArt(moduleDir, maxSide, art)
            }
        }
    }

    private fun applyLoadedArt(moduleDir: String, maxSide: Int, art: ModuleArt) {
        val existingMap = _moduleArt.value
        if (maxSide == HERO_ART_MAX_SIDE || !existingMap.containsKey(moduleDir)) {
            _moduleArt.value = existingMap + (moduleDir to art)
        }
        val selectedDir = _modulesState.value.selectedModule?.modulePath
        val isActiveHero = if (selectedDir == null) {
            moduleDir.isEmpty()
        } else {
            selectedDir == moduleDir
        }
        if (isActiveHero) {
            if (maxSide == HERO_ART_MAX_SIDE || _heroArt.value == null) {
                _heroArt.value = art
            }
            setSeedColor(art.seedArgb)
        }
    }

    private fun startPendingInstallChecks(uri: Uri, gen: Long) {
        val current = _modulesState.value
        val pendingPaths = current.modules
            .filter {
                val state = current.installStatuses[it.modulePath]?.state
                state == null || state == InstallCheckState.CHECKING
            }
            .map { it.modulePath }
        for (modulePath in pendingPaths) {
            executor.execute {
                if (gen != scanGeneration.get()) {
                    return@execute
                }
                val result = runInstallCheck(uri, modulePath)
                mainHandler.post {
                    if (isCleared.get() || gen != scanGeneration.get()) {
                        return@post
                    }
                    val latest = _modulesState.value
                    val updatedMap = HashMap(latest.installStatuses)
                    updatedMap[modulePath] = result
                    _modulesState.value = latest.copy(installStatuses = updatedMap)
                }
            }
        }
    }

    private fun runInstallCheck(uri: Uri, modulePath: String): ModuleInstallStatus {
        return try {
            val bridge = WseSafBridge(app, uri)
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

    private fun computeDriverStatusText(mode: String): String {
        if (mode == "system") {
            return "Uses the device's built-in Vulkan driver."
        }
        val prefs = prefs()
        val path = prefs.getString(KEY_DRIVER_PATH_PREFIX + mode, "") ?: ""
        val name = prefs.getString(KEY_DRIVER_NAME_PREFIX + mode, "") ?: ""
        return if (path.isNotEmpty() && File(path).isFile) {
            "Imported: $name"
        } else {
            "No $mode driver imported yet."
        }
    }

    // Reads meta.json and the library it names out of an AdrenoTools-style zip,
    // stages them natively (ELF check + libhardware.so patch) under app storage,
    // and marks the ICD read-only. dlopen needs a real file path, not a SAF URI.
    private fun stageDriverPackage(uri: Uri, selectedMode: String): String {
        var metaJson: String? = null
        val libraries = HashMap<String, ByteArray>()
        var total = 0L
        app.contentResolver.openInputStream(uri)?.use { raw ->
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

        val root = File(app.noBackupFilesDir, "gpu_drivers")
        val staging = File(root, "import")
        staging.deleteRecursively()
        if (!staging.mkdirs()) {
            return "ERROR: cannot create ${staging.path}"
        }
        val bridge = WseSafBridge(app, Uri.EMPTY)
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

    private fun applyDriverImportResult(result: String) {
        val parts = result.split('\t')
        if (parts.size < 5 || parts[0] != "OK") {
            _statusLog.value = result
            return
        }
        val mode = parts[1]
        val editor = prefs()
            .edit()
            .putString(KEY_DRIVER_PATH_PREFIX + mode, parts[2])
            .putString(KEY_DRIVER_NAME_PREFIX + mode, parts[3])
        val idx = DRIVER_MODE_KEYS.indexOf(mode)
        if (idx >= 0) {
            editor.putString(KEY_GPU_DRIVER, DRIVER_MODE_KEYS[idx])
        }
        editor.apply()
        val current = _driverState.value
        val nextIdx = if (idx >= 0) idx else current.selectedIndex
        val activeMode = DRIVER_MODE_KEYS[nextIdx.coerceIn(0, DRIVER_MODE_KEYS.lastIndex)]
        _driverState.value = current.copy(
            selectedIndex = nextIdx,
            statusText = computeDriverStatusText(activeMode)
        )
        _statusLog.value =
            "Imported ${parts[3]} as $mode (libhardware.so references patched: ${parts[4]})."
    }

    // Unbinding ends the probe process (WseVulkanProbeService.onUnbind exits it).
    // `result == null` means the ViewModel is clearing: clean up silently.
    private fun finishProbe(result: String?) {
        val connection = probeConnection ?: return
        probeConnection = null
        mainHandler.removeCallbacks(probeTimeout)
        try {
            app.unbindService(connection)
        } catch (_: IllegalArgumentException) {
            // Already unbound.
        }
        if (result != null && !isCleared.get()) {
            _statusLog.value = result
            _driverState.value = _driverState.value.copy(isProbeInProgress = false)
        }
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
                app.contentResolver.query(
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
    private fun maxDisplayRefreshHz(dm: DisplayManager?): Int {
        val display = dm?.getDisplay(Display.DEFAULT_DISPLAY) ?: return 60
        val max = display.supportedModes?.maxOfOrNull { it.refreshRate } ?: return 60
        return Math.round(max)
    }

    private fun prefs() = app.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    private val configRoot: String
        get() = app.filesDir.absolutePath

    companion object {
        val DRIVER_OPTIONS = LauncherDefaults.DRIVER_OPTIONS
        val DRIVER_MODE_KEYS = LauncherDefaults.DRIVER_MODE_KEYS
        val ALL_FRAME_RATE_VALUES = LauncherDefaults.ALL_FRAME_RATE_VALUES

        const val QUALITY_PRESET_CUSTOM = LauncherDefaults.QUALITY_PRESET_CUSTOM
        val QUALITY_PRESET_LABELS = LauncherDefaults.QUALITY_PRESET_LABELS
        val QUALITY_PRESET_KEYS = LauncherDefaults.QUALITY_PRESET_KEYS

        val PRESENT_MODE_KEYS = LauncherDefaults.PRESENT_MODE_KEYS
        val PRESENT_MODE_LABELS = LauncherDefaults.PRESENT_MODE_LABELS

        val TEXTURE_SIZE_LABELS = LauncherDefaults.TEXTURE_SIZE_LABELS
        val TEXTURE_SIZE_VALUES = LauncherDefaults.TEXTURE_SIZE_VALUES

        val UI_ASPECT_LABELS = LauncherDefaults.UI_ASPECT_LABELS
        val UI_ASPECT_KEYS = LauncherDefaults.UI_ASPECT_KEYS

        const val RENDER_SCALE_MIN = LauncherDefaults.RENDER_SCALE_MIN
        const val RENDER_SCALE_MAX = LauncherDefaults.RENDER_SCALE_MAX
        const val UI_SCALE_MIN = LauncherDefaults.UI_SCALE_MIN
        const val UI_SCALE_MAX = LauncherDefaults.UI_SCALE_MAX
        const val SCALE_STEP = LauncherDefaults.SCALE_STEP

        private const val ART_CACHE_MAX_ENTRIES = 24
        private const val MAX_DRIVER_PACKAGE_BYTES = 256L * 1024L * 1024L
        private const val PROBE_TIMEOUT_MS = 60_000L
        private const val PREFS_NAME = "wse2_launcher"
        private const val KEY_TREE_URI = "tree_uri"
        private const val KEY_SELECTED_MODULE = "selected_module"
        private const val KEY_GPU_DRIVER = "gpu_driver"
        private const val KEY_DRIVER_PATH_PREFIX = "driver_path_"
        private const val KEY_DRIVER_NAME_PREFIX = "driver_name_"
        private const val KEY_SHOW_FPS_OVERLAY = "show_fps_overlay"
        private const val KEY_FRAME_RATE_CAP = "frame_rate_cap_hz"
        private const val KEY_GYRO_AIM = "gyro_aim_enabled"
        private const val KEY_PINCH_ZOOM = "pinch_zoom_enabled"
        private const val KEY_THERMAL_THROTTLE = "thermal_throttling_enabled"
        private const val PERFORMANCE_JSON = "config/performance.json"

        fun frameRateLabel(hz: Int): String = LauncherDefaults.frameRateLabel(hz)

        fun snapPercent(percent: Int, minPct: Int, maxPct: Int): Int =
            LauncherDefaults.snapPercent(percent, minPct, maxPct)

        private fun frameRateCapKey(hz: Int): String = if (hz <= 0) "uncapped" else hz.toString()

        private fun formatScaleDecimal(percent: Int): String {
            val whole = percent / 100
            val frac = percent % 100
            return when {
                frac == 0 -> whole.toString()
                frac % 10 == 0 -> "$whole.${frac / 10}"
                else -> String.format(Locale.US, "%d.%02d", whole, frac)
            }
        }

        private fun isValidLanguageTag(tag: String): Boolean =
            LauncherDefaults.isValidLanguageTag(tag)

        private fun buildLanguageOptions(
            scanned: List<String>,
            activeLanguage: String
        ): List<String> {
            val base = if (scanned.isNotEmpty()) scanned else listOf("en")
            val merged = ArrayList<String>(base.size + 1)
            for (lang in base) {
                if (lang.isNotEmpty() && !merged.contains(lang)) {
                    merged.add(lang)
                }
            }
            if (activeLanguage.isNotEmpty() && !merged.contains(activeLanguage)) {
                merged.add(activeLanguage)
            }
            if (merged.isEmpty()) {
                merged.add("en")
            }
            return merged
        }

        private fun snapToOffered(
            state: PerformanceUiState,
            frameRateValues: List<Int>,
            languageOptions: List<String>
        ): PerformanceUiState {
            val cap = if (state.frameRateCapHz in frameRateValues) {
                state.frameRateCapHz
            } else {
                frameRateValues.filter { it in 1..state.frameRateCapHz }.maxOrNull()
                    ?: frameRateValues.firstOrNull { it > 0 }
                    ?: 0
            }
            val tex = if (state.maxTextureSize in TEXTURE_SIZE_VALUES) {
                state.maxTextureSize
            } else if (state.maxTextureSize <= 0) {
                0
            } else {
                TEXTURE_SIZE_VALUES.filter { it in 1..state.maxTextureSize }.maxOrNull() ?: 0
            }
            val preset = if (state.qualityPresetKey in QUALITY_PRESET_KEYS) {
                state.qualityPresetKey
            } else {
                QUALITY_PRESET_CUSTOM
            }
            val mode = if (state.presentModeKey in PRESENT_MODE_KEYS) {
                state.presentModeKey
            } else {
                PRESENT_MODE_KEYS.first()
            }
            val aspect = if (state.uiAspectKey in UI_ASPECT_KEYS) {
                state.uiAspectKey
            } else {
                UI_ASPECT_KEYS.first()
            }
            val lang = if (state.language in languageOptions) {
                state.language
            } else {
                languageOptions.firstOrNull() ?: "en"
            }
            return state.copy(
                qualityPresetKey = preset,
                frameRateCapHz = cap,
                presentModeKey = mode,
                renderScalePercent = snapPercent(
                    state.renderScalePercent,
                    RENDER_SCALE_MIN,
                    RENDER_SCALE_MAX
                ),
                maxTextureSize = tex,
                uiAspectKey = aspect,
                uiScalePercent = snapPercent(state.uiScalePercent, UI_SCALE_MIN, UI_SCALE_MAX),
                language = lang
            )
        }
    }
}
