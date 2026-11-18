package org.wse2.android.launcher.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CutCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.minimumInteractiveComponentSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.setProgress
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import kotlin.math.roundToInt
import org.wse2.android.launcher.theme.LocalWseColors
import org.wse2.android.launcher.theme.Wse2Theme
import org.wse2.android.launcher.theme.WseShapes
import org.wse2.android.launcher.theme.WseType
import org.wse2.android.launcher.theme.wseBackdrop

@Composable
fun WseSwitch(
    checked: Boolean,
    onCheckedChange: ((Boolean) -> Unit)?,
    modifier: Modifier = Modifier,
    enabled: Boolean = true
) {
    val colors = LocalWseColors.current
    val shape = WseShapes.small
    val thumbOffset by animateDpAsState(
        targetValue = if (checked) 22.dp else 2.dp,
        animationSpec = tween(durationMillis = 150, easing = FastOutSlowInEasing),
        label = "wse-switch-thumb"
    )
    val trackColor by animateColorAsState(
        targetValue = if (checked) colors.primary else colors.surfaceContainerHigh,
        animationSpec = tween(durationMillis = 150),
        label = "wse-switch-track"
    )
    val toggleModifier = if (onCheckedChange != null) {
        Modifier.toggleable(
            value = checked,
            enabled = enabled,
            role = Role.Switch,
            onValueChange = onCheckedChange
        )
    } else {
        Modifier
    }
    Box(
        modifier = modifier
            .minimumInteractiveComponentSize()
            .then(toggleModifier)
            .alpha(if (enabled) 1f else 0.45f),
        contentAlignment = Alignment.Center
    ) {
        Box(
            modifier = Modifier
                .size(width = 44.dp, height = 24.dp)
                .clip(shape)
                .background(trackColor)
                .border(1.dp, if (checked) colors.primaryGradientTop else colors.hairline, shape)
        ) {
            Box(
                modifier = Modifier
                    .align(Alignment.CenterStart)
                    .offset(x = thumbOffset)
                    .size(20.dp)
                    .clip(CutCornerShape(3.dp))
                    .background(if (checked) colors.onPrimary else colors.outline)
            )
        }
    }
}

@Composable
fun WseSlider(
    value: Float,
    onValueChange: (Float) -> Unit,
    valueRange: ClosedFloatingPointRange<Float>,
    valueLabel: String,
    modifier: Modifier = Modifier,
    steps: Int = 0,
    enabled: Boolean = true,
    label: String? = null,
    onValueChangeFinished: (() -> Unit)? = null
) {
    val colors = LocalWseColors.current
    Column(
        modifier = modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(4.dp)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = if (label != null) Arrangement.SpaceBetween else Arrangement.End,
            verticalAlignment = Alignment.CenterVertically
        ) {
            if (label != null) {
                Text(
                    text = label,
                    style = MaterialTheme.typography.titleMedium,
                    color = if (enabled) {
                        colors.onSurface
                    } else {
                        colors.onSurfaceVariant.copy(alpha = 0.45f)
                    },
                    modifier = Modifier.weight(1f)
                )
            }
            Text(
                text = valueLabel,
                style = WseType.overline,
                color = if (enabled) {
                    colors.primary
                } else {
                    colors.onSurfaceVariant.copy(alpha = 0.45f)
                }
            )
        }
        WseSliderTrack(
            value = value,
            onValueChange = onValueChange,
            valueRange = valueRange,
            steps = steps,
            enabled = enabled,
            onValueChangeFinished = onValueChangeFinished
        )
    }
}

