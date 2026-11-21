package org.wse2.android.launcher.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.animateDpAsState
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.selection.selectableGroup
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.graphics.vector.path
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import org.wse2.android.launcher.theme.LocalWseColors
import org.wse2.android.launcher.theme.Wse2Theme
import org.wse2.android.launcher.theme.wseBackdrop

data class WseNavItem(
    val label: String,
    val icon: ImageVector,
    val enabled: Boolean = true
)

@Composable
fun WseNavigationBar(
    items: List<WseNavItem>,
    selectedIndex: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
    windowInsets: WindowInsets = WindowInsets(0.dp),
    enabled: Boolean = true
) {
    val colors = LocalWseColors.current
    val animationsEnabled = rememberWseAnimationsEnabled()
    val navLabelStyle = MaterialTheme.typography.labelSmall.copy(
        fontSize = 11.sp,
        lineHeight = 14.sp,
        letterSpacing = 0.2.sp,
        fontWeight = FontWeight.SemiBold
    )
    Column(
        modifier = modifier
            .fillMaxWidth()
            .background(colors.surface.copy(alpha = 0.96f))
            .graphicsLayer { alpha = if (enabled) 1f else 0.45f }
    ) {
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .height(1.dp)
                .background(colors.hairline)
        )
        BoxWithConstraints(
            modifier = Modifier
                .fillMaxWidth()
                .windowInsetsPadding(windowInsets)
                .height(68.dp)
        ) {
            if (items.isNotEmpty()) {
                val safeIdx = selectedIndex.coerceIn(0, items.lastIndex)
                val slotWidth = maxWidth / items.size
                val indicatorWidth = 32.dp.coerceAtMost(slotWidth)
                val indicatorX by animateDpAsState(
                    targetValue = slotWidth * safeIdx + (slotWidth - indicatorWidth) / 2f,
                    animationSpec = wseAnimationSpec(
                        animationsEnabled = animationsEnabled,
                        durationMillis = 220
                    ),
                    label = "wse-nav-bar-indicator"
                )

                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .fillMaxHeight()
                        .selectableGroup()
                ) {
                    items.forEachIndexed { index, item ->
                        val isSelected = index == safeIdx
                        val itemEnabled = enabled && item.enabled
                        val itemColor by animateColorAsState(
                            targetValue = when {
                                !itemEnabled -> colors.onSurfaceVariant.copy(alpha = 0.45f)
                                isSelected -> colors.primary
                                else -> colors.onSurfaceVariant
                            },
                            animationSpec = wseAnimationSpec(
                                animationsEnabled = animationsEnabled,
                                durationMillis = 180
                            ),
                            label = "wse-nav-bar-item-color"
                        )
                        Column(
                            modifier = Modifier
                                .weight(1f)
                                .fillMaxHeight()
                                .selectable(
                                    selected = isSelected,
                                    enabled = itemEnabled,
                                    role = Role.Tab,
                                    onClick = { onSelect(index) }
                                )
                                .padding(horizontal = 4.dp, vertical = 8.dp),
                            horizontalAlignment = Alignment.CenterHorizontally,
                            verticalArrangement = Arrangement.Center
                        ) {
                            Icon(
                                imageVector = item.icon,
                                contentDescription = item.label,
                                tint = itemColor,
                                modifier = Modifier.size(22.dp)
                            )
                            Spacer(Modifier.height(4.dp))
                            Text(
                                text = item.label,
                                style = navLabelStyle,
                                color = itemColor,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis
                            )
                        }
                    }
                }

                Box(
                    modifier = Modifier
                        .align(Alignment.TopStart)
                        .offset { IntOffset(indicatorX.roundToPx(), 0) }
                        .width(indicatorWidth)
                        .height(2.dp)
                        .background(colors.accent, CircleShape)
                )
            }
        }
    }
}

