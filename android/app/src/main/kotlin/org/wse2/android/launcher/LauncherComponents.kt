package org.wse2.android.launcher

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MenuAnchorType
import androidx.compose.material3.MenuDefaults
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
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
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.role
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.toggleableState
import androidx.compose.ui.state.ToggleableState
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import org.wse2.android.launcher.theme.LocalWseColors
import org.wse2.android.launcher.theme.WseShapes
import org.wse2.android.launcher.theme.WseType
import org.wse2.android.launcher.ui.WseListRow
import org.wse2.android.launcher.ui.WseSectionHeader
import org.wse2.android.launcher.ui.WseSegmented
import org.wse2.android.launcher.ui.WseSlider
import org.wse2.android.launcher.ui.WseSwitch
import org.wse2.android.launcher.ui.WseTileGrid
import kotlin.math.roundToInt

@Composable
internal fun LauncherSectionHeader(
    title: String,
    modifier: Modifier = Modifier,
    badge: String? = null
) {
    WseSectionHeader(
        title = title,
        badge = badge,
        modifier = modifier.padding(horizontal = 4.dp)
    )
}

@Composable
internal fun LauncherGroupedCard(
    modifier: Modifier = Modifier,
    content: @Composable ColumnScope.() -> Unit
) {
    val colors = LocalWseColors.current
    Surface(
        shape = WseShapes.large,
        color = colors.surfaceContainerLow,
        border = BorderStroke(1.dp, colors.hairline.copy(alpha = 0.72f)),
        modifier = modifier.fillMaxWidth()
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .background(
                    Brush.verticalGradient(
                        colors = listOf(
                            colors.surfaceContainer.copy(alpha = 0.52f),
                            colors.surfaceContainerLow
                        )
                    )
                ),
            content = content
        )
    }
}

@Composable
internal fun LauncherSectionDivider() {
    HorizontalDivider(
        modifier = Modifier.padding(horizontal = 16.dp),
        thickness = 1.dp,
        color = LocalWseColors.current.hairline.copy(alpha = 0.40f)
    )
}

@Composable
internal fun SwitchListItem(
    headline: String,
    checked: Boolean,
    enabled: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    supporting: String? = null
) {
    WseListRow(
        title = headline,
        supporting = supporting,
        trailing = {
            WseSwitch(
                checked = checked,
                onCheckedChange = null,
                enabled = enabled
            )
        },
        onClick = { onCheckedChange(!checked) },
        enabled = enabled,
        showDivider = false,
        modifier = Modifier.semantics(mergeDescendants = true) {
            role = Role.Switch
            toggleableState = ToggleableState(checked)
        }
    )
}

@Suppress("UNUSED_PARAMETER")
@Composable
internal fun SegmentedListItem(
    headline: String,
    options: List<String>,
    selectedIndex: Int,
    enabled: Boolean,
    showSelectedIcon: Boolean,
    onSelected: (Int) -> Unit,
    supporting: String? = null
) {
    val colors = LocalWseColors.current
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 14.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Text(
            text = headline,
            style = MaterialTheme.typography.titleMedium,
            color = if (enabled) {
                colors.onSurface
            } else {
                colors.onSurfaceVariant.copy(alpha = 0.45f)
            }
        )
        if (supporting != null) {
            Text(
                text = supporting,
                style = MaterialTheme.typography.bodySmall,
                color = colors.onSurfaceVariant
            )
        }
        WseSegmented(
            options = options,
            selectedIndex = selectedIndex,
            onSelected = onSelected,
            enabled = enabled
        )
    }
}

