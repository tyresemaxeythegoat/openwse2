package org.wse2.android.launcher

import android.content.Context
import android.view.View
import android.widget.AdapterView
import android.widget.Button
import android.widget.CheckBox
import android.widget.TextView

class AdvancedPage(
    context: Context,
    driverOptions: List<String>,
    onVerifyScripts: () -> Unit,
    private val onDriverSelected: (Int) -> Unit,
    onImportDriver: () -> Unit,
    onProbeVulkan: () -> Unit,
    onLaunchSelfTest: () -> Unit
) {
    val view: View

    private val folderStatusText: TextView
    private val indexStatsText: TextView
    private val verifyScriptsButton: Button
    private val driverRow: LauncherUi.SpinnerRow
    private val driverStatusText: TextView
    private val mockFallbackCheck: CheckBox
    private val probeButton: Button
    private val outputStatusText: TextView

    private var lastDriverPos = -1

    init {
        val page = LauncherUi.page(context)
        page.addView(LauncherUi.pageTitle(context, "Advanced"))

        val safCard = LauncherUi.card(context)
        safCard.addView(LauncherUi.sectionTitle(context, "Warband Folder (SAF)"))
        folderStatusText = LauncherUi.captionText(context, "No Warband folder selected.")
        safCard.addView(folderStatusText)
        indexStatsText = LauncherUi.captionText(context, "SafVfs index: idle")
        safCard.addView(indexStatsText)
        verifyScriptsButton = LauncherUi.secondaryButton(
            context,
            "Verify scripts.txt via SafVfs",
            onVerifyScripts
        ).apply {
            isEnabled = false
        }
        safCard.addView(verifyScriptsButton)
        page.addView(safCard)

        val gpuCard = LauncherUi.card(context)
        gpuCard.addView(LauncherUi.sectionTitle(context, "GPU Driver"))
        driverRow = LauncherUi.spinnerRow(context, "Vulkan driver", driverOptions)
        driverRow.spinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(
                parent: AdapterView<*>?,
                view: View?,
                pos: Int,
                id: Long
            ) {
                if (pos == lastDriverPos) {
                    return
                }
                lastDriverPos = pos
                onDriverSelected(pos)
            }

            override fun onNothingSelected(parent: AdapterView<*>?) {}
        }
        gpuCard.addView(driverRow.root)
        driverStatusText = LauncherUi.captionText(context, "")
        gpuCard.addView(driverStatusText)
        gpuCard.addView(
            LauncherUi.secondaryButton(
                context,
                "Import Turnip / PanVK Driver Package (.zip)",
                onImportDriver
            )
        )
        mockFallbackCheck = LauncherUi.checkRow(
            context,
            "Allow mock ICD fallback (testing only)",
            checked = false
        )
        gpuCard.addView(mockFallbackCheck)
        probeButton = LauncherUi.secondaryButton(
            context,
            "Probe Vulkan Driver (CPU reference frame)",
            onProbeVulkan
        )
        gpuCard.addView(probeButton)
        page.addView(gpuCard)

        val runtimeCard = LauncherUi.card(context)
        runtimeCard.addView(LauncherUi.sectionTitle(context, "Runtime"))
        val prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
        val showFpsOverlayCheck = LauncherUi.checkRow(
            context,
            "Show FPS overlay",
            prefs.getBoolean(KEY_SHOW_FPS_OVERLAY, false)
        ) { isChecked ->
            prefs.edit().putBoolean(KEY_SHOW_FPS_OVERLAY, isChecked).apply()
        }
        runtimeCard.addView(showFpsOverlayCheck)
        runtimeCard.addView(
            LauncherUi.secondaryButton(
                context,
                "Launch WSE2 SDL3 Self-Test (WseActivity)",
                onLaunchSelfTest
            )
        )
        page.addView(runtimeCard)

        val logCard = LauncherUi.card(context)
        logCard.addView(LauncherUi.sectionTitle(context, "Status Log"))
        outputStatusText = LauncherUi.logText(context, "Ready.")
        logCard.addView(outputStatusText)
        page.addView(logCard)

        view = page.view
    }

    fun setFolderStatus(text: String) {
        folderStatusText.text = text
    }

    fun setIndexStats(text: String) {
        indexStatsText.text = text
    }

    fun setVerifyScriptsEnabled(enabled: Boolean) {
        verifyScriptsButton.isEnabled = enabled
    }

    fun setDriverSelection(index: Int) {
        lastDriverPos = index
        driverRow.spinner.setSelection(index)
    }

    fun selectedDriverIndex(): Int = driverRow.spinner.selectedItemPosition

    fun setDriverStatus(text: String) {
        driverStatusText.text = text
    }

    fun isMockFallbackChecked(): Boolean = mockFallbackCheck.isChecked

    fun setProbeEnabled(enabled: Boolean) {
        probeButton.isEnabled = enabled
    }

    fun setOutputStatus(text: String) {
        outputStatusText.text = text
    }

    companion object {
        private const val PREFS_NAME = "wse2_launcher"
        private const val KEY_SHOW_FPS_OVERLAY = "show_fps_overlay"
    }
}
