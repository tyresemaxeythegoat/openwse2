package org.wse2.android.launcher

import android.app.Application
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.os.RemoteException
import android.provider.DocumentsContract
import org.wse2.android.R4ForegroundService
import org.wse2.android.R4StartupService
import java.io.File
import java.util.concurrent.CopyOnWriteArraySet

// Phase 5 R4 startup run on the phone (docs/RECOMP_PLAN.md "R4 design", "Phone run"): optionally
// copies the install into app storage, then runs R4StartupService in its own ":recomp" process,
// which translates the user's exe and lua5164.dll (cached in filesDir/recomp/r4/so), starts the exe
// on the recording D3D9 device and ends after FRAME_LIMIT frames or the first fault. While it runs,
// `status` follows filesDir/recomp/r4/progress.txt and the report's last line; when the process
// ends, the report (filesDir/recomp/r4/report.txt) is shown.
//
// One runner per launcher process (`get`), not per screen: the run goes on in the background, with
// the screen off, or after the app is swiped from Recents, kept alive by R4ForegroundService (its
// notification shows the status and has Stop). Screens subscribe with `addListener`. Every ending
// of the worker (frame limit, fault, crash, being killed) reaches this process as the binding
// dying, so the result notification is always posted here.
class R4StartupRunner private constructor(private val app: Application) {
    private val main = Handler(Looper.getMainLooper())
    @Volatile
    private var state = R4UiState()
    private val listeners = CopyOnWriteArraySet<(R4UiState) -> Unit>()
    private var connection: ServiceConnection? = null
    private var finished = false
    @Volatile
    private var stopRequested = false

    fun addListener(listener: (R4UiState) -> Unit) {
        listeners.add(listener)
        val now = state
        main.post { listener(now) }
    }

    fun removeListener(listener: (R4UiState) -> Unit) {
        listeners.remove(listener)
    }

    fun isRunning(): Boolean = state.running

    // The notification's Stop: ends the worker process (translated batches stay cached).
    fun stop() {
        if (!state.running || stopRequested) return
        stopRequested = true
        // While copying, the copy thread sees the flag and finishes; otherwise end the worker now.
        if (connection != null) main.post { finish(STOPPED) }
    }

    // The result notification was tapped: show the last report, even if this process was
    // restarted since the run (the report is in app storage).
    fun showLastReport() {
        if (state.running || state.report != null) return
        val file = File(r4Dir, "report.txt")
        if (!file.isFile) return
        update(state.copy(report = reportText(null)))
    }

    private val workDir: File get() = File(app.filesDir, "recomp")
    private val r4Dir: File get() = File(workDir, "r4")
    private val copyDir: File get() = File(r4Dir, "install")

    private val poll = object : Runnable {
        override fun run() {
            if (!state.running) return
            val status = statusLine()
            update(state.copy(status = status))
            R4ForegroundService.showProgress(app, status)
            main.postDelayed(this, 1000L)
        }
    }

    fun setUseCopy(useCopy: Boolean) {
        if (!state.running) update(state.copy(useCopy = useCopy))
    }

    fun dismissReport() = update(state.copy(report = null))

    fun start(treeUri: Uri?) {
        if (state.running) return
        if (treeUri == null) {
            update(state.copy(report = "R4 startup run: select the Warband folder first."))
            return
        }
        R4Native.loadError?.let {
            update(state.copy(report = "R4 startup run unavailable: libwse_r0.so did not load ($it)"))
            return
        }
        r4Dir.mkdirs()
        File(r4Dir, "report.txt").delete()
        File(r4Dir, "progress.txt").delete()
        finished = false
        stopRequested = false
        R4ForegroundService.clearResult(app)
        update(state.copy(running = true, status = "Starting", report = null))
        R4ForegroundService.start(app)
        if (!state.useCopy) {
            bind(treeUri, "")
            return
        }
        Thread({
            val error = try {
                copyInstall(treeUri)
                null
            } catch (t: Throwable) {
                t.message ?: t.javaClass.simpleName
            }
            main.post {
                if (stopRequested) {
                    finish(STOPPED)
                } else if (error != null) {
                    finish("R4 startup run: copying the install failed: $error")
                } else {
                    bind(treeUri, copyDir.absolutePath)
                }
            }
        }, "wse-r4-copy").start()
    }