@Composable
fun WseNavigationRail(
    items: List<WseNavItem>,
    selectedIndex: Int,
    onSelect: (Int) -> Unit,
    modifier: Modifier = Modifier,
    windowInsets: WindowInsets = WindowInsets(0.dp),
    enabled: Boolean = true
) {
    val colors = LocalWseColors.current
    val animationsEnabled = rememberWseAnimationsEnabled()
    val navLabelStyle = MaterialTheme.typography.labelSmall.copy(
        fontSize = 10.sp,
        lineHeight = 13.sp,
        letterSpacing = 0.2.sp,
        fontWeight = FontWeight.SemiBold
    )
    Row(
        modifier = modifier
            .fillMaxHeight()
            .width(88.dp)
            .background(colors.surface.copy(alpha = 0.96f))
            .graphicsLayer { alpha = if (enabled) 1f else 0.45f }
    ) {
        Box(
            modifier = Modifier
                .weight(1f)
                .fillMaxHeight()
                .windowInsetsPadding(windowInsets),
            contentAlignment = Alignment.Center
        ) {
            if (items.isNotEmpty()) {
                val itemHeight = 68.dp
                val indicatorHeight = 30.dp
                val safeIdx = selectedIndex.coerceIn(0, items.lastIndex)
                val indicatorY by animateDpAsState(
                    targetValue = itemHeight * safeIdx + (itemHeight - indicatorHeight) / 2f,
                    animationSpec = wseAnimationSpec(
                        animationsEnabled = animationsEnabled,
                        durationMillis = 220
                    ),
                    label = "wse-nav-rail-indicator"
                )

                Box(modifier = Modifier.fillMaxWidth()) {
                    Column(
                        modifier = Modifier
                            .fillMaxWidth()
                            .selectableGroup()
                    ) {
                        items.forEachIndexed { index, item ->
                            val isSelected = index == safeIdx
                            val itemEnabled = enabled && item.enabled
                            val itemColor by animateColorAsState(
                                targetValue = when {
                                    !itemEnabled -> colors.onSurfaceVariant.copy(alpha = 0.45f)
                                    isSelected -> colors.primary
                                    else -> colors.onSurfaceVariant
                                },
                                animationSpec = wseAnimationSpec(
                                    animationsEnabled = animationsEnabled,
                                    durationMillis = 180
                                ),
                                label = "wse-nav-rail-item-color"
                            )
                            Column(
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .height(itemHeight)
                                    .selectable(
                                        selected = isSelected,
                                        enabled = itemEnabled,
                                        role = Role.Tab,
                                        onClick = { onSelect(index) }
                                    )
                                    .padding(horizontal = 6.dp),
                                horizontalAlignment = Alignment.CenterHorizontally,
                                verticalArrangement = Arrangement.Center
                            ) {
                                Icon(
                                    imageVector = item.icon,
                                    contentDescription = item.label,
                                    tint = itemColor,
                                    modifier = Modifier.size(22.dp)
                                )
                                Spacer(Modifier.height(4.dp))
                                Text(
                                    text = item.label,
                                    style = navLabelStyle,
                                    color = itemColor,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis
                                )
                            }
                        }
                    }

                    Box(
                        modifier = Modifier
                            .align(Alignment.TopStart)
                            .offset { IntOffset(0, indicatorY.roundToPx()) }
                            .padding(start = 4.dp)
                            .width(2.5.dp)
                            .height(indicatorHeight)
                            .background(colors.accent, CircleShape)
                    )
                }
            }
        }
        Box(
            modifier = Modifier
                .width(1.dp)
                .fillMaxHeight()
                .background(colors.hairline)
        )
    }
}

private val PreviewNavIcon: ImageVector by lazy {
    ImageVector.Builder(
        name = "PreviewNav",
        defaultWidth = 24.dp,
        defaultHeight = 24.dp,
        viewportWidth = 24f,
        viewportHeight = 24f
    ).apply {
        path(fill = SolidColor(Color.White)) {
            moveTo(4f, 4f)
            horizontalLineTo(20f)
            verticalLineTo(20f)
            horizontalLineTo(4f)
            close()
        }
    }.build()
}

@Preview(name = "WSE Navigation Bar", showBackground = true, widthDp = 390)
@Composable
private fun PreviewWseNavigationBar() {
    Wse2Theme {
        val colors = LocalWseColors.current
        Box(modifier = Modifier.wseBackdrop(colors.accent)) {
            WseNavigationBar(
                items = listOf(
                    WseNavItem("Home", PreviewNavIcon),
                    WseNavItem("Modules", PreviewNavIcon),
                    WseNavItem("Settings", PreviewNavIcon),
                    WseNavItem("Advanced", PreviewNavIcon)
                ),
                selectedIndex = 1,
                onSelect = {}
            )
        }
    }
}

@Preview(name = "WSE Navigation Rail", showBackground = true, widthDp = 120, heightDp = 360)
@Composable
private fun PreviewWseNavigationRail() {
    Wse2Theme {
        val colors = LocalWseColors.current
        Box(modifier = Modifier.wseBackdrop(colors.accent)) {
            WseNavigationRail(
                items = listOf(
                    WseNavItem("Home", PreviewNavIcon),
                    WseNavItem("Modules", PreviewNavIcon),
                    WseNavItem("Settings", PreviewNavIcon),
                    WseNavItem("Advanced", PreviewNavIcon)
                ),
                selectedIndex = 0,
                onSelect = {}
            )
        }
    }
}