// A 4 dp gold track with a chamfered diamond thumb; no tick marks. `steps` intermediate
// stops snap like the Material slider (`steps + 1` equal intervals).
@Composable
private fun WseSliderTrack(
    value: Float,
    onValueChange: (Float) -> Unit,
    valueRange: ClosedFloatingPointRange<Float>,
    steps: Int,
    enabled: Boolean,
    onValueChangeFinished: (() -> Unit)?
) {
    val colors = LocalWseColors.current
    val start = valueRange.start
    val end = valueRange.endInclusive
    val span = (end - start).takeIf { it > 0f } ?: 1f
    val fraction = ((value - start) / span).coerceIn(0f, 1f)
    val currentOnChange by rememberUpdatedState(onValueChange)
    val currentOnFinished by rememberUpdatedState(onValueChangeFinished)
    val activeColor = if (enabled) colors.primary else colors.outline.copy(alpha = 0.45f)
    val inactiveColor = colors.outlineVariant
    val thumbTop = if (enabled) colors.primaryGradientTop else colors.outline
    val thumbBottom = if (enabled) colors.primaryGradientBottom else colors.outlineVariant
    val thumbEdge = if (enabled) colors.onPrimaryContainer.copy(alpha = 0.55f) else colors.outlineVariant

    fun snap(f: Float): Float {
        val clamped = f.coerceIn(0f, 1f)
        if (steps <= 0) return start + clamped * span
        val intervals = steps + 1
        return start + (clamped * intervals).roundToInt() / intervals.toFloat() * span
    }

    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(36.dp)
            .semantics {
                progressBarRangeInfo = ProgressBarRangeInfo(value, valueRange, steps)
                if (enabled) {
                    setProgress { target ->
                        currentOnChange(target.coerceIn(start, end))
                        currentOnFinished?.invoke()
                        true
                    }
                }
            }
            .pointerInput(enabled, start, end, steps) {
                if (!enabled) return@pointerInput
                val inset = 9.dp.toPx()
                detectTapGestures { offset ->
                    val usable = (size.width - 2f * inset).coerceAtLeast(1f)
                    currentOnChange(snap((offset.x - inset) / usable))
                    currentOnFinished?.invoke()
                }
            }
            .pointerInput(enabled, start, end, steps) {
                if (!enabled) return@pointerInput
                val inset = 9.dp.toPx()
                detectHorizontalDragGestures(
                    onDragEnd = { currentOnFinished?.invoke() },
                    onDragCancel = { currentOnFinished?.invoke() }
                ) { change, _ ->
                    change.consume()
                    val usable = (size.width - 2f * inset).coerceAtLeast(1f)
                    currentOnChange(snap((change.position.x - inset) / usable))
                }
            }
            .drawBehind {
                val inset = 9.dp.toPx()
                val cy = size.height / 2f
                val trackH = 4.dp.toPx()
                val x0 = inset
                val x1 = size.width - inset
                val tx = x0 + (x1 - x0) * fraction
                drawRect(
                    color = inactiveColor,
                    topLeft = Offset(x0, cy - trackH / 2f),
                    size = Size((x1 - x0).coerceAtLeast(0f), trackH)
                )
                drawRect(
                    color = activeColor,
                    topLeft = Offset(x0, cy - trackH / 2f),
                    size = Size((tx - x0).coerceAtLeast(0f), trackH)
                )
                val r = 9.dp.toPx()
                val diamond = Path().apply {
                    moveTo(tx, cy - r)
                    lineTo(tx + r, cy)
                    lineTo(tx, cy + r)
                    lineTo(tx - r, cy)
                    close()
                }
                drawPath(
                    path = diamond,
                    brush = Brush.verticalGradient(
                        colors = listOf(thumbTop, thumbBottom),
                        startY = cy - r,
                        endY = cy + r
                    )
                )
                drawPath(path = diamond, color = thumbEdge, style = Stroke(width = 1.dp.toPx()))
            }
    )
}

