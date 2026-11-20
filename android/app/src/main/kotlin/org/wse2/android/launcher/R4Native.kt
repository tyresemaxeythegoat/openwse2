package org.wse2.android.launcher

import org.wse2.android.WseSafBridge

// Phase 5 R4 startup run (docs/RECOMP_PLAN.md "R4 design", "Phone run"): `libwse_r0.so`
// (`platform/android/r4_jni.cpp`). Called only in R4StartupService's own ":recomp" process:
// the run maps the game at fixed addresses and ends the process when it is done.
object R4Native {
    val loadError: String? by lazy {
        try {
            System.loadLibrary("wse_r0")
            null
        } catch (t: Throwable) {
            t.message ?: t.javaClass.simpleName
        }
    }

    // Reads the install through `bridge` (the SAF folder) or, when `copyDir` is not empty, from
    // that directory; works in `workDir/r4`. Returns only when nothing could be started (the
    // report says why); otherwise the process ends when the run does.
    @JvmStatic
    external fun nativeRun(
        bridge: WseSafBridge?,
        copyDir: String,
        workDir: String,
        module: String,
        jobs: Int,
        frames: Long
    ): Int
}
