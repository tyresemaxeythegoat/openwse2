package org.wse2.android

import android.app.Activity
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
import android.view.Display
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.TextView
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.InputStream
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.zip.ZipInputStream

class WseLauncherActivity : Activity() {

    private data class ModuleChoice(
        val folderName: String,
        val modulePath: String,
        val displayName: String,
        val hasScriptsTxt: Boolean,
        val loadResourceCount: Int,
        val loadModResourceCount: Int
    ) {
        override fun toString(): String =
            if (displayName == folderName) {
                folderName
            } else {
                "$displayName ($folderName)"
            }
    }

    private lateinit var folderStatusText: TextView
    private lateinit var indexStatsText: TextView
    private lateinit var moduleSpinner: Spinner
    private lateinit var moduleDetailText: TextView
    private lateinit var driverSpinner: Spinner
    private lateinit var driverStatusText: TextView
    private lateinit var frameRateSpinner: Spinner
    private lateinit var gyroAimCheck: CheckBox
    private lateinit var pinchZoomCheck: CheckBox
    private lateinit var thermalThrottleCheck: CheckBox
    private lateinit var presentModeSpinner: Spinner
    private lateinit var dynamicResolutionCheck: CheckBox
    private lateinit var lodBiasScalingCheck: CheckBox
    private lateinit var mockFallbackCheck: CheckBox
    private lateinit var outputStatusText: TextView
    private lateinit var rescanButton: Button
    private lateinit var verifyScriptsButton: Button
    private lateinit var probeButton: Button

    private val modules = ArrayList<ModuleChoice>()
    private val driverOptions = listOf("System Vulkan", "Turnip (Adreno custom)", "PanVK (Mali direct ICD)")
    // Native `parse_driver_mode` keys, index-aligned with driverOptions.
    private val driverModeKeys = listOf("system", "turnip", "panvk")
    // Caps offered on this device: `allFrameRateValues` filtered to what the display can
    // refresh at (a 120 Hz cap on a 60 Hz panel only wastes power), index-aligned labels.
    private val allFrameRateValues = listOf(30, 60, 90, 120, 0)
    private var frameRateValues = allFrameRateValues
    private var frameRateLabels = allFrameRateValues.map { frameRateLabel(it) }
    private val executor: ExecutorService = Executors.newSingleThreadExecutor()
    private val mainHandler = Handler(Looper.getMainLooper())
    private var probeConnection: ServiceConnection? = null

    private var selectedTreeUri: Uri? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        val dp = resources.displayMetrics.density
        fun px(v: Int): Int = (v * dp).toInt()

        val rootScroll = ScrollView(this)
        val container = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(px(16), px(20), px(16), px(24))
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
        rootScroll.addView(container)

        fun addSectionTitle(text: String) {
            container.addView(TextView(this).apply {
                this.text = text
                textSize = 15f
                setPadding(0, px(12), 0, px(4))
            })
        }

        container.addView(TextView(this).apply {
            text = "WSE2 Android Launcher (Slices P2 + P4)"
            textSize = 20f
            setPadding(0, 0, 0, px(8))
        })

        addSectionTitle("Warband Folder (SAF)")
        folderStatusText = TextView(this).apply {
            text = "No Warband folder selected."
            textSize = 13f
            setPadding(0, 0, 0, px(6))
        }
        container.addView(folderStatusText)

        container.addView(Button(this).apply {
            text = "Select Warband Folder"
            setOnClickListener { openWarbandFolderPicker() }
        })

        rescanButton = Button(this).apply {
            text = "Rescan Folder & Rebuild SafVfs Index"
            isEnabled = false
            setOnClickListener {
                val uri = selectedTreeUri
                if (uri != null) {
                    scanTree(uri)
                }
            }
        }
        container.addView(rescanButton)

        indexStatsText = TextView(this).apply {
            text = "SafVfs index: idle"
            textSize = 13f
            setPadding(0, px(4), 0, px(4))
        }
        container.addView(indexStatsText)

        addSectionTitle("Module (Modules/*/module.ini)")
        moduleSpinner = Spinner(this)
        container.addView(moduleSpinner)

        moduleDetailText = TextView(this).apply {
            text = "Select a Warband folder to discover modules."
            textSize = 13f
            setPadding(0, px(4), 0, px(6))
        }
        container.addView(moduleDetailText)