@Composable
fun WseSegmented(
    options: List<String>,
    selectedIndex: Int,
    onSelected: (Int) -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true
) {
    val colors = LocalWseColors.current
    val borderColor = if (enabled) {
        colors.hairline
    } else {
        colors.outlineVariant.copy(alpha = 0.45f)
    }
    val containerColor = if (enabled) {
        colors.surfaceContainer
    } else {
        colors.surfaceContainer.copy(alpha = 0.45f)
    }

    Box(
        modifier = modifier
            .fillMaxWidth()
            .heightIn(min = 48.dp)
            .clip(WseShapes.medium)
            .background(containerColor)
            .border(width = 1.dp, color = borderColor, shape = WseShapes.medium)
            .padding(3.dp)
    ) {
        if (options.isNotEmpty()) {
            BoxWithConstraints(modifier = Modifier.fillMaxWidth()) {
                val safeIdx = selectedIndex.coerceIn(0, options.lastIndex)
                val segmentWidth = maxWidth / options.size
                val indicatorOffset by animateDpAsState(
                    targetValue = segmentWidth * safeIdx,
                    animationSpec = tween(durationMillis = 220, easing = FastOutSlowInEasing),
                    label = "wse-segmented-offset"
                )
                val indicatorBrush = if (enabled) {
                    Brush.verticalGradient(
                        colors = listOf(colors.primaryGradientTop, colors.primaryGradientBottom)
                    )
                } else {
                    SolidColor(colors.outlineVariant.copy(alpha = 0.45f))
                }

                Box(
                    modifier = Modifier
                        .offset { IntOffset(indicatorOffset.roundToPx(), 0) }
                        .width(segmentWidth)
                        .height(42.dp)
                        .clip(WseShapes.small)
                        .background(indicatorBrush)
                )

                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(42.dp)
                ) {
                    options.forEachIndexed { index, option ->
                        val isSelected = index == safeIdx
                        val labelColor by animateColorAsState(
                            targetValue = when {
                                !enabled && isSelected -> colors.onSurface.copy(alpha = 0.45f)
                                !enabled -> colors.onSurfaceVariant.copy(alpha = 0.45f)
                                isSelected -> colors.onPrimary
                                else -> colors.onSurfaceVariant
                            },
                            animationSpec = tween(durationMillis = 180),
                            label = "wse-segmented-label"
                        )
                        Box(
                            modifier = Modifier
                                .weight(1f)
                                .fillMaxHeight()
                                .clip(WseShapes.small)
                                .semantics {
                                    role = Role.RadioButton
                                    selected = isSelected
                                }
                                .clickable(
                                    enabled = enabled,
                                    onClick = { onSelected(index) }
                                ),
                            contentAlignment = Alignment.Center
                        ) {
                            Text(
                                text = option.uppercase(),
                                style = WseType.overline,
                                color = labelColor,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                                modifier = Modifier.padding(horizontal = 6.dp)
                            )
                        }
                    }
                }
            }
        }
    }
}

@Preview(name = "WSE Inputs", showBackground = true, widthDp = 380)
@Composable
private fun PreviewWseInputs() {
    Wse2Theme {
        val colors = LocalWseColors.current
        var checked by remember { mutableStateOf(true) }
        var sliderVal by remember { mutableFloatStateOf(85f) }
        var segIdx by remember { mutableIntStateOf(1) }

        Column(
            modifier = Modifier
                .fillMaxWidth()
                .wseBackdrop(colors.accent)
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                WseSwitch(checked = checked, onCheckedChange = { checked = it })
                WseSwitch(checked = false, onCheckedChange = {}, enabled = false)
            }
            WseSlider(
                value = sliderVal,
                onValueChange = { sliderVal = it },
                valueRange = 50f..100f,
                valueLabel = "${sliderVal.toInt()}%",
                steps = 9,
                label = "Render scale"
            )
            WseSegmented(
                options = listOf("Low", "Medium", "High", "Custom"),
                selectedIndex = segIdx,
                onSelected = { segIdx = it }
            )
            WseSegmented(
                options = listOf("4:3", "16:9", "Stretch"),
                selectedIndex = 0,
                onSelected = {},
                enabled = false
            )
        }
    }
}
