package org.wse2.android.launcher

import android.content.Context
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.TextView

class HomePage(
    context: Context,
    onChooseFolder: () -> Unit,
    onChangeModule: () -> Unit,
    onPlay: () -> Unit
) {
    val view: View

    private val installFolderText: TextView
    private val installHintText: TextView
    private val chooseFolderButton: Button
    private val moduleNameText: TextView
    private val moduleFolderText: TextView
    private val installStatusText: TextView
    private val playButton: Button
    private val lastResultText: TextView

    private var installReady = false
    private var scanInProgress = false

    init {
        val page = LauncherUi.page(context)
        page.addView(LauncherUi.pageTitle(context, "WSE2 Android"))

        val installCard = LauncherUi.card(context)
        installCard.addView(LauncherUi.sectionTitle(context, "Warband Install"))
        installFolderText = LauncherUi.bodyText(context, "No Warband folder selected")
        installCard.addView(installFolderText)
        installHintText = LauncherUi.captionText(
            context,
            "Choose the folder that contains Modules/ and CommonRes/"
        )
        installCard.addView(installHintText)
        chooseFolderButton = LauncherUi.secondaryButton(context, "Choose folder", onChooseFolder)
        installCard.addView(chooseFolderButton)
        page.addView(installCard)

        val moduleCard = LauncherUi.card(context)
        moduleCard.addView(LauncherUi.sectionTitle(context, "Selected Module"))
        moduleNameText = LauncherUi.bodyText(context, "No module selected")
        moduleCard.addView(moduleNameText)
        moduleFolderText = LauncherUi.captionText(context, "Select a Warband folder to discover modules.")
        moduleCard.addView(moduleFolderText)
        moduleCard.addView(LauncherUi.secondaryButton(context, "Change", onChangeModule))
        page.addView(moduleCard)

        installStatusText = LauncherUi.captionText(context, "Choose your Warband folder")
        page.addView(installStatusText)

        playButton = LauncherUi.primaryButton(context, "Play", onPlay).apply {
            isEnabled = false
        }
        page.addView(playButton)

        lastResultText = LauncherUi.statusLineText(context, "Ready.")
        page.addView(lastResultText)

        view = page.view
    }

    fun setInstallFolder(displayName: String?) {
        val validName = displayName?.takeIf { it.isNotBlank() }
        if (validName != null) {
            installFolderText.text = validName
            installHintText.visibility = View.GONE
        } else {
            installFolderText.text = "No Warband folder selected"
            installHintText.visibility = View.VISIBLE
        }
    }

    fun setScanInProgress(inProgress: Boolean) {
        scanInProgress = inProgress
        refreshButtons()
    }

    fun setInstallStatus(
        message: String?,
        isReady: Boolean = true,
        onClick: (() -> Unit)? = null
    ) {
        if (message.isNullOrBlank()) {
            installStatusText.text = ""
            installStatusText.visibility = View.GONE
            installStatusText.isClickable = false
            installStatusText.isFocusable = false
            installStatusText.minimumHeight = 0
            installStatusText.minHeight = 0
            installStatusText.setOnClickListener(null)
        } else {
            installStatusText.text = message
            installStatusText.visibility = View.VISIBLE
            if (onClick != null) {
                val minH = (48 * installStatusText.resources.displayMetrics.density).toInt()
                installStatusText.gravity = Gravity.CENTER_VERTICAL
                installStatusText.minimumHeight = minH
                installStatusText.minHeight = minH
                installStatusText.isClickable = true
                installStatusText.isFocusable = true
                installStatusText.setOnClickListener { onClick() }
            } else {
                installStatusText.gravity = Gravity.TOP or Gravity.START
                installStatusText.minimumHeight = 0
                installStatusText.minHeight = 0
                installStatusText.isClickable = false
                installStatusText.isFocusable = false
                installStatusText.setOnClickListener(null)
            }
        }
        installReady = isReady
        refreshButtons()
    }

    fun setSelectedModule(module: ModuleChoice?, hasFolder: Boolean) {
        if (module != null) {
            moduleNameText.text = module.displayName.ifBlank { module.folderName }
            moduleFolderText.text = "Folder: ${module.folderName}"
        } else if (hasFolder && scanInProgress) {
            moduleNameText.text = "No module selected"
            moduleFolderText.text = "Scanning folder for modules…"
        } else if (hasFolder) {
            moduleNameText.text = "No module selected"
            moduleFolderText.text = "No Warband modules found in the selected folder."
        } else {
            moduleNameText.text = "No module selected"
            moduleFolderText.text = "Select a Warband folder to discover modules."
        }
    }

    private fun refreshButtons() {
        chooseFolderButton.isEnabled = !scanInProgress
        playButton.isEnabled = installReady && !scanInProgress
    }

    fun setLastStatus(status: String) {
        val singleLine = status
            .lineSequence()
            .map { it.trim() }
            .filter { it.isNotEmpty() }
            .joinToString(" · ")
        lastResultText.text = if (singleLine.isEmpty()) "Ready." else singleLine
    }
}
