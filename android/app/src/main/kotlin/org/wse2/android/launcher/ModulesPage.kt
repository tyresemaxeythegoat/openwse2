package org.wse2.android.launcher

import android.content.Context
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView

data class ModuleChoice(
    val folderName: String,
    val modulePath: String,
    val displayName: String,
    val hasScriptsTxt: Boolean,
    val loadResourceCount: Int,
    val loadModResourceCount: Int
) {
    override fun toString(): String =
        if (displayName == folderName) {
            folderName
        } else {
            "$displayName ($folderName)"
        }
}

enum class InstallCheckState {
    CHECKING,
    READY,
    MISSING
}

data class ModuleInstallStatus(
    val state: InstallCheckState = InstallCheckState.CHECKING,
    val missingFiles: List<String> = emptyList()
)

class ModulesPage(
    private val context: Context,
    onChooseFolder: () -> Unit,
    onRescan: () -> Unit,
    private val onSelectModule: (Int) -> Unit,
    onCancelScan: () -> Unit = {},
    private val onShowMissingFiles: (ModuleChoice, List<String>) -> Unit = { _, _ -> }
) {
    private data class ModuleRowHolder(
        val module: ModuleChoice,
        val row: LauncherUi.SelectableRow,
        val statusView: TextView,
        val detailsButton: Button
    )

    val view: View

    private val folderLabelText: TextView
    private val chooseFolderButton: Button
    private val rescanButton: Button
    private val scanProgressContainer: LinearLayout
    private val scanProgressText: TextView
    private val cancelScanButton: Button
    private val scanStatusText: TextView
    private val emptyText: TextView
    private val listContainer: LinearLayout
    private val detailText: TextView

    private val rowHolders = ArrayList<ModuleRowHolder>()
    private var currentModules: List<ModuleChoice> = emptyList()
    private val currentStatuses = HashMap<String, ModuleInstallStatus>()
    private var lastSelectedIndex = -1
    private var hasSelectedFolder = false
    private var scanInProgress = false

    init {
        val page = LauncherUi.page(context)
        page.addView(LauncherUi.pageTitle(context, "Modules"))

        val folderCard = LauncherUi.card(context)
        folderCard.addView(LauncherUi.sectionTitle(context, "Warband Folder"))
        folderLabelText = LauncherUi.bodyText(context, "No Warband folder selected")
        folderCard.addView(folderLabelText)
        chooseFolderButton = LauncherUi.secondaryButton(context, "Choose folder", onChooseFolder)
        rescanButton = LauncherUi.secondaryButton(context, "Rescan", onRescan).apply {
            isEnabled = false
        }
        folderCard.addView(LauncherUi.buttonRow(context, chooseFolderButton, rescanButton))

        val scanProgressRow = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                bottomMargin = dp(context, 8)
            }
        }
        val spinner = ProgressBar(context).apply {
            isIndeterminate = true
            layoutParams = LinearLayout.LayoutParams(dp(context, 24), dp(context, 24)).apply {
                marginEnd = dp(context, 10)
            }
        }
        scanProgressText = LauncherUi.bodyText(context, "").apply {
            setPadding(0, 0, 0, 0)
            layoutParams = LinearLayout.LayoutParams(
                0,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                1f
            )
        }
        scanProgressRow.addView(spinner)
        scanProgressRow.addView(scanProgressText)

        cancelScanButton = LauncherUi.secondaryButton(context, "Cancel", onCancelScan)
        scanProgressContainer = LauncherUi.verticalGroup(context).apply {
            visibility = View.GONE
            addView(scanProgressRow)
            addView(cancelScanButton)
        }
        folderCard.addView(scanProgressContainer)

        scanStatusText = LauncherUi.captionText(context, "").apply {
            visibility = View.GONE
        }
        folderCard.addView(scanStatusText)
        page.addView(folderCard)

        val listCard = LauncherUi.card(context)
        listCard.addView(LauncherUi.sectionTitle(context, "Available Modules"))
        emptyText = LauncherUi.captionText(context, "Select a Warband folder to discover modules.")
        listCard.addView(emptyText)
        listContainer = LauncherUi.verticalGroup(context)
        listCard.addView(listContainer)
        page.addView(listCard)

        val detailCard = LauncherUi.card(context)
        detailCard.addView(LauncherUi.sectionTitle(context, "Module Details"))
        detailText = LauncherUi.captionText(context, "Select a Warband folder to discover modules.")
        detailCard.addView(detailText)
        page.addView(detailCard)

        view = page.view
    }

    fun setFolderState(displayName: String?, hasFolder: Boolean) {
        hasSelectedFolder = hasFolder
        folderLabelText.text =
            displayName?.takeIf { it.isNotBlank() } ?: "No Warband folder selected"
        rescanButton.isEnabled = hasFolder && !scanInProgress
    }

    fun setScanInProgress(inProgress: Boolean, folderName: String? = null) {
        scanInProgress = inProgress
        chooseFolderButton.isEnabled = !inProgress
        rescanButton.isEnabled = hasSelectedFolder && !inProgress
        if (inProgress) {
            val label = folderName?.takeIf { it.isNotBlank() } ?: "folder"
            scanProgressText.text = "Scanning $label…"
            scanProgressContainer.visibility = View.VISIBLE
            cancelScanButton.isEnabled = true
            listContainer.visibility = View.GONE
            emptyText.text = "Scanning $label…"
            emptyText.visibility = View.VISIBLE
            detailText.text = "Scanning $label…"
        } else {
            scanProgressContainer.visibility = View.GONE
        }
    }

    fun setScanStatus(message: String?) {
        if (message.isNullOrBlank()) {
            scanStatusText.text = ""
            scanStatusText.visibility = View.GONE
        } else {
            scanStatusText.text = message
            scanStatusText.visibility = View.VISIBLE
        }
    }

    fun showModules(
        modules: List<ModuleChoice>,
        selectedIndex: Int,
        emptyMessage: String? = null,
        installStatuses: Map<String, ModuleInstallStatus> = emptyMap()
    ) {
        currentModules = modules.toList()
        currentStatuses.clear()
        currentStatuses.putAll(installStatuses)
        listContainer.removeAllViews()
        rowHolders.clear()

        if (modules.isEmpty()) {
            lastSelectedIndex = -1
            val msg = emptyMessage ?: "Select a Warband folder to discover modules."
            emptyText.text = msg
            emptyText.visibility = View.VISIBLE
            listContainer.visibility = View.GONE
            detailText.text = msg
            return
        }

        emptyText.visibility = View.GONE
        listContainer.visibility = View.VISIBLE
        modules.forEachIndexed { index, mod ->
            val scriptsLabel = if (mod.hasScriptsTxt) "scripts.txt present" else "scripts.txt missing"
            val subtitle = "${mod.modulePath} | $scriptsLabel"
            val row = LauncherUi.selectableRow(
                ctx = context,
                title = mod.toString(),
                subtitle = subtitle,
                selected = index == selectedIndex
            ) {
                onSelectModule(index)
                val status = currentStatuses[mod.modulePath]
                if (status?.state == InstallCheckState.MISSING) {
                    onShowMissingFiles(mod, status.missingFiles)
                }
            }
            val statusView = LauncherUi.captionText(context, "").apply {
                setPadding(0, dp(context, 4), 0, 0)
            }
            val detailsButton = LauncherUi.secondaryButton(context, "Details") {
                val missing = currentStatuses[mod.modulePath]?.missingFiles ?: emptyList()
                onShowMissingFiles(mod, missing)
            }.apply {
                visibility = View.GONE
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                ).apply {
                    topMargin = dp(context, 6)
                    bottomMargin = 0
                }
            }
            row.root.addView(statusView)
            row.root.addView(detailsButton)

            val holder = ModuleRowHolder(mod, row, statusView, detailsButton)
            applyRowStatus(holder, currentStatuses[mod.modulePath])
            rowHolders.add(holder)
            listContainer.addView(row.root)
        }
        updateSelectedIndex(selectedIndex)
    }

    fun updateModuleStatus(modulePath: String, status: ModuleInstallStatus) {
        currentStatuses[modulePath] = status
        for (holder in rowHolders) {
            if (holder.module.modulePath == modulePath) {
                applyRowStatus(holder, status)
            }
        }
        refreshDetailText(lastSelectedIndex)
    }

    fun updateSelectedIndex(selectedIndex: Int) {
        lastSelectedIndex = selectedIndex
        rowHolders.forEachIndexed { index, holder ->
            LauncherUi.updateSelectableRow(holder.row, index == selectedIndex)
        }
        refreshDetailText(selectedIndex)
    }

    fun showScanError(error: String) {
        showModules(emptyList(), -1, "No modules available.")
    }

    private fun applyRowStatus(holder: ModuleRowHolder, status: ModuleInstallStatus?) {
        when (status?.state) {
            null, InstallCheckState.CHECKING -> {
                holder.statusView.text = "Checking…"
                holder.detailsButton.visibility = View.GONE
            }
            InstallCheckState.READY -> {
                holder.statusView.text = "Ready"
                holder.detailsButton.visibility = View.GONE
            }
            InstallCheckState.MISSING -> {
                val missingCount = status.missingFiles.size
                holder.statusView.text = if (missingCount > 0) {
                    "Missing $missingCount file(s)"
                } else {
                    "Install check failed"
                }
                holder.detailsButton.visibility = View.VISIBLE
            }
        }
    }

    private fun refreshDetailText(selectedIndex: Int) {
        if (selectedIndex in currentModules.indices) {
            val m = currentModules[selectedIndex]
            val scriptsLabel = if (m.hasScriptsTxt) "scripts.txt present" else "scripts.txt missing"
            val installLabel = when (val st = currentStatuses[m.modulePath]) {
                null -> "Checking…"
                else -> when (st.state) {
                    InstallCheckState.CHECKING -> "Checking…"
                    InstallCheckState.READY -> "Ready"
                    InstallCheckState.MISSING -> if (st.missingFiles.isNotEmpty()) {
                        "Missing ${st.missingFiles.size} file(s)"
                    } else {
                        "Install check failed"
                    }
                }
            }
            detailText.text =
                "${m.modulePath} | $scriptsLabel | BRFs: ${m.loadResourceCount} common + ${m.loadModResourceCount} mod | $installLabel"
        } else if (currentModules.isNotEmpty()) {
            detailText.text = "Tap a module above to select it."
        }
    }

    private fun dp(ctx: Context, value: Int): Int =
        (value * ctx.resources.displayMetrics.density).toInt()
}
