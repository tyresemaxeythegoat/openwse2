package org.wse2.android

import android.content.Context
import android.os.Build
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
    // read (e.g. under getExternalFilesDir); SAF-picked installs are not wired to it yet.
    override fun getArguments(): Array<String> {
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
