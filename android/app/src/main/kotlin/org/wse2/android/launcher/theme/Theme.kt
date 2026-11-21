package org.wse2.android.launcher.theme

import android.app.Activity
import android.content.Context
import android.content.ContextWrapper
import androidx.compose.animation.animateColorAsState
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalView
import androidx.core.view.WindowCompat
import org.wse2.android.launcher.ui.rememberWseAnimationsEnabled
import org.wse2.android.launcher.ui.wseAnimationSpec

/**
 * Dark-only iron-and-gold theme for the WSE2 launcher.
 *
 * [darkTheme] and [dynamicColor] are kept for caller compatibility and are intentionally ignored;
 * [seedColorArgb] tints the accent via [accentFor] and animates changes over 600 ms when system
 * animations are enabled.
 */
@Suppress("UNUSED_PARAMETER")
@Composable
fun Wse2Theme(
    darkTheme: Boolean = true,
    dynamicColor: Boolean = true,
    seedColorArgb: Int = DefaultSeedColorArgb,
    content: @Composable () -> Unit
) {
    val animationsEnabled = rememberWseAnimationsEnabled()
    val targetAccent = accentFor(seedColorArgb)
    val animatedAccent by animateColorAsState(
        targetValue = targetAccent,
        animationSpec = wseAnimationSpec(
            animationsEnabled = animationsEnabled,
            durationMillis = 600
        ),
        label = "wse-accent"
    )
    val wseColors = remember(animatedAccent) {
        WseColors(accent = animatedAccent)
    }

    val context = LocalContext.current
    val view = LocalView.current
    if (!view.isInEditMode) {
        SideEffect {
            val activity = context.findActivity() ?: return@SideEffect
            val insetsController = WindowCompat.getInsetsController(activity.window, view)
            insetsController.isAppearanceLightStatusBars = false
            insetsController.isAppearanceLightNavigationBars = false
        }
    }

    CompositionLocalProvider(LocalWseColors provides wseColors) {
        MaterialTheme(
            colorScheme = WseDarkColorScheme,
            shapes = Wse2Shapes,
            typography = Wse2Typography,
            content = content
        )
    }
}

private tailrec fun Context.findActivity(): Activity? = when (this) {
    is Activity -> this
    is ContextWrapper -> baseContext.findActivity()
    else -> null
}
