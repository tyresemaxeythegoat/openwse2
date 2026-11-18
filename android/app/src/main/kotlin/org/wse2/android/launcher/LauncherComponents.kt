package org.wse2.android.launcher

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MenuAnchorType
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import kotlin.math.roundToInt

@Composable
internal fun LauncherSectionHeader(
    title: String,
    modifier: Modifier = Modifier
) {
    Text(
        text = title,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = modifier.padding(horizontal = 4.dp)
    )
}

@Composable
internal fun LauncherGroupedCard(
    modifier: Modifier = Modifier,
    content: @Composable ColumnScope.() -> Unit
) {
    Surface(
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surfaceContainerLow,
        modifier = modifier.fillMaxWidth()
    ) {
        Column(
            modifier = Modifier.fillMaxWidth(),
            content = content
        )
    }
}

@Composable
internal fun LauncherSectionDivider() {
    HorizontalDivider(
        modifier = Modifier.padding(horizontal = 16.dp),
        color = MaterialTheme.colorScheme.outlineVariant.copy(alpha = 0.55f)
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
    ListItem(
        headlineContent = {
            Text(
                text = headline,
                color = if (enabled) {
                    MaterialTheme.colorScheme.onSurface
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
                }
            )
        },
        supportingContent = if (supporting != null) {
            {
                Text(
                    text = supporting,
                    color = MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        } else {
            null
        },
        trailingContent = {
            Switch(
                checked = checked,
                onCheckedChange = null,
                enabled = enabled
            )
        },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier
            .fillMaxWidth()
            .toggleable(
                value = checked,
                enabled = enabled,
                role = Role.Switch,
                onValueChange = onCheckedChange
            )
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun SegmentedListItem(
    headline: String,
    options: List<String>,
    selectedIndex: Int,
    enabled: Boolean,
    showSelectedIcon: Boolean,
    onSelected: (Int) -> Unit
) {
    val safeIdx = if (options.isNotEmpty()) selectedIndex.coerceIn(0, options.lastIndex) else -1
    ListItem(
        headlineContent = { Text(headline) },
        supportingContent = {
            SingleChoiceSegmentedButtonRow(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(top = 8.dp)
            ) {
                options.forEachIndexed { index, option ->
                    val isSelected = index == safeIdx
                    val iconSlot: @Composable () -> Unit = if (showSelectedIcon) {
                        { SegmentedButtonDefaults.Icon(isSelected) }
                    } else {
                        {}
                    }
                    SegmentedButton(
                        selected = isSelected,
                        onClick = { onSelected(index) },
                        shape = SegmentedButtonDefaults.itemShape(
                            index = index,
                            count = options.size
                        ),
                        enabled = enabled,
                        icon = iconSlot,
                        label = {
                            Text(
                                text = option,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis
                            )
                        }
                    )
                }
            }
        },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier.fillMaxWidth()
    )
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
    var expanded by remember { mutableStateOf(false) }
    val safeIdx = if (options.isNotEmpty()) selectedIndex.coerceIn(0, options.lastIndex) else -1
    val selectedText = if (safeIdx >= 0) options[safeIdx] else ""

    ListItem(
        headlineContent = { Text(headline) },
        supportingContent = {
            Column(modifier = Modifier.fillMaxWidth()) {
                Spacer(Modifier.height(6.dp))
                ExposedDropdownMenuBox(
                    expanded = expanded,
                    onExpandedChange = { next ->
                        if (enabled && options.isNotEmpty()) {
                            expanded = next
                        }
                    },
                    modifier = Modifier.fillMaxWidth()
                ) {
                    OutlinedTextField(
                        value = selectedText,
                        onValueChange = {},
                        readOnly = true,
                        enabled = enabled && options.isNotEmpty(),
                        singleLine = true,
                        trailingIcon = {
                            ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded)
                        },
                        colors = ExposedDropdownMenuDefaults.outlinedTextFieldColors(),
                        modifier = Modifier
                            .menuAnchor(
                                MenuAnchorType.PrimaryNotEditable,
                                enabled = enabled && options.isNotEmpty()
                            )
                            .fillMaxWidth()
                    )
                    ExposedDropdownMenu(
                        expanded = expanded,
                        onDismissRequest = { expanded = false }
                    ) {
                        options.forEachIndexed { index, option ->
                            DropdownMenuItem(
                                text = {
                                    Text(
                                        text = option,
                                        fontWeight = if (index == safeIdx) {
                                            FontWeight.Bold
                                        } else {
                                            FontWeight.Normal
                                        }
                                    )
                                },
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
                    Spacer(Modifier.height(6.dp))
                    Text(
                        text = supporting,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier.fillMaxWidth()
    )
}

@Composable
internal fun SliderListItem(
    headline: String,
    persistedPercent: Int,
    minPercent: Int,
    maxPercent: Int,
    stepPercent: Int,
    enabled: Boolean,
    onCommitPercent: (Int) -> Unit
) {
    var draftPercent by remember(persistedPercent) { mutableIntStateOf(persistedPercent) }
    val steps = ((maxPercent - minPercent) / stepPercent - 1).coerceAtLeast(0)

    ListItem(
        headlineContent = {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(headline)
                Text(
                    text = "$draftPercent%",
                    style = MaterialTheme.typography.bodyMedium,
                    fontWeight = FontWeight.Bold,
                    color = if (enabled) {
                        MaterialTheme.colorScheme.primary
                    } else {
                        MaterialTheme.colorScheme.onSurfaceVariant
                    }
                )
            }
        },
        supportingContent = {
            Slider(
                value = draftPercent.toFloat(),
                onValueChange = { raw ->
                    draftPercent = LauncherDefaults.snapPercent(
                        raw.roundToInt(),
                        minPercent,
                        maxPercent
                    )
                },
                onValueChangeFinished = {
                    onCommitPercent(draftPercent)
                },
                valueRange = minPercent.toFloat()..maxPercent.toFloat(),
                steps = steps,
                enabled = enabled,
                modifier = Modifier.fillMaxWidth()
            )
        },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier.fillMaxWidth()
    )
}
