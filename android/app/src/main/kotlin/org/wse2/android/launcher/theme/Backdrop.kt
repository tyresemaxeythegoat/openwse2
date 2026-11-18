package org.wse2.android.launcher.theme

import android.graphics.Bitmap
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithCache
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.ImageShader
import androidx.compose.ui.graphics.ShaderBrush
import androidx.compose.ui.graphics.TileMode
import androidx.compose.ui.graphics.asImageBitmap
import kotlin.math.hypot

@Composable
fun Modifier.wseBackdrop(accent: Color): Modifier {
    val colors = LocalWseColors.current
    val topColor = colors.surfaceContainerLow
    val bottomColor = colors.background
    val accentState = rememberUpdatedState(accent)
    val grainBrush = remember {
        ShaderBrush(
            ImageShader(
                image = buildFilmGrainBitmap(),
                tileModeX = TileMode.Repeated,
                tileModeY = TileMode.Repeated
            )
        )
    }

    return this.drawWithCache {
        val currentAccent = accentState.value
        val verticalBrush = Brush.verticalGradient(
            colors = listOf(topColor, bottomColor),
            startY = 0f,
            endY = size.height.coerceAtLeast(1f)
        )
        val glowCenter = Offset(size.width * 0.5f, size.height * 0.14f)
        val glowRadius = maxOf(size.width, size.height).coerceAtLeast(1f) * 0.78f
        val glowBrush = Brush.radialGradient(
            colors = listOf(
                currentAccent.copy(alpha = 0.14f),
                currentAccent.copy(alpha = 0.04f),
                Color.Transparent
            ),
            center = glowCenter,
            radius = glowRadius
        )
        val vignetteCenter = Offset(size.width * 0.5f, size.height * 0.5f)
        val vignetteRadius = hypot(size.width * 0.5f, size.height * 0.5f).coerceAtLeast(1f)
        val vignetteBrush = Brush.radialGradient(
            0.0f to Color.Transparent,
            0.52f to Color.Transparent,
            0.84f to Color.Black.copy(alpha = 0.25f),
            1.0f to Color.Black.copy(alpha = 0.55f),
            center = vignetteCenter,
            radius = vignetteRadius
        )

        onDrawBehind {
            drawRect(brush = verticalBrush)
            drawRect(brush = glowBrush)
            drawRect(brush = vignetteBrush)
            drawRect(brush = grainBrush, alpha = 0.05f)
        }
    }
}

private fun buildFilmGrainBitmap(): ImageBitmap {
    val size = 128
    val pixels = IntArray(size * size)
    var state = 0x57534532L
    for (i in pixels.indices) {
        state = state * 6364136223846793005L + 1442695040888963407L
        val luma = ((state ushr 56) and 0xFFL).toInt()
        pixels[i] = (0xFF shl 24) or (luma shl 16) or (luma shl 8) or luma
    }
    return Bitmap.createBitmap(pixels, size, size, Bitmap.Config.ARGB_8888).asImageBitmap()
}
