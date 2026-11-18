package org.wse2.android

import android.content.Context
import android.os.Build
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import org.libsdl.app.SDLActivity

class WseActivity : SDLActivity() {

    // SDLActivity's default list ends in "main" (libmain.so), which this APK does not
    // ship: the engine and its SDL_main live in libwse2.so.
    override fun getLibraries(): Array<String> = arrayOf("SDL3", "wse2")

    // `adb shell am start -n org.wse2.android/.WseActivity -e wse.module_root <dir>
    // [-e wse.module Modules/<name>]` runs that module's UI (`run_module_shell` in
    // sdl_selftest_main.cpp) instead of the self-test. `<dir>` is a plain directory the app can
    // read (e.g. under getExternalFilesDir).
    //
    // From the launcher's "Run Selected Module" button (EXTRA_RUN_MODULE) the module comes from
    // the SAF tree it picked instead: onCreate registers a WseSafBridge for that tree and the
    // shell runs `--saf-module Modules/<name>`.
    override fun getArguments(): Array<String> {
        val launched = intent
        if (launched != null && launched.getBooleanExtra(WseLauncherActivity.EXTRA_RUN_MODULE, false)) {
            val name = launched.getStringExtra(WseLauncherActivity.EXTRA_MODULE_NAME)
            if (gameTreeRegistered && !name.isNullOrEmpty()) {
                return arrayOf("--saf-module", "Modules/$name")
            }
        }
        val root = intent?.getStringExtra(EXTRA_MODULE_ROOT)
        if (root.isNullOrEmpty()) {
            return emptyArray()
        }
        val module = intent?.getStringExtra(EXTRA_MODULE)
        return if (module.isNullOrEmpty()) {
            arrayOf("--module", root)
        } else {
            arrayOf("--module", root, module)
        }
    }

    private var powerManager: PowerManager? = null
    private var gameTreeRegistered = false
    private var thermalListener: PowerManager.OnThermalStatusChangedListener? = null
    private val thermalHandler = Handler(Looper.getMainLooper())
    private var lastHeadroomPollMs = Long.MIN_VALUE / 2

    private val thermalPollRunnable = object : Runnable {
        override fun run() {
            pushThermalUpdate(pollHeadroom = true)
            thermalHandler.postDelayed(this, HEADROOM_POLL_INTERVAL_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        // The launcher saved its choices into config/performance.json under filesDir.
        WseSafBridge.nativeApplyPerformanceSettings(filesDir.absolutePath)
        registerGameTree()

        powerManager = getSystemService(Context.POWER_SERVICE) as? PowerManager
        super.onCreate(savedInstanceState)
    }

    override fun onResume() {
        super.onResume()
        val pm = powerManager ?: return
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val listener = PowerManager.OnThermalStatusChangedListener { status ->
                pushThermalStatus(status, pollHeadroom = false)
            }
            thermalListener = listener
            pm.addThermalStatusListener(mainExecutor, listener)
        }
        pushThermalUpdate(pollHeadroom = true)
        thermalHandler.removeCallbacks(thermalPollRunnable)
        thermalHandler.postDelayed(thermalPollRunnable, HEADROOM_POLL_INTERVAL_MS)
    }

    override fun onPause() {
        thermalHandler.removeCallbacks(thermalPollRunnable)
        val pm = powerManager
        val listener = thermalListener
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && pm != null && listener != null) {
            pm.removeThermalStatusListener(listener)
        }
        thermalListener = null
        super.onPause()
    }

    // The launcher holds a persistable read/write grant on the tree it picked, so this activity
    // can open it by URI. Runs before super.onCreate, which later starts the SDL thread that
    // reads the registration.
    private fun registerGameTree() {
        val launched = intent ?: return
        if (!launched.getBooleanExtra(WseLauncherActivity.EXTRA_RUN_MODULE, false)) {
            return
        }
        val tree = launched.getStringExtra(WseLauncherActivity.EXTRA_TREE_URI)
        if (tree.isNullOrEmpty()) {
            return
        }
        gameTreeRegistered = try {
            // WseSafBridge's companion object loads libSDL3.so and libwse2.so.
            WseSafBridge(applicationContext, Uri.parse(tree)).nativeRegisterGameTree()
            true
        } catch (_: Throwable) {
            false
        }
    }

    private fun pushThermalUpdate(pollHeadroom: Boolean) {
        val pm = powerManager ?: return
        val status = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            pm.currentThermalStatus
        } else {
            0
        }
        pushThermalStatus(status, pollHeadroom)
    }

    private fun pushThermalStatus(status: Int, pollHeadroom: Boolean) {
        val pm = powerManager
        var headroom = -1.0f
        if (pollHeadroom && pm != null && Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            // Monotonic: a wall-clock change must not stall or burst the headroom polls.
            val nowMs = SystemClock.elapsedRealtime()
            if (nowMs - lastHeadroomPollMs >= HEADROOM_MIN_GAP_MS) {
                lastHeadroomPollMs = nowMs
                val raw = try {
                    pm.getThermalHeadroom(HEADROOM_FORECAST_SECONDS)
                } catch (_: Exception) {
                    Float.NaN
                }
                if (!raw.isNaN() && raw >= 0.0f) {
                    headroom = raw
                }
            }
        }
        WseSafBridge.nativeUpdateThermalState(status, headroom)
    }

    companion object {
        private const val EXTRA_MODULE_ROOT = "wse.module_root"
        private const val EXTRA_MODULE = "wse.module"
        private const val HEADROOM_FORECAST_SECONDS = 30
        private const val HEADROOM_POLL_INTERVAL_MS = 10_000L
        private const val HEADROOM_MIN_GAP_MS = 5_000L
    }
}
