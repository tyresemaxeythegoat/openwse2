package org.wse2.android.launcher.ui

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.drawWithCache
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.wse2.android.launcher.art.moduleFallbackBrush
import org.wse2.android.launcher.theme.CinzelFontFamily
import org.wse2.android.launcher.theme.LocalWseColors
import org.wse2.android.launcher.theme.ManropeFontFamily
import org.wse2.android.launcher.theme.Wse2Theme
import org.wse2.android.launcher.theme.WseShapes
import org.wse2.android.launcher.theme.WseType
import org.wse2.android.launcher.theme.wseBackdrop

private val BannerTitleStyle = TextStyle(
    fontFamily = CinzelFontFamily,
    fontWeight = FontWeight.SemiBold,
    fontSize = 16.sp,
    lineHeight = 21.sp,
    letterSpacing = 0.5.sp
)

private val BannerSubtitleStyle = TextStyle(
    fontFamily = ManropeFontFamily,
    fontWeight = FontWeight.Medium,
    fontSize = 12.sp,
    lineHeight = 16.sp,
    letterSpacing = 0.2.sp
)

@Composable
fun WseBannerCard(
    art: ImageBitmap?,
    fallbackSeed: String,
    title: String,
    subtitle: String,
    chip: @Composable () -> Unit,
    selected: Boolean,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    artModifier: Modifier = Modifier
) {
    val colors = LocalWseColors.current
    val animationsEnabled = rememberWseAnimationsEnabled()
    val interactionSource = remember { MutableInteractionSource() }
    val isPressed by interactionSource.collectIsPressedAsState()

    val scale by animateFloatAsState(
        targetValue = if (enabled && animationsEnabled && isPressed) 0.98f else 1.0f,
        animationSpec = wseAnimationSpec(animationsEnabled = animationsEnabled, durationMillis = 120),
        label = "wse-banner-scale"
    )

    val glowColorState = rememberUpdatedState(colors.accent)
    val selectedState = rememberUpdatedState(selected && enabled)

    val borderStroke = BorderStroke(
        width = if (selected) 1.5.dp else 1.dp,
        color = when {
            !enabled -> colors.outlineVariant.copy(alpha = 0.45f)
            selected -> colors.accent
            else -> colors.hairline
        }
    )

    Box(
        modifier = modifier
            .fillMaxWidth()
            .graphicsLayer {
                scaleX = scale
                scaleY = scale
                alpha = if (enabled) 1f else 0.45f
            }
            .drawWithCache {
                val cutPx = 8.dp.toPx()
                // Twelve 1 dp rings with quadratically falling alpha: a soft glow, no visible steps.
                val ringCount = 12
                val glowLayers = (1..ringCount).map { i ->
                    val falloff = 1f - (i - 1).toFloat() / ringCount
                    Triple(i.dp.toPx(), 1.25.dp.toPx(), 0.17f * falloff * falloff)
                }
                val cachedPaths = glowLayers.map { (spread, strokeWidth, layerAlpha) ->
                    val path = Path().apply {
                        val l = -spread
                        val t = -spread
                        val r = size.width + spread
                        val b = size.height + spread
                        val c = (cutPx + spread * 0.55f).coerceAtMost(minOf(r - l, b - t) * 0.5f)
                        moveTo(l + c, t)
                        lineTo(r - c, t)
                        lineTo(r, t + c)
                        lineTo(r, b - c)
                        lineTo(r - c, b)
                        lineTo(l + c, b)
                        lineTo(l, b - c)
                        lineTo(l, t + c)
                        close()
                    }
                    Triple(path, Stroke(width = strokeWidth), layerAlpha)
                }
                onDrawBehind {
                    if (selectedState.value && size.width > 0f && size.height > 0f) {
                        val glowColor = glowColorState.value
                        for ((path, stroke, layerAlpha) in cachedPaths) {
                            drawPath(
                                path = path,
                                color = glowColor.copy(alpha = layerAlpha),
                                style = stroke
                            )
                        }
                    }
                }
            }
            .semantics { this.selected = selected }
    ) {
        Surface(
            shape = WseShapes.medium,
            color = if (selected) colors.surfaceContainer else colors.surfaceContainerLow,
            border = borderStroke,
            modifier = Modifier
                .fillMaxWidth()
                .clickable(
                    interactionSource = interactionSource,
                    indication = null,
                    enabled = enabled,
                    role = Role.Button,
                    onClick = onClick
                )
        ) {
            Column(modifier = Modifier.fillMaxWidth()) {
                val cardSurface = if (selected) colors.surfaceContainer else colors.surfaceContainerLow
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .aspectRatio(16f / 9f)
                ) {
                    if (art != null) {
                        Image(
                            bitmap = art,
                            contentDescription = title,
                            contentScale = ContentScale.Crop,
                            modifier = artModifier.fillMaxSize()
                        )
                    } else {
                        val fallbackBrush = remember(fallbackSeed) {
                            moduleFallbackBrush(fallbackSeed)
                        }
                        Box(
                            modifier = artModifier
                                .fillMaxSize()
                                .background(fallbackBrush)
                        )
                    }

                    Box(
                        modifier = Modifier
                            .fillMaxSize()
                            .background(
                                Brush.verticalGradient(
                                    0.0f to Color.Transparent,
                                    0.28f to Color.Transparent,
                                    0.72f to cardSurface.copy(alpha = 0.72f),
                                    1.0f to cardSurface
                                )
                            )
                    )

                    if (selected) {
                        Row(
                            modifier = Modifier
                                .align(Alignment.TopEnd)
                                .padding(8.dp)
                                .clip(WseShapes.small)
                                .background(colors.background.copy(alpha = 0.82f))
                                .border(1.dp, colors.accent.copy(alpha = 0.85f), WseShapes.small)
                                .padding(horizontal = 8.dp, vertical = 4.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            horizontalArrangement = Arrangement.spacedBy(5.dp)
                        ) {
                            Box(
                                modifier = Modifier
                                    .size(5.dp)
                                    .rotate(45f)
                                    .background(colors.accent)
                            )
                            Text(
                                text = "ACTIVE",
                                style = WseType.overline.copy(fontSize = 10.sp, lineHeight = 13.sp),
                                color = colors.primary,
                                maxLines = 1
                            )
                        }
                    }

                    Column(
                        modifier = Modifier
                            .align(Alignment.BottomStart)
                            .fillMaxWidth()
                            .padding(horizontal = 12.dp, vertical = 10.dp),
                        verticalArrangement = Arrangement.spacedBy(2.dp)
                    ) {
                        Text(
                            text = title,
                            style = BannerTitleStyle,
                            color = colors.onSurface,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                        Text(
                            text = subtitle,
                            style = BannerSubtitleStyle,
                            color = colors.onSurfaceVariant,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                    }
                }

                // Dedicated full-width footer row for the status chip so longer labels like
                // "3 FILES MISSING" never collide with or clip the active badge.
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 12.dp, vertical = 10.dp),
                    horizontalArrangement = Arrangement.Start,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    chip()
                }
            }
        }
    }
}

@Preview(name = "WSE Banner Cards", showBackground = true, widthDp = 380)
@Composable
private fun PreviewWseBannerCard() {
    Wse2Theme {
        val colors = LocalWseColors.current
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .wseBackdrop(colors.accent)
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp)
        ) {
            WseBannerCard(
                art = null,
                fallbackSeed = "Native",
                title = "Native",
                subtitle = "Native",
                chip = { WseStatusChip(text = "Ready", tone = WseStatusTone.Ready) },
                selected = true,
                onClick = {}
            )
            WseBannerCard(
                art = null,
                fallbackSeed = "Prophesy of Pendor",
                title = "Prophesy of Pendor",
                subtitle = "Prophesy_of_Pendor",
                chip = { WseStatusChip(text = "3 files missing", tone = WseStatusTone.Error, onClick = {}) },
                selected = true,
                onClick = {}
            )
        }
    }
}
