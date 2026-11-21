package org.wse2.android.launcher.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.wse2.android.launcher.theme.LocalWseColors
import org.wse2.android.launcher.theme.Wse2Theme
import org.wse2.android.launcher.theme.wseBackdrop

enum class WseStatusTone {
    Ready,
    Warning,
    Error,
    Neutral
}

@Composable
fun WseStatusChip(
    text: String,
    tone: WseStatusTone,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    onClick: (() -> Unit)? = null
) {
    val colors = LocalWseColors.current
    val baseColor: Color
    val labelColor: Color
    when (tone) {
        WseStatusTone.Ready -> {
            baseColor = colors.ready
            labelColor = colors.readyText
        }
        WseStatusTone.Warning -> {
            baseColor = colors.warning
            labelColor = colors.warningText
        }
        WseStatusTone.Error -> {
            baseColor = colors.errorTone
            labelColor = colors.errorToneText
        }
        WseStatusTone.Neutral -> {
            baseColor = colors.neutral
            labelColor = colors.neutralText
        }
    }

    val containerColor = baseColor.copy(alpha = 0.12f)
    val borderStroke = BorderStroke(1.dp, baseColor.copy(alpha = 0.28f))
    val visualModifier = modifier.graphicsLayer {
        alpha = if (enabled) 1f else 0.45f
    }

    val chipContent: @Composable () -> Unit = {
        Row(
            modifier = Modifier.padding(horizontal = 10.dp, vertical = 5.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(7.dp)
        ) {
            Box(
                modifier = Modifier
                    .size(6.dp)
                    .background(color = baseColor, shape = CircleShape)
            )
            Text(
                text = text,
                style = MaterialTheme.typography.labelMedium.copy(
                    letterSpacing = 0.2.sp,
                    fontWeight = FontWeight.SemiBold
                ),
                color = labelColor,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
    }

    if (onClick != null) {
        Surface(
            onClick = onClick,
            enabled = enabled,
            shape = CircleShape,
            color = containerColor,
            contentColor = labelColor,
            border = borderStroke,
            modifier = visualModifier,
            content = chipContent
        )
    } else {
        Surface(
            shape = CircleShape,
            color = containerColor,
            contentColor = labelColor,
            border = borderStroke,
            modifier = visualModifier,
            content = chipContent
        )
    }
}

@Composable
fun WseStatusDot(
    text: String,
    tone: WseStatusTone,
    modifier: Modifier = Modifier
) {
    val colors = LocalWseColors.current
    val baseColor: Color
    val labelColor: Color
    when (tone) {
        WseStatusTone.Ready -> {
            baseColor = colors.ready
            labelColor = colors.readyText
        }
        WseStatusTone.Warning -> {
            baseColor = colors.warning
            labelColor = colors.warningText
        }
        WseStatusTone.Error -> {
            baseColor = colors.errorTone
            labelColor = colors.errorToneText
        }
        WseStatusTone.Neutral -> {
            baseColor = colors.neutral
            labelColor = colors.neutralText
        }
    }
    Row(
        modifier = modifier,
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        Box(
            modifier = Modifier
                .size(6.dp)
                .background(color = baseColor, shape = CircleShape)
        )
        Text(
            text = text,
            style = MaterialTheme.typography.bodySmall.copy(fontWeight = FontWeight.SemiBold),
            color = labelColor,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis
        )
    }
}

@Preview(name = "WSE Status Chips", showBackground = true, widthDp = 320)
@Composable
private fun PreviewWseStatusChips() {
    Wse2Theme {
        val colors = LocalWseColors.current
        Column(
            modifier = Modifier
                .wseBackdrop(colors.accent)
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            WseStatusDot(text = "Ready", tone = WseStatusTone.Ready)
            WseStatusChip(text = "Checking…", tone = WseStatusTone.Neutral)
            WseStatusChip(text = "Thermal limit", tone = WseStatusTone.Warning)
            WseStatusChip(text = "3 files missing", tone = WseStatusTone.Error, onClick = {})
            WseStatusChip(text = "Disabled", tone = WseStatusTone.Neutral, enabled = false)
        }
    }
}
