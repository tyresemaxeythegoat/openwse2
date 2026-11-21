package org.wse2.android.launcher.theme

import androidx.compose.foundation.shape.CutCornerShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Shapes
import androidx.compose.ui.unit.dp

object WseShapes {
    val small: RoundedCornerShape = RoundedCornerShape(6.dp)
    val medium: RoundedCornerShape = RoundedCornerShape(10.dp)
    val large: RoundedCornerShape = RoundedCornerShape(14.dp)

    // Asymmetric chamfer reserved for the signature primary Play CTA.
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
