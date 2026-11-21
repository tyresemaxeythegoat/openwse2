package org.wse2.android.launcher.theme

import androidx.compose.material3.ColorScheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.compositionLocalOf
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.lerp
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

/** Default gold seed ARGB used when no module art is available. */
val DefaultSeedColorArgb: Int = 0xFFC9A15A.toInt()

internal object WsePalette {
    val Background = Color(0xFF0D0D10)
    val Surface = Color(0xFF131317)
    val SurfaceContainerLowest = Color(0xFF09090B)
    val SurfaceContainerLow = Color(0xFF18181D)
    val SurfaceContainer = Color(0xFF1F1F25)
    val SurfaceContainerHigh = Color(0xFF282830)
    val SurfaceContainerHighest = Color(0xFF32323B)

    val OnSurface = Color(0xFFF0ECE4)
    val OnSurfaceVariant = Color(0xFF98938B)
    val Outline = Color(0xFF3E3D46)
    val OutlineVariant = Color(0xFF27272F)

    val Gold = Color(0xFFC9A25B)
    val OnGold = Color(0xFF171208)
    val GoldContainer = Color(0xFF2B2418)
    val OnGoldContainer = Color(0xFFF3E4C2)
    val GoldBright = Color(0xFFD9B56E)
    val GoldDeep = Color(0xFFAE8743)

    val Bronze = Color(0xFFA8743F)
    val OnBronze = Color(0xFF140E07)
    val BronzeContainer = Color(0xFF2A2118)
    val OnBronzeContainer = Color(0xFFEEDAC2)

    val Error = Color(0xFFD96852)
    val OnError = Color(0xFF1E0A06)
    val ErrorContainer = Color(0xFF381A15)
    val OnErrorContainer = Color(0xFFF7CFC6)

    // Neutral structural hairline so gold stays reserved for active states and primary CTAs.
    val Hairline = Color(0xFF2B2A33)

    val StatusReady = Color(0xFF78B074)
    val StatusReadyText = Color(0xFF98CC94)
    val StatusWarning = Color(0xFFD59F4A)
    val StatusWarningText = Color(0xFFE3B66B)
    val StatusErrorText = Color(0xFFEE9886)
}

@Immutable
data class WseColors(
    val background: Color = WsePalette.Background,
    val surface: Color = WsePalette.Surface,
    val surfaceContainerLow: Color = WsePalette.SurfaceContainerLow,
    val surfaceContainer: Color = WsePalette.SurfaceContainer,
    val surfaceContainerHigh: Color = WsePalette.SurfaceContainerHigh,
    val surfaceContainerHighest: Color = WsePalette.SurfaceContainerHighest,
    val onSurface: Color = WsePalette.OnSurface,
    val onSurfaceVariant: Color = WsePalette.OnSurfaceVariant,
    val outline: Color = WsePalette.Outline,
    val outlineVariant: Color = WsePalette.OutlineVariant,
    val primary: Color = WsePalette.Gold,
    val onPrimary: Color = WsePalette.OnGold,
    val primaryContainer: Color = WsePalette.GoldContainer,
    val onPrimaryContainer: Color = WsePalette.OnGoldContainer,
    val primaryGradientTop: Color = WsePalette.GoldBright,
    val primaryGradientBottom: Color = WsePalette.GoldDeep,
    val secondary: Color = WsePalette.Bronze,
    val onSecondary: Color = WsePalette.OnBronze,
    val error: Color = WsePalette.Error,
    val onError: Color = WsePalette.OnError,
    val errorContainer: Color = WsePalette.ErrorContainer,
    val onErrorContainer: Color = WsePalette.OnErrorContainer,
    val hairline: Color = WsePalette.Hairline,
    val accent: Color = WsePalette.Gold,
    val ready: Color = WsePalette.StatusReady,
    val readyText: Color = WsePalette.StatusReadyText,
    val warning: Color = WsePalette.StatusWarning,
    val warningText: Color = WsePalette.StatusWarningText,
    val errorTone: Color = WsePalette.Error,
    val errorToneText: Color = WsePalette.StatusErrorText,
    val neutral: Color = WsePalette.OnSurfaceVariant,
    val neutralText: Color = WsePalette.OnSurfaceVariant
) {
    companion object {
        val hairline: Color = WsePalette.Hairline
    }
}

