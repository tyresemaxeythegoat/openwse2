package org.wse2.android.launcher.theme

import androidx.compose.material3.ColorScheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.ui.graphics.Color
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow

/** Default Warband warm amber/gold seed (matches legacy wse_accent #E59A3C). */
val DefaultSeedColorArgb: Int = 0xFFE59A3C.toInt()

/**
 * Derives a Material 3 [ColorScheme] from an ARGB [seed] via pure tonal HSL derivation.
 * Used on Android < 12 or when dynamic colour is disabled (e.g. module-art themed accents).
 */
fun schemeFromSeed(seed: Int, dark: Boolean): ColorScheme {
    val (rawH, rawS, _) = argbToHsl(seed)
    val isAchromatic = rawS < 0.08f
    val primarySat = if (isAchromatic) rawS.coerceAtMost(0.06f) else rawS.coerceIn(0.35f, 0.82f)
    val secondarySat = if (isAchromatic) {
        rawS.coerceAtMost(0.05f)
    } else {
        (primarySat * 0.36f).coerceIn(0.14f, 0.30f)
    }
    val tertiaryHue = (rawH + 60f) % 360f
    val tertiarySat = if (isAchromatic) {
        rawS.coerceAtMost(0.06f)
    } else {
        (primarySat * 0.45f).coerceIn(0.18f, 0.45f)
    }
    val neutralSat = if (isAchromatic) 0f else (primarySat * 0.08f).coerceAtMost(0.06f)
    val neutralVariantSat = if (isAchromatic) 0f else (primarySat * 0.14f).coerceIn(0.03f, 0.11f)

    return if (dark) {
        val primary = hslToColor(rawH, primarySat, 0.72f)
        val primaryContainer = hslToColor(rawH, (primarySat * 0.85f).coerceAtMost(0.75f), 0.27f)
        val secondary = hslToColor(rawH, secondarySat, 0.74f)
        val secondaryContainer = hslToColor(rawH, secondarySat, 0.26f)
        val tertiary = hslToColor(tertiaryHue, tertiarySat, 0.74f)
        val tertiaryContainer = hslToColor(tertiaryHue, tertiarySat, 0.26f)

        darkColorScheme(
            primary = primary,
            onPrimary = pickReadableOnColor(
                background = primary,
                darkCandidate = hslToColor(rawH, primarySat, 0.14f),
                lightCandidate = hslToColor(rawH, primarySat, 0.96f)
            ),
            primaryContainer = primaryContainer,
            onPrimaryContainer = pickReadableOnColor(
                background = primaryContainer,
                darkCandidate = hslToColor(rawH, primarySat, 0.12f),
                lightCandidate = hslToColor(rawH, primarySat, 0.91f)
            ),
            inversePrimary = hslToColor(rawH, primarySat, 0.38f),
            secondary = secondary,
            onSecondary = pickReadableOnColor(
                background = secondary,
                darkCandidate = hslToColor(rawH, secondarySat, 0.14f),
                lightCandidate = hslToColor(rawH, secondarySat, 0.96f)
            ),
            secondaryContainer = secondaryContainer,
            onSecondaryContainer = hslToColor(rawH, secondarySat, 0.90f),
            tertiary = tertiary,
            onTertiary = pickReadableOnColor(
                background = tertiary,
                darkCandidate = hslToColor(tertiaryHue, tertiarySat, 0.14f),
                lightCandidate = hslToColor(tertiaryHue, tertiarySat, 0.96f)
            ),
            tertiaryContainer = tertiaryContainer,
            onTertiaryContainer = hslToColor(tertiaryHue, tertiarySat, 0.90f),
            background = hslToColor(rawH, neutralSat, 0.07f),
            onBackground = hslToColor(rawH, neutralSat, 0.91f),
            surface = hslToColor(rawH, neutralSat, 0.07f),
            onSurface = hslToColor(rawH, neutralSat, 0.91f),
            surfaceVariant = hslToColor(rawH, neutralVariantSat, 0.22f),
            onSurfaceVariant = hslToColor(rawH, neutralVariantSat, 0.78f),
            surfaceTint = primary,
            inverseSurface = hslToColor(rawH, neutralSat, 0.90f),
            inverseOnSurface = hslToColor(rawH, neutralSat, 0.16f),
            error = Color(0xFFFFB4AB),
            onError = Color(0xFF690005),
            errorContainer = Color(0xFF93000A),
            onErrorContainer = Color(0xFFFFDAD6),
            outline = hslToColor(rawH, neutralVariantSat, 0.56f),
            outlineVariant = hslToColor(rawH, neutralVariantSat, 0.26f),
            scrim = Color(0xFF000000),
            surfaceContainerLowest = hslToColor(rawH, neutralSat, 0.05f),
            surfaceContainerLow = hslToColor(rawH, neutralSat, 0.10f),
            surfaceContainer = hslToColor(rawH, neutralSat, 0.12f),
            surfaceContainerHigh = hslToColor(rawH, neutralSat, 0.15f),
            surfaceContainerHighest = hslToColor(rawH, neutralSat, 0.19f)
        )
    } else {
        val primary = hslToColor(rawH, primarySat, 0.36f)
        val primaryContainer = hslToColor(rawH, (primarySat * 0.85f).coerceAtMost(0.80f), 0.88f)
        val secondary = hslToColor(rawH, secondarySat, 0.39f)
        val secondaryContainer = hslToColor(rawH, secondarySat, 0.88f)
        val tertiary = hslToColor(tertiaryHue, tertiarySat, 0.39f)
        val tertiaryContainer = hslToColor(tertiaryHue, tertiarySat, 0.88f)

        lightColorScheme(
            primary = primary,
            onPrimary = pickReadableOnColor(
                background = primary,
                darkCandidate = hslToColor(rawH, primarySat, 0.10f),
                lightCandidate = Color.White
            ),
            primaryContainer = primaryContainer,
            onPrimaryContainer = pickReadableOnColor(
                background = primaryContainer,
                darkCandidate = hslToColor(rawH, primarySat, 0.14f),
                lightCandidate = Color.White
            ),
            inversePrimary = hslToColor(rawH, primarySat, 0.74f),
            secondary = secondary,
            onSecondary = pickReadableOnColor(
                background = secondary,
                darkCandidate = hslToColor(rawH, secondarySat, 0.10f),
                lightCandidate = Color.White
            ),
            secondaryContainer = secondaryContainer,
            onSecondaryContainer = hslToColor(rawH, secondarySat, 0.16f),
            tertiary = tertiary,
            onTertiary = pickReadableOnColor(
                background = tertiary,
                darkCandidate = hslToColor(tertiaryHue, tertiarySat, 0.10f),
                lightCandidate = Color.White
            ),
            tertiaryContainer = tertiaryContainer,
            onTertiaryContainer = hslToColor(tertiaryHue, tertiarySat, 0.16f),
            background = hslToColor(rawH, neutralSat, 0.97f),
            onBackground = hslToColor(rawH, neutralSat, 0.11f),
            surface = hslToColor(rawH, neutralSat, 0.97f),
            onSurface = hslToColor(rawH, neutralSat, 0.11f),
            surfaceVariant = hslToColor(rawH, neutralVariantSat, 0.88f),
            onSurfaceVariant = hslToColor(rawH, neutralVariantSat, 0.29f),
            surfaceTint = primary,
            inverseSurface = hslToColor(rawH, neutralSat, 0.18f),
            inverseOnSurface = hslToColor(rawH, neutralSat, 0.95f),
            error = Color(0xFFBA1A1A),
            onError = Color(0xFFFFFFFF),
            errorContainer = Color(0xFFFFDAD6),
            onErrorContainer = Color(0xFF410002),
            outline = hslToColor(rawH, neutralVariantSat, 0.48f),
            outlineVariant = hslToColor(rawH, neutralVariantSat, 0.78f),
            scrim = Color(0xFF000000),
            surfaceContainerLowest = Color.White,
            surfaceContainerLow = hslToColor(rawH, neutralSat, 0.95f),
            surfaceContainer = hslToColor(rawH, neutralSat, 0.93f),
            surfaceContainerHigh = hslToColor(rawH, neutralSat, 0.90f),
            surfaceContainerHighest = hslToColor(rawH, neutralSat, 0.87f)
        )
    }
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

private fun pickReadableOnColor(
    background: Color,
    darkCandidate: Color,
    lightCandidate: Color
): Color {
    val bgLum = relativeLuminance(background)
    val darkContrast = contrastRatio(bgLum, relativeLuminance(darkCandidate))
    val lightContrast = contrastRatio(bgLum, relativeLuminance(lightCandidate))
    return if (darkContrast >= lightContrast) darkCandidate else lightCandidate
}

private fun contrastRatio(l1: Float, l2: Float): Float {
    val hi = max(l1, l2)
    val lo = min(l1, l2)
    return (hi + 0.05f) / (lo + 0.05f)
}

private fun relativeLuminance(c: Color): Float {
    fun linearize(channel: Float): Float =
        if (channel <= 0.04045f) channel / 12.92f else ((channel + 0.055f) / 1.055f).pow(2.4f)
    return 0.2126f * linearize(c.red) +
        0.7152f * linearize(c.green) +
        0.0722f * linearize(c.blue)
}
