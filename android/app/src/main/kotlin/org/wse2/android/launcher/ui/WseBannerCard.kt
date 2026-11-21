package org.wse2.android.launcher.ui

import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
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
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Icon
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.path
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
import org.wse2.android.launcher.theme.wseBackdrop

private val BannerTitleStyle = TextStyle(
    fontFamily = CinzelFontFamily,
    fontWeight = FontWeight.SemiBold,
    fontSize = 15.sp,
    lineHeight = 20.sp,
    letterSpacing = 0.4.sp
)

private val BannerSubtitleStyle = TextStyle(
    fontFamily = ManropeFontFamily,
    fontWeight = FontWeight.Medium,
    fontSize = 12.sp,
    lineHeight = 16.sp,
    letterSpacing = 0.1.sp
)

private val CheckmarkIcon: ImageVector by lazy {
    ImageVector.Builder(
        name = "SelectedCheck",
        defaultWidth = 14.dp,
        defaultHeight = 14.dp,
        viewportWidth = 24f,
        viewportHeight = 24f
    ).apply {
        path(
            stroke = SolidColor(Color.Black),
            strokeLineWidth = 3f,
            strokeLineCap = StrokeCap.Round,
            strokeLineJoin = StrokeJoin.Round
        ) {
            moveTo(5f, 12.5f)
            lineTo(9.5f, 17f)
            lineTo(19f, 7.5f)
        }
    }.build()
}

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

    val borderStroke = BorderStroke(
        width = if (selected) 1.5.dp else 1.dp,
        color = when {
            !enabled -> colors.outlineVariant.copy(alpha = 0.45f)
            selected -> colors.accent
            else -> colors.hairline
        }
    )

    val showSubtitle = subtitle.isNotBlank() && !subtitle.equals(title, ignoreCase = true)

    Surface(
        shape = WseShapes.medium,
        color = if (selected) colors.surfaceContainer else colors.surfaceContainerLow,
        border = borderStroke,
        modifier = modifier
            .fillMaxWidth()
            .graphicsLayer {
                scaleX = scale
                scaleY = scale
                alpha = if (enabled) 1f else 0.45f
            }
            .semantics { this.selected = selected }
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
                                0.35f to Color.Transparent,
                                0.78f to cardSurface.copy(alpha = 0.78f),
                                1.0f to cardSurface
                            )
                        )
                )

                if (selected) {
                    Box(
                        modifier = Modifier
                            .align(Alignment.TopEnd)
                            .padding(8.dp)
                            .size(22.dp)
                            .clip(CircleShape)
                            .background(colors.accent),
                        contentAlignment = Alignment.Center
                    ) {
                        Icon(
                            imageVector = CheckmarkIcon,
                            contentDescription = "Selected",
                            tint = colors.onPrimary,
                            modifier = Modifier.size(13.dp)
                        )
                    }
                }

                Column(
                    modifier = Modifier
                        .align(Alignment.BottomStart)
                        .fillMaxWidth()
                        .padding(horizontal = 12.dp, vertical = 8.dp),
                    verticalArrangement = Arrangement.spacedBy(1.dp)
                ) {
                    Text(
                        text = title,
                        style = BannerTitleStyle,
                        color = colors.onSurface,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis
                    )
                    if (showSubtitle) {
                        Text(
                            text = subtitle,
                            style = BannerSubtitleStyle,
                            color = colors.onSurfaceVariant,
                            maxLines = 1,
                            overflow = TextOverflow.Ellipsis
                        )
                    }
                }
            }

            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(horizontal = 12.dp, vertical = 9.dp),
                horizontalArrangement = Arrangement.Start,
                verticalAlignment = Alignment.CenterVertically
            ) {
                chip()
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