val LocalWseColors = compositionLocalOf { WseColors() }

internal val WseDarkColorScheme: ColorScheme = darkColorScheme(
    primary = WsePalette.Gold,
    onPrimary = WsePalette.OnGold,
    primaryContainer = WsePalette.GoldContainer,
    onPrimaryContainer = WsePalette.OnGoldContainer,
    inversePrimary = Color(0xFF7D602B),
    secondary = WsePalette.Bronze,
    onSecondary = WsePalette.OnBronze,
    secondaryContainer = WsePalette.BronzeContainer,
    onSecondaryContainer = WsePalette.OnBronzeContainer,
    tertiary = Color(0xFF9E8E72),
    onTertiary = Color(0xFF17130D),
    tertiaryContainer = Color(0xFF332D22),
    onTertiaryContainer = Color(0xFFE6DEC9),
    background = WsePalette.Background,
    onBackground = WsePalette.OnSurface,
    surface = WsePalette.Surface,
    onSurface = WsePalette.OnSurface,
    surfaceVariant = WsePalette.SurfaceContainerHigh,
    onSurfaceVariant = WsePalette.OnSurfaceVariant,
    surfaceTint = WsePalette.Gold,
    inverseSurface = WsePalette.OnSurface,
    inverseOnSurface = WsePalette.Surface,
    error = WsePalette.Error,
    onError = WsePalette.OnError,
    errorContainer = WsePalette.ErrorContainer,
    onErrorContainer = WsePalette.OnErrorContainer,
    outline = WsePalette.Outline,
    outlineVariant = WsePalette.OutlineVariant,
    scrim = Color(0xFF000000),
    surfaceDim = WsePalette.Background,
    surfaceBright = WsePalette.SurfaceContainerHigh,
    surfaceContainerLowest = WsePalette.SurfaceContainerLowest,
    surfaceContainerLow = WsePalette.SurfaceContainerLow,
    surfaceContainer = WsePalette.SurfaceContainer,
    surfaceContainerHigh = WsePalette.SurfaceContainerHigh,
    surfaceContainerHighest = WsePalette.SurfaceContainerHighest
)

/**
 * Blends the module art [seedArgb] into WSE gold (35% seed, 65% gold), clamping the seed's
 * saturation to at most 0.6 and its lightness to [0.45, 0.70] before blending.
 */
fun accentFor(seedArgb: Int): Color {
    val (h, s, l) = argbToHsl(seedArgb)
    val clampedSeed = hslToColor(
        hue = h,
        sat = s.coerceAtMost(0.6f),
        lightness = l.coerceIn(0.45f, 0.70f)
    )
    return lerp(WsePalette.Gold, clampedSeed, 0.35f)
}

private fun argbToHsl(argb: Int): Triple<Float, Float, Float> {
    val r = ((argb ushr 16) and 0xFF) / 255f
    val g = ((argb ushr 8) and 0xFF) / 255f
    val b = (argb and 0xFF) / 255f
    val cMax = max(r, max(g, b))
    val cMin = min(r, min(g, b))
    val delta = cMax - cMin
    val l = (cMax + cMin) * 0.5f
    if (delta < 1e-5f) {
        return Triple(0f, 0f, l)
    }
    val denom = 1f - abs(2f * l - 1f)
    val s = if (denom > 1e-5f) (delta / denom).coerceIn(0f, 1f) else 0f
    var h = when (cMax) {
        r -> ((g - b) / delta) % 6f
        g -> ((b - r) / delta) + 2f
        else -> ((r - g) / delta) + 4f
    } * 60f
    if (h < 0f) {
        h += 360f
    }
    return Triple(h, s, l)
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
