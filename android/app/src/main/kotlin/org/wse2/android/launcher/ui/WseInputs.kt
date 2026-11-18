package org.wse2.android.launcher.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.layout.width
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
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
    Switch(
        checked = checked,
        onCheckedChange = onCheckedChange,
        enabled = enabled,
        modifier = modifier,
        colors = SwitchDefaults.colors(
            checkedThumbColor = colors.onPrimary,
            checkedTrackColor = colors.primary,
            checkedBorderColor = colors.primary,
            checkedIconColor = colors.onPrimary,
            uncheckedThumbColor = colors.onSurfaceVariant,
            uncheckedTrackColor = colors.surfaceContainerHigh,
            uncheckedBorderColor = colors.outline,
            uncheckedIconColor = colors.surfaceContainerHigh,
            disabledCheckedThumbColor = colors.onSurface.copy(alpha = 0.45f),
            disabledCheckedTrackColor = colors.primaryContainer.copy(alpha = 0.45f),
            disabledCheckedBorderColor = colors.outlineVariant.copy(alpha = 0.45f),
            disabledCheckedIconColor = colors.onSurface.copy(alpha = 0.45f),
            disabledUncheckedThumbColor = colors.onSurfaceVariant.copy(alpha = 0.45f),
            disabledUncheckedTrackColor = colors.surfaceContainer.copy(alpha = 0.45f),
            disabledUncheckedBorderColor = colors.outlineVariant.copy(alpha = 0.45f),
            disabledUncheckedIconColor = colors.surfaceContainer.copy(alpha = 0.45f)
        )
    )
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
        Slider(
            value = value,
            onValueChange = onValueChange,
            enabled = enabled,
            valueRange = valueRange,
            steps = steps,
            onValueChangeFinished = onValueChangeFinished,
            colors = SliderDefaults.colors(
                thumbColor = colors.primary,
                activeTrackColor = colors.accent,
                activeTickColor = colors.onPrimary.copy(alpha = 0.55f),
                inactiveTrackColor = colors.surfaceContainerHigh,
                inactiveTickColor = colors.outline.copy(alpha = 0.65f),
                disabledThumbColor = colors.onSurfaceVariant.copy(alpha = 0.45f),
                disabledActiveTrackColor = colors.outline.copy(alpha = 0.45f),
                disabledActiveTickColor = colors.onSurfaceVariant.copy(alpha = 0.25f),
                disabledInactiveTrackColor = colors.surfaceContainer.copy(alpha = 0.45f),
                disabledInactiveTickColor = colors.outlineVariant.copy(alpha = 0.25f)
            ),
            modifier = Modifier.fillMaxWidth()
        )
    }
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
