package org.wse2.android.launcher.art

import android.graphics.Bitmap
import android.os.Build
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.BlurredEdgeTreatment
import androidx.compose.ui.draw.blur
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.wse2.android.launcher.theme.DefaultSeedColorArgb
import kotlin.math.abs

const val HERO_ART_MAX_SIDE = 1280
const val CARD_ART_MAX_SIDE = 480

data class ModuleArt(
    val bitmap: ImageBitmap?,
    val seedArgb: Int
)

fun decodeModuleArtIntArray(
    raw: IntArray?,
    fallbackSeedArgb: Int = DefaultSeedColorArgb
): ModuleArt {
    if (raw == null || raw.size < 3) {
        return ModuleArt(bitmap = null, seedArgb = fallbackSeedArgb)
    }
    val width = raw[0]
    val height = raw[1]
    val seedArgb = raw[2]
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return ModuleArt(bitmap = null, seedArgb = fallbackSeedArgb)
    }
    val pixelCountLong = width.toLong() * height.toLong()
    if (pixelCountLong <= 0L || pixelCountLong > (raw.size - 3).toLong()) {
        return ModuleArt(bitmap = null, seedArgb = fallbackSeedArgb)
    }
    val pixelCount = pixelCountLong.toInt()
    return try {
        val pixels = raw.copyOfRange(3, 3 + pixelCount)
        val bitmap = Bitmap.createBitmap(pixels, width, height, Bitmap.Config.ARGB_8888)
        ModuleArt(bitmap = bitmap.asImageBitmap(), seedArgb = seedArgb)
    } catch (_: Throwable) {
        ModuleArt(bitmap = null, seedArgb = fallbackSeedArgb)
    }
}

fun moduleFallbackColors(moduleName: String): List<Color> {
    val key = moduleName.ifBlank { "WSE2 Android" }
    var hash = 0x811c9dc5.toInt()
    for (ch in key) {
        hash = (hash xor ch.code) * 0x01000193
    }
    val baseHue = ((hash ushr 1) % 360).toFloat()
    val accentHue = (baseHue + 34f + ((hash ushr 10) % 28)) % 360f
    val sat1 = 0.42f + (((hash ushr 16) and 0x1F) / 31f) * 0.22f
    val sat2 = (sat1 * 0.82f).coerceIn(0.28f, 0.62f)
    return listOf(
        hslToColor(baseHue, sat1, 0.30f),
        hslToColor(accentHue, sat2, 0.19f),
        hslToColor((baseHue + 14f) % 360f, sat1 * 0.65f, 0.11f)
    )
}

fun moduleFallbackBrush(moduleName: String): Brush =
    Brush.linearGradient(colors = moduleFallbackColors(moduleName))

@Composable
fun ModuleArtGraphic(
    art: ModuleArt?,
    fallbackTitle: String,
    contentDescription: String?,
    modifier: Modifier = Modifier,
    blurRadius: Dp = 0.dp,
    imageModifier: Modifier = Modifier
) {
    val bitmap = art?.bitmap
    if (bitmap != null) {
        val blurModifier = if (blurRadius > 0.dp && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            Modifier.blur(radius = blurRadius, edgeTreatment = BlurredEdgeTreatment.Rectangle)
        } else {
            Modifier
        }
        Box(modifier = modifier.clipToBounds()) {
            Image(
                bitmap = bitmap,
                contentDescription = contentDescription,
                contentScale = ContentScale.Crop,
                modifier = Modifier
                    .fillMaxSize()
                    .then(imageModifier)
                    .then(blurModifier)
            )
        }
    } else {
        val brush = remember(fallbackTitle) { moduleFallbackBrush(fallbackTitle) }
        Box(modifier = modifier.clipToBounds()) {
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .then(imageModifier)
                    .background(brush)
            )
        }
    }
}

private fun hslToColor(hue: Float, sat: Float, lightness: Float): Color {
    val h = ((hue % 360f) + 360f) % 360f
    val s = sat.coerceIn(0f, 1f)
    val l = lightness.coerceIn(0f, 1f)
    val c = (1f - abs(2f * l - 1f)) * s
    val x = c * (1f - abs(((h / 60f) % 2f) - 1f))
    val m = l - c * 0.5f
    val (r1, g1, b1) = when {
        h < 60f -> Triple(c, x, 0f)
        h < 120f -> Triple(x, c, 0f)
        h < 180f -> Triple(0f, c, x)
        h < 240f -> Triple(0f, x, c)
        h < 300f -> Triple(x, 0f, c)
        else -> Triple(c, 0f, x)
    }
    return Color(
        red = (r1 + m).coerceIn(0f, 1f),
        green = (g1 + m).coerceIn(0f, 1f),
        blue = (b1 + m).coerceIn(0f, 1f),
        alpha = 1f
    )
}