@Composable
internal fun TileGridListItem(
    headline: String,
    options: List<String>,
    selectedIndex: Int,
    enabled: Boolean,
    onSelected: (Int) -> Unit,
    columns: Int = 3,
    valueReadout: String? = null,
    supporting: String? = null
) {
    val colors = LocalWseColors.current
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 14.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = headline,
                style = MaterialTheme.typography.titleMedium,
                color = if (enabled) {
                    colors.onSurface
                } else {
                    colors.onSurfaceVariant.copy(alpha = 0.45f)
                },
                modifier = Modifier.weight(1f)
            )
            if (valueReadout != null) {
                Text(
                    text = valueReadout.uppercase(),
                    style = WseType.overline,
                    color = if (enabled) {
                        colors.primary
                    } else {
                        colors.onSurfaceVariant.copy(alpha = 0.45f)
                    }
                )
            }
        }
        if (supporting != null) {
            Text(
                text = supporting,
                style = MaterialTheme.typography.bodySmall,
                color = colors.onSurfaceVariant
            )
        }
        WseTileGrid(
            options = options,
            selectedIndex = selectedIndex,
            onSelected = onSelected,
            columns = columns,
            enabled = enabled
        )
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun ExposedDropdownListItem(
    headline: String,
    options: List<String>,
    selectedIndex: Int,
    enabled: Boolean,
    onSelected: (Int) -> Unit,
    supporting: String? = null
) {
    val colors = LocalWseColors.current
    var expanded by remember { mutableStateOf(false) }
    val safeIdx = if (options.isNotEmpty()) selectedIndex.coerceIn(0, options.lastIndex) else -1
    val selectedText = if (safeIdx >= 0) options[safeIdx] else ""
    val fieldEnabled = enabled && options.isNotEmpty()

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 14.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Text(
            text = headline,
            style = MaterialTheme.typography.titleMedium,
            color = if (enabled) {
                colors.onSurface
            } else {
                colors.onSurfaceVariant.copy(alpha = 0.45f)
            }
        )
        ExposedDropdownMenuBox(
            expanded = expanded,
            onExpandedChange = { next ->
                if (fieldEnabled) {
                    expanded = next
                }
            },
            modifier = Modifier.fillMaxWidth()
        ) {
            OutlinedTextField(
                value = selectedText,
                onValueChange = {},
                readOnly = true,
                enabled = fieldEnabled,
                singleLine = true,
                shape = WseShapes.medium,
                textStyle = MaterialTheme.typography.bodyMedium,
                trailingIcon = {
                    ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded)
                },
                colors = ExposedDropdownMenuDefaults.outlinedTextFieldColors(
                    focusedTextColor = colors.onSurface,
                    unfocusedTextColor = colors.onSurface,
                    disabledTextColor = colors.onSurfaceVariant.copy(alpha = 0.45f),
                    focusedContainerColor = colors.surfaceContainer,
                    unfocusedContainerColor = colors.surfaceContainer,
                    disabledContainerColor = colors.surfaceContainer.copy(alpha = 0.45f),
                    focusedBorderColor = colors.accent,
                    unfocusedBorderColor = colors.hairline,
                    disabledBorderColor = colors.outlineVariant.copy(alpha = 0.45f),
                    focusedTrailingIconColor = colors.primary,
                    unfocusedTrailingIconColor = colors.primary,
                    disabledTrailingIconColor = colors.onSurfaceVariant.copy(alpha = 0.45f)
                ),
                modifier = Modifier
                    .menuAnchor(
                        MenuAnchorType.PrimaryNotEditable,
                        enabled = fieldEnabled
                    )
                    .fillMaxWidth()
            )
            ExposedDropdownMenu(
                expanded = expanded,
                onDismissRequest = { expanded = false },
                shape = WseShapes.medium,
                containerColor = colors.surfaceContainerHigh,
                border = BorderStroke(1.dp, colors.hairline)
            ) {
                options.forEachIndexed { index, option ->
                    DropdownMenuItem(
                        text = {
                            Text(
                                text = option,
                                style = MaterialTheme.typography.bodyMedium,
                                fontWeight = if (index == safeIdx) {
                                    FontWeight.SemiBold
                                } else {
                                    FontWeight.Normal
                                }
                            )
                        },
                        colors = MenuDefaults.itemColors(
                            textColor = if (index == safeIdx) colors.primary else colors.onSurface
                        ),
                        onClick = {
                            expanded = false
                            onSelected(index)
                        },
                        contentPadding = ExposedDropdownMenuDefaults.ItemContentPadding
                    )
                }
            }
        }
        if (supporting != null) {
            Spacer(Modifier.height(2.dp))
            Text(
                text = supporting,
                style = MaterialTheme.typography.bodySmall,
                color = colors.onSurfaceVariant
            )
        }
    }
}

