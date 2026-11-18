package org.wse2.android.launcher.theme

import androidx.compose.foundation.shape.CutCornerShape
import androidx.compose.material3.Shapes
import androidx.compose.ui.unit.dp

object WseShapes {
    val small: CutCornerShape = CutCornerShape(4.dp)
    val medium: CutCornerShape = CutCornerShape(8.dp)
    val large: CutCornerShape = CutCornerShape(14.dp)
    val button: CutCornerShape = CutCornerShape(
        topStart = 10.dp,
        topEnd = 0.dp,
        bottomEnd = 10.dp,
        bottomStart = 0.dp
    )
}

val Wse2Shapes = Shapes(
    extraSmall = WseShapes.small,
    small = WseShapes.small,
    medium = WseShapes.medium,
    large = WseShapes.large,
    extraLarge = WseShapes.large
)
