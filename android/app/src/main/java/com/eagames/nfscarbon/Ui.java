package com.eagames.nfscarbon;

import android.app.AlertDialog;
import android.content.Context;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.text.TextUtils;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.CompoundButton;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

/** Palette and view builders shared by the launcher, settings and controls editor. */
public final class Ui {
    public static final int BG = 0xFF06090F;
    public static final int SURFACE = 0xE6101826;
    public static final int SURFACE_2 = 0xFF172234;
    public static final int STROKE = 0x1FFFFFFF;
    public static final int TEXT = 0xFFEAF0F8;
    public static final int MUTED = 0xFF8A96A8;
    public static final int FAINT = 0xFF56627A;
    public static final int ACCENT = 0xFF19C6E6;
    public static final int ACCENT_2 = 0xFF6A3DFF;
    public static final int OK = 0xFF2ED47A;
    public static final int WARN = 0xFFFFB020;
    public static final int DANGER = 0xFFFF6B6B;

    public static final Typeface CONDENSED = Typeface.create("sans-serif-condensed", Typeface.BOLD);
    public static final Typeface CONDENSED_ITALIC =
            Typeface.create("sans-serif-condensed", Typeface.BOLD_ITALIC);
    public static final Typeface MEDIUM = Typeface.create("sans-serif-medium", Typeface.NORMAL);

    /** Dialog theme matching the dark UI. */
    public static final int DIALOG_THEME = R.style.LauncherDialog;

    private Ui() {
    }

    public interface IntChoice {
        void onChoice(int index);
    }

    public interface FloatChoice {
        void onValue(float value);
    }

    public static int dp(Context c, float value) {
        return Math.round(value * c.getResources().getDisplayMetrics().density);
    }

