package org.wse2.android.launcher

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Typeface
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.TextView
import org.json.JSONObject
import java.util.Locale
import kotlin.math.isFinite
import kotlin.math.roundToInt

data class PerformanceUiState(
    val qualityPresetKey: String,
    val frameRateCapHz: Int,
    val presentModeKey: String,
    val renderScalePercent: Int,
    val maxTextureSize: Int,
    val thermalThrottlingEnabled: Boolean,
    val dynamicResolutionEnabled: Boolean,
    val lodBiasScalingEnabled: Boolean,
    val uiAspectKey: String,
    val uiScalePercent: Int,
    val gyroAimEnabled: Boolean,
    val pinchZoomEnabled: Boolean,
    val language: String
) {
    companion object {
        fun fromJson(rawJson: String): PerformanceUiState {
            val obj = try {
                JSONObject(rawJson)
            } catch (_: Exception) {
                JSONObject()
            }
            val preset = optStringClean(obj, "quality_preset", "custom")
                .lowercase(Locale.US)
                .takeIf { it in VALID_QUALITY_PRESETS }
                ?: "custom"
            val capHz = parseFrameRateCap(obj)
            val presentMode = optStringClean(obj, "present_mode", "fifo")
                .lowercase(Locale.US)
                .takeIf { it in VALID_PRESENT_MODES }
                ?: "fifo"
            val renderScalePct = parseSteppedPercent(
                obj = obj,
                key = "render_scale",
                defaultScale = 1.0,
                minPct = 50,
                maxPct = 100,
                stepPct = 5
            )
            val maxTex = parseIntSetting(obj, "max_texture_size", 0).coerceAtLeast(0)
            val uiAspect = optStringClean(obj, "ui_aspect", "4:3")
                .lowercase(Locale.US)
                .takeIf { it in VALID_UI_ASPECTS }
                ?: "4:3"
            val uiScalePct = parseSteppedPercent(
                obj = obj,
                key = "ui_scale",
                defaultScale = 1.0,
                minPct = 75,
                maxPct = 200,
                stepPct = 5
            )
            val language = optStringClean(obj, "language", "en").ifEmpty { "en" }

            return PerformanceUiState(
                qualityPresetKey = preset,
                frameRateCapHz = capHz,
                presentModeKey = presentMode,
                renderScalePercent = renderScalePct,
                maxTextureSize = maxTex,
                thermalThrottlingEnabled = parseBoolSetting(obj, "thermal_throttling_enabled", true),
                dynamicResolutionEnabled = parseBoolSetting(obj, "dynamic_resolution_enabled", true),
                lodBiasScalingEnabled = parseBoolSetting(obj, "lod_bias_scaling_enabled", true),
                uiAspectKey = uiAspect,
                uiScalePercent = uiScalePct,
                gyroAimEnabled = parseBoolSetting(obj, "gyro_aim_enabled", false),
                pinchZoomEnabled = parseBoolSetting(obj, "pinch_zoom_enabled", true),
                language = language
            )
        }

        private val VALID_QUALITY_PRESETS = setOf("low", "medium", "high", "custom")
        private val VALID_PRESENT_MODES = setOf("fifo", "mailbox", "immediate", "fifo_relaxed")
        private val VALID_UI_ASPECTS = setOf("4:3", "16:9", "stretch")

        private fun optStringClean(obj: JSONObject, key: String, fallback: String): String {
            if (!obj.has(key) || obj.isNull(key)) {
                return fallback
            }
            return obj.optString(key, fallback).trim()
        }

        private fun parseFrameRateCap(obj: JSONObject): Int {
            val raw = if (obj.has("frame_rate_cap") && !obj.isNull("frame_rate_cap")) {
                obj.opt("frame_rate_cap")
            } else {
                null
            }
            if (raw is Number) {
                val hz = raw.toInt()
                return if (hz <= 0) 0 else hz
            }
            val text = (raw as? String)?.trim()?.lowercase(Locale.US) ?: ""
            return when {
                text == "uncapped" || text == "unlimited" || text == "0" -> 0
                text.endsWith("hz") -> text.removeSuffix("hz").toIntOrNull()?.let { if (it <= 0) 0 else it } ?: 60
                text.isNotEmpty() -> text.toIntOrNull()?.let { if (it <= 0) 0 else it } ?: 60
                else -> 60
            }
        }

        private fun parseIntSetting(obj: JSONObject, key: String, fallback: Int): Int {
            if (!obj.has(key) || obj.isNull(key)) {
                return fallback
            }
            return when (val raw = obj.opt(key)) {
                is Number -> raw.toInt()
                is String -> raw.trim().toIntOrNull() ?: fallback
                else -> fallback
            }
        }

        private fun parseSteppedPercent(
            obj: JSONObject,
            key: String,
            defaultScale: Double,
            minPct: Int,
            maxPct: Int,
            stepPct: Int
        ): Int {
            val raw = if (obj.has(key) && !obj.isNull(key)) obj.opt(key) else null
            val scale = when (raw) {
                is Number -> raw.toDouble()
                is String -> raw.trim().toDoubleOrNull()
                else -> null
            }
            val valid = if (scale != null && scale.isFinite()) scale else defaultScale
            val clampedPct = (valid * 100.0).roundToInt().coerceIn(minPct, maxPct)
            val steps = ((clampedPct - minPct + (stepPct / 2)) / stepPct)
            return (minPct + steps * stepPct).coerceIn(minPct, maxPct)
        }

        private fun parseBoolSetting(obj: JSONObject, key: String, fallback: Boolean): Boolean {
            if (!obj.has(key) || obj.isNull(key)) {
                return fallback
            }
            return when (val raw = obj.opt(key)) {
                is Boolean -> raw
                is Number -> raw.toInt() != 0
                is String -> when (raw.trim().lowercase(Locale.US)) {
                    "true", "1" -> true
                    "false", "0" -> false
                    else -> fallback
                }
                else -> fallback
            }
        }
    }
}

