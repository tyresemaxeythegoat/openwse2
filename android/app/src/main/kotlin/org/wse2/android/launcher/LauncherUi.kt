package org.wse2.android.launcher

import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.os.Build
import android.text.TextUtils
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.WindowInsets
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.CheckBox
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Spinner
import android.widget.TextView
import org.wse2.android.R

enum class LauncherTab(val title: String, private val labelRes: Int) {
    HOME("Home", R.string.tab_home),
    MODULES("Modules", R.string.tab_modules),
    SETTINGS("Settings", R.string.tab_settings),
    ADVANCED("Advanced", R.string.tab_advanced);

    internal fun resolvedTitle(ctx: Context): String = ctx.getString(labelRes)
}

// Single owner of launcher look: colours (colors.xml), typography & spacing
// (dimens.xml), drawables, wide-screen width cap, and >= 48 dp touch targets.
// Pages route all view creation through here.
object LauncherUi {

    private const val SECTION_LETTER_SPACING = 0.08f

    class PageContainer internal constructor(
        val view: ScrollView,
        val content: LinearLayout
    ) {
        fun addView(child: View) {
            content.addView(child)
        }
    }

    class SpinnerRow internal constructor(
        val root: LinearLayout,
        val label: TextView,
        val spinner: Spinner
    )

    class SelectableRow internal constructor(
        val root: LinearLayout,
        val titleView: TextView,
        val subtitleView: TextView
    )

    class TabItem internal constructor(
        val root: LinearLayout,
        val indicator: View,
        val label: TextView
    )

    class TabBar internal constructor(
        val root: LinearLayout,
        private val items: List<TabItem>
    ) {
        fun setSelected(selectedIndex: Int) {
            val ctx = root.context
            val accent = ctx.getColor(R.color.wse_accent)
            val inactive = ctx.getColor(R.color.wse_text_secondary)
            items.forEachIndexed { index, item ->
                val active = index == selectedIndex
                item.root.isSelected = active
                item.indicator.setBackgroundColor(if (active) accent else Color.TRANSPARENT)
                item.label.setTextColor(if (active) accent else inactive)
                item.label.setTypeface(Typeface.DEFAULT, if (active) Typeface.BOLD else Typeface.NORMAL)
            }
        }
    }