    public static GradientDrawable rounded(Context c, int fill, int stroke, float radiusDp) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(fill);
        if (stroke != 0) {
            d.setStroke(dp(c, 1), stroke);
        }
        d.setCornerRadius(dp(c, radiusDp));
        return d;
    }

    public static GradientDrawable gradient(Context c, float radiusDp, int... colors) {
        GradientDrawable d = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT, colors);
        d.setCornerRadius(dp(c, radiusDp));
        return d;
    }

    public static Drawable ripple(Drawable content, int color) {
        return new RippleDrawable(ColorStateList.valueOf(color), content, null);
    }

    public static LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    public static LinearLayout.LayoutParams wrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    public static LinearLayout.LayoutParams weight(float w) {
        return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, w);
    }

    public static LinearLayout vertical(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.VERTICAL);
        return l;
    }

    public static LinearLayout horizontal(Context c) {
        LinearLayout l = new LinearLayout(c);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER_VERTICAL);
        return l;
    }

    public static TextView text(Context c, String s, float sp, int color) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        return t;
    }

    public static TextView title(Context c, String s, float sp) {
        TextView t = text(c, s, sp, TEXT);
        t.setTypeface(MEDIUM);
        return t;
    }

    /** Small all-caps label above a group. */
    public static TextView overline(Context c, String s) {
        TextView t = text(c, s.toUpperCase(), 11f, ACCENT);
        t.setTypeface(CONDENSED);
        t.setLetterSpacing(0.14f);
        return t;
    }

    public static ImageView icon(Context c, int glyph, int color, float sizeDp) {
        ImageView v = new ImageView(c);
        v.setImageDrawable(new Icons(glyph, color));
        v.setLayoutParams(new LinearLayout.LayoutParams(dp(c, sizeDp), dp(c, sizeDp)));
        return v;
    }

    /** A rounded group container. */
    public static LinearLayout card(Context c) {
        LinearLayout card = vertical(c);
        card.setBackground(rounded(c, SURFACE, STROKE, 16));
        card.setPadding(dp(c, 16), dp(c, 6), dp(c, 16), dp(c, 6));
        return card;
    }

    public static View divider(Context c) {
        View v = new View(c);
        v.setBackgroundColor(0x14FFFFFF);
        v.setLayoutParams(new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 1));
        return v;
    }

    /** Title + optional description, for the left part of a setting row. */
    public static LinearLayout labels(Context c, String title, String hint) {
        LinearLayout texts = vertical(c);
        texts.addView(title(c, title, 15f), matchWrap());
        if (hint != null && !hint.isEmpty()) {
            TextView h = text(c, hint, 12.5f, MUTED);
            h.setPadding(0, dp(c, 2), 0, 0);
            h.setLineSpacing(0, 1.1f);
            texts.addView(h, matchWrap());
        }
        return texts;
    }

    public static Switch styledSwitch(Context c) {
        Switch s = new Switch(c);
        int[][] states = {{android.R.attr.state_checked}, {}};
        s.setThumbTintList(new ColorStateList(states, new int[]{ACCENT, 0xFFB7C0CC}));
        s.setTrackTintList(new ColorStateList(states, new int[]{0x8019C6E6, 0x40FFFFFF}));
        return s;
    }

    /** Row with labels on the left and a switch on the right; the whole row toggles. */
    public static Switch switchRow(Context c, LinearLayout parent, String title, String hint,
                                   boolean checked, final CompoundButton.OnCheckedChangeListener l) {
        LinearLayout row = horizontal(c);
        row.setPadding(0, dp(c, 12), 0, dp(c, 12));
        row.addView(labels(c, title, hint), weight(1));
        final Switch toggle = styledSwitch(c);
        toggle.setChecked(checked);
        toggle.setOnCheckedChangeListener(l);
        LinearLayout.LayoutParams sp = wrap();
        sp.leftMargin = dp(c, 12);
        row.addView(toggle, sp);
        row.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                toggle.toggle();
            }
        });
        row.setBackground(ripple(null, 0x22FFFFFF));
        parent.addView(row, matchWrap());
        return toggle;
    }

    /**
     * Segmented control: equal-width chips, the selected one filled with the accent
     * gradient. Returns the container; call {@link #selectSegment} to change the selection.
     */
    public static LinearLayout segmented(final Context c, String[] labels, int selected,
                                         final IntChoice choice) {
        final LinearLayout group = horizontal(c);
        group.setBackground(rounded(c, 0xFF0B111B, STROKE, 12));
        group.setPadding(dp(c, 3), dp(c, 3), dp(c, 3), dp(c, 3));
        for (int i = 0; i < labels.length; i++) {
            final int index = i;
            TextView chip = text(c, labels[i], 13f, MUTED);
            chip.setGravity(Gravity.CENTER);
            chip.setSingleLine(true);
            chip.setEllipsize(TextUtils.TruncateAt.END);
            chip.setPadding(dp(c, 6), dp(c, 9), dp(c, 6), dp(c, 9));
            chip.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    selectSegment(c, group, index);
                    choice.onChoice(index);
                }
            });
            group.addView(chip, weight(1));
        }
        selectSegment(c, group, selected);
        return group;
    }

    public static void selectSegment(Context c, LinearLayout group, int selected) {
        for (int i = 0; i < group.getChildCount(); i++) {
            TextView chip = (TextView) group.getChildAt(i);
            boolean on = i == selected;
            chip.setTextColor(on ? Color.WHITE : MUTED);
            chip.setTypeface(on ? MEDIUM : Typeface.DEFAULT);
            chip.setBackground(on ? gradient(c, 9, 0xFF12B5DB, 0xFF5B44F2)
                    : ripple(null, 0x22FFFFFF));
        }
    }

    /** Labels above a full-width segmented control. */
    public static LinearLayout segmentedRow(Context c, LinearLayout parent, String title,
                                            String hint, String[] options, int selected,
                                            IntChoice choice) {
        LinearLayout block = vertical(c);
        block.setPadding(0, dp(c, 12), 0, dp(c, 12));
        block.addView(labels(c, title, hint), matchWrap());
        LinearLayout seg = segmented(c, options, selected, choice);
        LinearLayout.LayoutParams p = matchWrap();
        p.topMargin = dp(c, 10);
        block.addView(seg, p);
        parent.addView(block, matchWrap());
        return seg;
    }

    /** Labels with a value on the right; tapping opens a single-choice dialog. */
    public static TextView pickerRow(final Context c, LinearLayout parent, final String title,
                                     String hint, final String[] options, final int[] selected,
                                     final IntChoice choice) {
        LinearLayout row = horizontal(c);
        row.setPadding(0, dp(c, 12), 0, dp(c, 12));
        row.addView(labels(c, title, hint), weight(1));
        final TextView value = text(c, options[Math.max(0, selected[0])] + "  ›", 14f, ACCENT);
        value.setTypeface(MEDIUM);
        value.setMaxWidth(dp(c, 220));
        value.setGravity(Gravity.END);
        LinearLayout.LayoutParams vp = wrap();
        vp.leftMargin = dp(c, 12);
        row.addView(value, vp);
        row.setBackground(ripple(null, 0x22FFFFFF));
        row.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                new AlertDialog.Builder(c, DIALOG_THEME)
                        .setTitle(title)
                        .setSingleChoiceItems(options, selected[0],
                                new android.content.DialogInterface.OnClickListener() {
                                    @Override
                                    public void onClick(android.content.DialogInterface d,
                                                        int which) {
                                        selected[0] = which;
                                        value.setText(options[which] + "  ›");
                                        d.dismiss();
                                        choice.onChoice(which);
                                    }
                                })
                        .setNegativeButton(android.R.string.cancel, null)
                        .show();
            }
        });
        parent.addView(row, matchWrap());
        return value;
    }

    /** Labels with a live value, and a slider below. Values are shown by `format`. */
    public static SeekBar sliderRow(Context c, LinearLayout parent, String title, String hint,
                                    final float min, final float max, float value,
                                    final String format, final float displayFactor,
                                    final FloatChoice choice) {
        LinearLayout block = vertical(c);
        block.setPadding(0, dp(c, 12), 0, dp(c, 6));
        LinearLayout head = horizontal(c);
        head.addView(labels(c, title, hint), weight(1));
        final TextView valueView = text(c, "", 14f, ACCENT);
        valueView.setTypeface(MEDIUM);
        head.addView(valueView, wrap());
        block.addView(head, matchWrap());
        SeekBar bar = new SeekBar(c);
        bar.setMax(1000);
        bar.setProgressTintList(ColorStateList.valueOf(ACCENT));
        bar.setThumbTintList(ColorStateList.valueOf(Color.WHITE));
        bar.setProgressBackgroundTintList(ColorStateList.valueOf(0x40FFFFFF));
        final float range = max - min;
        bar.setProgress(Math.round((value - min) / range * 1000f));
        valueView.setText(String.format(java.util.Locale.US, format, value * displayFactor));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar s, int progress, boolean fromUser) {
                float v = min + range * progress / 1000f;
                valueView.setText(String.format(java.util.Locale.US, format, v * displayFactor));
                if (fromUser) {
                    choice.onValue(v);
                }
            }

            @Override
            public void onStartTrackingTouch(SeekBar s) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar s) {
            }
        });
        LinearLayout.LayoutParams bp = matchWrap();
        bp.topMargin = dp(c, 6);
        block.addView(bar, bp);
        parent.addView(block, matchWrap());
        return bar;
    }

    /** Filled accent button. */
    public static TextView primaryButton(Context c, String label, View.OnClickListener l) {
        TextView b = text(c, label, 15f, Color.WHITE);
        b.setTypeface(MEDIUM);
        b.setGravity(Gravity.CENTER);
        b.setPadding(dp(c, 18), dp(c, 13), dp(c, 18), dp(c, 13));
        b.setBackground(ripple(gradient(c, 12, 0xFF12B5DB, 0xFF5B44F2), 0x55FFFFFF));
        b.setOnClickListener(l);
        return b;
    }

    /** Outlined button. */
    public static TextView secondaryButton(Context c, String label, View.OnClickListener l) {
        TextView b = text(c, label, 14.5f, TEXT);
        b.setTypeface(MEDIUM);
        b.setGravity(Gravity.CENTER);
        b.setPadding(dp(c, 16), dp(c, 12), dp(c, 16), dp(c, 12));
        b.setBackground(ripple(rounded(c, SURFACE_2, 0x33FFFFFF, 12), 0x33FFFFFF));
        b.setOnClickListener(l);
        return b;
    }

    /** Small rounded label, e.g. "ACTIVE" or "RESTART". */
    public static TextView badge(Context c, String label, int color) {
        TextView b = text(c, label.toUpperCase(), 10f, color);
        b.setTypeface(CONDENSED);
        b.setLetterSpacing(0.08f);
        b.setPadding(dp(c, 8), dp(c, 2), dp(c, 8), dp(c, 2));
        b.setBackground(rounded(c, (color & 0x00FFFFFF) | 0x26000000, 0, 8));
        return b;
    }

    /** Disables a whole subtree visually and for input. */
    public static void setEnabledDeep(View v, boolean enabled) {
        v.setAlpha(enabled ? 1f : 0.4f);
        setEnabledTree(v, enabled);
    }

    private static void setEnabledTree(View v, boolean enabled) {
        // Disabled views still take touches but never perform clicks.
        v.setEnabled(enabled);
        if (v instanceof ViewGroup) {
            ViewGroup g = (ViewGroup) v;
            for (int i = 0; i < g.getChildCount(); i++) {
                setEnabledTree(g.getChildAt(i), enabled);
            }
        }
    }
}