@Composable
internal fun SliderListItem(
    headline: String,
    persistedPercent: Int,
    minPercent: Int,
    maxPercent: Int,
    stepPercent: Int,
    enabled: Boolean,
    onCommitPercent: (Int) -> Unit,
    supporting: String? = null
) {
    var draftPercent by remember(persistedPercent) { mutableIntStateOf(persistedPercent) }
    val steps = ((maxPercent - minPercent) / stepPercent - 1).coerceAtLeast(0)

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 14.dp)
    ) {
        WseSlider(
            value = draftPercent.toFloat(),
            onValueChange = { raw ->
                val clamped = raw.roundToInt().coerceIn(minPercent, maxPercent)
                val stepCount = (clamped - minPercent + stepPercent / 2) / stepPercent
                draftPercent = (minPercent + stepCount * stepPercent).coerceIn(minPercent, maxPercent)
            },
            valueRange = minPercent.toFloat()..maxPercent.toFloat(),
            valueLabel = "$draftPercent%",
            steps = steps,
            enabled = enabled,
            label = headline,
            supporting = supporting,
            onValueChangeFinished = {
                onCommitPercent(draftPercent)
            }
        )
    }
}

@Composable
internal fun IntSliderListItem(
    headline: String,
    persistedValue: Int,
    minValue: Int,
    maxValue: Int,
    step: Int,
    enabled: Boolean,
    formatValue: (Int) -> String,
    onCommitValue: (Int) -> Unit,
    supporting: String? = null
) {
    var draft by remember(persistedValue) { mutableIntStateOf(persistedValue.coerceIn(minValue, maxValue)) }
    val safeStep = step.coerceAtLeast(1)
    val steps = ((maxValue - minValue) / safeStep - 1).coerceAtLeast(0)

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 14.dp)
    ) {
        WseSlider(
            value = draft.toFloat(),
            onValueChange = { raw ->
                val clamped = raw.roundToInt().coerceIn(minValue, maxValue)
                val idx = (clamped - minValue + safeStep / 2) / safeStep
                draft = (minValue + idx * safeStep).coerceIn(minValue, maxValue)
            },
            valueRange = minValue.toFloat()..maxValue.toFloat(),
            valueLabel = formatValue(draft),
            steps = steps,
            enabled = enabled,
            label = headline,
            supporting = supporting,
            onValueChangeFinished = {
                onCommitValue(draft)
            }
        )
    }
}

@Composable
internal fun FloatSliderListItem(
    headline: String,
    persistedValue: Float,
    minValue: Float,
    maxValue: Float,
    step: Float,
    enabled: Boolean,
    formatValue: (Float) -> String,
    onCommitValue: (Float) -> Unit,
    supporting: String? = null
) {
    var draft by remember(persistedValue) {
        mutableFloatStateOf(persistedValue.coerceIn(minValue, maxValue))
    }
    val safeStep = step.coerceAtLeast(0.001f)
    val steps = (((maxValue - minValue) / safeStep).roundToInt() - 1).coerceAtLeast(0)

    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = 16.dp, vertical = 14.dp)
    ) {
        WseSlider(
            value = draft,
            onValueChange = { raw ->
                val clamped = raw.coerceIn(minValue, maxValue)
                val idx = ((clamped - minValue) / safeStep).roundToInt()
                draft = (minValue + idx * safeStep).coerceIn(minValue, maxValue)
            },
            valueRange = minValue..maxValue,
            valueLabel = formatValue(draft),
            steps = steps,
            enabled = enabled,
            label = headline,
            supporting = supporting,
            onValueChangeFinished = {
                onCommitValue(draft)
            }
        )
    }
}
