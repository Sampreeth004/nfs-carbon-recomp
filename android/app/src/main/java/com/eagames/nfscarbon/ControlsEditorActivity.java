package com.eagames.nfscarbon;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * Full-screen touch layout editor. The canvas is the real screen, so controls land
 * exactly where they will be in game. Drag to move, pinch to resize, tap to select;
 * the inspector next to the selection changes size and the mapped button.
 */
public class ControlsEditorActivity extends Activity {
    private static final String[] LAYOUT_VALUES = {TouchLayout.LAYOUT_DRIVING, TouchLayout.LAYOUT_XBOX};
    private static final String[] LAYOUT_NAMES = {"Driving", "Gamepad"};

    // Mappable actions: label, button bit, analog axis, label shown on driving controls.
    private static final String[] ACTION_NAMES = {
            "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Start", "Back", "L3", "R3", "Guide", "None"
    };
    private static final int[] ACTION_BITS = {
            4096, 8192, 16384, 32768, 256, 512, 0, 0, 16, 32, 64, 128, 1024, 0
    };
    private static final String[] ACTION_AXES = {
            "", "", "", "", "", "", "lt", "rt", "", "", "", "", "", ""
    };
    private static final String[] ACTION_GAME = {
            "E-BRAKE", "NOS", "BREAKER", "CREW", "RESET", "VIEW", "BRAKE", "GAS", "", "EVENT",
            "L3", "R3", "GUIDE", ""
    };
    private static final String[] ACTION_HINTS = {
            "Handbrake", "Nitrous", "Speedbreaker", "Crew", "Reset car", "Change view", "Brake",
            "Accelerate", "Pause", "Engage event", "Look back", "", "", ""
    };

    private TouchLayout layout;
    private TouchEditorView editor;
    private LinearLayout layoutPicker;
    private LinearLayout inspector;
    private LinearLayout overlayPanel;
    private TextView hint;
    private TextView inspectorTitle;
    private TextView inspectorSub;
    private TextView sizeValue;
    private SeekBar sizeBar;
    private LinearLayout actionSection;
    private final List<TextView> actionChips = new ArrayList<>();
    private ImageView undoButton;
    private ImageView gridButton;
    private final ArrayDeque<String> undo = new ArrayDeque<>();
    private boolean dirty;
    private boolean updatingInspector;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        layout = TouchLayout.load(this);

        // Toolbar strip on top; below it the canvas, fitted at the screen's own aspect
        // ratio so no control is ever hidden under the toolbar.
        LinearLayout root = Ui.vertical(this);
        root.setBackgroundColor(0xFF04060A);
        root.addView(buildToolbar(), Ui.matchWrap());