    private fun bind(treeUri: Uri, copy: String) {
        val args = Bundle().apply {
            putString(R4StartupService.KEY_TREE_URI, treeUri.toString())
            putString(R4StartupService.KEY_COPY_DIR, copy)
            putString(R4StartupService.KEY_WORK_DIR, workDir.absolutePath)
            putString(R4StartupService.KEY_MODULE, MODULE)
            // Every core (the user's choice, 2026-11-21): the phone does nothing else meanwhile. The
            // thread count is not part of the batch cache key.
            putInt(R4StartupService.KEY_JOBS, Runtime.getRuntime().availableProcessors().coerceAtLeast(1))
            putLong(R4StartupService.KEY_FRAMES, FRAME_LIMIT)
        }
        val replies = Messenger(object : Handler(Looper.getMainLooper()) {
            override fun handleMessage(msg: Message) {
                if (msg.what == R4StartupService.MSG_RESULT) {
                    val code = msg.data.getInt(R4StartupService.KEY_RESULT)
                    finish("R4 startup run did not start (code $code).")
                } else {
                    super.handleMessage(msg)
                }
            }
        })
        val conn = object : ServiceConnection {
            override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
                val request = Message.obtain(null, R4StartupService.MSG_RUN).apply {
                    data = args
                    replyTo = replies
                }
                try {
                    Messenger(service).send(request)
                } catch (_: RemoteException) {
                    finish("R4 startup run: the :recomp process died before the request was sent.")
                }
            }

            // The run ends its process: frame limit, fault, or a crash in translated code.
            override fun onServiceDisconnected(name: ComponentName?) = finish(null)

            override fun onBindingDied(name: ComponentName?) = finish(null)
        }
        connection = conn
        // BIND_IMPORTANT | BIND_ABOVE_CLIENT: the worker gets the launcher's priority (top app while
        // it is on screen, foreground service otherwise) and is the last of the two to be killed.
        val flags = Context.BIND_AUTO_CREATE or Context.BIND_IMPORTANT or Context.BIND_ABOVE_CLIENT
        if (!app.bindService(Intent(app, R4StartupService::class.java), conn, flags)) {
            finish("R4 startup run: cannot start the :recomp service.")
            return
        }
        main.post(poll)
    }

    private fun finish(error: String?) {
        if (finished) return
        finished = true
        connection?.let {
            try {
                app.unbindService(it)
            } catch (_: Throwable) {
            }
        }
        connection = null
        main.removeCallbacks(poll)
        val report = reportText(error)
        update(state.copy(running = false, status = "", report = report))
        R4ForegroundService.finish(app, report.substringBefore("\n\n"))
    }

    // The headline (the given error, or the report's RESULT line) and the report.
    private fun reportText(error: String?): String {
        val text = try {
            File(r4Dir, "report.txt").readText()
        } catch (_: Throwable) {
            ""
        }
        val result = text.lineSequence().lastOrNull { it.startsWith("RESULT ") }?.removePrefix("RESULT ")
        val headline = when {
            error != null -> error
            result == null -> "The :recomp process ended without a result line (killed, or a crash the run could not report)."
            result.startsWith("pass") -> "PASS: $result"
            else -> "Stopped: $result"
        }
        return "$headline\n\n$text"
    }

    private fun statusLine(): String {
        val progress = try {
            File(r4Dir, "progress.txt").readText().trim().split(' ')
        } catch (_: Throwable) {
            emptyList()
        }
        val last = try {
            File(r4Dir, "report.txt").readLines().lastOrNull { it.isNotBlank() }?.trim() ?: ""
        } catch (_: Throwable) {
            ""
        }
        val translating = if (progress.size == 3 && progress[1] != progress[2]) {
            "Translating ${progress[0]}: ${progress[1]}/${progress[2]} batches. "
        } else {
            ""
        }
        return translating + last.take(160)
    }

    // The copy thread and the main thread both update `state`.
    @Synchronized
    private fun update(next: R4UiState) {
        state = next
        main.post { for (l in listeners) l(next) }
    }

    // Everything under the folder except the other modules in Modules/: root files, CommonRes,
    // Data, Textures, languages, ... and Modules/<MODULE>. A file already copied with the same size
    // is kept, so a second run copies only what changed.
    private fun copyInstall(treeUri: Uri) {
        copyDir.mkdirs()
        var files = 0
        var bytes = 0L
        fun walk(parentId: String, dest: File, depth: Int) {
            val children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentId)
            val projection = arrayOf(
                DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME,
                DocumentsContract.Document.COLUMN_MIME_TYPE,
                DocumentsContract.Document.COLUMN_SIZE
            )
            val entries = ArrayList<Array<Any?>>()
            app.contentResolver.query(children, projection, null, null, null)?.use { c ->
                while (c.moveToNext()) {
                    entries.add(arrayOf(c.getString(0), c.getString(1), c.getString(2), if (c.isNull(3)) -1L else c.getLong(3)))
                }
            }
            for (e in entries) {
                if (stopRequested) return
                val id = e[0] as String? ?: continue
                val name = e[1] as String? ?: continue
                val isDir = e[2] == DocumentsContract.Document.MIME_TYPE_DIR
                val size = e[3] as Long
                if (isDir) {
                    // Inside Modules/, only the module the run starts.
                    if (depth == 1 && dest.name.equals("Modules", ignoreCase = true) && !name.equals(MODULE, ignoreCase = true)) {
                        continue
                    }
                    val sub = File(dest, name)
                    sub.mkdirs()
                    walk(id, sub, depth + 1)
                    continue
                }
                val out = File(dest, name)
                if (size >= 0 && out.isFile && out.length() == size) continue
                val doc = DocumentsContract.buildDocumentUriUsingTree(treeUri, id)
                app.contentResolver.openInputStream(doc)?.use { input ->
                    out.outputStream().use { output -> bytes += input.copyTo(output, 1 shl 16) }
                } ?: throw IllegalStateException("cannot open $name")
                ++files
                if (files % 50 == 0) {
                    update(state.copy(status = "Copying the install: $files files, ${bytes / (1024 * 1024)} MB"))
                }
            }
        }
        walk(DocumentsContract.getTreeDocumentId(treeUri), copyDir, 0)
        update(state.copy(status = "Copied $files files (${bytes / (1024 * 1024)} MB); starting"))
    }

    companion object {
        const val MODULE = "Native"
        const val FRAME_LIMIT = 1000L
        private const val STOPPED = "Stopped by you. Translated batches are kept: the next run resumes."

        @Volatile
        private var instance: R4StartupRunner? = null

        fun get(app: Application): R4StartupRunner =
            instance ?: synchronized(this) { instance ?: R4StartupRunner(app).also { instance = it } }
    }
}