    fun rootLayout(ctx: Context): LinearLayout {
        return LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(color(ctx, R.color.wse_bg_window))
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
            setOnApplyWindowInsetsListener { view, insets ->
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                    val bars = insets.getInsets(
                        WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout()
                    )
                    view.setPadding(bars.left, bars.top, bars.right, bars.bottom)
                    WindowInsets.CONSUMED
                } else {
                    val cutout = insets.displayCutout
                    @Suppress("DEPRECATION")
                    val left = maxOf(insets.systemWindowInsetLeft, cutout?.safeInsetLeft ?: 0)
                    @Suppress("DEPRECATION")
                    val top = maxOf(insets.systemWindowInsetTop, cutout?.safeInsetTop ?: 0)
                    @Suppress("DEPRECATION")
                    val right = maxOf(insets.systemWindowInsetRight, cutout?.safeInsetRight ?: 0)
                    @Suppress("DEPRECATION")
                    val bottom = maxOf(insets.systemWindowInsetBottom, cutout?.safeInsetBottom ?: 0)
                    view.setPadding(left, top, right, bottom)
                    @Suppress("DEPRECATION")
                    insets.consumeSystemWindowInsets()
                }
            }
        }
    }

    fun pageHost(ctx: Context): FrameLayout {
        return FrameLayout(ctx).apply {
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1f
            )
        }
    }

    fun page(ctx: Context): PageContainer {
        val pad = dimenPx(ctx, R.dimen.wse_page_padding)
        val padBottom = dimenPx(ctx, R.dimen.wse_page_padding_bottom)
        val content = constrainedLinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, padBottom)
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER_HORIZONTAL
            )
        }
        val scroll = ScrollView(ctx).apply {
            isFillViewport = true
            layoutParams = FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            )
            addView(content)
        }
        return PageContainer(scroll, content)
    }

    fun verticalGroup(ctx: Context): LinearLayout {
        return LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
    }

    fun card(ctx: Context): LinearLayout {
        val pad = dimenPx(ctx, R.dimen.wse_card_padding)
        return LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundResource(R.drawable.wse_card_bg)
            setPadding(pad, pad, pad, pad)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                bottomMargin = dimenPx(ctx, R.dimen.wse_card_spacing)
            }
        }
    }

    fun pageTitle(ctx: Context, text: String): TextView {
        return TextView(ctx).apply {
            this.text = text
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dimenF(ctx, R.dimen.wse_text_page_title))
            setTextColor(color(ctx, R.color.wse_text_primary))
            setTypeface(Typeface.DEFAULT, Typeface.BOLD)
            setPadding(0, 0, 0, dimenPx(ctx, R.dimen.wse_page_title_spacing))
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
    }

    fun sectionTitle(ctx: Context, text: String): TextView {
        return TextView(ctx).apply {
            this.text = text
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dimenF(ctx, R.dimen.wse_text_section_title))
            setTextColor(color(ctx, R.color.wse_text_secondary))
            setTypeface(Typeface.DEFAULT, Typeface.BOLD)
            isAllCaps = true
            letterSpacing = SECTION_LETTER_SPACING
            setPadding(0, 0, 0, dimenPx(ctx, R.dimen.wse_section_title_spacing))
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
    }

    fun bodyText(ctx: Context, text: String = ""): TextView {
        return TextView(ctx).apply {
            this.text = text
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dimenF(ctx, R.dimen.wse_text_body))
            setTextColor(color(ctx, R.color.wse_text_primary))
            setPadding(0, 0, 0, dimenPx(ctx, R.dimen.wse_text_spacing))
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
    }

    fun captionText(ctx: Context, text: String = ""): TextView {
        return TextView(ctx).apply {
            this.text = text
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dimenF(ctx, R.dimen.wse_text_caption))
            setTextColor(color(ctx, R.color.wse_text_secondary))
            setPadding(0, 0, 0, dimenPx(ctx, R.dimen.wse_text_spacing))
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
    }

    fun statusLineText(ctx: Context, text: String = ""): TextView {
        return TextView(ctx).apply {
            this.text = text
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dimenF(ctx, R.dimen.wse_text_caption))
            setTextColor(color(ctx, R.color.wse_text_secondary))
            maxLines = 2
            ellipsize = TextUtils.TruncateAt.END
            val padH = dimenPx(ctx, R.dimen.wse_status_padding_h)
            setPadding(
                padH,
                dimenPx(ctx, R.dimen.wse_status_padding_top),
                padH,
                dimenPx(ctx, R.dimen.wse_status_padding_bottom)
            )
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
    }

    fun logText(ctx: Context, text: String = ""): TextView {
        return TextView(ctx).apply {
            this.text = text
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dimenF(ctx, R.dimen.wse_text_log))
            typeface = Typeface.MONOSPACE
            setTextColor(color(ctx, R.color.wse_text_secondary))
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
    }

    fun primaryButton(ctx: Context, text: String, onClick: () -> Unit): Button {
        return buildButton(
            ctx = ctx,
            text = text,
            textSizeRes = R.dimen.wse_text_button_primary,
            bold = true,
            minHeightRes = R.dimen.wse_primary_button_min_height,
            backgroundRes = R.drawable.wse_button_primary_bg,
            textNormalRes = R.color.wse_on_accent,
            textDisabledRes = R.color.wse_on_accent_disabled,
            onClick = onClick
        )
    }

    fun secondaryButton(ctx: Context, text: String, onClick: () -> Unit): Button {
        return buildButton(
            ctx = ctx,
            text = text,
            textSizeRes = R.dimen.wse_text_button_secondary,
            bold = false,
            minHeightRes = R.dimen.wse_min_touch_target,
            backgroundRes = R.drawable.wse_button_secondary_bg,
            textNormalRes = R.color.wse_text_primary,
            textDisabledRes = R.color.wse_text_disabled,
            onClick = onClick
        )
    }

    fun buttonRow(ctx: Context, vararg buttons: Button): LinearLayout {
        val gap = dimenPx(ctx, R.dimen.wse_row_spacing)
        return LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                bottomMargin = gap
            }
            buttons.forEachIndexed { index, button ->
                button.layoutParams = LinearLayout.LayoutParams(
                    0,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    1f
                ).apply {
                    if (index > 0) {
                        marginStart = gap
                    }
                }
                addView(button)
            }
        }
    }

    fun checkRow(
        ctx: Context,
        text: String,
        checked: Boolean = false,
        onCheckedChange: ((Boolean) -> Unit)? = null
    ): CheckBox {
        val minH = dimenPx(ctx, R.dimen.wse_min_touch_target)
        val padH = dimenPx(ctx, R.dimen.wse_check_padding_h)
        val padV = dimenPx(ctx, R.dimen.wse_check_padding_v)
        val boxTint = ColorStateList(
            arrayOf(
                intArrayOf(-android.R.attr.state_enabled),
                intArrayOf(android.R.attr.state_checked),
                intArrayOf()
            ),
            intArrayOf(
                color(ctx, R.color.wse_text_disabled),
                color(ctx, R.color.wse_accent),
                color(ctx, R.color.wse_text_secondary)
            )
        )
        val textColors = ColorStateList(
            arrayOf(
                intArrayOf(-android.R.attr.state_enabled),
                intArrayOf()
            ),
            intArrayOf(
                color(ctx, R.color.wse_text_disabled),
                color(ctx, R.color.wse_text_primary)
            )
        )
        return CheckBox(ctx).apply {
            this.text = text
            isChecked = checked
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dimenF(ctx, R.dimen.wse_text_body))
            setTextColor(textColors)
            buttonTintList = boxTint
            setBackgroundResource(R.drawable.wse_check_row_bg)
            gravity = Gravity.CENTER_VERTICAL
            minimumHeight = minH
            minHeight = minH
            setPaddingRelative(padH, padV, padH, padV)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                bottomMargin = dimenPx(ctx, R.dimen.wse_check_margin_bottom)
            }
            if (onCheckedChange != null) {
                setOnCheckedChangeListener { _, isChecked -> onCheckedChange(isChecked) }
            }
        }
    }

    fun spinnerRow(
        ctx: Context,
        label: String,
        items: List<String>,
        onItemSelected: ((Int) -> Unit)? = null
    ): SpinnerRow {
        val root = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                bottomMargin = dimenPx(ctx, R.dimen.wse_row_spacing)
            }
        }
        val labelView = TextView(ctx).apply {
            text = label
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dimenF(ctx, R.dimen.wse_text_caption))
            setTextColor(color(ctx, R.color.wse_text_secondary))
            setPadding(0, 0, 0, dimenPx(ctx, R.dimen.wse_spinner_label_spacing))
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
        val spinner = Spinner(ctx).apply {
            minimumHeight = dimenPx(ctx, R.dimen.wse_min_touch_target)
            setBackgroundResource(R.drawable.wse_spinner_bg)
            setPopupBackgroundResource(R.drawable.wse_popup_bg)
            adapter = createSpinnerAdapter(ctx, items)
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
            if (onItemSelected != null) {
                onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                    override fun onItemSelected(
                        parent: AdapterView<*>?,
                        view: View?,
                        pos: Int,
                        id: Long
                    ) {
                        onItemSelected(pos)
                    }

                    override fun onNothingSelected(parent: AdapterView<*>?) {}
                }
            }
        }
        root.addView(labelView)
        root.addView(spinner)
        return SpinnerRow(root, labelView, spinner)
    }

    fun selectableRow(
        ctx: Context,
        title: String,
        subtitle: String,
        selected: Boolean,
        onClick: () -> Unit
    ): SelectableRow {
        val padH = dimenPx(ctx, R.dimen.wse_row_padding_h)
        val padV = dimenPx(ctx, R.dimen.wse_row_padding_v)
        val root = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_VERTICAL
            isClickable = true
            isFocusable = true
            minimumHeight = dimenPx(ctx, R.dimen.wse_min_touch_target)
            setBackgroundResource(R.drawable.wse_module_row_bg)
            setPaddingRelative(padH, padV, padH, padV)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                bottomMargin = dimenPx(ctx, R.dimen.wse_row_spacing)
            }
            setOnClickListener { onClick() }
        }
        val titleView = TextView(ctx).apply {
            text = title
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dimenF(ctx, R.dimen.wse_text_body))
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
        val subtitleView = TextView(ctx).apply {
            text = subtitle
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dimenF(ctx, R.dimen.wse_text_caption))
            setTextColor(color(ctx, R.color.wse_text_secondary))
            setPadding(0, dimenPx(ctx, R.dimen.wse_row_subtitle_spacing), 0, 0)
            layoutParams = ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
        root.addView(titleView)
        root.addView(subtitleView)
        val row = SelectableRow(root, titleView, subtitleView)
        updateSelectableRow(row, selected)
        return row
    }

    fun updateSelectableRow(row: SelectableRow, selected: Boolean) {
        val ctx = row.root.context
        row.root.isSelected = selected
        row.titleView.setTextColor(
            color(ctx, if (selected) R.color.wse_accent else R.color.wse_text_primary)
        )
        row.titleView.setTypeface(Typeface.DEFAULT, if (selected) Typeface.BOLD else Typeface.NORMAL)
    }

    fun tabBar(
        ctx: Context,
        tabs: List<LauncherTab>,
        selectedIndex: Int,
        onTabSelected: (Int) -> Unit
    ): TabBar {
        val container = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(color(ctx, R.color.wse_bg_tab_bar))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        }
        val divider = View(ctx).apply {
            setBackgroundColor(color(ctx, R.color.wse_border))
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                dimenPx(ctx, R.dimen.wse_divider_height).coerceAtLeast(1)
            )
        }
        val row = constrainedLinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                gravity = Gravity.CENTER_HORIZONTAL
            }
        }
        val minTabH = dimenPx(ctx, R.dimen.wse_tab_bar_min_height)
        val indicatorH = dimenPx(ctx, R.dimen.wse_tab_indicator_height)
        val padH = dimenPx(ctx, R.dimen.wse_tab_padding_h)
        val padTop = dimenPx(ctx, R.dimen.wse_tab_padding_top)
        val padBottom = dimenPx(ctx, R.dimen.wse_tab_padding_bottom)
        val items = ArrayList<TabItem>(tabs.size)
        tabs.forEachIndexed { index, tab ->
            val tabLabel = tab.resolvedTitle(ctx)
            val tabRoot = LinearLayout(ctx).apply {
                orientation = LinearLayout.VERTICAL
                gravity = Gravity.CENTER_HORIZONTAL
                isClickable = true
                isFocusable = true
                minimumHeight = minTabH
                contentDescription = tabLabel
                setBackgroundResource(R.drawable.wse_tab_item_bg)
                layoutParams = LinearLayout.LayoutParams(
                    0,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    1f
                )
                setOnClickListener { onTabSelected(index) }
            }
            val indicator = View(ctx).apply {
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    indicatorH
                )
            }
            val label = TextView(ctx).apply {
                text = tabLabel
                setTextSize(TypedValue.COMPLEX_UNIT_PX, dimenF(ctx, R.dimen.wse_text_tab))
                gravity = Gravity.CENTER
                maxLines = 1
                ellipsize = TextUtils.TruncateAt.END
                setPaddingRelative(padH, padTop, padH, padBottom)
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT,
                    1f
                )
            }
            tabRoot.addView(indicator)
            tabRoot.addView(label)
            row.addView(tabRoot)
            items.add(TabItem(tabRoot, indicator, label))
        }
        container.addView(divider)
        container.addView(row)
        return TabBar(container, items).apply {
            setSelected(selectedIndex)
        }
    }

    private fun buildButton(
        ctx: Context,
        text: String,
        textSizeRes: Int,
        bold: Boolean,
        minHeightRes: Int,
        backgroundRes: Int,
        textNormalRes: Int,
        textDisabledRes: Int,
        onClick: () -> Unit
    ): Button {
        val textColors = ColorStateList(
            arrayOf(
                intArrayOf(-android.R.attr.state_enabled),
                intArrayOf()
            ),
            intArrayOf(color(ctx, textDisabledRes), color(ctx, textNormalRes))
        )
        val minH = dimenPx(ctx, minHeightRes)
        val padH = dimenPx(ctx, R.dimen.wse_button_padding_h)
        val padV = dimenPx(ctx, R.dimen.wse_button_padding_v)
        return Button(ctx).apply {
            this.text = text
            setTextSize(TypedValue.COMPLEX_UNIT_PX, dimenF(ctx, textSizeRes))
            setTextColor(textColors)
            setTypeface(Typeface.DEFAULT, if (bold) Typeface.BOLD else Typeface.NORMAL)
            isAllCaps = false
            minimumHeight = minH
            minHeight = minH
            stateListAnimator = null
            setBackgroundResource(backgroundRes)
            setPaddingRelative(padH, padV, padH, padV)
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply {
                bottomMargin = dimenPx(ctx, R.dimen.wse_row_spacing)
            }
            setOnClickListener { onClick() }
        }
    }

    private fun createSpinnerAdapter(ctx: Context, items: List<String>): ArrayAdapter<String> {
        val copy = ArrayList(items)
        return object : ArrayAdapter<String>(
            ctx,
            android.R.layout.simple_spinner_item,
            copy
        ) {
            override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
                val view = super.getView(position, convertView, parent)
                styleSpinnerTextView(ctx, view, isDropdown = false)
                return view
            }

            override fun getDropDownView(
                position: Int,
                convertView: View?,
                parent: ViewGroup
            ): View {
                val view = super.getDropDownView(position, convertView, parent)
                styleSpinnerTextView(ctx, view, isDropdown = true)
                return view
            }
        }.apply {
            setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item)
        }
    }

    private fun styleSpinnerTextView(ctx: Context, view: View, isDropdown: Boolean) {
        val minH = dimenPx(ctx, R.dimen.wse_min_touch_target)
        view.minimumHeight = minH
        if (isDropdown) {
            view.setBackgroundResource(R.drawable.wse_dropdown_item_bg)
        }
        val tv: TextView? = view as? TextView ?: view.findViewById<TextView>(android.R.id.text1)
        if (tv != null) {
            tv.setTextSize(TypedValue.COMPLEX_UNIT_PX, dimenF(ctx, R.dimen.wse_text_body))
            tv.setTextColor(color(ctx, R.color.wse_text_primary))
            tv.gravity = Gravity.CENTER_VERTICAL
            tv.minHeight = minH
            tv.maxLines = 1
            tv.ellipsize = TextUtils.TruncateAt.END
            if (isDropdown) {
                val padH = dimenPx(ctx, R.dimen.wse_card_padding)
                val padV = dimenPx(ctx, R.dimen.wse_row_padding_v)
                tv.setPaddingRelative(padH, padV, padH, padV)
            } else {
                val padStart = dimenPx(ctx, R.dimen.wse_spinner_padding_h)
                val padEnd = dimenPx(ctx, R.dimen.wse_spinner_arrow_space)
                val padV = dimenPx(ctx, R.dimen.wse_spinner_padding_v)
                tv.setPaddingRelative(padStart, padV, padEnd, padV)
            }
        }
    }

    private fun constrainedLinearLayout(ctx: Context): LinearLayout {
        val wideBreakpointPx = dimenPx(ctx, R.dimen.wse_wide_screen_width)
        val maxWidthPx = dimenPx(ctx, R.dimen.wse_content_max_width)
        return object : LinearLayout(ctx) {
            override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
                val mode = MeasureSpec.getMode(widthMeasureSpec)
                val width = MeasureSpec.getSize(widthMeasureSpec)
                val constrainedSpec = if (
                    mode != MeasureSpec.UNSPECIFIED &&
                    width >= wideBreakpointPx &&
                    width > maxWidthPx
                ) {
                    MeasureSpec.makeMeasureSpec(maxWidthPx, mode)
                } else {
                    widthMeasureSpec
                }
                super.onMeasure(constrainedSpec, heightMeasureSpec)
            }
        }
    }

    private fun color(ctx: Context, resId: Int): Int = ctx.getColor(resId)

    private fun dimenPx(ctx: Context, resId: Int): Int =
        ctx.resources.getDimensionPixelSize(resId)

    private fun dimenF(ctx: Context, resId: Int): Float =
        ctx.resources.getDimension(resId)
}