        verifyScriptsButton = Button(this).apply {
            text = "Verify scripts.txt via SafVfs"
            isEnabled = false
            setOnClickListener { verifySelectedModuleScripts() }
        }
        container.addView(verifyScriptsButton)

        addSectionTitle("GPU Driver")
        driverSpinner = Spinner(this).apply {
            adapter = ArrayAdapter(
                this@WseLauncherActivity,
                android.R.layout.simple_spinner_dropdown_item,
                driverOptions
            )
        }
        container.addView(driverSpinner)

        driverStatusText = TextView(this).apply {
            text = ""
            textSize = 13f
            setPadding(0, px(4), 0, px(4))
        }
        container.addView(driverStatusText)

        container.addView(Button(this).apply {
            text = "Import Turnip / PanVK Driver Package (.zip)"
            setOnClickListener { openDriverPackagePicker() }
        })

        mockFallbackCheck = CheckBox(this).apply {
            text = "Allow mock ICD fallback (testing only)"
            isChecked = false
        }
        container.addView(mockFallbackCheck)

        probeButton = Button(this).apply {
            text = "Probe Vulkan Driver (CPU reference frame)"
            setOnClickListener { probeVulkanDriver() }
        }
        container.addView(probeButton)

        addSectionTitle("Sensors & Performance (P8)")
        val maxRefreshHz = maxDisplayRefreshHz()
        frameRateValues = allFrameRateValues.filter { it == 0 || it <= maxRefreshHz + 1 }
        frameRateLabels = frameRateValues.map { frameRateLabel(it) }
        frameRateSpinner = Spinner(this).apply {
            adapter = ArrayAdapter(
                this@WseLauncherActivity,
                android.R.layout.simple_spinner_dropdown_item,
                frameRateLabels
            )
        }
        container.addView(frameRateSpinner)

        val sensorManager = getSystemService(Context.SENSOR_SERVICE) as? SensorManager
        val hasPhoneGyro = sensorManager?.getDefaultSensor(Sensor.TYPE_GYROSCOPE) != null
        gyroAimCheck = CheckBox(this).apply {
            // Controller gyros (DualSense, Switch Pro) work without a phone gyroscope.
            text = if (hasPhoneGyro) {
                "Opt-in gyro aiming (Mission)"
            } else {
                "Opt-in gyro aiming (Mission) - no phone gyroscope, controller gyro only"
            }
            isChecked = false
        }
        container.addView(gyroAimCheck)

        pinchZoomCheck = CheckBox(this).apply {
            text = "Pinch-to-zoom (Campaign map)"
            isChecked = true
        }
        container.addView(pinchZoomCheck)

        thermalThrottleCheck = CheckBox(this).apply {
            text = "Thermal headroom throttling (res / LOD / FPS)"
            isChecked = true
        }
        container.addView(thermalThrottleCheck)

        presentModeSpinner = Spinner(this).apply {
            adapter = ArrayAdapter(
                this@WseLauncherActivity,
                android.R.layout.simple_spinner_dropdown_item,
                presentModeLabels
            )
        }
        container.addView(presentModeSpinner)

        dynamicResolutionCheck = CheckBox(this).apply {
            text = "Dynamic resolution under thermal load"
            isChecked = true
        }
        container.addView(dynamicResolutionCheck)

        lodBiasScalingCheck = CheckBox(this).apply {
            text = "LOD bias scaling under thermal load"
            isChecked = true
        }
        container.addView(lodBiasScalingCheck)

        addSectionTitle("Runtime")
        container.addView(Button(this).apply {
            text = "Launch WSE2 SDL3 Self-Test (WseActivity)"
            setOnClickListener { launchSelfTestActivity() }
        })
        container.addView(Button(this).apply {
            text = "Run Selected Module (UI shell)"
            setOnClickListener { launchSelfTestActivity(runModule = true) }
        })

        addSectionTitle("Status Log")
        outputStatusText = TextView(this).apply {
            text = "Ready."
            textSize = 12f
        }
        container.addView(outputStatusText)

