package org.wse2.android.launcher.ui

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.PressInteraction
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import org.wse2.android.launcher.theme.LocalWseColors
import org.wse2.android.launcher.theme.Wse2Theme
import org.wse2.android.launcher.theme.WseShapes
import org.wse2.android.launcher.theme.WseType
import org.wse2.android.launcher.theme.wseBackdrop

@Composable
fun WsePrimaryButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    leadingIcon: (@Composable () -> Unit)? = null
) {
    val colors = LocalWseColors.current
    val interactionSource = remember { MutableInteractionSource() }
    val isPressed by interactionSource.collectIsPressedAsState()

    val scale by animateFloatAsState(
        targetValue = if (enabled && isPressed) 0.97f else 1.0f,
        animationSpec = tween(durationMillis = 120),
        label = "wse-primary-button-scale"
    )

    val sheenProgress = remember { Animatable(-0.6f) }
    var sheenActive by remember { mutableStateOf(false) }

    LaunchedEffect(interactionSource, enabled) {
        var activeJob: Job? = null
        interactionSource.interactions.collect { interaction ->
            if (enabled && interaction is PressInteraction.Press) {
                activeJob?.cancel()
                activeJob = launch {
                    sheenActive = true
                    sheenProgress.snapTo(-0.6f)
                    sheenProgress.animateTo(
                        targetValue = 1.6f,
                        animationSpec = tween(durationMillis = 450, easing = FastOutSlowInEasing)
                    )
                    sheenActive = false
                }
            }
        }
    }

    val fillBrush = if (enabled) {
        Brush.verticalGradient(
            colors = listOf(colors.primaryGradientTop, colors.primaryGradientBottom)
        )
    } else {
        Brush.verticalGradient(
            colors = listOf(Color(0xFF7D7669), Color(0xFF645E53))
        )
    }
    val contentColor = if (enabled) colors.onPrimary else colors.onSurface

    Box(
        modifier = modifier
            .graphicsLayer {
                scaleX = scale
                scaleY = scale
                alpha = if (enabled) 1f else 0.45f
            }
            .heightIn(min = 56.dp)
            .clip(WseShapes.button)
            .background(brush = fillBrush)
            .clickable(
                interactionSource = interactionSource,
                indication = null,
                enabled = enabled,
                role = Role.Button,
                onClick = onClick
            )
            .drawWithContent {
                drawContent()
                if (size.width > 0f && size.height > 0f) {
                    val edgePx = 1.dp.toPx()
                    val halfEdge = edgePx * 0.5f
                    val cutPx = 10.dp.toPx().coerceAtMost(minOf(size.width, size.height) * 0.5f)
                    val highlightColor = Color.White.copy(alpha = if (enabled) 0.32f else 0.14f)
                    drawLine(
                        color = highlightColor,
                        start = Offset(0f, cutPx),
                        end = Offset(cutPx, 0f),
                        strokeWidth = edgePx
                    )
                    drawLine(
                        color = highlightColor,
                        start = Offset(cutPx, halfEdge),
                        end = Offset(size.width, halfEdge),
                        strokeWidth = edgePx
                    )
                    if (sheenActive && enabled) {
                        val w = size.width
                        val h = size.height
                        val center = sheenProgress.value * w
                        val bandHalf = (w * 0.22f).coerceAtLeast(32f)
                        drawRect(
                            brush = Brush.linearGradient(
                                colors = listOf(
                                    Color.Transparent,
                                    Color.White.copy(alpha = 0.18f),
                                    Color.Transparent
                                ),
                                start = Offset(center - bandHalf, 0f),
                                end = Offset(center + bandHalf, h)
                            )
                        )
                    }
                }
            }
            .padding(horizontal = 20.dp, vertical = 14.dp),
        contentAlignment = Alignment.Center
    ) {
        CompositionLocalProvider(LocalContentColor provides contentColor) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.Center
            ) {
                if (leadingIcon != null) {
                    leadingIcon()
                    Spacer(Modifier.width(8.dp))
                }
                Text(
                    text = text.uppercase(),
                    style = WseType.overline,
                    color = contentColor,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis
                )
            }
        }
    }
}

@Composable
fun WseSecondaryButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    leadingIcon: (@Composable () -> Unit)? = null
) {
    val colors = LocalWseColors.current
    val contentColor = if (enabled) {
        colors.primary
    } else {
        colors.onSurfaceVariant.copy(alpha = 0.45f)
    }
    val borderColor = if (enabled) {
        colors.hairline
    } else {
        colors.outlineVariant.copy(alpha = 0.45f)
    }

    Surface(
        onClick = onClick,
        enabled = enabled,
        shape = WseShapes.button,
        color = Color.Transparent,
        contentColor = contentColor,
        border = BorderStroke(1.dp, borderColor),
        modifier = modifier.heightIn(min = 48.dp)
    ) {
        Box(
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
            contentAlignment = Alignment.Center
        ) {
            CompositionLocalProvider(LocalContentColor provides contentColor) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.Center
                ) {
                    if (leadingIcon != null) {
                        leadingIcon()
                        Spacer(Modifier.width(8.dp))
                    }
                    Text(
                        text = text.uppercase(),
                        style = WseType.overline,
                        color = contentColor,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                }
            }
        }
    }
}

@Composable
fun WseTextButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true
) {
    val colors = LocalWseColors.current
    val contentColor = if (enabled) {
        colors.primary
    } else {
        colors.onSurfaceVariant.copy(alpha = 0.45f)
    }

    Surface(
        onClick = onClick,
        enabled = enabled,
        shape = WseShapes.button,
        color = Color.Transparent,
        contentColor = contentColor,
        border = null,
        modifier = modifier.heightIn(min = 48.dp)
    ) {
        Box(
            modifier = Modifier.padding(horizontal = 16.dp, vertical = 12.dp),
            contentAlignment = Alignment.Center
        ) {
            Text(
                text = text.uppercase(),
                style = WseType.overline,
                color = contentColor,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
    }
}

@Preview(name = "WSE Buttons", showBackground = true, widthDp = 360)
@Composable
private fun PreviewWseButtons() {
    Wse2Theme {
        val colors = LocalWseColors.current
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .wseBackdrop(colors.accent)
                .padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            WsePrimaryButton(
                text = "Play",
                onClick = {},
                modifier = Modifier.fillMaxWidth()
            )
            WsePrimaryButton(
                text = "Play (Disabled)",
                onClick = {},
                enabled = false,
                modifier = Modifier.fillMaxWidth()
            )
            WseSecondaryButton(
                text = "Choose folder",
                onClick = {},
                modifier = Modifier.fillMaxWidth()
            )
            WseSecondaryButton(
                text = "Rescan (Disabled)",
                onClick = {},
                enabled = false,
                modifier = Modifier.fillMaxWidth()
            )
            Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                WseTextButton(text = "View missing files", onClick = {})
                WseTextButton(text = "Disabled", onClick = {}, enabled = false)
            }
        }
    }
}
