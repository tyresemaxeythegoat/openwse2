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
//
// Resuming by itself: when Android ends the worker before the run has a RESULT line (in practice
// for memory while translating on every core), the runner binds it again a few seconds later with
// half the threads (kept for later runs); finished batches are cached, so only the batches in
// flight are lost. It gives up after MAX_STALLS deaths in a row that cached no new batch. When the
// launcher process itself was killed mid-run, R4ForegroundService (sticky) or the next app start
// calls `resumeIfInterrupted`, which starts the run again from the saved folder and counts the
// kill like a worker death (halving the threads, and giving up the same way), from preferences:
// otherwise a few huge batches that run the phone out of memory on every core restart from zero
// forever (seen on the phone at 77/80, 2026-11-21).
class R4StartupRunner private constructor(private val app: Application) {
    private val main = Handler(Looper.getMainLooper())
    @Volatile
    private var state = R4UiState()
    private val listeners = CopyOnWriteArraySet<(R4UiState) -> Unit>()
    private var connection: ServiceConnection? = null
    private var finished = false
    @Volatile
    private var stopRequested = false
    private val prefs = app.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    private var runTree: Uri? = null
    private var runCopy = ""
    private var jobs = 1
    private var soAtBind = 0
    private var stalls = 0
    private var resumes = 0
    @Volatile
    private var copying = false

    fun addListener(listener: (R4UiState) -> Unit) {
        listeners.add(listener)
        val now = state
        main.post { listener(now) }
    }

    fun removeListener(listener: (R4UiState) -> Unit) {
        listeners.remove(listener)
    }

    fun isRunning(): Boolean = state.running

    // The launcher process was killed while a run was going on: start it again (cached batches are
    // reused). Called by R4ForegroundService's sticky restart and when the launcher opens.
    fun resumeIfInterrupted() {
        if (state.running || !prefs.getBoolean(PREF_WANTED, false)) return
        val uri = prefs.getString(PREF_TREE, null) ?: return
        update(state.copy(useCopy = prefs.getBoolean(PREF_USE_COPY, state.useCopy)))
        start(Uri.parse(uri), resumed = true)
    }

