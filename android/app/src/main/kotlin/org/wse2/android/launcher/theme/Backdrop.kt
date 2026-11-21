package org.wse2.android.launcher.theme

import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithCache
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color

@Suppress("UNUSED_PARAMETER")
@Composable
fun Modifier.wseBackdrop(accent: Color): Modifier {
    val colors = LocalWseColors.current
    val topColor = colors.surface
    val bottomColor = colors.background

    return this.drawWithCache {
        val verticalBrush = Brush.verticalGradient(
            colors = listOf(topColor, bottomColor),
            startY = 0f,
            endY = size.height.coerceAtLeast(1f)
        )

        onDrawBehind {
            drawRect(brush = verticalBrush)
        }
    }
}
