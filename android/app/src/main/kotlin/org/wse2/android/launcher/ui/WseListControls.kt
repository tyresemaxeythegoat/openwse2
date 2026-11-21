package org.wse2.android.launcher.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.wse2.android.launcher.theme.LocalWseColors
import org.wse2.android.launcher.theme.Wse2Theme
import org.wse2.android.launcher.theme.WseType
import org.wse2.android.launcher.theme.wseBackdrop

@Composable
fun WseScreenHeader(
    eyebrow: String = "",
    title: String,
    subtitle: String? = null,
    trailing: (@Composable () -> Unit)? = null,
    modifier: Modifier = Modifier
) {
    val colors = LocalWseColors.current
    Column(
        modifier = modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(4.dp)
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(2.dp)
            ) {
                if (eyebrow.isNotBlank()) {
                    Text(
                        text = eyebrow.uppercase(),
                        style = WseType.overline.copy(
                            fontSize = 11.sp,
                            lineHeight = 14.sp,
                            letterSpacing = 1.4.sp
                        ),
                        color = colors.primary,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                }
                Text(
                    text = title,
                    style = MaterialTheme.typography.headlineMedium,
                    color = colors.onSurface,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
            if (trailing != null) {
                Box(modifier = Modifier.padding(start = 12.dp)) {
                    trailing()
                }
            }
        }
        if (!subtitle.isNullOrBlank()) {
            Text(
                text = subtitle,
                style = MaterialTheme.typography.bodySmall,
                color = colors.onSurfaceVariant
            )
        }
    }
}

@Composable
fun WseSectionHeader(
    title: String,
    modifier: Modifier = Modifier,
    badge: String? = null
) {
    val colors = LocalWseColors.current
    Row(
        modifier = modifier
            .fillMaxWidth()
            .padding(top = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(
            text = title.uppercase(),
            style = MaterialTheme.typography.labelMedium.copy(
                letterSpacing = 1.1.sp,
                fontWeight = FontWeight.SemiBold
            ),
            color = colors.onSurfaceVariant,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
        if (!badge.isNullOrBlank()) {
            Text(
                text = badge,
                style = MaterialTheme.typography.bodySmall,
                color = colors.onSurfaceVariant,
                maxLines = 1
            )
        }
    }
}

data class WseMetricItem(
    val label: String,
    val value: String,
    val accentValue: Boolean = false
)

@Composable
fun WseKeyValueGrid(
    items: List<WseMetricItem>,
    modifier: Modifier = Modifier,
    columns: Int = 2
) {
    val colors = LocalWseColors.current
    val safeCols = columns.coerceAtLeast(1)
    val rows = items.chunked(safeCols)
    Column(
        modifier = modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        rows.forEachIndexed { rowIdx, rowItems ->
            if (rowIdx > 0) {
                HorizontalDivider(thickness = 1.dp, color = colors.hairline)
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(16.dp)
            ) {
                rowItems.forEach { item ->
                    Column(
                        modifier = Modifier.weight(1f),
                        verticalArrangement = Arrangement.spacedBy(3.dp)
                    ) {
                        Text(
                            text = item.label,
                            style = MaterialTheme.typography.bodySmall,
                            color = colors.onSurfaceVariant,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                        Text(
                            text = item.value,
                            style = MaterialTheme.typography.bodyMedium.copy(
                                fontWeight = FontWeight.SemiBold
                            ),
                            color = if (item.accentValue) colors.readyText else colors.onSurface,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                    }
                }
                repeat(safeCols - rowItems.size) {
                    Spacer(Modifier.weight(1f))
                }
            }
        }
    }
}

@Composable
fun WseListRow(
    title: String,
    supporting: String? = null,
    leading: (@Composable () -> Unit)? = null,
    trailing: (@Composable () -> Unit)? = null,
    onClick: (() -> Unit)? = null,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    showDivider: Boolean = true
) {
    val colors = LocalWseColors.current
    val animationsEnabled = rememberWseAnimationsEnabled()
    val interactionSource = remember { MutableInteractionSource() }
    val isPressed by interactionSource.collectIsPressedAsState()

    val pressHighlight by animateColorAsState(
        targetValue = if (enabled && onClick != null && isPressed) {
            colors.surfaceContainerHigh
        } else {
            Color.Transparent
        },
        animationSpec = wseAnimationSpec(animationsEnabled = animationsEnabled, durationMillis = 100),
        label = "wse-list-row-highlight"
    )

    val titleColor = if (enabled) {
        colors.onSurface
    } else {
        colors.onSurfaceVariant.copy(alpha = 0.45f)
    }
    val supportingColor = if (enabled) {
        colors.onSurfaceVariant
    } else {
        colors.onSurfaceVariant.copy(alpha = 0.45f)
    }

    val clickModifier = if (onClick != null) {
        Modifier.clickable(
            interactionSource = interactionSource,
            indication = null,
            enabled = enabled,
            role = Role.Button,
            onClick = onClick
        )
    } else {
        Modifier
    }

    Column(modifier = modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(pressHighlight)
                .then(clickModifier)
                .heightIn(min = 56.dp)
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            if (leading != null) {
                Box(contentAlignment = Alignment.Center) {
                    leading()
                }
            }
            Column(
                modifier = Modifier.weight(1f),
                verticalArrangement = Arrangement.spacedBy(2.dp)
            ) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.titleMedium,
                    color = titleColor
                )
                if (!supporting.isNullOrBlank()) {
                    Text(
                        text = supporting,
                        style = MaterialTheme.typography.bodySmall,
                        color = supportingColor
                    )
                }
            }
            if (trailing != null) {
                Box(contentAlignment = Alignment.Center) {
                    trailing()
                }
            }
        }
        if (showDivider) {
            HorizontalDivider(
                modifier = Modifier.padding(horizontal = 16.dp),
                thickness = 1.dp,
                color = colors.hairline
            )
        }
    }
}

@Preview(name = "WSE List Controls", showBackground = true, widthDp = 380)
@Composable
private fun PreviewWseListControls() {
    Wse2Theme {
        val colors = LocalWseColors.current
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .wseBackdrop(colors.accent)
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            WseScreenHeader(title = "Settings")
            WseSectionHeader(title = "Graphics")
            WseKeyValueGrid(
                items = listOf(
                    WseMetricItem("Path", "Modules/Native"),
                    WseMetricItem("Status", "Ready", accentValue = true)
                )
            )
            WseListRow(
                title = "Tree URI",
                supporting = "content://com.android.externalstorage.documents/tree/primary:Warband",
                onClick = {}
            )
        }
    }
}
