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
import org.wse2.android.launcher.R4Native

// Runs the Phase 5 R4 startup run (docs/RECOMP_PLAN.md "R4 design", "Phone run") in its own
// process (`android:process=":recomp"` in the manifest). The run maps the translated game at
// fixed addresses, starts guest threads that cannot be stopped, and ends this process when it is
// done (frame limit reached, a fault, or a crash in translated code); the launcher sees the
// binding die and reads the report the run wrote. Only a setup error is replied to.
class R4StartupService : Service() {

    private val incoming = Messenger(object : Handler(Looper.getMainLooper()) {
        override fun handleMessage(msg: Message) {
            if (msg.what != MSG_RUN) {
                super.handleMessage(msg)
                return
            }
            val replyTo = msg.replyTo ?: return
            val args = Bundle(msg.data)
            // Translation takes minutes and the run never returns: keep it off the main thread. The
            // translation workers and guest threads are created from this thread and inherit its
            // priority, so raise it as far as Android lets an app.
            Thread({
                raisePriority()
                reply(replyTo, run(args))
            }, "wse-r4-startup").start()
        }
    })

    override fun onBind(intent: Intent?): IBinder = incoming.binder

    override fun onUnbind(intent: Intent?): Boolean {
        // The launcher stopped waiting: end the run with this process.
        stopSelf()
        Process.killProcess(Process.myPid())
        return false
    }

    private fun raisePriority() {
        for (p in intArrayOf(Process.THREAD_PRIORITY_URGENT_DISPLAY, Process.THREAD_PRIORITY_DISPLAY, Process.THREAD_PRIORITY_FOREGROUND)) {
            try {
                Process.setThreadPriority(p)
                return
            } catch (_: Throwable) {
                // Not allowed at this level: try the next one.
            }
        }
    }

    private fun run(args: Bundle): Int {
        R4Native.loadError?.let { return SETUP_LIBRARY_MISSING }
        val copyDir = args.getString(KEY_COPY_DIR, "")
        val bridge = if (copyDir.isEmpty()) {
            val tree = args.getString(KEY_TREE_URI, "")
            if (tree.isEmpty()) return SETUP_NO_FOLDER
            WseSafBridge(applicationContext, Uri.parse(tree))
        } else {
            null
        }
        return try {
            R4Native.nativeRun(
                bridge,
                copyDir,
                args.getString(KEY_WORK_DIR, ""),
                args.getString(KEY_MODULE, "Native"),
                args.getInt(KEY_JOBS, 1),
                args.getLong(KEY_FRAMES, 1000L)
            )
        } catch (t: Throwable) {
            SETUP_EXCEPTION
        }
    }

    private fun reply(replyTo: Messenger, code: Int) {
        val msg = Message.obtain(null, MSG_RESULT)
        msg.data = Bundle().apply { putInt(KEY_RESULT, code) }
        try {
            replyTo.send(msg)
        } catch (_: RemoteException) {
            // Launcher went away; nothing to report to.
        }
    }

    companion object {
        const val MSG_RUN = 1
        const val MSG_RESULT = 2
        const val KEY_TREE_URI = "tree_uri"
        const val KEY_COPY_DIR = "copy_dir"
        const val KEY_WORK_DIR = "work_dir"
        const val KEY_MODULE = "module"
        const val KEY_JOBS = "jobs"
        const val KEY_FRAMES = "frames"
        const val KEY_RESULT = "result"
        // Result codes the service itself returns (the native setup error is 1).
        const val SETUP_LIBRARY_MISSING = 101
        const val SETUP_NO_FOLDER = 102
        const val SETUP_EXCEPTION = 103
    }
}