        FrameLayout stage = new FrameLayout(this);
        root.addView(stage, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0,
                1f));
        editor = new TouchEditorView(this);
        editor.setBackground(new SpeedBackground());
        stage.addView(editor, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT, Gravity.CENTER));

        inspector = buildInspector();
        inspector.setVisibility(View.GONE);
        stage.addView(inspector, new FrameLayout.LayoutParams(dp(300),
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER_VERTICAL | Gravity.END));

        overlayPanel = buildOverlayPanel();
        overlayPanel.setVisibility(View.GONE);
        FrameLayout.LayoutParams op = new FrameLayout.LayoutParams(dp(320),
                ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        op.topMargin = dp(10);
        stage.addView(overlayPanel, op);

        setContentView(root);

        String start = TouchLayout.LAYOUT_XBOX.equals(layout.active) ? TouchLayout.LAYOUT_XBOX
                : TouchLayout.LAYOUT_DRIVING;
        editor.setLayout(layout, start);
        Ui.selectSegment(this, layoutPicker, TouchLayout.LAYOUT_XBOX.equals(start) ? 1 : 0);
        editor.setListener(new TouchEditorView.Listener() {
            @Override
            public void onSelectionChanged(TouchLayout.Control control) {
                showInspector(control);
            }

            @Override
            public void onEditStarted() {
                pushUndo();
            }

            @Override
            public void onLayoutEdited() {
                dirty = true;
                refreshInspectorValues();
            }
        });
        updateUndo();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        save();
    }

    private void save() {
        if (dirty) {
            layout.save(this);
            dirty = false;
        }
    }

    // ------------------------------------------------------------------ Toolbar

    private LinearLayout buildToolbar() {
        LinearLayout bar = Ui.horizontal(this);
        bar.setBackgroundColor(0xFF0B111B);
        bar.setPadding(dp(10), dp(4), dp(10), dp(4));

        TextView done = Ui.text(this, "Done", 14f, Color.WHITE);
        done.setTypeface(Ui.MEDIUM);
        done.setGravity(Gravity.CENTER);
        done.setPadding(dp(16), dp(8), dp(16), dp(8));
        done.setBackground(Ui.ripple(Ui.gradient(this, 12, 0xFF12B5DB, 0xFF5B44F2), 0x55FFFFFF));
        done.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                save();
                finish();
            }
        });
        bar.addView(done, Ui.wrap());

        layoutPicker = Ui.segmented(this, LAYOUT_NAMES, 0, new Ui.IntChoice() {
            @Override
            public void onChoice(int index) {
                editor.setLayout(layout, LAYOUT_VALUES[index]);
                showInspector(null);
            }
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(200),
                ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.leftMargin = dp(6);
        lp.rightMargin = dp(6);
        bar.addView(layoutPicker, lp);

        hint = Ui.text(this, "Tap a control to select it · drag to move · pinch to resize",
                12.5f, Ui.MUTED);
        hint.setSingleLine(true);
        hint.setEllipsize(android.text.TextUtils.TruncateAt.END);
        hint.setGravity(Gravity.CENTER);
        bar.addView(hint, Ui.weight(1));

        undoButton = toolButton(Icons.UNDO, "Undo", Ui.TEXT, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                undoLast();
            }
        });
        bar.addView(undoButton);
        gridButton = toolButton(Icons.GRID, "Snap to grid", Ui.ACCENT, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                editor.setSnap(!editor.isSnap());
                gridButton.setImageDrawable(new Icons(Icons.GRID,
                        editor.isSnap() ? Ui.ACCENT : Ui.MUTED));
                toast(editor.isSnap() ? "Snap to grid on" : "Snap to grid off");
            }
        });
        bar.addView(gridButton);
        bar.addView(toolButton(Icons.TUNE, "Overlay size and opacity", Ui.TEXT,
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        boolean show = overlayPanel.getVisibility() != View.VISIBLE;
                        overlayPanel.setVisibility(show ? View.VISIBLE : View.GONE);
                        if (show) {
                            editor.select(null);
                        }
                    }
                }));
        bar.addView(toolButton(Icons.RESET, "Reset layout", Ui.TEXT, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                confirmReset();
            }
        }));
        return bar;
    }

    private ImageView toolButton(int glyph, String description, int color,
                                 View.OnClickListener l) {
        ImageView b = new ImageView(this);
        b.setImageDrawable(new Icons(glyph, color));
        b.setContentDescription(description);
        b.setPadding(dp(10), dp(10), dp(10), dp(10));
        b.setBackground(Ui.ripple(null, 0x33FFFFFF));
        b.setOnClickListener(l);
        b.setOnLongClickListener(new View.OnLongClickListener() {
            @Override
            public boolean onLongClick(View v) {
                toast(String.valueOf(v.getContentDescription()));
                return true;
            }
        });
        b.setLayoutParams(new LinearLayout.LayoutParams(dp(44), dp(44)));
        return b;
    }

    // ---------------------------------------------------------------- Inspector

    private LinearLayout buildInspector() {
        LinearLayout panel = Ui.vertical(this);
        panel.setBackground(Ui.rounded(this, 0xF00D1420, 0x26FFFFFF, 18));
        panel.setPadding(dp(16), dp(12), dp(12), dp(14));
        panel.setClickable(true);

        LinearLayout head = Ui.horizontal(this);
        LinearLayout titles = Ui.vertical(this);
        inspectorTitle = Ui.title(this, "", 16f);
        inspectorSub = Ui.text(this, "", 12f, Ui.MUTED);
        titles.addView(inspectorTitle, Ui.matchWrap());
        titles.addView(inspectorSub, Ui.matchWrap());
        head.addView(titles, Ui.weight(1));
        ImageView close = toolButton(Icons.CLOSE, "Deselect", Ui.MUTED, new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                editor.select(null);
            }
        });
        head.addView(close);
        panel.addView(head, Ui.matchWrap());

        // Size: - slider +
        LinearLayout sizeHead = Ui.horizontal(this);
        TextView sizeLabel = Ui.overline(this, "Size");
        sizeHead.addView(sizeLabel, Ui.weight(1));
        sizeValue = Ui.text(this, "", 13f, Ui.ACCENT);
        sizeHead.addView(sizeValue, Ui.wrap());
        LinearLayout.LayoutParams shp = Ui.matchWrap();
        shp.topMargin = dp(10);
        panel.addView(sizeHead, shp);

        LinearLayout sizeRow = Ui.horizontal(this);
        sizeRow.addView(stepButton(Icons.MINUS, -0.005f));
        sizeBar = new SeekBar(this);
        sizeBar.setMax(370);
        sizeBar.setProgressTintList(android.content.res.ColorStateList.valueOf(Ui.ACCENT));
        sizeBar.setThumbTintList(android.content.res.ColorStateList.valueOf(Color.WHITE));
        sizeBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar s, int progress, boolean fromUser) {
                if (fromUser && !updatingInspector) {
                    editor.setControlSize(0.03f + progress / 1000f);
                }
            }

            @Override
            public void onStartTrackingTouch(SeekBar s) {
                pushUndo();
            }

            @Override
            public void onStopTrackingTouch(SeekBar s) {
            }
        });
        sizeRow.addView(sizeBar, Ui.weight(1));
        sizeRow.addView(stepButton(Icons.PLUS, 0.005f));
        panel.addView(sizeRow, Ui.matchWrap());

        // Mapped button: a grid of chips.
        actionSection = Ui.vertical(this);
        TextView actionLabel = Ui.overline(this, "Button");
        LinearLayout.LayoutParams alp = Ui.matchWrap();
        alp.topMargin = dp(8);
        actionSection.addView(actionLabel, alp);
        LinearLayout row = null;
        for (int i = 0; i < ACTION_NAMES.length; i++) {
            if (i % 5 == 0) {
                row = Ui.horizontal(this);
                LinearLayout.LayoutParams rp = Ui.matchWrap();
                rp.topMargin = dp(6);
                actionSection.addView(row, rp);
            }
            final int index = i;
            TextView chip = Ui.text(this, ACTION_NAMES[i], 12.5f, Ui.TEXT);
            chip.setGravity(Gravity.CENTER);
            chip.setPadding(0, dp(7), 0, dp(7));
            chip.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    assignAction(index);
                }
            });
            LinearLayout.LayoutParams cp = Ui.weight(1);
            cp.rightMargin = dp(5);
            row.addView(chip, cp);
            actionChips.add(chip);
        }
        // Pad the last row so chips keep the same width.
        while (row != null && row.getChildCount() < 5) {
            View spacer = new View(this);
            // A plain View measured with WRAP_CONTENT takes all the height it is offered.
            LinearLayout.LayoutParams cp = new LinearLayout.LayoutParams(0, 1, 1f);
            cp.rightMargin = dp(5);
            row.addView(spacer, cp);
        }
        panel.addView(actionSection, Ui.matchWrap());
        return panel;
    }

    private ImageView stepButton(int glyph, final float step) {
        ImageView b = new ImageView(this);
        b.setImageDrawable(new Icons(glyph, Ui.TEXT));
        b.setPadding(dp(8), dp(8), dp(8), dp(8));
        b.setBackground(Ui.ripple(Ui.rounded(this, Ui.SURFACE_2, 0x22FFFFFF, 10), 0x33FFFFFF));
        b.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                TouchLayout.Control c = editor.getSelected();
                if (c != null) {
                    pushUndo();
                    editor.setControlSize(c.size + step);
                }
            }
        });
        b.setLayoutParams(new LinearLayout.LayoutParams(dp(36), dp(36)));
        return b;
    }

    private void showInspector(TouchLayout.Control control) {
        if (control == null) {
            inspector.setVisibility(View.GONE);
            hint.setText("Tap a control to select it · drag to move · pinch to resize");
            return;
        }
        overlayPanel.setVisibility(View.GONE);
        hint.setText("Drag to move · pinch or use the slider to resize");
        inspector.setVisibility(View.VISIBLE);
        // Keep the panel on the other side of the screen from the selected control.
        FrameLayout.LayoutParams p = (FrameLayout.LayoutParams) inspector.getLayoutParams();
        boolean rightSide = control.x < 0.5f;
        p.gravity = Gravity.CENTER_VERTICAL | (rightSide ? Gravity.END : Gravity.START);
        p.leftMargin = rightSide ? 0 : dp(24);
        p.rightMargin = rightSide ? dp(24) : 0;
        inspector.setLayoutParams(p);
        inspectorTitle.setText(controlName(control));
        boolean mappable = isMappable(control);
        actionSection.setVisibility(mappable ? View.VISIBLE : View.GONE);
        sizeBar.setEnabled(!"wheel".equals(control.kind));
        refreshInspectorValues();
    }

    private void refreshInspectorValues() {
        TouchLayout.Control c = editor.getSelected();
        if (c == null) {
            return;
        }
        updatingInspector = true;
        sizeBar.setProgress(Math.round((c.size - 0.03f) * 1000f));
        sizeValue.setText(String.format(Locale.US, "%.0f%%", c.size / 0.1f * 100f));
        int action = actionIndex(c);
        for (int i = 0; i < actionChips.size(); i++) {
            TextView chip = actionChips.get(i);
            boolean on = i == action;
            chip.setTextColor(on ? Color.WHITE : Ui.TEXT);
            chip.setTypeface(on ? Ui.MEDIUM : android.graphics.Typeface.DEFAULT);
            chip.setBackground(on ? Ui.gradient(this, 9, 0xFF12B5DB, 0xFF5B44F2)
                    : Ui.ripple(Ui.rounded(this, Ui.SURFACE_2, 0x1FFFFFFF, 9), 0x33FFFFFF));
        }
        String sub = kindName(c);
        if (isMappable(c) && action >= 0 && !ACTION_HINTS[action].isEmpty()) {
            sub += "  ·  " + ACTION_NAMES[action] + " = " + ACTION_HINTS[action];
        }
        inspectorSub.setText(sub);
        updatingInspector = false;
    }

    private void assignAction(int index) {
        TouchLayout.Control c = editor.getSelected();
        if (c == null) {
            return;
        }
        pushUndo();
        boolean driving = TouchLayout.LAYOUT_DRIVING.equals(editor.getLayoutName());
        String label;
        if (driving) {
            label = ACTION_GAME[index];
        } else if ("Start".equals(ACTION_NAMES[index]) || "Back".equals(ACTION_NAMES[index])
                || "None".equals(ACTION_NAMES[index])) {
            label = "";
        } else {
            label = ACTION_NAMES[index].toUpperCase(Locale.US);
        }
        editor.setControlAction(ACTION_BITS[index], ACTION_AXES[index], label);
    }

    private static int actionIndex(TouchLayout.Control c) {
        for (int i = 0; i < ACTION_NAMES.length; i++) {
            if (ACTION_BITS[i] != 0 && c.bit == ACTION_BITS[i]) {
                return i;
            }
            if (!ACTION_AXES[i].isEmpty() && ACTION_AXES[i].equals(c.axis)) {
                return i;
            }
        }
        return c.bit == 0 && c.axis.isEmpty() ? ACTION_NAMES.length - 1 : -1;
    }

    private static boolean isMappable(TouchLayout.Control c) {
        return "button".equals(c.kind) || "pedal".equals(c.kind) || "trigger".equals(c.kind);
    }

    private static String kindName(TouchLayout.Control c) {
        switch (c.kind) {
            case "stick":
                return "Analog stick";
            case "dpad":
                return "D-pad";
            case "steer":
                return "Steering button";
            case "wheel":
                return "Steering wheel";
            case "pedal":
                return "Analog pedal";
            case "trigger":
                return "Analog trigger";
            default:
                return "Button";
        }
    }

    private static String controlName(TouchLayout.Control c) {
        switch (c.id) {
            case "steer_left":
                return "Steer left";
            case "steer_right":
                return "Steer right";
            case "gas":
                return "Gas pedal";
            case "brake":
                return "Brake pedal";
            case "ls":
                return "Left stick";
            case "rs":
                return "Right stick";
            case "dpad":
                return "D-pad";
            case "start":
                return "Start / Pause";
            case "back":
                return "Back / Event";
            default:
                // The label follows the mapped button (NOS, E-BRAKE, A...).
                if (c.label != null && !c.label.isEmpty()) {
                    return c.label;
                }
                return c.id.toUpperCase(Locale.US) + " button";
        }
    }

    // ------------------------------------------------------------ Overlay panel

    private LinearLayout buildOverlayPanel() {
        LinearLayout panel = Ui.vertical(this);
        panel.setBackground(Ui.rounded(this, 0xF00D1420, 0x26FFFFFF, 18));
        panel.setPadding(dp(16), dp(10), dp(16), dp(10));
        panel.setClickable(true);
        panel.addView(Ui.overline(this, "All controls"), Ui.matchWrap());
        Ui.sliderRow(this, panel, "Size", null, 0.5f, 1.6f, layout.scale, "%.0f%%", 100f,
                new Ui.FloatChoice() {
                    @Override
                    public void onValue(float value) {
                        layout.scale = value;
                        dirty = true;
                        editor.invalidate();
                    }
                });
        Ui.sliderRow(this, panel, "Opacity", null, 0.1f, 1.0f, layout.opacity, "%.0f%%", 100f,
                new Ui.FloatChoice() {
                    @Override
                    public void onValue(float value) {
                        layout.opacity = value;
                        dirty = true;
                        editor.invalidate();
                    }
                });
        Ui.sliderRow(this, panel, "Stick deadzone", null, 0.0f, 0.3f, layout.deadzone,
                "%.0f%%", 100f, new Ui.FloatChoice() {
                    @Override
                    public void onValue(float value) {
                        layout.deadzone = value;
                        dirty = true;
                    }
                });
        return panel;
    }

    // --------------------------------------------------------------- Undo/reset

    private void pushUndo() {
        undo.push(layout.toJson());
        while (undo.size() > 40) {
            undo.removeLast();
        }
        updateUndo();
    }

    private void undoLast() {
        if (undo.isEmpty()) {
            toast("Nothing to undo");
            return;
        }
        String name = editor.getLayoutName();
        layout.fromJson(undo.pop());
        layout.version = TouchLayout.VERSION;
        dirty = true;
        editor.setLayout(layout, name);
        showInspector(null);
        updateUndo();
    }

    private void updateUndo() {
        if (undoButton != null) {
            undoButton.setAlpha(undo.isEmpty() ? 0.35f : 1f);
        }
    }

    private void confirmReset() {
        final String name = editor.getLayoutName();
        new AlertDialog.Builder(this, Ui.DIALOG_THEME)
                .setTitle("Reset " + (TouchLayout.LAYOUT_DRIVING.equals(name) ? "Driving"
                        : "Gamepad") + " layout?")
                .setMessage("Every control in this layout goes back to its default position, "
                        + "size and button. You can undo this.")
                .setPositiveButton("Reset", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        pushUndo();
                        TouchLayout defaults = new TouchLayout();
                        defaults.fromJson(TouchLayout.defaultJson(ControlsEditorActivity.this));
                        List<TouchLayout.Control> replacement = defaults.layouts.get(name);
                        if (replacement != null) {
                            List<TouchLayout.Control> copy = new ArrayList<>();
                            for (TouchLayout.Control control : replacement) {
                                copy.add(control.copy());
                            }
                            layout.layouts.put(name, copy);
                            dirty = true;
                            editor.setLayout(layout, name);
                            showInspector(null);
                        }
                    }
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    @Override
    public void onBackPressed() {
        if (inspector.getVisibility() == View.VISIBLE) {
            editor.select(null);
            return;
        }
        if (overlayPanel.getVisibility() == View.VISIBLE) {
            overlayPanel.setVisibility(View.GONE);
            return;
        }
        save();
        super.onBackPressed();
    }

    private void toast(String text) {
        android.widget.Toast.makeText(this, text, android.widget.Toast.LENGTH_SHORT).show();
    }

    private int dp(float value) {
        return Ui.dp(this, value);
    }
}