    // The notification's Stop: ends the worker process (translated batches stay cached).
    fun stop() {
        if (!state.running || stopRequested) return
        stopRequested = true
        // While copying, the copy thread sees the flag and finishes; otherwise end the worker now
        // (or the resume waiting to bind it again).
        if (!copying) main.post { finish(STOPPED) }
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

    fun start(treeUri: Uri?) = start(treeUri, resumed = false)

    private fun start(treeUri: Uri?, resumed: Boolean) {
        if (state.running) return
        if (treeUri == null) {
            update(state.copy(report = "Choose your Warband folder first."))
            return
        }
        R4Native.loadError?.let {
            update(state.copy(report = "The game test is not available: part of the app did not load.\n\n(libwse_r0.so: $it)"))
            return
        }
        r4Dir.mkdirs()
        File(r4Dir, "report.txt").delete()
        File(r4Dir, "progress.txt").delete()
        finished = false
        stopRequested = false
        runTree = treeUri
        if (resumed) {
            // Android killed the whole app mid-run (the launcher too, not only the worker), which
            // `workerEnded` never sees: count it the same way, from what the last bind recorded.
            val before = prefs.getInt(PREF_SO_AT_BIND, -1)
            stalls = if (before < 0 || cachedBatches() > before) 0 else prefs.getInt(PREF_STALLS, 0) + 1
            resumes = prefs.getInt(PREF_RESUMES, 0) + 1
        } else {
            stalls = 0
            resumes = 0
        }
        val cores = Runtime.getRuntime().availableProcessors().coerceAtLeast(1)
        // One thread per core, but no more than the free memory holds: on the PC (tools/r4_run, 2
        // threads, 2026-11-20) translating this exe took ~1.8 GB, ~0.9 GB a thread. More threads
        // than memory make Android swap until the run crawls, or kill the worker.
        val memory = android.app.ActivityManager.MemoryInfo()
        (app.getSystemService(Context.ACTIVITY_SERVICE) as android.app.ActivityManager).getMemoryInfo(memory)
        val fit = (memory.availMem / BYTES_PER_THREAD).toInt().coerceIn(1, cores)
        jobs = prefs.getInt(PREF_JOBS, fit).coerceIn(1, fit)
        if (resumed) {
            // As in `workerEnded`: half the threads, down to 1 once a death brought no new batch.
            val floor = if (stalls > 0) 1 else 2
            if (jobs > floor) jobs = (jobs / 2).coerceAtLeast(floor)
        }
        prefs.edit().putBoolean(PREF_WANTED, true).putString(PREF_TREE, treeUri.toString())
            .putBoolean(PREF_USE_COPY, state.useCopy).putInt(PREF_STALLS, stalls)
            .putInt(PREF_RESUMES, resumes).apply()
        if (resumed) prefs.edit().putInt(PREF_JOBS, jobs).apply()
        if (stalls > MAX_STALLS) {
            finish(
                "The phone kept running out of memory and closing the app, so the game test stopped " +
                    "after $stalls tries in a row. Everything finished so far is saved: tap the test " +
                    "again to continue."
            )
            return
        }
        R4ForegroundService.clearResult(app)
        update(state.copy(running = true, status = "Getting ready…", report = null))
        try {
            R4ForegroundService.start(app)
        } catch (_: Throwable) {
            // A sticky restart is already the foreground service; elsewhere the run still goes on.
        }
        if (!state.useCopy) {
            bind(treeUri, "")
            return
        }
        copying = true
        Thread({
            val error = try {
                copyInstall(treeUri)
                null
            } catch (t: Throwable) {
                t.message ?: t.javaClass.simpleName
            }
            main.post {
                copying = false
                if (stopRequested) {
                    finish(STOPPED)
                } else if (error != null) {
                    finish("Could not copy the game folder: $error")
                } else {
                    bind(treeUri, copyDir.absolutePath)
                }
            }
        }, "wse-r4-copy").start()
    }

    private fun bind(treeUri: Uri, copy: String) {
        runCopy = copy
        soAtBind = cachedBatches()
        prefs.edit().putInt(PREF_SO_AT_BIND, soAtBind).apply()
        val args = Bundle().apply {
            putString(R4StartupService.KEY_TREE_URI, treeUri.toString())
            putString(R4StartupService.KEY_COPY_DIR, copy)
            putString(R4StartupService.KEY_WORK_DIR, workDir.absolutePath)
            putString(R4StartupService.KEY_MODULE, MODULE)
            // Every core the free memory holds (the user's choice, 2026-11-21: the phone does
            // nothing else meanwhile; see `start`), halved each time Android ends the worker
            // (`workerEnded`). The thread count is not part of the batch cache key.
            putInt(R4StartupService.KEY_JOBS, jobs)
            putLong(R4StartupService.KEY_FRAMES, FRAME_LIMIT)
        }
        val replies = Messenger(object : Handler(Looper.getMainLooper()) {
            override fun handleMessage(msg: Message) {
                if (msg.what == R4StartupService.MSG_RESULT) {
                    val code = msg.data.getInt(R4StartupService.KEY_RESULT)
                    finish("The game test could not start (error code $code).")
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
                    finish("The game test could not start: its background process closed right away.")
                }
            }

            // The run ends its process: frame limit, fault, or a crash in translated code.
            override fun onServiceDisconnected(name: ComponentName?) = workerEnded()

            override fun onBindingDied(name: ComponentName?) = workerEnded()
        }
        connection = conn
        // BIND_IMPORTANT | BIND_ABOVE_CLIENT: the worker gets the launcher's priority (top app while
        // it is on screen, foreground service otherwise) and is the last of the two to be killed.
        val flags = Context.BIND_AUTO_CREATE or Context.BIND_IMPORTANT or Context.BIND_ABOVE_CLIENT
        if (!app.bindService(Intent(app, R4StartupService::class.java), conn, flags)) {
            finish("The game test could not start its background process.")
            return
        }
        main.post(poll)
    }

    // The worker process ended. With a RESULT line the run is over; without one Android ended it
    // (or it crashed): bind again with half the threads unless it keeps dying without progress.
    private fun workerEnded() {
        if (finished || connection == null) return
        val tree = runTree
        if (stopRequested || tree == null || resultLine() != null) {
            finish(null)
            return
        }
        val cached = cachedBatches()
        stalls = if (cached > soAtBind) 0 else stalls + 1
        if (stalls > MAX_STALLS) {
            finish(
                "The phone kept running out of memory, so the game test stopped after $stalls tries " +
                    "in a row. Everything finished so far is saved: tap the test again to continue."
            )
            return
        }
        val floor = if (stalls > 0) 1 else 2
        if (jobs > floor) jobs = (jobs / 2).coerceAtLeast(floor)
        ++resumes
        prefs.edit().putInt(PREF_JOBS, jobs).putInt(PREF_STALLS, stalls).putInt(PREF_RESUMES, resumes).apply()
        unbindQuietly()
        main.removeCallbacks(poll)
        val status = "The phone ran low on memory and paused the work. Continuing in a moment with " +
            "fewer processor cores; nothing finished is lost."
        update(state.copy(status = status))
        R4ForegroundService.showProgress(app, status)
        main.postDelayed({ if (!finished && !stopRequested) bind(tree, runCopy) }, RESUME_DELAY_MS)
    }

    private fun unbindQuietly() {
        connection?.let {
            try {
                app.unbindService(it)
            } catch (_: Throwable) {
            }
        }
        connection = null
    }

    private fun cachedBatches(): Int =
        File(r4Dir, "so").listFiles { f -> f.name.endsWith(".so") }?.size ?: 0

    private fun resultLine(): String? = try {
        File(r4Dir, "report.txt").readLines().lastOrNull { it.startsWith("RESULT ") }
    } catch (_: Throwable) {
        null
    }

    private fun finish(error: String?) {
        if (finished) return
        finished = true
        prefs.edit().putBoolean(PREF_WANTED, false).remove(PREF_SO_AT_BIND).remove(PREF_STALLS)
            .remove(PREF_RESUMES).apply()
        unbindQuietly()
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
            result == null -> "The game test ended unexpectedly (the phone may have closed it). " +
                "Everything finished so far is saved."
            result.startsWith("pass") -> "The game test passed."
            else -> "The game test stopped with a problem. Please share this report."
        }
        return "$headline\n\nDetails for the developers:\n$text"
    }

    private fun statusLine(): String {
        val progress = try {
            File(r4Dir, "progress.txt").readText().trim().split(' ')
        } catch (_: Throwable) {
            emptyList()
        }
        // Plain words for the player (2026-11-21, the user's request): the report's raw lines stay in
        // the final report, under "Details for the developers".
        // progress.txt counts this attempt's batches; the report's "<tag>: ...; N batches (...), T to
        // translate" line adds the cached ones, so the status counts the whole image.
        val translating = if (progress.size == 3 && progress[1] != progress[2]) {
            val done = progress[1].toIntOrNull() ?: 0
            val todo = progress[2].toIntOrNull() ?: 0
            val summary = try {
                File(r4Dir, "report.txt").readLines()
                    .lastOrNull { it.startsWith(progress[0] + ": ") && it.contains(" to translate") }
            } catch (_: Throwable) {
                null
            }
            val total = summary?.let { BATCHES.find(it)?.groupValues?.get(1)?.toIntOrNull() } ?: todo
            val kept = (total - todo).coerceAtLeast(0)
            val what = if (progress[0] == "exe") "the game" else "the game's script engine"
            "Preparing $what for your phone: ${kept + done} of $total parts done. This is needed " +
                "only once; finished parts are saved."
        } else if (progress.size == 3) {
            "Starting a short test of the game (nothing is shown on screen)…"
        } else {
            "Getting ready…"
        }
        val again = when (resumes) {
            0 -> ""
            1 -> "\nRestarted once after the phone ran low on memory."
            else -> "\nRestarted $resumes times after the phone ran low on memory."
        }
        return translating + again
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
                    update(state.copy(status = "Copying the game folder: $files files, ${bytes / (1024 * 1024)} MB"))
                }
            }
        }
        walk(DocumentsContract.getTreeDocumentId(treeUri), copyDir, 0)
        update(state.copy(status = "Copied the game folder ($files files, ${bytes / (1024 * 1024)} MB). Starting…"))
    }

    companion object {
        const val MODULE = "Native"
        const val FRAME_LIMIT = 1000L
        private const val STOPPED = "You stopped the game test. Everything finished so far is saved, and the next " +
            "run continues from there."
        private const val PREFS = "r4_startup"
        private const val PREF_WANTED = "running"
        private const val PREF_TREE = "tree_uri"
        private const val PREF_USE_COPY = "use_copy"
        private const val PREF_JOBS = "jobs"
        // Kept across launcher restarts, so a run Android keeps killing whole still slows down and
        // gives up: the cached batch count when the worker was last bound, the deaths in a row
        // without a new batch, and the resumes so far.
        private const val PREF_SO_AT_BIND = "so_at_bind"
        private const val PREF_STALLS = "stalls"
        private const val PREF_RESUMES = "resumes"
        private const val MAX_STALLS = 3
        private const val RESUME_DELAY_MS = 3000L
        private const val BYTES_PER_THREAD = 1200L * 1024 * 1024
        private val BATCHES = Regex("""(\d+) batches \(""")

        @Volatile
        private var instance: R4StartupRunner? = null

        fun get(app: Application): R4StartupRunner =
            instance ?: synchronized(this) { instance ?: R4StartupRunner(app).also { instance = it } }
    }
}
