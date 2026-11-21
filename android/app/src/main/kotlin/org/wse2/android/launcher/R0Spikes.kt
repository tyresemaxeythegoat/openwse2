package org.wse2.android.launcher

import android.app.Application
import android.net.Uri
import android.provider.DocumentsContract
import java.io.ByteArrayOutputStream
import java.io.File
import java.security.MessageDigest
import java.util.Locale

// Phase 5 R0 feasibility spikes (docs/RECOMP_PLAN.md): runs `libwse_r0.so`, which embeds
// LLVM + LLD, on the user's own `mb_warband_wse2_x64.exe` read from the install folder.
// The exe bytes stay in memory and in app-private storage; nothing leaves the device.
object R0Spikes {
    private const val EXE_NAME = "mb_warband_wse2_x64.exe"
    private const val PDB_NAME = "mb_warband_wse2_x64.pdb"
    private const val MAX_EXE_BYTES = 64L * 1024L * 1024L

    private val loadError: String? by lazy {
        try {
            System.loadLibrary("wse_r0")
            null
        } catch (t: Throwable) {
            t.message ?: t.javaClass.simpleName
        }
    }

    @JvmStatic
    private external fun nativeRun(
        workDir: String,
        exe: ByteArray?,
        exeSha256: String,
        exeError: String,
        pdb: ByteArray?,
        pdbError: String,
        apkBytes: Long
    ): String

    private fun workDir(app: Application): File = File(app.filesDir, "recomp").apply { mkdirs() }

    private fun runningMarker(app: Application): File = File(workDir(app), "r0/running")

    private fun partialReport(app: Application): File = File(workDir(app), "r0/report.txt")

    // Report of a run that never finished (the process died inside the spikes), or null.
    fun crashedRunReport(app: Application): String? {
        if (!runningMarker(app).exists()) {
            return null
        }
        val partial = try {
            partialReport(app).readText()
        } catch (_: Throwable) {
            ""
        }
        runningMarker(app).delete()
        return "The previous R0 run stopped before it finished (the app crashed or was " +
            "killed). Its report up to that point:\n\n" + partial
    }

    fun run(app: Application, treeUri: Uri?): String {
        val crashed = crashedRunReport(app)
        loadError?.let { return "The phone check is not available: part of the app did not load.\n\n(libwse_r0.so: $it)" }

        var exe: ByteArray? = null
        var sha = ""
        var exeError = ""
        if (treeUri == null) {
            exeError = "no Warband folder selected"
        } else {
            try {
                val doc = findRootChild(app, treeUri, EXE_NAME)
                if (doc == null) {
                    exeError = "$EXE_NAME not found in the folder root"
                } else {
                    exe = readAll(app, doc)
                    sha = MessageDigest.getInstance("SHA-256").digest(exe)
                        .joinToString("") { String.format(Locale.ROOT, "%02x", it) }
                }
            } catch (t: Throwable) {
                exeError = "reading $EXE_NAME failed: ${t.message ?: t.javaClass.simpleName}"
                exe = null
            }
        }
        // WSE2 ships its PDB next to the exe; it lets the translator find functions by name in
        // whatever build is installed.
        var pdb: ByteArray? = null
        var pdbError = ""
        if (treeUri != null && exe != null) {
            try {
                val doc = findRootChild(app, treeUri, PDB_NAME)
                if (doc == null) {
                    pdbError = "$PDB_NAME not found in the folder root"
                } else {
                    pdb = readAll(app, doc)
                }
            } catch (t: Throwable) {
                pdbError = "reading $PDB_NAME failed: ${t.message ?: t.javaClass.simpleName}"
                pdb = null
            }
        }
        val apkBytes = try {
            File(app.applicationInfo.sourceDir).length()
        } catch (_: Throwable) {
            0L
        }
        val marker = runningMarker(app)
        marker.parentFile?.mkdirs()
        marker.writeText("running")
        val report = nativeRun(workDir(app).absolutePath, exe, sha, exeError, pdb, pdbError, apkBytes)
        marker.delete()
        return if (crashed != null) "$report\n\n----\n$crashed" else report
    }

    private fun findRootChild(app: Application, treeUri: Uri, name: String): Uri? {
        val rootId = DocumentsContract.getTreeDocumentId(treeUri)
        val children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, rootId)
        val projection = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME
        )
        app.contentResolver.query(children, projection, null, null, null)?.use { c ->
            while (c.moveToNext()) {
                if (c.getString(1).equals(name, ignoreCase = true)) {
                    return DocumentsContract.buildDocumentUriUsingTree(treeUri, c.getString(0))
                }
            }
        }
        return null
    }

    private fun readAll(app: Application, doc: Uri): ByteArray {
        val input = app.contentResolver.openInputStream(doc)
            ?: throw IllegalStateException("cannot open $doc")
        input.use { stream ->
            val out = ByteArrayOutputStream()
            val buf = ByteArray(1 shl 16)
            var total = 0L
            while (true) {
                val n = stream.read(buf)
                if (n < 0) break
                total += n
                if (total > MAX_EXE_BYTES) throw IllegalStateException("file larger than 64 MB")
                out.write(buf, 0, n)
            }
            return out.toByteArray()
        }
    }
}