class SettingsPage(
    context: Context,
    private val frameRateValues: List<Int>,
    frameRateLabels: List<String>,
    private val presentModeKeys: List<String>,
    presentModeLabels: List<String>,
    hasPhoneGyro: Boolean,
    private val onSaveChoices: (choicesJson: String, refreshGraphics: Boolean) -> Unit
) {
    val view: View

    private val qualityPresetRow: LauncherUi.SpinnerRow
    private val frameRateRow: LauncherUi.SpinnerRow
    private val presentModeRow: LauncherUi.SpinnerRow
    private val renderScaleRow: SliderRow
    private val textureCapRow: LauncherUi.SpinnerRow
    private val thermalThrottleCheck: CheckBox
    private val dynamicResolutionCheck: CheckBox
    private val lodBiasScalingCheck: CheckBox

    private val uiAspectRow: LauncherUi.SpinnerRow
    private val uiScaleRow: SliderRow

    private val gyroAimCheck: CheckBox
    private val pinchZoomCheck: CheckBox

    private val languageRow: LauncherUi.SpinnerRow

    private var scannedLanguages: List<String> = emptyList()
    private var languageOptions: List<String> = listOf("en")

    private var isBinding = false
    private var isTrackingRenderScale = false
    private var isTrackingUiScale = false
    private var lastCommittedState: PerformanceUiState? = null

    private class SliderRow(
        val root: LinearLayout,
        val valueLabel: TextView,
        val seekBar: SeekBar
    )

    private enum class ChangeSource {
        QUALITY_PRESET,
        FRAME_RATE_CAP,
        PRESENT_MODE,
        RENDER_SCALE,
        MAX_TEXTURE_SIZE,
        THERMAL_THROTTLING,
        DYNAMIC_RESOLUTION,
        LOD_BIAS_SCALING,
        UI_ASPECT,
        UI_SCALE,
        GYRO_AIM,
        PINCH_ZOOM,
        LANGUAGE
    }

    init {
        val page = LauncherUi.page(context)
        page.addView(LauncherUi.pageTitle(context, "Settings"))

        val graphicsCard = LauncherUi.card(context)
        graphicsCard.addView(LauncherUi.sectionTitle(context, "Graphics"))
        qualityPresetRow = LauncherUi.spinnerRow(context, "Quality preset", QUALITY_PRESET_LABELS)
        graphicsCard.addView(qualityPresetRow.root)
        frameRateRow = LauncherUi.spinnerRow(context, "Frame-rate cap", frameRateLabels)
        graphicsCard.addView(frameRateRow.root)
        presentModeRow = LauncherUi.spinnerRow(context, "Present mode", presentModeLabels)
        graphicsCard.addView(presentModeRow.root)

        thermalThrottleCheck = LauncherUi.checkRow(
            context,
            "Thermal headroom throttling (res / LOD / FPS)",
            checked = true
        )
        val accentTint = thermalThrottleCheck.buttonTintList
        val trackTint = frameRateRow.spinner.backgroundTintList

        renderScaleRow = sliderRow(
            context = context,
            label = "Render scale",
            maxSteps = (RENDER_SCALE_MAX - RENDER_SCALE_MIN) / SCALE_STEP,
            initialValueLabel = "100%",
            tint = accentTint,
            trackTint = trackTint
        )
        graphicsCard.addView(renderScaleRow.root)

        textureCapRow = LauncherUi.spinnerRow(context, "Texture size cap", TEXTURE_SIZE_LABELS)
        graphicsCard.addView(textureCapRow.root)

        graphicsCard.addView(thermalThrottleCheck)
        dynamicResolutionCheck = LauncherUi.checkRow(
            context,
            "Dynamic resolution under thermal load",
            checked = true
        )
        graphicsCard.addView(dynamicResolutionCheck)
        lodBiasScalingCheck = LauncherUi.checkRow(
            context,
            "LOD bias scaling under thermal load",
            checked = true
        )
        graphicsCard.addView(lodBiasScalingCheck)
        page.addView(graphicsCard)

        val displayCard = LauncherUi.card(context)
        displayCard.addView(LauncherUi.sectionTitle(context, "Display"))
        uiAspectRow = LauncherUi.spinnerRow(context, "UI aspect", UI_ASPECT_LABELS)
        displayCard.addView(uiAspectRow.root)
        uiScaleRow = sliderRow(
            context = context,
            label = "UI scale",
            maxSteps = (UI_SCALE_MAX - UI_SCALE_MIN) / SCALE_STEP,
            initialValueLabel = "100%",
            tint = accentTint,
            trackTint = trackTint
        )
        displayCard.addView(uiScaleRow.root)
        displayCard.addView(
            LauncherUi.captionText(context, "Applies to the in-game interface in a later update.")
        )
        page.addView(displayCard)

        val controlsCard = LauncherUi.card(context)
        controlsCard.addView(LauncherUi.sectionTitle(context, "Controls"))
        // Controller gyros (DualSense, Switch Pro) work without a phone gyroscope.
        val gyroLabel = if (hasPhoneGyro) {
            "Opt-in gyro aiming (Mission)"
        } else {
            "Opt-in gyro aiming (Mission) - no phone gyroscope, controller gyro only"
        }
        gyroAimCheck = LauncherUi.checkRow(context, gyroLabel, checked = false)
        controlsCard.addView(gyroAimCheck)
        pinchZoomCheck = LauncherUi.checkRow(
            context,
            "Pinch-to-zoom (Campaign map)",
            checked = true
        )
        controlsCard.addView(pinchZoomCheck)
        page.addView(controlsCard)

        val languageCard = LauncherUi.card(context)
        languageCard.addView(LauncherUi.sectionTitle(context, "Language"))
        languageRow = LauncherUi.spinnerRow(context, "Language", languageOptions)
        languageCard.addView(languageRow.root)
        page.addView(languageCard)

        setControlsEnabled(false)
        attachListeners()

        view = page.view
    }

    fun applyState(state: PerformanceUiState) {
        isBinding = true
        try {
            setControlsEnabled(true)
            bindGraphicsControls(state)

            val aspectIdx = UI_ASPECT_KEYS.indexOf(state.uiAspectKey).coerceAtLeast(0)
            uiAspectRow.spinner.setSelection(aspectIdx)

            val uiPct = snapPercent(state.uiScalePercent, UI_SCALE_MIN, UI_SCALE_MAX)
            uiScaleRow.seekBar.progress = (uiPct - UI_SCALE_MIN) / SCALE_STEP
            uiScaleRow.valueLabel.text = "$uiPct%"

            gyroAimCheck.isChecked = state.gyroAimEnabled
            pinchZoomCheck.isChecked = state.pinchZoomEnabled

            updateLanguageSpinner(state.language)

            lastCommittedState = currentState()
        } finally {
            isBinding = false
        }
    }

    fun applyGraphicsState(state: PerformanceUiState) {
        isBinding = true
        try {
            bindGraphicsControls(state)
            lastCommittedState = currentState()
        } finally {
            isBinding = false
        }
    }

    fun setAvailableLanguages(languages: List<String>) {
        val cleaned = languages
            .map { it.trim() }
            .filter { isValidLanguageTag(it) }
            .distinct()
        if (cleaned == scannedLanguages) {
            return
        }
        scannedLanguages = cleaned
        val activeLanguage = lastCommittedState?.language ?: "en"
        isBinding = true
        try {
            updateLanguageSpinner(activeLanguage)
        } finally {
            isBinding = false
        }
    }

    fun currentState(): PerformanceUiState {
        val presetIdx = qualityPresetRow.spinner.selectedItemPosition
            .coerceIn(0, QUALITY_PRESET_KEYS.lastIndex)
        val capIdx = frameRateRow.spinner.selectedItemPosition
            .coerceIn(0, frameRateValues.lastIndex)
        val modeIdx = presentModeRow.spinner.selectedItemPosition
            .coerceIn(0, presentModeKeys.lastIndex)
        val texIdx = textureCapRow.spinner.selectedItemPosition
            .coerceIn(0, TEXTURE_SIZE_VALUES.lastIndex)
        val aspectIdx = uiAspectRow.spinner.selectedItemPosition
            .coerceIn(0, UI_ASPECT_KEYS.lastIndex)
        val langIdx = languageRow.spinner.selectedItemPosition
            .coerceIn(0, languageOptions.lastIndex)

        return PerformanceUiState(
            qualityPresetKey = QUALITY_PRESET_KEYS[presetIdx],
            frameRateCapHz = frameRateValues[capIdx],
            presentModeKey = presentModeKeys[modeIdx],
            renderScalePercent = progressToRenderScalePercent(renderScaleRow.seekBar.progress),
            maxTextureSize = TEXTURE_SIZE_VALUES[texIdx],
            thermalThrottlingEnabled = thermalThrottleCheck.isChecked,
            dynamicResolutionEnabled = dynamicResolutionCheck.isChecked,
            lodBiasScalingEnabled = lodBiasScalingCheck.isChecked,
            uiAspectKey = UI_ASPECT_KEYS[aspectIdx],
            uiScalePercent = progressToUiScalePercent(uiScaleRow.seekBar.progress),
            gyroAimEnabled = gyroAimCheck.isChecked,
            pinchZoomEnabled = pinchZoomCheck.isChecked,
            language = languageOptions[langIdx]
        )
    }

    private fun bindGraphicsControls(state: PerformanceUiState) {
        val presetIdx = QUALITY_PRESET_KEYS.indexOf(state.qualityPresetKey).let { idx ->
            if (idx >= 0) idx else QUALITY_PRESET_KEYS.indexOf(QUALITY_PRESET_CUSTOM)
        }
        qualityPresetRow.spinner.setSelection(presetIdx)

        // A cap saved on another display (or before a panel mode change) falls back to the
        // highest offered rate below it.
        val capIdx = frameRateValues.indexOf(state.frameRateCapHz).let { exact ->
            if (exact >= 0) {
                exact
            } else {
                val best = frameRateValues.filter { it in 1..state.frameRateCapHz }.maxOrNull()
                    ?: frameRateValues.firstOrNull { it > 0 }
                    ?: 0
                frameRateValues.indexOf(best).coerceAtLeast(0)
            }
        }
        frameRateRow.spinner.setSelection(capIdx)

        val modeIdx = presentModeKeys.indexOf(state.presentModeKey).coerceAtLeast(0)
        presentModeRow.spinner.setSelection(modeIdx)

        val renderPct = snapPercent(state.renderScalePercent, RENDER_SCALE_MIN, RENDER_SCALE_MAX)
        renderScaleRow.seekBar.progress = (renderPct - RENDER_SCALE_MIN) / SCALE_STEP
        renderScaleRow.valueLabel.text = "$renderPct%"

        val texIdx = TEXTURE_SIZE_VALUES.indexOf(state.maxTextureSize).let { exact ->
            if (exact >= 0) {
                exact
            } else if (state.maxTextureSize <= 0) {
                0
            } else {
                val best = TEXTURE_SIZE_VALUES.filter { it in 1..state.maxTextureSize }.maxOrNull() ?: 0
                TEXTURE_SIZE_VALUES.indexOf(best).coerceAtLeast(0)
            }
        }
        textureCapRow.spinner.setSelection(texIdx)

        thermalThrottleCheck.isChecked = state.thermalThrottlingEnabled
        dynamicResolutionCheck.isChecked = state.dynamicResolutionEnabled
        lodBiasScalingCheck.isChecked = state.lodBiasScalingEnabled
    }

    private fun updateLanguageSpinner(activeLanguage: String) {
        val nextOptions = buildLanguageOptions(activeLanguage)
        if (nextOptions != languageOptions) {
            languageOptions = nextOptions
            @Suppress("UNCHECKED_CAST")
            val adapter = languageRow.spinner.adapter as? ArrayAdapter<String>
            if (adapter != null) {
                adapter.setNotifyOnChange(false)
                adapter.clear()
                adapter.addAll(nextOptions)
                adapter.notifyDataSetChanged()
            }
        }
        val idx = languageOptions.indexOf(activeLanguage).coerceAtLeast(0)
        languageRow.spinner.setSelection(idx)
    }

    private fun buildLanguageOptions(activeLanguage: String): List<String> {
        val base = if (scannedLanguages.isNotEmpty()) scannedLanguages else listOf("en")
        val merged = ArrayList<String>(base.size + 1)
        for (lang in base) {
            if (lang.isNotEmpty() && !merged.contains(lang)) {
                merged.add(lang)
            }
        }
        if (activeLanguage.isNotEmpty() && !merged.contains(activeLanguage)) {
            merged.add(activeLanguage)
        }
        if (merged.isEmpty()) {
            merged.add("en")
        }
        return merged
    }

    private fun setControlsEnabled(enabled: Boolean) {
        qualityPresetRow.spinner.isEnabled = enabled
        frameRateRow.spinner.isEnabled = enabled
        presentModeRow.spinner.isEnabled = enabled
        renderScaleRow.seekBar.isEnabled = enabled
        textureCapRow.spinner.isEnabled = enabled
        thermalThrottleCheck.isEnabled = enabled
        dynamicResolutionCheck.isEnabled = enabled
        lodBiasScalingCheck.isEnabled = enabled
        uiAspectRow.spinner.isEnabled = enabled
        uiScaleRow.seekBar.isEnabled = enabled
        gyroAimCheck.isEnabled = enabled
        pinchZoomCheck.isEnabled = enabled
        languageRow.spinner.isEnabled = enabled
    }

    private fun attachListeners() {
        fun bindSpinner(row: LauncherUi.SpinnerRow, source: ChangeSource) {
            row.spinner.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(
                    parent: AdapterView<*>?,
                    view: View?,
                    pos: Int,
                    id: Long
                ) {
                    if (!isBinding) {
                        commitChange(source)
                    }
                }

                override fun onNothingSelected(parent: AdapterView<*>?) {}
            }
        }

        bindSpinner(qualityPresetRow, ChangeSource.QUALITY_PRESET)
        bindSpinner(frameRateRow, ChangeSource.FRAME_RATE_CAP)
        bindSpinner(presentModeRow, ChangeSource.PRESENT_MODE)
        bindSpinner(textureCapRow, ChangeSource.MAX_TEXTURE_SIZE)
        bindSpinner(uiAspectRow, ChangeSource.UI_ASPECT)
        bindSpinner(languageRow, ChangeSource.LANGUAGE)

        renderScaleRow.seekBar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                val pct = progressToRenderScalePercent(progress)
                renderScaleRow.valueLabel.text = "$pct%"
                if (!isBinding && fromUser && !isTrackingRenderScale) {
                    commitChange(ChangeSource.RENDER_SCALE)
                }
            }

            override fun onStartTrackingTouch(seekBar: SeekBar?) {
                isTrackingRenderScale = true
            }

            override fun onStopTrackingTouch(seekBar: SeekBar?) {
                isTrackingRenderScale = false
                if (!isBinding) {
                    commitChange(ChangeSource.RENDER_SCALE)
                }
            }
        })

        uiScaleRow.seekBar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                val pct = progressToUiScalePercent(progress)
                uiScaleRow.valueLabel.text = "$pct%"
                if (!isBinding && fromUser && !isTrackingUiScale) {
                    commitChange(ChangeSource.UI_SCALE)
                }
            }

            override fun onStartTrackingTouch(seekBar: SeekBar?) {
                isTrackingUiScale = true
            }

            override fun onStopTrackingTouch(seekBar: SeekBar?) {
                isTrackingUiScale = false
                if (!isBinding) {
                    commitChange(ChangeSource.UI_SCALE)
                }
            }
        })

        thermalThrottleCheck.setOnCheckedChangeListener { _, _ ->
            if (!isBinding) commitChange(ChangeSource.THERMAL_THROTTLING)
        }
        dynamicResolutionCheck.setOnCheckedChangeListener { _, _ ->
            if (!isBinding) commitChange(ChangeSource.DYNAMIC_RESOLUTION)
        }
        lodBiasScalingCheck.setOnCheckedChangeListener { _, _ ->
            if (!isBinding) commitChange(ChangeSource.LOD_BIAS_SCALING)
        }
        gyroAimCheck.setOnCheckedChangeListener { _, _ ->
            if (!isBinding) commitChange(ChangeSource.GYRO_AIM)
        }
        pinchZoomCheck.setOnCheckedChangeListener { _, _ ->
            if (!isBinding) commitChange(ChangeSource.PINCH_ZOOM)
        }
    }

    private fun commitChange(source: ChangeSource) {
        if (isBinding) {
            return
        }
        val prev = lastCommittedState ?: return
        when (source) {
            ChangeSource.QUALITY_PRESET -> {
                val idx = qualityPresetRow.spinner.selectedItemPosition
                    .coerceIn(0, QUALITY_PRESET_KEYS.lastIndex)
                val preset = QUALITY_PRESET_KEYS[idx]
                if (preset == prev.qualityPresetKey) {
                    return
                }
                lastCommittedState = prev.copy(qualityPresetKey = preset)
                val json = JSONObject().put("quality_preset", preset).toString()
                onSaveChoices(json, preset != QUALITY_PRESET_CUSTOM)
            }

            ChangeSource.FRAME_RATE_CAP -> {
                val idx = frameRateRow.spinner.selectedItemPosition
                    .coerceIn(0, frameRateValues.lastIndex)
                val capHz = frameRateValues[idx]
                if (capHz == prev.frameRateCapHz) {
                    return
                }
                selectQualityPresetCustomSilently()
                lastCommittedState = prev.copy(
                    qualityPresetKey = QUALITY_PRESET_CUSTOM,
                    frameRateCapHz = capHz
                )
                val json = JSONObject()
                    .put("frame_rate_cap", frameRateCapKey(capHz))
                    .put("quality_preset", QUALITY_PRESET_CUSTOM)
                    .toString()
                onSaveChoices(json, false)
            }

            ChangeSource.PRESENT_MODE -> {
                val idx = presentModeRow.spinner.selectedItemPosition
                    .coerceIn(0, presentModeKeys.lastIndex)
                val mode = presentModeKeys[idx]
                if (mode == prev.presentModeKey) {
                    return
                }
                selectQualityPresetCustomSilently()
                lastCommittedState = prev.copy(
                    qualityPresetKey = QUALITY_PRESET_CUSTOM,
                    presentModeKey = mode
                )
                val json = JSONObject()
                    .put("present_mode", mode)
                    .put("quality_preset", QUALITY_PRESET_CUSTOM)
                    .toString()
                onSaveChoices(json, false)
            }

            ChangeSource.RENDER_SCALE -> {
                val pct = progressToRenderScalePercent(renderScaleRow.seekBar.progress)
                if (pct == prev.renderScalePercent) {
                    return
                }
                selectQualityPresetCustomSilently()
                lastCommittedState = prev.copy(
                    qualityPresetKey = QUALITY_PRESET_CUSTOM,
                    renderScalePercent = pct
                )
                val json = JSONObject()
                    .put("render_scale", formatScaleDecimal(pct))
                    .put("quality_preset", QUALITY_PRESET_CUSTOM)
                    .toString()
                onSaveChoices(json, false)
            }

            ChangeSource.MAX_TEXTURE_SIZE -> {
                val idx = textureCapRow.spinner.selectedItemPosition
                    .coerceIn(0, TEXTURE_SIZE_VALUES.lastIndex)
                val maxTex = TEXTURE_SIZE_VALUES[idx]
                if (maxTex == prev.maxTextureSize) {
                    return
                }
                selectQualityPresetCustomSilently()
                lastCommittedState = prev.copy(
                    qualityPresetKey = QUALITY_PRESET_CUSTOM,
                    maxTextureSize = maxTex
                )
                val json = JSONObject()
                    .put("max_texture_size", maxTex.toString())
                    .put("quality_preset", QUALITY_PRESET_CUSTOM)
                    .toString()
                onSaveChoices(json, false)
            }

            ChangeSource.THERMAL_THROTTLING -> {
                val checked = thermalThrottleCheck.isChecked
                if (checked == prev.thermalThrottlingEnabled) {
                    return
                }
                selectQualityPresetCustomSilently()
                lastCommittedState = prev.copy(
                    qualityPresetKey = QUALITY_PRESET_CUSTOM,
                    thermalThrottlingEnabled = checked
                )
                val json = JSONObject()
                    .put("thermal_throttling_enabled", checked.toString())
                    .put("quality_preset", QUALITY_PRESET_CUSTOM)
                    .toString()
                onSaveChoices(json, false)
            }

            ChangeSource.DYNAMIC_RESOLUTION -> {
                val checked = dynamicResolutionCheck.isChecked
                if (checked == prev.dynamicResolutionEnabled) {
                    return
                }
                selectQualityPresetCustomSilently()
                lastCommittedState = prev.copy(
                    qualityPresetKey = QUALITY_PRESET_CUSTOM,
                    dynamicResolutionEnabled = checked
                )
                val json = JSONObject()
                    .put("dynamic_resolution_enabled", checked.toString())
                    .put("quality_preset", QUALITY_PRESET_CUSTOM)
                    .toString()
                onSaveChoices(json, false)
            }

            ChangeSource.LOD_BIAS_SCALING -> {
                val checked = lodBiasScalingCheck.isChecked
                if (checked == prev.lodBiasScalingEnabled) {
                    return
                }
                selectQualityPresetCustomSilently()
                lastCommittedState = prev.copy(
                    qualityPresetKey = QUALITY_PRESET_CUSTOM,
                    lodBiasScalingEnabled = checked
                )
                val json = JSONObject()
                    .put("lod_bias_scaling_enabled", checked.toString())
                    .put("quality_preset", QUALITY_PRESET_CUSTOM)
                    .toString()
                onSaveChoices(json, false)
            }

            ChangeSource.UI_ASPECT -> {
                val idx = uiAspectRow.spinner.selectedItemPosition
                    .coerceIn(0, UI_ASPECT_KEYS.lastIndex)
                val aspect = UI_ASPECT_KEYS[idx]
                if (aspect == prev.uiAspectKey) {
                    return
                }
                lastCommittedState = prev.copy(uiAspectKey = aspect)
                val json = JSONObject().put("ui_aspect", aspect).toString()
                onSaveChoices(json, false)
            }

            ChangeSource.UI_SCALE -> {
                val pct = progressToUiScalePercent(uiScaleRow.seekBar.progress)
                if (pct == prev.uiScalePercent) {
                    return
                }
                lastCommittedState = prev.copy(uiScalePercent = pct)
                val json = JSONObject().put("ui_scale", formatScaleDecimal(pct)).toString()
                onSaveChoices(json, false)
            }

            ChangeSource.GYRO_AIM -> {
                val checked = gyroAimCheck.isChecked
                if (checked == prev.gyroAimEnabled) {
                    return
                }
                lastCommittedState = prev.copy(gyroAimEnabled = checked)
                val json = JSONObject().put("gyro_aim_enabled", checked.toString()).toString()
                onSaveChoices(json, false)
            }

            ChangeSource.PINCH_ZOOM -> {
                val checked = pinchZoomCheck.isChecked
                if (checked == prev.pinchZoomEnabled) {
                    return
                }
                lastCommittedState = prev.copy(pinchZoomEnabled = checked)
                val json = JSONObject().put("pinch_zoom_enabled", checked.toString()).toString()
                onSaveChoices(json, false)
            }

            ChangeSource.LANGUAGE -> {
                val idx = languageRow.spinner.selectedItemPosition
                if (idx !in languageOptions.indices) {
                    return
                }
                val lang = languageOptions[idx]
                if (lang == prev.language) {
                    return
                }
                lastCommittedState = prev.copy(language = lang)
                val json = JSONObject().put("language", lang).toString()
                onSaveChoices(json, false)
            }
        }
    }

    private fun selectQualityPresetCustomSilently() {
        val customIdx = QUALITY_PRESET_KEYS.indexOf(QUALITY_PRESET_CUSTOM)
        if (customIdx >= 0 && qualityPresetRow.spinner.selectedItemPosition != customIdx) {
            isBinding = true
            try {
                qualityPresetRow.spinner.setSelection(customIdx)
            } finally {
                isBinding = false
            }
        }
    }

    private fun sliderRow(
        context: Context,
        label: String,
        maxSteps: Int,
        initialValueLabel: String,
        tint: ColorStateList?,
        trackTint: ColorStateList?
    ): SliderRow {
        val density = context.resources.displayMetrics.density
        fun dp(v: Int): Int = (v * density).toInt()

        val root = LauncherUi.verticalGroup(context).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                bottomMargin = dp(8)
            }
        }
        val header = LinearLayout(context).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
        val titleView = LauncherUi.captionText(context, label).apply {
            setPadding(0, 0, dp(8), 0)
            layoutParams = LinearLayout.LayoutParams(
                0,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                1f
            )
        }
        val valueView = LauncherUi.bodyText(context, initialValueLabel).apply {
            setTypeface(Typeface.DEFAULT, Typeface.BOLD)
            setPadding(0, 0, 0, 0)
            gravity = Gravity.END
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
        header.addView(titleView)
        header.addView(valueView)

        val seekBar = SeekBar(context).apply {
            max = maxSteps
            minimumHeight = dp(48)
            contentDescription = label
            if (tint != null) {
                progressTintList = tint
                thumbTintList = tint
            }
            if (trackTint != null) {
                progressBackgroundTintList = trackTint
            }
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
        root.addView(header)
        root.addView(seekBar)
        return SliderRow(root, valueView, seekBar)
    }

    private fun progressToRenderScalePercent(progress: Int): Int =
        (RENDER_SCALE_MIN + progress * SCALE_STEP).coerceIn(RENDER_SCALE_MIN, RENDER_SCALE_MAX)

    private fun progressToUiScalePercent(progress: Int): Int =
        (UI_SCALE_MIN + progress * SCALE_STEP).coerceIn(UI_SCALE_MIN, UI_SCALE_MAX)

    private fun snapPercent(percent: Int, minPct: Int, maxPct: Int): Int {
        val clamped = percent.coerceIn(minPct, maxPct)
        val steps = (clamped - minPct + (SCALE_STEP / 2)) / SCALE_STEP
        return (minPct + steps * SCALE_STEP).coerceIn(minPct, maxPct)
    }

    private fun frameRateCapKey(hz: Int): String = if (hz <= 0) "uncapped" else hz.toString()

    private fun formatScaleDecimal(percent: Int): String {
        val whole = percent / 100
        val frac = percent % 100
        return when {
            frac == 0 -> whole.toString()
            frac % 10 == 0 -> "$whole.${frac / 10}"
            else -> String.format(Locale.US, "%d.%02d", whole, frac)
        }
    }

    private fun isValidLanguageTag(tag: String): Boolean {
        if (tag.isEmpty() || tag.length > 16) {
            return false
        }
        return tag.all { c ->
            c in 'A'..'Z' || c in 'a'..'z' || c in '0'..'9' || c == '_' || c == '-'
        }
    }

    companion object {
        private const val QUALITY_PRESET_CUSTOM = "custom"
        private val QUALITY_PRESET_LABELS = listOf("Low", "Medium", "High", "Custom")
        private val QUALITY_PRESET_KEYS = listOf("low", "medium", "high", "custom")

        private val TEXTURE_SIZE_LABELS = listOf("No cap", "256", "512", "1024", "2048", "4096")
        private val TEXTURE_SIZE_VALUES = listOf(0, 256, 512, 1024, 2048, 4096)

        private val UI_ASPECT_LABELS = listOf("4:3", "16:9", "Stretch")
        private val UI_ASPECT_KEYS = listOf("4:3", "16:9", "stretch")

        private const val RENDER_SCALE_MIN = 50
        private const val RENDER_SCALE_MAX = 100
        private const val UI_SCALE_MIN = 75
        private const val UI_SCALE_MAX = 200
        private const val SCALE_STEP = 5
    }
}