        setContentView(rootScroll)
        restorePreferences()
    }

    private val configRoot: String
        get() = filesDir.absolutePath

    // config/performance.json is the one saved copy of these settings (native serializer via
    // JNI). Builds that kept them in SharedPreferences are migrated once.
    private fun restorePerformanceSettings() {
        val prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        val json = File(configRoot, PERFORMANCE_JSON)
        if (!json.isFile && prefs.contains(KEY_FRAME_RATE_CAP)) {
            WseSafBridge.nativeSavePerformanceSettings(
                configRoot,
                prefs.getInt(KEY_FRAME_RATE_CAP, 60),
                "fifo",
                prefs.getBoolean(KEY_THERMAL_THROTTLE, true),
                true,
                true,
                prefs.getBoolean(KEY_GYRO_AIM, false),
                prefs.getBoolean(KEY_PINCH_ZOOM, true)
            )
        }
        prefs.edit()
            .remove(KEY_FRAME_RATE_CAP)
            .remove(KEY_GYRO_AIM)
            .remove(KEY_PINCH_ZOOM)
            .remove(KEY_THERMAL_THROTTLE)
            .apply()

        val stored = try {
            JSONObject(WseSafBridge.nativeLoadPerformanceSettings(configRoot))
        } catch (_: Exception) {
            JSONObject()
        }
        val capKey = stored.optString("frame_rate_cap", "60")
        val savedCapHz = if (capKey == "uncapped") 0 else capKey.toIntOrNull() ?: 60
        // A cap saved on another display (or before a panel mode change) falls back to the
        // highest offered rate below it.
        val capIdx = frameRateValues.indexOf(savedCapHz).let { exact ->
            if (exact >= 0) {
                exact
            } else {
                val best = frameRateValues.filter { it in 1..savedCapHz }.maxOrNull() ?: 60
                frameRateValues.indexOf(best).coerceAtLeast(0)
            }
        }
        frameRateSpinner.setSelection(capIdx)
        presentModeSpinner.setSelection(
            presentModeKeys.indexOf(stored.optString("present_mode", "fifo")).coerceAtLeast(0)
        )
        fun flag(key: String, fallback: Boolean) =
            stored.optString(key, fallback.toString()).let { it == "true" || it == "1" }
        gyroAimCheck.isChecked = flag("gyro_aim_enabled", false)
        pinchZoomCheck.isChecked = flag("pinch_zoom_enabled", true)
        thermalThrottleCheck.isChecked = flag("thermal_throttling_enabled", true)
        dynamicResolutionCheck.isChecked = flag("dynamic_resolution_enabled", true)
        lodBiasScalingCheck.isChecked = flag("lod_bias_scaling_enabled", true)

        val spinnerListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, pos: Int, id: Long) {
                savePerformanceSettings()
            }

            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }
        frameRateSpinner.onItemSelectedListener = spinnerListener
        presentModeSpinner.onItemSelectedListener = spinnerListener
        for (check in listOf(
            gyroAimCheck, pinchZoomCheck, thermalThrottleCheck, dynamicResolutionCheck,
            lodBiasScalingCheck
        )) {
            check.setOnCheckedChangeListener { _, _ -> savePerformanceSettings() }
        }
    }

    private fun savePerformanceSettings() {
        val modeIdx = presentModeSpinner.selectedItemPosition.coerceIn(0, presentModeKeys.size - 1)
        val ok = WseSafBridge.nativeSavePerformanceSettings(
            configRoot,
            selectedFrameRateCapHz(),
            presentModeKeys[modeIdx],
            thermalThrottleCheck.isChecked,
            dynamicResolutionCheck.isChecked,
            lodBiasScalingCheck.isChecked,
            gyroAimCheck.isChecked,
            pinchZoomCheck.isChecked
        )
        if (!ok) {
            outputStatusText.text = "ERROR: could not save $PERFORMANCE_JSON"
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        finishProbe(null)
        executor.shutdownNow()
    }

    private fun restorePreferences() {
        val prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        // Stored as the stable mode id; older builds stored the display label.
        val savedDriver = prefs.getString(KEY_GPU_DRIVER, driverModeKeys.first()) ?: ""
        val driverIdx = driverModeKeys.indexOf(savedDriver).takeIf { it >= 0 }
            ?: driverOptions.indexOf(savedDriver).coerceAtLeast(0)
        driverSpinner.setSelection(driverIdx)
        prefs.edit().putString(KEY_GPU_DRIVER, driverModeKeys[driverIdx]).apply()
        updateDriverStatus()
        driverSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(parent: AdapterView<*>?, view: View?, pos: Int, id: Long) {
                prefs.edit().putString(KEY_GPU_DRIVER, driverModeKeys[pos]).apply()
                updateDriverStatus()
            }

            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }

        restorePerformanceSettings()

        val savedTree = prefs.getString(KEY_TREE_URI, null)
        if (!savedTree.isNullOrEmpty()) {
            val uri = Uri.parse(savedTree)
            selectedTreeUri = uri
            folderStatusText.text = "Tree URI: $savedTree"
            rescanButton.isEnabled = true
            scanTree(uri)
        }
    }

    @Suppress("DEPRECATION")
    private fun openWarbandFolderPicker() {
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

        selectedTreeUri = uri
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
            .edit()
            .putString(KEY_TREE_URI, uri.toString())
            .apply()
        folderStatusText.text = "Tree URI: $uri"
        rescanButton.isEnabled = true
        scanTree(uri)
    }

    private fun scanTree(uri: Uri) {
        outputStatusText.text = "Indexing SAF tree and scanning Modules/*/module.ini..."
        verifyScriptsButton.isEnabled = false
        executor.execute {
            val bridge = WseSafBridge(applicationContext, uri)
            val rawJson = bridge.nativeScanTree()
            runOnUiThread {
                if (isFinishing || isDestroyed) {
                    return@runOnUiThread
                }
                applyScanJson(rawJson)
            }
        }
    }

    private fun applyScanJson(rawJson: String) {
        try {
            val obj = JSONObject(rawJson)
            if (!obj.optBoolean("ok", false)) {
                val err = obj.optString("error", "unknown scan failure")
                indexStatsText.text = "SafVfs index: error"
                moduleDetailText.text = "Scan failed: $err"
                outputStatusText.text = "ERROR: $err"
                return
            }

            val indexMs = obj.optLong("index_ms", 0L)
            val fileCount = obj.optLong("files", 0L)
            val dirCount = obj.optLong("dirs", 0L)
            indexStatsText.text = "SafVfs index: $fileCount files, $dirCount dirs in $indexMs ms"

            modules.clear()
            val arr = obj.optJSONArray("modules")
            if (arr != null) {
                for (i in 0 until arr.length()) {
                    val m = arr.getJSONObject(i)
                    modules.add(
                        ModuleChoice(
                            folderName = m.optString("folder_name", ""),
                            modulePath = m.optString("module_path", ""),
                            displayName = m.optString("display_name", ""),
                            hasScriptsTxt = m.optBoolean("has_scripts_txt", false),
                            loadResourceCount = m.optInt("load_resource_count", 0),
                            loadModResourceCount = m.optInt("load_mod_resource_count", 0)
                        )
                    )
                }
            }

            val prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
            val savedModule = prefs.getString(KEY_SELECTED_MODULE, "Native")
            val adapter = ArrayAdapter(
                this,
                android.R.layout.simple_spinner_dropdown_item,
                modules
            )
            moduleSpinner.adapter = adapter
            if (modules.isNotEmpty()) {
                val idx = modules.indexOfFirst {
                    it.folderName.equals(savedModule, ignoreCase = true)
                }.coerceAtLeast(0)
                moduleSpinner.setSelection(idx)
                updateSelectedModuleUi(idx)
                moduleSpinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                    override fun onItemSelected(
                        parent: AdapterView<*>?,
                        view: View?,
                        pos: Int,
                        id: Long
                    ) {
                        updateSelectedModuleUi(pos)
                    }

                    override fun onNothingSelected(parent: AdapterView<*>?) {}
                }
                outputStatusText.text = "Found ${modules.size} module(s) in $indexMs ms."
            } else {
                moduleDetailText.text = "No Modules/*/module.ini folders found in selected tree."
                verifyScriptsButton.isEnabled = false
                outputStatusText.text = "Indexed $fileCount file(s), 0 Warband modules."
            }
        } catch (e: Exception) {
            outputStatusText.text = "JSON parse error: ${e.message}"
        }
    }

    private fun updateSelectedModuleUi(position: Int) {
        if (position !in modules.indices) {
            verifyScriptsButton.isEnabled = false
            return
        }
        val m = modules[position]
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
            .edit()
            .putString(KEY_SELECTED_MODULE, m.folderName)
            .apply()
        val scriptsLabel = if (m.hasScriptsTxt) "scripts.txt present" else "scripts.txt missing"
        moduleDetailText.text =
            "${m.modulePath} | $scriptsLabel | BRFs: ${m.loadResourceCount} common + ${m.loadModResourceCount} mod"
        verifyScriptsButton.isEnabled = m.hasScriptsTxt && selectedTreeUri != null
    }

    private fun verifySelectedModuleScripts() {
        val uri = selectedTreeUri ?: return
        val pos = moduleSpinner.selectedItemPosition
        if (pos !in modules.indices) {
            return
        }
        val mod = modules[pos]
        outputStatusText.text = "Verifying ${mod.modulePath}/scripts.txt via SafVfs..."
        executor.execute {
            val bridge = WseSafBridge(applicationContext, uri)
            val result = bridge.nativeVerifyModule(mod.modulePath)
            runOnUiThread {
                if (!isFinishing && !isDestroyed) {
                    outputStatusText.text = result
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

    private fun selectedFrameRateCapHz(): Int =
        frameRateValues[frameRateSpinner.selectedItemPosition.coerceIn(0, frameRateValues.size - 1)]

    private fun selectedDriverModeKey(): String =
        driverModeKeys[driverSpinner.selectedItemPosition.coerceIn(0, driverModeKeys.size - 1)]

    private fun updateDriverStatus() {
        val mode = selectedDriverModeKey()
        if (mode == "system") {
            driverStatusText.text = "Uses the device's built-in Vulkan driver."
            return
        }
        val prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        val path = prefs.getString(KEY_DRIVER_PATH_PREFIX + mode, "") ?: ""
        val name = prefs.getString(KEY_DRIVER_NAME_PREFIX + mode, "") ?: ""
        driverStatusText.text = if (path.isNotEmpty() && File(path).isFile) {
            "Imported: $name"
        } else {
            "No $mode driver imported yet."
        }
    }

    private fun importDriverPackage(uri: Uri) {
        val selectedMode = selectedDriverModeKey()
        outputStatusText.text = "Importing GPU driver package..."
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
            outputStatusText.text = result
            return
        }
        val mode = parts[1]
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
            .edit()
            .putString(KEY_DRIVER_PATH_PREFIX + mode, parts[2])
            .putString(KEY_DRIVER_NAME_PREFIX + mode, parts[3])
            .apply()
        val idx = driverModeKeys.indexOf(mode)
        if (idx >= 0) {
            driverSpinner.setSelection(idx)
        }
        updateDriverStatus()
        outputStatusText.text =
            "Imported ${parts[3]} as $mode (libhardware.so references patched: ${parts[4]})."
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
            putBoolean(WseVulkanProbeService.KEY_ALLOW_MOCK, mockFallbackCheck.isChecked)
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

        outputStatusText.text = "Probing $driver Vulkan driver in an isolated process..."
        probeButton.isEnabled = false
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
            outputStatusText.text = result
            probeButton.isEnabled = true
        }
    }

    // `runModule`: WseActivity runs the selected module's UI from the picked tree
    // (`--saf-module`) instead of the self-test.
    private fun launchSelfTestActivity(runModule: Boolean = false) {
        val selectedPos = moduleSpinner.selectedItemPosition
        val selectedMod = if (selectedPos in modules.indices) modules[selectedPos].folderName else "Native"
        if (runModule && (selectedTreeUri == null || selectedPos !in modules.indices)) {
            outputStatusText.text = "Pick a Warband folder and a module first."
            return
        }
        val intent = Intent(this, WseActivity::class.java).apply {
            putExtra(EXTRA_RUN_MODULE, runModule)
            putExtra(EXTRA_TREE_URI, selectedTreeUri?.toString() ?: "")
            putExtra(EXTRA_MODULE_NAME, selectedMod)
            // Stable mode id ("system" / "turnip" / "panvk"), as parse_driver_mode expects.
            putExtra(EXTRA_GPU_DRIVER, selectedDriverModeKey())
        }
        startActivity(intent)
    }

    companion object {
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
