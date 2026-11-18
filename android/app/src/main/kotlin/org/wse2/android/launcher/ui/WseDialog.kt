package org.wse2.android.launcher.ui

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.tooling.preview.Preview
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Dialog
import org.wse2.android.launcher.theme.LocalWseColors
import org.wse2.android.launcher.theme.Wse2Theme
import org.wse2.android.launcher.theme.WseShapes

data class WseDialogAction(
    val label: String,
    val onClick: () -> Unit
)

@OptIn(ExperimentalLayoutApi::class)
@Composable
fun WseDialog(
    title: String,
    text: String,
    confirm: WseDialogAction,
    dismiss: WseDialogAction? = null,
    onDismissRequest: () -> Unit = dismiss?.onClick ?: confirm.onClick,
    modifier: Modifier = Modifier
) {
    val colors = LocalWseColors.current
    Dialog(onDismissRequest = onDismissRequest) {
        Surface(
            shape = WseShapes.large,
            color = colors.surfaceContainerHigh,
            border = BorderStroke(1.dp, colors.hairline),
            modifier = modifier
                .fillMaxWidth()
                .heightIn(max = 560.dp)
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(24.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp)
            ) {
                Text(
                    text = title,
                    style = MaterialTheme.typography.headlineSmall,
                    color = colors.onSurface
                )
                HorizontalDivider(
                    thickness = 1.dp,
                    color = colors.hairline.copy(alpha = 0.45f)
                )
                Box(
                    modifier = Modifier
                        .weight(1f, fill = false)
                        .verticalScroll(rememberScrollState())
                ) {
                    Text(
                        text = text,
                        style = MaterialTheme.typography.bodyMedium,
                        color = colors.onSurfaceVariant
                    )
                }
                FlowRow(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(12.dp, androidx.compose.ui.Alignment.End),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    if (dismiss != null) {
                        WseTextButton(
                            text = dismiss.label,
                            onClick = dismiss.onClick
                        )
                    }
                    WsePrimaryButton(
                        text = confirm.label,
                        onClick = confirm.onClick
                    )
                }
            }
        }
    }
}

@Preview(name = "WSE Dialog", showBackground = true)
@Composable
private fun PreviewWseDialog() {
    Wse2Theme {
        WseDialog(
            title = "Missing files (Native)",
            text = "These Warband files were not found:\n  - CommonRes/\n  - Data/font_data.xml",
            confirm = WseDialogAction("Choose folder") {},
            dismiss = WseDialogAction("OK") {}
        )
    }
}
