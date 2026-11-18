package org.wse2.android.launcher.ui

import android.provider.Settings
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.tween
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalInspectionMode
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay

@Composable
internal fun rememberWseAnimationsEnabled(): Boolean {
    if (LocalInspectionMode.current) {
        return false
    }
    val context = LocalContext.current
    return remember(context) {
        val scale = try {
            Settings.Global.getFloat(
                context.contentResolver,
                Settings.Global.ANIMATOR_DURATION_SCALE,
                1.0f
            )
        } catch (_: Throwable) {
            1.0f
        }
        scale > 0f
    }
}

@Composable
internal fun Modifier.wseStaggeredEntrance(
    key: Any,
    order: Int,
    revealedKeys: MutableSet<Any>,
    animationsEnabled: Boolean,
    slideDistance: Dp = 12.dp,
    staggerMs: Int = 40,
    durationMs: Int = 220
): Modifier {
    val alreadyRevealed = remember(key) { key in revealedKeys }
    if (!animationsEnabled || alreadyRevealed) {
        revealedKeys.add(key)
        return this
    }
    val progress = remember(key) { Animatable(0f) }
    LaunchedEffect(key) {
        if (revealedKeys.add(key)) {
            val delayMs = (order.coerceIn(0, 8) * staggerMs).toLong()
            if (delayMs > 0L) {
                delay(delayMs)
            }
            progress.animateTo(
                targetValue = 1f,
                animationSpec = tween(durationMillis = durationMs, easing = FastOutSlowInEasing)
            )
        } else {
            progress.snapTo(1f)
        }
    }
    return this.graphicsLayer {
        val t = progress.value
        alpha = t
        translationY = slideDistance.toPx() * (1f - t)
    }
}
