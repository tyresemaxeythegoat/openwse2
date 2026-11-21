package org.wse2.android.launcher.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectHorizontalDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.minimumInteractiveComponentSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
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
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.setProgress
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlin.math.roundToInt
import org.wse2.android.launcher.theme.LocalWseColors
import org.wse2.android.launcher.theme.Wse2Theme
import org.wse2.android.launcher.theme.WseShapes
import org.wse2.android.launcher.theme.wseBackdrop

@Composable
fun WseSwitch(
    checked: Boolean,
    onCheckedChange: ((Boolean) -> Unit)?,
    modifier: Modifier = Modifier,
    enabled: Boolean = true
) {
    val colors = LocalWseColors.current
    val animationsEnabled = rememberWseAnimationsEnabled()
    val thumbOffset by animateDpAsState(
        targetValue = if (checked) 23.dp else 3.dp,
        animationSpec = wseAnimationSpec(animationsEnabled = animationsEnabled, durationMillis = 150),
        label = "wse-switch-thumb"
    )
    val trackColor by animateColorAsState(
        targetValue = if (checked) colors.primary else colors.surfaceContainerHigh,
        animationSpec = wseAnimationSpec(animationsEnabled = animationsEnabled, durationMillis = 150),
        label = "wse-switch-track"
    )
    val borderColor by animateColorAsState(
        targetValue = if (checked) colors.primary else colors.outline,
        animationSpec = wseAnimationSpec(animationsEnabled = animationsEnabled, durationMillis = 150),
        label = "wse-switch-border"
    )
    val thumbColor by animateColorAsState(
        targetValue = if (checked) colors.onPrimary else colors.onSurfaceVariant,
        animationSpec = wseAnimationSpec(animationsEnabled = animationsEnabled, durationMillis = 150),
        label = "wse-switch-thumb-color"
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
                .clip(CircleShape)
                .background(trackColor)
                .border(1.dp, borderColor, CircleShape)
        ) {
            Box(
                modifier = Modifier
                    .align(Alignment.CenterStart)
                    .offset(x = thumbOffset)
                    .size(18.dp)
                    .clip(CircleShape)
                    .background(thumbColor)
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
    supporting: String? = null,
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
                style = MaterialTheme.typography.labelLarge,
                color = if (enabled) {
                    colors.primary
                } else {
                    colors.onSurfaceVariant.copy(alpha = 0.45f)
                }
            )
        }
        if (supporting != null) {
            Text(
                text = supporting,
                style = MaterialTheme.typography.bodySmall,
                color = if (enabled) {
                    colors.onSurfaceVariant
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

// A 3 dp rounded brass track with a clean 14 dp circular thumb. `steps` intermediate
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
    val inactiveColor = colors.surfaceContainerHighest
    val thumbFill = if (enabled) colors.primaryGradientTop else colors.outline
    val thumbHalo = colors.surfaceContainerLow

    fun snap(f: Float): Float {
        val clamped = f.coerceIn(0f, 1f)
        if (steps <= 0) return start + clamped * span
        val intervals = steps + 1
        return start + (clamped * intervals).roundToInt() / intervals.toFloat() * span
    }

    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(34.dp)
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
                val trackH = 3.dp.toPx()
                val x0 = inset
                val x1 = size.width - inset
                val tx = x0 + (x1 - x0) * fraction
                drawLine(
                    color = inactiveColor,
                    start = Offset(x0, cy),
                    end = Offset(x1, cy),
                    strokeWidth = trackH,
                    cap = StrokeCap.Round
                )
                drawLine(
                    color = activeColor,
                    start = Offset(x0, cy),
                    end = Offset(tx, cy),
                    strokeWidth = trackH,
                    cap = StrokeCap.Round
                )
                val r = 7.dp.toPx()
                drawCircle(
                    color = thumbFill,
                    radius = r,
                    center = Offset(tx, cy)
                )
                drawCircle(
                    color = thumbHalo,
                    radius = r + 1.dp.toPx(),
                    center = Offset(tx, cy),
                    style = Stroke(width = 2.dp.toPx())
                )
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
    val animationsEnabled = rememberWseAnimationsEnabled()
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
            .heightIn(min = 44.dp)
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
                    animationSpec = wseAnimationSpec(
                        animationsEnabled = animationsEnabled,
                        durationMillis = 200
                    ),
                    label = "wse-segmented-offset"
                )
                val indicatorBg = if (enabled) {
                    colors.primary.copy(alpha = 0.18f)
                } else {
                    colors.outlineVariant.copy(alpha = 0.45f)
                }
                val indicatorBorder = if (enabled) {
                    colors.primary.copy(alpha = 0.65f)
                } else {
                    colors.outlineVariant.copy(alpha = 0.45f)
                }

                Box(
                    modifier = Modifier
                        .offset { IntOffset(indicatorOffset.roundToPx(), 0) }
                        .width(segmentWidth)
                        .height(38.dp)
                        .clip(WseShapes.small)
                        .background(indicatorBg)
                        .border(1.dp, indicatorBorder, WseShapes.small)
                )

                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(38.dp)
                ) {
                    options.forEachIndexed { index, option ->
                        val isSelected = index == safeIdx
                        val labelColor by animateColorAsState(
                            targetValue = when {
                                !enabled && isSelected -> colors.onSurface.copy(alpha = 0.45f)
                                !enabled -> colors.onSurfaceVariant.copy(alpha = 0.45f)
                                isSelected -> colors.onPrimaryContainer
                                else -> colors.onSurfaceVariant
                            },
                            animationSpec = wseAnimationSpec(
                                animationsEnabled = animationsEnabled,
                                durationMillis = 160
                            ),
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
                                text = option,
                                style = MaterialTheme.typography.labelMedium.copy(
                                    letterSpacing = 0.2.sp,
                                    fontWeight = if (isSelected) FontWeight.Bold else FontWeight.SemiBold
                                ),
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

/**
 * Multi-column grid of restrained tonal selector tiles. Active tiles wear a subtle brass tint and
 * crisp accent border rather than a heavy solid gold fill so the Play CTA keeps visual priority.
 */
@Composable
fun WseTileGrid(
    options: List<String>,
    selectedIndex: Int,
    onSelected: (Int) -> Unit,
    modifier: Modifier = Modifier,
    columns: Int = 3,
    enabled: Boolean = true
) {
    val colors = LocalWseColors.current
    val animationsEnabled = rememberWseAnimationsEnabled()
    val safeCols = columns.coerceAtLeast(1)
    val safeIdx = if (options.isNotEmpty()) selectedIndex.coerceIn(0, options.lastIndex) else -1

    Column(
        modifier = modifier
            .fillMaxWidth()
            .selectableGroup(),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        options.indices.chunked(safeCols).forEach { rowIndices ->
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                rowIndices.forEach { index ->
                    val isSelected = index == safeIdx
                    val interactionSource = remember { MutableInteractionSource() }
                    val isPressed by interactionSource.collectIsPressedAsState()
                    val scale by animateFloatAsState(
                        targetValue = if (enabled && animationsEnabled && isPressed) 0.97f else 1f,
                        animationSpec = wseAnimationSpec(
                            animationsEnabled = animationsEnabled,
                            durationMillis = 110
                        ),
                        label = "wse-tile-scale"
                    )
                    val textColor by animateColorAsState(
                        targetValue = when {
                            !enabled && isSelected -> colors.onSurface.copy(alpha = 0.55f)
                            !enabled -> colors.onSurfaceVariant.copy(alpha = 0.45f)
                            isSelected -> colors.onPrimaryContainer
                            else -> colors.onSurfaceVariant
                        },
                        animationSpec = wseAnimationSpec(
                            animationsEnabled = animationsEnabled,
                            durationMillis = 150
                        ),
                        label = "wse-tile-text"
                    )
                    val tileBg = when {
                        isSelected && enabled -> colors.primary.copy(alpha = 0.16f)
                        isSelected -> colors.outlineVariant.copy(alpha = 0.45f)
                        isPressed && enabled -> colors.surfaceContainerHigh
                        else -> colors.surfaceContainer
                    }
                    val tileBorder = when {
                        !enabled -> colors.outlineVariant.copy(alpha = 0.4f)
                        isSelected -> colors.primary.copy(alpha = 0.72f)
                        else -> colors.hairline
                    }

                    Box(
                        modifier = Modifier
                            .weight(1f)
                            .heightIn(min = 40.dp)
                            .graphicsLayer {
                                scaleX = scale
                                scaleY = scale
                                alpha = if (enabled) 1f else 0.5f
                            }
                            .clip(WseShapes.small)
                            .background(tileBg)
                            .border(width = 1.dp, color = tileBorder, shape = WseShapes.small)
                            .selectable(
                                selected = isSelected,
                                enabled = enabled,
                                interactionSource = interactionSource,
                                indication = null,
                                role = Role.RadioButton,
                                onClick = { onSelected(index) }
                            )
                            .padding(horizontal = 8.dp, vertical = 9.dp),
                        contentAlignment = Alignment.Center
                    ) {
                        Text(
                            text = options[index],
                            style = MaterialTheme.typography.labelMedium.copy(
                                letterSpacing = 0.2.sp,
                                fontWeight = if (isSelected) FontWeight.Bold else FontWeight.SemiBold
                            ),
                            color = textColor,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                    }
                }
            }
        }
    }
}

data class WseCategoryTab(
    val label: String,
    val icon: ImageVector? = null
)

@Composable
fun WseCategoryStrip(
    tabs: List<WseCategoryTab>,
    selectedIndex: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
    contentPadding: PaddingValues = PaddingValues(0.dp)
) {
    val colors = LocalWseColors.current
    val animationsEnabled = rememberWseAnimationsEnabled()
    val safeIdx = if (tabs.isNotEmpty()) selectedIndex.coerceIn(0, tabs.lastIndex) else -1
    val listState = rememberLazyListState()

    LaunchedEffect(safeIdx) {
        if (safeIdx >= 0) {
            if (animationsEnabled) {
                listState.animateScrollToItem(safeIdx)
            } else {
                listState.scrollToItem(safeIdx)
            }
        }
    }

    Column(modifier = modifier.fillMaxWidth()) {
        LazyRow(
            state = listState,
            modifier = Modifier
                .fillMaxWidth()
                .selectableGroup(),
            contentPadding = contentPadding,
            horizontalArrangement = Arrangement.spacedBy(4.dp)
        ) {
            itemsIndexed(tabs) { index, tab ->
                val isSelected = index == safeIdx
                val interactionSource = remember { MutableInteractionSource() }
                val labelColor by animateColorAsState(
                    targetValue = if (isSelected) colors.onSurface else colors.onSurfaceVariant,
                    animationSpec = wseAnimationSpec(
                        animationsEnabled = animationsEnabled,
                        durationMillis = 150
                    ),
                    label = "wse-category-color"
                )
                val indicatorColor by animateColorAsState(
                    targetValue = if (isSelected) colors.primary else Color.Transparent,
                    animationSpec = wseAnimationSpec(
                        animationsEnabled = animationsEnabled,
                        durationMillis = 150
                    ),
                    label = "wse-category-indicator"
                )

                Column(
                    modifier = Modifier
                        .width(IntrinsicSize.Max)
                        .selectable(
                            selected = isSelected,
                            interactionSource = interactionSource,
                            indication = null,
                            role = Role.Tab,
                            onClick = { onSelect(index) }
                        ),
                    horizontalAlignment = Alignment.CenterHorizontally
                ) {
                    Row(
                        modifier = Modifier.padding(horizontal = 12.dp, vertical = 10.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(6.dp)
                    ) {
                        if (tab.icon != null) {
                            Icon(
                                imageVector = tab.icon,
                                contentDescription = null,
                                tint = if (isSelected) colors.primary else labelColor,
                                modifier = Modifier.size(15.dp)
                            )
                        }
                        Text(
                            text = tab.label,
                            style = MaterialTheme.typography.labelLarge.copy(
                                fontWeight = if (isSelected) FontWeight.Bold else FontWeight.SemiBold,
                                letterSpacing = 0.2.sp
                            ),
                            color = labelColor,
                            maxLines = 1
                        )
                    }
                    Box(
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(2.dp)
                            .background(indicatorColor)
                    )
                }
            }
        }
        HorizontalDivider(
            thickness = 1.dp,
            color = colors.hairline
        )
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
        var tileIdx by remember { mutableIntStateOf(1) }

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
                label = "Render scale",
                supporting = "Scales the internal 3D viewport resolution"
            )
            WseSegmented(
                options = listOf("Low", "Medium", "High", "Custom"),
                selectedIndex = segIdx,
                onSelected = { segIdx = it }
            )
            WseTileGrid(
                options = listOf("30", "60", "90", "120", "Uncapped"),
                selectedIndex = tileIdx,
                columns = 3,
                onSelected = { tileIdx = it }
            )
        }
    }
}
