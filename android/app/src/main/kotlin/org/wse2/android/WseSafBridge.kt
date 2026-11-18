package org.wse2.android

import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract

class WseSafBridge(
    private val context: Context,
    private val treeUri: Uri
) {
    fun treeUriString(): String = treeUri.toString()

    fun rootDocumentId(): String =
        try {
            DocumentsContract.getTreeDocumentId(treeUri)
        } catch (_: Exception) {
            ""
        }

    fun listChildren(parentDocId: String): Array<String>? {
        val childrenUri = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, parentDocId)
        val projection = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE
        )
        return try {
            val out = ArrayList<String>()
            context.contentResolver.query(childrenUri, projection, null, null, null)?.use { cursor ->
                val idxId = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DOCUMENT_ID)
                val idxName = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_DISPLAY_NAME)
                val idxMime = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_MIME_TYPE)
                val idxSize = cursor.getColumnIndex(DocumentsContract.Document.COLUMN_SIZE)
                while (cursor.moveToNext()) {
                    if (idxId < 0 || idxName < 0 || cursor.isNull(idxId) || cursor.isNull(idxName)) {
                        continue
                    }
                    val docId = cursor.getString(idxId) ?: continue
                    val name = cursor.getString(idxName) ?: continue
                    val mime = if (idxMime >= 0 && !cursor.isNull(idxMime)) cursor.getString(idxMime) else ""
                    val isDir = if (mime == DocumentsContract.Document.MIME_TYPE_DIR) "1" else "0"
                    val size = if (idxSize >= 0 && !cursor.isNull(idxSize)) {
                        cursor.getLong(idxSize).coerceAtLeast(0L)
                    } else {
                        0L
                    }
                    out.add("$isDir\t$size\t$docId\t$name")
                }
            } ?: return null
            out.toTypedArray()
        } catch (_: Exception) {
            null
        }
    }

    fun readDocument(docId: String): ByteArray? {
        val docUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, docId)
        return try {
            val pfd = context.contentResolver.openFileDescriptor(docUri, "r") ?: return null
            ParcelFileDescriptor.AutoCloseInputStream(pfd).use { input ->
                input.readBytes()
            }
        } catch (_: Exception) {
            null
        }
    }

    fun writeDocument(
        parentDocId: String,
        existingDocId: String,
        displayName: String,
        data: ByteArray
    ): String? {
        return try {
            val targetUri: Uri
            val resultDocId: String
            if (existingDocId.isNotEmpty()) {
                resultDocId = existingDocId
                targetUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, existingDocId)
            } else {
                val parentUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, parentDocId)
                val created = DocumentsContract.createDocument(
                    context.contentResolver,
                    parentUri,
                    "application/octet-stream",
                    displayName
                ) ?: return null
                targetUri = created
                resultDocId = DocumentsContract.getDocumentId(created)
            }

            val pfd = context.contentResolver.openFileDescriptor(targetUri, "wt")
                ?: context.contentResolver.openFileDescriptor(targetUri, "w")
                ?: return null
            ParcelFileDescriptor.AutoCloseOutputStream(pfd).use { output ->
                output.write(data)
            }
            resultDocId
        } catch (_: Exception) {
            null
        }
    }

    fun createDirectory(parentDocId: String, displayName: String): String? {
        return try {
            val parentUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, parentDocId)
            val created = DocumentsContract.createDocument(
                context.contentResolver,
                parentUri,
                DocumentsContract.Document.MIME_TYPE_DIR,
                displayName
            ) ?: return null
            DocumentsContract.getDocumentId(created)
        } catch (_: Exception) {
            null
        }
    }

    fun deleteDocument(docId: String): Boolean {
        return try {
            val docUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, docId)
            DocumentsContract.deleteDocument(context.contentResolver, docUri)
        } catch (_: Exception) {
            false
        }
    }

    // Renames a document in place; returns its (possibly new) document id, or null.
    fun renameDocument(docId: String, newName: String): String? {
        return try {
            val docUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, docId)
            val renamed = DocumentsContract.renameDocument(context.contentResolver, docUri, newName)
                ?: return null
            DocumentsContract.getDocumentId(renamed)
        } catch (_: Exception) {
            null
        }
    }

    // Moves a document to another directory of the same tree; returns its new document id, or null.
    fun moveDocument(docId: String, sourceParentId: String, targetParentId: String): String? {
        return try {
            val docUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, docId)
            val srcUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, sourceParentId)
            val dstUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, targetParentId)
            val moved = DocumentsContract.moveDocument(context.contentResolver, docUri, srcUri, dstUri)
                ?: return null
            DocumentsContract.getDocumentId(moved)
        } catch (_: Exception) {
            null
        }
    }

    external fun nativeScanTree(): String
    external fun nativeVerifyModule(moduleRelDir: String): String
    external fun nativeProbeVulkanAndRenderMesh(
        driverMode: String,
        customIcdPath: String,
        nativeLibDir: String,
        shaderCacheDir: String,
        allowMockFallback: Boolean
    ): String

    // Validates meta.json (minApi vs this device; a turnip/panvk requestedMode
    // refuses the other kind), ELF-checks and patches the ICD, and writes both
    // into stageDir. Returns "OK\t<mode>\t<icd path>\t<name>\t<patched>" or "ERROR: ...".
    external fun nativeStageDriverPackage(
        metaJson: String,
        icdBytes: ByteArray,
        stageDir: String,
        requestedMode: String
    ): String

    companion object {
        // `<configRoot>/config/performance.json` is the one saved copy of the performance and
        // launcher settings; these read, update and apply it through the native serializer.
        @JvmStatic
        external fun nativeLoadPerformanceSettings(configRoot: String): String

        @JvmStatic
        external fun nativeSavePerformanceSettings(
            configRoot: String,
            frameRateCapHz: Int,
            presentMode: String,
            thermalThrottlingEnabled: Boolean,
            dynamicResolutionEnabled: Boolean,
            lodBiasScalingEnabled: Boolean,
            gyroAimEnabled: Boolean,
            pinchZoomEnabled: Boolean
        ): Boolean

        @JvmStatic
        external fun nativeApplyPerformanceSettings(configRoot: String)

        @JvmStatic
        external fun nativeUpdateThermalState(
            thermalStatus: Int,
            thermalHeadroom: Float
        )

        init {
            System.loadLibrary("SDL3")
            System.loadLibrary("wse2")
        }
    }
}
