package org.wse2.android

import android.app.Service
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.os.Process
import android.os.RemoteException

// Runs the Vulkan driver probe in its own process (`android:process=":vkprobe"`
// in the manifest). A user-imported Turnip/PanVK driver is arbitrary native
// code: a crash inside dlopen, a driver constructor, or vkCreateInstance would
// otherwise take the whole launcher down. Here it only kills this process, and
// the launcher sees the binding die and reports it.
//
// It also keeps everything the probe leaves behind out of the launcher: the
// driver stays mapped (RTLD_NODELETE) and Mesa environment variables stay set
// only in this process, which exits once the launcher unbinds, so every probe
// starts from a clean process.
class WseVulkanProbeService : Service() {

    private val incoming = Messenger(object : Handler(Looper.getMainLooper()) {
        override fun handleMessage(msg: Message) {
            if (msg.what != MSG_PROBE) {
                super.handleMessage(msg)
                return
            }
            val replyTo = msg.replyTo ?: return
            val args = Bundle(msg.data)
            // The probe can take seconds (driver load + device creation); keep
            // it off the service's main thread so the binder stays responsive.
            // Messenger.send is thread-safe, so the worker replies directly.
            Thread({ reply(replyTo, runProbe(args)) }, "wse-vkprobe").start()
        }
    })

    override fun onBind(intent: Intent?): IBinder = incoming.binder

    override fun onUnbind(intent: Intent?): Boolean {
        // One probe per process: exit so the next probe loads its driver fresh.
        stopSelf()
        Process.killProcess(Process.myPid())
        return false
    }

    private fun runProbe(args: Bundle): String =
        try {
            WseSafBridge(applicationContext, Uri.EMPTY).nativeProbeVulkanAndRenderMesh(
                args.getString(KEY_MODE, "system"),
                args.getString(KEY_ICD_PATH, ""),
                args.getString(KEY_NATIVE_LIB_DIR, ""),
                args.getString(KEY_SHADER_CACHE_DIR, ""),
                args.getBoolean(KEY_ALLOW_MOCK, false)
            )
        } catch (t: Throwable) {
            "ERROR: probe failed: ${t.javaClass.simpleName}: ${t.message ?: ""}"
        }

    private fun reply(replyTo: Messenger, result: String) {
        val msg = Message.obtain(null, MSG_RESULT)
        msg.data = Bundle().apply { putString(KEY_RESULT, result) }
        try {
            replyTo.send(msg)
        } catch (_: RemoteException) {
            // Launcher went away; nothing to report to.
        }
    }

    companion object {
        const val MSG_PROBE = 1
        const val MSG_RESULT = 2
        const val KEY_MODE = "mode"
        const val KEY_ICD_PATH = "icd_path"
        const val KEY_NATIVE_LIB_DIR = "native_lib_dir"
        const val KEY_SHADER_CACHE_DIR = "shader_cache_dir"
        const val KEY_ALLOW_MOCK = "allow_mock"
        const val KEY_RESULT = "result"
    }
}
