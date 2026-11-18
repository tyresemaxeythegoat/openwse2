package org.wse2.android.launcher.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
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
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import org.wse2.android.launcher.theme.LocalWseColors
import org.wse2.android.launcher.theme.Wse2Theme
import org.wse2.android.launcher.theme.WseType
import org.wse2.android.launcher.theme.wseBackdrop

@Composable
fun WseSectionHeader(
    title: String,
    modifier: Modifier = Modifier
) {
    val colors = LocalWseColors.current
    Row(
        modifier = modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        Text(
            text = title.uppercase(),
            style = WseType.overline,
            color = colors.primary,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
        Box(
            modifier = Modifier
                .weight(1f)
                .height(1.dp)
                .background(
                    Brush.horizontalGradient(
                        colors = listOf(colors.hairline, Color.Transparent)
                    )
                )
        )
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
    val interactionSource = remember { MutableInteractionSource() }
    val isPressed by interactionSource.collectIsPressedAsState()

    val pressHighlight by animateColorAsState(
        targetValue = if (enabled && onClick != null && isPressed) {
            colors.surfaceContainerHigh
        } else {
            Color.Transparent
        },
        animationSpec = tween(durationMillis = 100),
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
                .heightIn(min = 64.dp)
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
                if (supporting != null) {
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
                color = colors.hairline.copy(alpha = 0.45f)
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
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            WseSectionHeader(title = "Graphics")
            WseListRow(
                title = "Tree URI",
                supporting = "content://com.android.externalstorage.documents/tree/primary:Warband",
                onClick = {}
            )
            WseListRow(
                title = "Disabled row",
                supporting = "Unavailable while scanning",
                enabled = false,
                showDivider = false,
                onClick = {}
            )
        }
    }
}
