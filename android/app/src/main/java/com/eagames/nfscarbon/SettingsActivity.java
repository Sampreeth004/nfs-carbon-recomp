package com.eagames.nfscarbon;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.ProgressDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.CompoundButton;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public class SettingsActivity extends Activity {
    public static final String EXTRA_PAGE = "page";

    public static final int PAGE_PERFORMANCE = 0;
    public static final int PAGE_DISPLAY = 1;
    public static final int PAGE_CONTROLS = 2;
    public static final int PAGE_DRIVER = 3;
    public static final int PAGE_GAME_DATA = 4;
    public static final int PAGE_ADVANCED = 5;
    public static final int PAGE_ABOUT = 6;

    private static final String[] PAGE_NAMES = {
            "Performance", "Display", "Controls", "GPU driver", "Game data", "Advanced", "About"
    };
    private static final String[] PAGE_SUBTITLES = {
            "Frame rate, resolution and effects. Lower settings run cooler.",
            "Renderer, resolution and how the picture fits your screen.",
            "On-screen touch controls. Controllers work without any setup.",
            "Use the phone's Vulkan driver or an imported Adreno (turnip) driver.",
            "Where the game is loaded from: your own ISO or an extracted folder.",
            "Troubleshooting switches for the renderers. Defaults are best for most players.",
            "Version and credits."
    };
    private static final int[] PAGE_ICONS = {
            Icons.GAUGE, Icons.DISPLAY, Icons.GAMEPAD, Icons.CHIP, Icons.DISC, Icons.TUNE,
            Icons.INFO
    };

    private SharedPreferences prefs;
    private TouchLayout touchLayout;
    private int page = 0;
    private boolean landscape;

    private LinearLayout nav;
    private ScrollView scroll;
    private LinearLayout content;
    private LinearLayout presetRow;

    private boolean pendingIsoScan = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = GameConfig.prefs(this);
        touchLayout = TouchLayout.load(this);
        landscape = getResources().getConfiguration().orientation
                == Configuration.ORIENTATION_LANDSCAPE;
        page = getIntent().getIntExtra(EXTRA_PAGE, PAGE_PERFORMANCE);
        if (savedInstanceState != null) {
            page = savedInstanceState.getInt("page", page);
        }
        setContentView(buildShell());
        showPage(page);
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.putInt("page", page);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // The controls editor saves its own copy of the layouts.
        touchLayout = TouchLayout.load(this);
        if (page == PAGE_CONTROLS && content != null) {
            rebuild();
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        touchLayout.save(this);
        GameConfig.writeToml(this);
    }

    // ------------------------------------------------------------------- Shell

    private View buildShell() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(landscape ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        root.setBackground(new SpeedBackground());

        // Navigation: a rail on the left in landscape, tabs on top in portrait.
        LinearLayout side = Ui.vertical(this);
        if (landscape) {
            side.setPadding(dp(12), dp(14), dp(10), dp(14));
            side.setBackgroundColor(0x66060A12);
        } else {
            side.setPadding(dp(12), dp(12), dp(12), 0);
        }
        LinearLayout head = Ui.horizontal(this);
        ImageView back = new ImageView(this);
        back.setImageDrawable(new Icons(Icons.BACK, Ui.TEXT));
        back.setPadding(dp(8), dp(8), dp(8), dp(8));
        back.setBackground(Ui.ripple(Ui.rounded(this, Ui.SURFACE_2, 0x22FFFFFF, 12), 0x33FFFFFF));
        back.setContentDescription("Back");
        back.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                finish();
            }
        });
        head.addView(back, new LinearLayout.LayoutParams(dp(40), dp(40)));
        TextView title = Ui.text(this, "SETTINGS", 20f, Color.WHITE);
        title.setTypeface(Ui.CONDENSED_ITALIC);
        title.setLetterSpacing(0.08f);
        LinearLayout.LayoutParams tp = Ui.wrap();
        tp.leftMargin = dp(12);
        head.addView(title, tp);
        side.addView(head, Ui.matchWrap());

        nav = new LinearLayout(this);
        nav.setOrientation(landscape ? LinearLayout.VERTICAL : LinearLayout.HORIZONTAL);
        for (int i = 0; i < PAGE_NAMES.length; i++) {
            nav.addView(navItem(i));
        }
        if (landscape) {
            ScrollView navScroll = new ScrollView(this);
            navScroll.setVerticalScrollBarEnabled(false);
            navScroll.addView(nav, Ui.matchWrap());
            LinearLayout.LayoutParams np = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
            np.topMargin = dp(14);
            side.addView(navScroll, np);
            TextView saved = Ui.text(this, "Changes save automatically and apply the next time "
                    + "the game starts.", 11f, Ui.FAINT);
            saved.setPadding(dp(4), dp(8), dp(4), 0);
            side.addView(saved, Ui.matchWrap());
            root.addView(side, new LinearLayout.LayoutParams(dp(218),
                    ViewGroup.LayoutParams.MATCH_PARENT));
        } else {
            HorizontalScrollView tabs = new HorizontalScrollView(this);
            tabs.setHorizontalScrollBarEnabled(false);
            tabs.addView(nav);
            LinearLayout.LayoutParams np = Ui.matchWrap();
            np.topMargin = dp(10);
            side.addView(tabs, np);
            root.addView(side, Ui.matchWrap());
        }

        scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        FrameLayout center = new FrameLayout(this);
        content = Ui.vertical(this);
        content.setPadding(dp(landscape ? 24 : 14), dp(16), dp(landscape ? 24 : 14), dp(32));
        FrameLayout.LayoutParams cp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER_HORIZONTAL);
        int widthDp = (int) (getResources().getDisplayMetrics().widthPixels
                / getResources().getDisplayMetrics().density);
        if (widthDp - (landscape ? 218 : 0) > 720) {
            cp.width = dp(720);
        }
        center.addView(content, cp);
        scroll.addView(center, Ui.matchWrap());
        LinearLayout.LayoutParams sp = landscape
                ? new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f)
                : new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        root.addView(scroll, sp);
        return root;
    }

    private View navItem(final int index) {
        LinearLayout item = Ui.horizontal(this);
        item.setPadding(dp(12), dp(10), dp(14), dp(10));
        item.addView(Ui.icon(this, PAGE_ICONS[index], Ui.MUTED, 20));
        TextView label = Ui.text(this, PAGE_NAMES[index], 14.5f, Ui.MUTED);
        LinearLayout.LayoutParams lp = Ui.wrap();
        lp.leftMargin = dp(12);
        item.addView(label, lp);
        item.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showPage(index);
            }
        });
        LinearLayout.LayoutParams p = landscape ? Ui.matchWrap() : Ui.wrap();
        if (landscape) {
            p.bottomMargin = dp(4);
        } else {
            p.rightMargin = dp(6);
        }
        item.setLayoutParams(p);
        return item;
    }

    private void updateNav() {
        for (int i = 0; i < nav.getChildCount(); i++) {
            LinearLayout item = (LinearLayout) nav.getChildAt(i);
            boolean on = i == page;
            ((ImageView) item.getChildAt(0)).setImageDrawable(
                    new Icons(PAGE_ICONS[i], on ? Ui.ACCENT : Ui.MUTED));
            TextView label = (TextView) item.getChildAt(1);
            label.setTextColor(on ? Color.WHITE : Ui.MUTED);
            label.setTypeface(on ? Ui.MEDIUM : android.graphics.Typeface.DEFAULT);
            item.setBackground(on ? Ui.rounded(this, 0x2619C6E6, 0x4019C6E6, 12)
                    : Ui.ripple(null, 0x22FFFFFF));
        }
    }

    private void showPage(int index) {
        page = Math.max(0, Math.min(PAGE_NAMES.length - 1, index));
        updateNav();
        rebuild();
        scroll.scrollTo(0, 0);
    }

    private void rebuild() {
        final int y = scroll.getScrollY();
        content.removeAllViews();
        presetRow = null;
        TextView heading = Ui.text(this, PAGE_NAMES[page], 26f, Color.WHITE);
        heading.setTypeface(Ui.CONDENSED);
        content.addView(heading, Ui.matchWrap());
        TextView sub = Ui.text(this, PAGE_SUBTITLES[page], 13f, Ui.MUTED);
        sub.setPadding(0, dp(2), 0, dp(4));
        content.addView(sub, Ui.matchWrap());
        switch (page) {
            case PAGE_PERFORMANCE:
                buildPerformance();
                break;
            case PAGE_DISPLAY:
                buildDisplay();
                break;
            case PAGE_CONTROLS:
                buildControls();
                break;
            case PAGE_DRIVER:
                buildDriver();
                break;
            case PAGE_GAME_DATA:
                buildGameData();
                break;
            case PAGE_ADVANCED:
                buildAdvanced();
                break;
            default:
                buildAbout();
                break;
        }
        scroll.post(new Runnable() {
            @Override
            public void run() {
                scroll.scrollTo(0, y);
            }
        });
    }

    /** Starts a titled group; returns the card that rows are added to. */
    private LinearLayout group(String title) {
        if (title != null) {
            TextView t = Ui.overline(this, title);
            t.setPadding(dp(4), dp(18), 0, dp(8));
            content.addView(t, Ui.matchWrap());
        }
        LinearLayout card = Ui.card(this);
        LinearLayout.LayoutParams p = Ui.matchWrap();
        if (title == null) {
            p.topMargin = dp(14);
        }
        content.addView(card, p);
        return card;
    }

    private void gap(LinearLayout card) {
        if (card.getChildCount() > 0) {
            card.addView(Ui.divider(this));
        }
    }

    // ------------------------------------------------------------ Row helpers

    private void bool(LinearLayout card, String title, String hint, final String key,
                      boolean def) {
        gap(card);
        Ui.switchRow(this, card, title, hint, prefs.getBoolean(key, def),
                new CompoundButton.OnCheckedChangeListener() {
                    @Override
                    public void onCheckedChanged(CompoundButton b, boolean checked) {
                        prefs.edit().putBoolean(key, checked).apply();
                        refreshPresets();
                    }
                });
    }

    private void ints(LinearLayout card, String title, String hint, String[] labels,
                      final int[] values, final String key, int def) {
        gap(card);
        int stored = prefs.getInt(key, def);
        int selected = -1;
        for (int i = 0; i < values.length; i++) {
            if (values[i] == stored) {
                selected = i;
            }
        }
        Ui.segmentedRow(this, card, title, hint, labels, selected, new Ui.IntChoice() {
            @Override
            public void onChoice(int index) {
                prefs.edit().putInt(key, values[index]).apply();
                refreshPresets();
            }
        });
    }

    private void strings(LinearLayout card, String title, String hint, String[] labels,
                         final String[] values, final String key, String def,
                         final Runnable after) {
        gap(card);
        String stored = prefs.getString(key, def);
        int selected = 0;
        for (int i = 0; i < values.length; i++) {
            if (values[i].equals(stored)) {
                selected = i;
            }
        }
        Ui.segmentedRow(this, card, title, hint, labels, selected, new Ui.IntChoice() {
            @Override
            public void onChoice(int index) {
                prefs.edit().putString(key, values[index]).apply();
                if (after != null) {
                    after.run();
                }
            }
        });
    }

    // ------------------------------------------------------------ Performance

    private void buildPerformance() {
        TextView t = Ui.overline(this, "Preset");
        t.setPadding(dp(4), dp(18), 0, dp(8));
        content.addView(t, Ui.matchWrap());
        presetRow = Ui.horizontal(this);
        presetRow.setGravity(Gravity.FILL_VERTICAL);
        for (int i = 0; i < GameConfig.PRESET_NAMES.length; i++) {
            final int index = i;
            LinearLayout tile = Ui.vertical(this);
            tile.setPadding(dp(14), dp(12), dp(14), dp(12));
            tile.addView(Ui.title(this, GameConfig.PRESET_NAMES[i], 15f), Ui.matchWrap());
            tile.addView(Ui.text(this, GameConfig.PRESET_HINTS[i], 12f, Ui.MUTED), Ui.matchWrap());
            tile.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    GameConfig.applyPreset(prefs, index);
                    rebuild();
                    toast(GameConfig.PRESET_NAMES[index] + " applied");
                }
            });
            LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(0,
                    ViewGroup.LayoutParams.MATCH_PARENT, 1f);
            p.rightMargin = i < GameConfig.PRESET_NAMES.length - 1 ? dp(10) : 0;
            presetRow.addView(tile, p);
        }
        content.addView(presetRow, Ui.matchWrap());
        refreshPresets();

        LinearLayout card = group("Frame rate and resolution");
        ints(card, "Frame rate cap", "Lower caps run cooler and steadier. The game is designed "
                        + "for 30 or 60.",
                new String[]{"30", "45", "60", "Unlimited"}, new int[]{30, 45, 60, 0},
                GameConfig.KEY_FPS_CAP, 60);
        ints(card, "Render resolution", "Size of the native renderer's images: 100% is "
                        + "1280x720, 75% is 960x540. Lower is cooler and faster.",
                new String[]{"50%", "60%", "75%", "85%", "100%"},
                new int[]{50, 60, 75, 85, 100}, GameConfig.KEY_RENDER_SCALE, 100);

        card = group("Effects");
        bool(card, "Bloom and glow", "Heavy on phone GPUs: costs frame rate and heats the "
                + "phone.", GameConfig.KEY_BLOOM, false);
        ints(card, "Car reflections", "Not shown by the native renderer yet, so Off costs "
                        + "nothing visible. Other values only add GPU work.",
                new String[]{"Off", "1 face", "2 faces", "All 6"}, new int[]{0, 1, 2, 6},
                GameConfig.KEY_REFLECTIONS, 0);
        bool(card, "Rear-view mirror at half rate", "The mirror updates every other frame. "
                + "Lighter, barely noticeable.", GameConfig.KEY_MIRROR_HALF, true);
        ints(card, "Texture filtering", "Anisotropic filtering keeps road textures sharp at "
                        + "angles.",
                new String[]{"Off", "2x", "4x", "16x", "Game"}, new int[]{0, 2, 3, 5, -1},
                GameConfig.KEY_ANISO, 3);
    }

    private void refreshPresets() {
        if (presetRow == null) {
            return;
        }
        int active = GameConfig.detectPreset(prefs);
        for (int i = 0; i < presetRow.getChildCount(); i++) {
            View tile = presetRow.getChildAt(i);
            boolean on = i == active;
            tile.setBackground(on
                    ? Ui.ripple(Ui.rounded(this, 0x3319C6E6, Ui.ACCENT, 14), 0x33FFFFFF)
                    : Ui.ripple(Ui.rounded(this, Ui.SURFACE, Ui.STROKE, 14), 0x33FFFFFF));
        }
    }

    // ---------------------------------------------------------------- Display

    private void buildDisplay() {
        LinearLayout card = group("Renderer");
        strings(card, "Renderer", "Native Vulkan is much faster. Xenos emulation is a slower "
                        + "compatibility fallback; its options are under Advanced.",
                new String[]{"Native Vulkan", "Xenos emulation"}, new String[]{"carbon", "xenos"},
                GameConfig.KEY_RENDERER, GameConfig.DEFAULT_RENDERER, null);

        card = group("Picture");
        gap(card);
        resolutionRow(card);
        bool(card, "Widescreen", "Fills wide phone screens with a wider camera view and a "
                + "proportional HUD. Off shows the original 16:9 view with black bars.",
                GameConfig.KEY_WIDESCREEN, true);
        bool(card, "Vertical sync", "Avoids tearing. Leave on.", GameConfig.KEY_VSYNC, true);
        bool(card, "Fine frame pacing", "120 Hz timing with a 60 fps cap: late frames show "
                + "sooner, so motion is smoother.", GameConfig.KEY_FINE_PACING, true);
        bool(card, "Frame rate counter", "Shows fps and frame time at the top of the screen "
                + "while playing.", GameConfig.KEY_SHOW_FPS, true);
    }

    private void resolutionRow(LinearLayout card) {
        int width = prefs.getInt(GameConfig.KEY_WIDTH, GameConfig.DEFAULT_WIDTH);
        int height = prefs.getInt(GameConfig.KEY_HEIGHT, GameConfig.DEFAULT_HEIGHT);
        android.util.DisplayMetrics m = new android.util.DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(m);
        final int nativeLong = Math.max(m.widthPixels, m.heightPixels);
        final int nativeShort = Math.min(m.widthPixels, m.heightPixels);
        final int[][] dims = {
                {720, 480}, {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160},
                {nativeLong, nativeShort}
        };
        final String[] labels = {
                "480p  ·  720x480", "720p  ·  1280x720 (recommended)", "1080p  ·  1920x1080",
                "1440p  ·  2560x1440", "4K  ·  3840x2160",
                "Native  ·  " + nativeLong + "x" + nativeShort, "Custom..."
        };
        int selection = dims.length;
        for (int i = 0; i < dims.length; i++) {
            if (dims[i][0] == width && dims[i][1] == height) {
                selection = i;
                break;
            }
        }
        final String[] shown = labels.clone();
        shown[dims.length] = selection == dims.length ? "Custom  ·  " + width + "x" + height
                : "Custom...";
        final int[] selected = {selection};
        Ui.pickerRow(this, card, "Game resolution",
                "The resolution the game believes it renders at. 720p-class values suit phones.",
                shown, selected, new Ui.IntChoice() {
                    @Override
                    public void onChoice(int index) {
                        if (index < dims.length) {
                            prefs.edit().putInt(GameConfig.KEY_WIDTH, dims[index][0])
                                    .putInt(GameConfig.KEY_HEIGHT, dims[index][1]).apply();
                        } else {
                            customResolution();
                        }
                    }
                });
    }

    private void customResolution() {
        LinearLayout box = Ui.horizontal(this);
        box.setPadding(dp(20), dp(8), dp(20), 0);
        final EditText w = numberField(prefs.getInt(GameConfig.KEY_WIDTH, GameConfig.DEFAULT_WIDTH));
        final EditText h = numberField(prefs.getInt(GameConfig.KEY_HEIGHT, GameConfig.DEFAULT_HEIGHT));
        box.addView(w, Ui.weight(1));
        TextView x = Ui.text(this, "  ×  ", 16f, Ui.MUTED);
        box.addView(x, Ui.wrap());
        box.addView(h, Ui.weight(1));
        new AlertDialog.Builder(this, Ui.DIALOG_THEME)
                .setTitle("Custom resolution")
                .setMessage("Width 640-4095, height 480-4095.")
                .setView(box)
                .setPositiveButton(android.R.string.ok, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        prefs.edit()
                                .putInt(GameConfig.KEY_WIDTH, parseInt(w.getText().toString(),
                                        GameConfig.DEFAULT_WIDTH, 640, 4095))
                                .putInt(GameConfig.KEY_HEIGHT, parseInt(h.getText().toString(),
                                        GameConfig.DEFAULT_HEIGHT, 480, 4095))
                                .apply();
                        rebuild();
                    }
                })
                .setNegativeButton(android.R.string.cancel, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        rebuild();
                    }
                })
                .show();
    }

    private EditText numberField(int value) {
        EditText edit = new EditText(this);
        edit.setText(String.valueOf(value));
        edit.setTextColor(Ui.TEXT);
        edit.setGravity(Gravity.CENTER);
        edit.setInputType(InputType.TYPE_CLASS_NUMBER);
        edit.setSelectAllOnFocus(true);
        return edit;
    }

    // --------------------------------------------------------------- Controls

    private void buildControls() {
        LinearLayout card = group(null);
        card.setPadding(dp(14), dp(14), dp(14), dp(14));
        TouchEditorView preview = new TouchEditorView(this);
        preview.setPreview(true);
        preview.setBackground(new SpeedBackground());
        preview.setLayout(touchLayout, touchLayout.active);
        preview.setClipToOutline(true);
        preview.setOutlineProvider(new android.view.ViewOutlineProvider() {
            @Override
            public void getOutline(View view, android.graphics.Outline outline) {
                outline.setRoundRect(0, 0, view.getWidth(), view.getHeight(), dp(10));
            }
        });
        preview.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                openEditor();
            }
        });
        card.addView(preview, Ui.matchWrap());
        LinearLayout buttons = Ui.horizontal(this);
        TextView edit = Ui.primaryButton(this, "Edit layout", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                openEditor();
            }
        });
        buttons.addView(edit, Ui.weight(1));
        TextView reset = Ui.secondaryButton(this, "Reset all", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                confirm("Reset touch controls?", "Both layouts, their size, opacity and "
                        + "deadzone go back to the defaults.", "Reset", new Runnable() {
                    @Override
                    public void run() {
                        touchLayout.resetFromDefaults(SettingsActivity.this);
                        touchLayout.save(SettingsActivity.this);
                        toast("Touch controls reset");
                        rebuild();
                    }
                });
            }
        });
        LinearLayout.LayoutParams rp = Ui.wrap();
        rp.leftMargin = dp(10);
        buttons.addView(reset, rp);
        LinearLayout.LayoutParams bp = Ui.matchWrap();
        bp.topMargin = dp(12);
        card.addView(buttons, bp);

        card = group("Overlay");
        bool(card, "Touch controls", "Show the on-screen controls in game. A connected "
                + "controller always works.", GameConfig.KEY_TOUCH_ENABLED, true);
        gap(card);
        Ui.segmentedRow(this, card, "Starting layout", "Switch any time in game with the "
                        + "layout button in the top-left corner.",
                new String[]{"Driving", "Gamepad"},
                TouchLayout.LAYOUT_XBOX.equals(touchLayout.active) ? 1 : 0, new Ui.IntChoice() {
                    @Override
                    public void onChoice(int index) {
                        touchLayout.active = index == 1 ? TouchLayout.LAYOUT_XBOX
                                : TouchLayout.LAYOUT_DRIVING;
                        touchLayout.save(SettingsActivity.this);
                        rebuild();
                    }
                });
        gap(card);
        Ui.sliderRow(this, card, "Opacity", null, 0.1f, 1.0f, touchLayout.opacity, "%.0f%%",
                100f, new Ui.FloatChoice() {
                    @Override
                    public void onValue(float value) {
                        touchLayout.opacity = value;
                    }
                });
        gap(card);
        Ui.sliderRow(this, card, "Size", "Scales every control together.", 0.5f, 1.6f,
                touchLayout.scale, "%.0f%%", 100f, new Ui.FloatChoice() {
                    @Override
                    public void onValue(float value) {
                        touchLayout.scale = value;
                    }
                });
        gap(card);
        Ui.sliderRow(this, card, "Stick deadzone", "How far a stick moves before it "
                        + "registers.", 0.0f, 0.3f, touchLayout.deadzone, "%.0f%%", 100f,
                new Ui.FloatChoice() {
                    @Override
                    public void onValue(float value) {
                        touchLayout.deadzone = value;
                    }
                });
    }

    private void openEditor() {
        touchLayout.save(this);
        startActivity(new Intent(this, ControlsEditorActivity.class));
    }

    // -------------------------------------------------------------- GPU driver

    private static boolean isArm64() {
        return Build.SUPPORTED_ABIS.length > 0 && "arm64-v8a".equals(Build.SUPPORTED_ABIS[0]);
    }

    private void buildDriver() {
        final List<DriverStore.Driver> drivers = DriverStore.list(this);
        String selectedPath = prefs.getString(GameConfig.KEY_DRIVER_LOADER, "");
        DriverStore.Driver active = null;
        for (DriverStore.Driver driver : drivers) {
            if (driver.loader.getAbsolutePath().equals(selectedPath)) {
                active = driver;
            }
        }

        String detail;
        int color;
        if (active == null) {
            detail = "The driver built into your phone. Always works.";
            color = Ui.OK;
        } else if (active.isFullLoader) {
            detail = "Full Vulkan loader, loaded directly.";
            color = Ui.ACCENT;
        } else if (isArm64()) {
            detail = "Adreno driver (turnip), loaded next to the system driver with "
                    + "libadrenotools. Qualcomm Adreno GPUs only.";
            color = Ui.ACCENT;
        } else {
            detail = "Adreno driver packages need a 64-bit ARM phone. The system driver will "
                    + "be used instead.";
            color = Ui.WARN;
        }
        LinearLayout card = group("In use");
        card.setPadding(dp(16), dp(14), dp(16), dp(14));
        LinearLayout hero = Ui.horizontal(this);
        hero.addView(Ui.icon(this, Icons.CHIP, color, 34));
        LinearLayout texts = Ui.vertical(this);
        texts.setPadding(dp(14), 0, 0, 0);
        texts.addView(Ui.title(this, active == null ? "System Vulkan driver" : active.label, 17f),
                Ui.matchWrap());
        TextView d = Ui.text(this, detail, 12.5f, Ui.MUTED);
        texts.addView(d, Ui.matchWrap());
        hero.addView(texts, Ui.weight(1));
        card.addView(hero, Ui.matchWrap());
        TextView phone = Ui.text(this, Build.MANUFACTURER + " " + Build.MODEL + "  ·  "
                + (Build.SUPPORTED_ABIS.length > 0 ? Build.SUPPORTED_ABIS[0] : "unknown ABI")
                + "  ·  Android " + Build.VERSION.RELEASE, 11.5f, Ui.FAINT);
        phone.setPadding(0, dp(10), 0, 0);
        card.addView(phone, Ui.matchWrap());

        card = group("Drivers");
        driverRow(card, "System Vulkan driver", "Built into the phone", active == null,
                new View.OnClickListener() {
                    @Override
                    public void onClick(View view) {
                        prefs.edit().putString(GameConfig.KEY_DRIVER_LOADER, "").apply();
                        rebuild();
                    }
                }, null);
        for (final DriverStore.Driver driver : drivers) {
            driverRow(card, driver.label, driver.isFullLoader ? "Full Vulkan loader"
                            : "Adreno driver (turnip)",
                    active != null && driver.id.equals(active.id),
                    new View.OnClickListener() {
                        @Override
                        public void onClick(View view) {
                            prefs.edit().putString(GameConfig.KEY_DRIVER_LOADER,
                                    driver.loader.getAbsolutePath()).apply();
                            rebuild();
                        }
                    },
                    new View.OnClickListener() {
                        @Override
                        public void onClick(View view) {
                            confirmRemoveDriver(driver);
                        }
                    });
        }

        LinearLayout.LayoutParams ip = Ui.matchWrap();
        ip.topMargin = dp(14);
        content.addView(Ui.primaryButton(this, "Import driver (.zip or .so)",
                new View.OnClickListener() {
                    @Override
                    public void onClick(View view) {
                        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                        intent.addCategory(Intent.CATEGORY_OPENABLE);
                        intent.setType("*/*");
                        try {
                            startActivityForResult(intent, StorageUtil.REQUEST_PICK_DRIVER);
                        } catch (Exception e) {
                            toast("No document picker available");
                        }
                    }
                }), ip);
        TextView info = Ui.text(this, "Turnip packages are .zip files with a meta.json and a "
                + "libvulkan_freedreno.so; a full libvulkan.so loader also works. The file is "
                + "copied into app storage. If a driver cannot be opened the game falls back to "
                + "the system driver (logged under the tag nfscarbon-driver).", 12f, Ui.MUTED);
        info.setPadding(dp(4), dp(10), dp(4), 0);
        content.addView(info, Ui.matchWrap());
    }

    private void driverRow(LinearLayout card, String title, String subtitle, boolean selected,
                           View.OnClickListener select, View.OnClickListener remove) {
        gap(card);
        LinearLayout row = Ui.horizontal(this);
        row.setPadding(0, dp(12), 0, dp(12));
        row.setOnClickListener(select);
        row.setBackground(Ui.ripple(null, 0x22FFFFFF));
        View radio = new View(this);
        radio.setBackground(selected ? Ui.rounded(this, Ui.ACCENT, 0, 10)
                : Ui.rounded(this, 0, 0x66FFFFFF, 10));
        if (selected) {
            ((android.graphics.drawable.GradientDrawable) radio.getBackground())
                    .setStroke(dp(5), 0xFF0E2A35);
        }
        row.addView(radio, new LinearLayout.LayoutParams(dp(20), dp(20)));
        LinearLayout texts = Ui.labels(this, title, subtitle);
        texts.setPadding(dp(14), 0, 0, 0);
        row.addView(texts, Ui.weight(1));
        if (selected) {
            row.addView(Ui.badge(this, "In use", Ui.OK));
        }
        if (remove != null) {
            ImageView trash = Ui.icon(this, Icons.TRASH, Ui.DANGER, 36);
            trash.setPadding(dp(8), dp(8), dp(8), dp(8));
            trash.setBackground(Ui.ripple(null, 0x33FF6B6B));
            trash.setContentDescription("Remove");
            trash.setOnClickListener(remove);
            LinearLayout.LayoutParams tp = new LinearLayout.LayoutParams(dp(36), dp(36));
            tp.leftMargin = dp(6);
            row.addView(trash, tp);
        }
        card.addView(row, Ui.matchWrap());
    }

    private void confirmRemoveDriver(final DriverStore.Driver driver) {
        confirm("Remove driver?", driver.label + " will be deleted from app storage.",
                "Remove", new Runnable() {
                    @Override
                    public void run() {
                        String selected = prefs.getString(GameConfig.KEY_DRIVER_LOADER, "");
                        DriverStore.remove(driver);
                        if (driver.loader.getAbsolutePath().equals(selected)) {
                            prefs.edit().putString(GameConfig.KEY_DRIVER_LOADER, "").apply();
                        }
                        rebuild();
                        toast("Driver removed");
                    }
                });
    }

    // --------------------------------------------------------------- Game data

    private void buildGameData() {
        boolean ready = GameConfig.isValidGameData(this);
        String path = prefs.getString(GameConfig.KEY_ISO_PATH, "");
        String name = prefs.getString(GameConfig.KEY_ISO_LABEL, "");
        String titleText;
        String detail;
        if (path == null || path.isEmpty()) {
            titleText = ready ? "Extracted game folder" : "No game data yet";
            detail = ready ? "Found an extracted NFS folder in app storage."
                    : "Pick your NFS Carbon ISO below to get started.";
        } else {
            File file = new File(path);
            String size = file.isFile()
                    ? String.format(Locale.US, "  ·  %.2f GB", file.length() / 1073741824.0) : "";
            titleText = (name == null || name.isEmpty() ? file.getName() : name);
            detail = path + size;
        }
        LinearLayout card = group("Current");
        card.setPadding(dp(16), dp(14), dp(16), dp(14));
        LinearLayout hero = Ui.horizontal(this);
        hero.addView(Ui.icon(this, Icons.DISC, ready ? Ui.OK : Ui.WARN, 36));
        LinearLayout texts = Ui.vertical(this);
        texts.setPadding(dp(14), 0, 0, 0);
        texts.addView(Ui.title(this, titleText, 17f), Ui.matchWrap());
        texts.addView(Ui.text(this, detail, 12f, Ui.MUTED), Ui.matchWrap());
        hero.addView(texts, Ui.weight(1));
        hero.addView(Ui.badge(this, ready ? "Ready" : "Missing", ready ? Ui.OK : Ui.WARN));
        card.addView(hero, Ui.matchWrap());

        TextView t = Ui.overline(this, "Choose game data");
        t.setPadding(dp(4), dp(18), 0, dp(8));
        content.addView(t, Ui.matchWrap());
        content.addView(Ui.primaryButton(this, "Pick ISO file  (copied into the app)",
                new View.OnClickListener() {
                    @Override
                    public void onClick(View view) {
                        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                        intent.addCategory(Intent.CATEGORY_OPENABLE);
                        intent.setType("*/*");
                        intent.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{
                                "application/x-iso9660-image", "application/octet-stream"});
                        try {
                            startActivityForResult(intent, StorageUtil.REQUEST_PICK_ISO);
                        } catch (Exception e) {
                            toast("No document picker available");
                        }
                    }
                }), Ui.matchWrap());
        TextView copyNote = Ui.text(this, "The safe choice. Needs free space for a copy of the "
                + "ISO.", 12f, Ui.MUTED);
        copyNote.setPadding(dp(4), dp(6), dp(4), dp(4));
        content.addView(copyNote, Ui.matchWrap());

        LinearLayout.LayoutParams sp = Ui.matchWrap();
        sp.topMargin = dp(10);
        content.addView(Ui.secondaryButton(this, "Find ISOs on this device  (use in place)",
                new View.OnClickListener() {
                    @Override
                    public void onClick(View view) {
                        scanForIsos();
                    }
                }), sp);
        TextView scanNote = Ui.text(this, "No copy, but needs All files access, and some phones "
                + "cannot map large files from shared storage.", 12f, Ui.MUTED);
        scanNote.setPadding(dp(4), dp(6), dp(4), dp(4));
        content.addView(scanNote, Ui.matchWrap());

        if (path != null && !path.isEmpty()) {
            LinearLayout.LayoutParams cp = Ui.matchWrap();
            cp.topMargin = dp(10);
            TextView clear = Ui.secondaryButton(this, "Forget this game data",
                    new View.OnClickListener() {
                        @Override
                        public void onClick(View view) {
                            prefs.edit().remove(GameConfig.KEY_ISO_PATH)
                                    .remove(GameConfig.KEY_ISO_LABEL).apply();
                            rebuild();
                            toast("Cleared; the game will look for an extracted folder");
                        }
                    });
            clear.setTextColor(Ui.DANGER);
            content.addView(clear, cp);
        }
    }

    private void scanForIsos() {
        if (!StorageUtil.hasAllFilesAccess(this)) {
            pendingIsoScan = true;
            StorageUtil.requestAllFilesAccess(this);
            return;
        }
        final List<File> isos = StorageUtil.findIsos(Environment.getExternalStorageDirectory(),
                3, 50);
        if (isos.isEmpty()) {
            toast("No .iso files found on shared storage");
            return;
        }
        final String[] names = new String[isos.size()];
        for (int i = 0; i < isos.size(); i++) {
            names[i] = isos.get(i).getAbsolutePath();
        }
        new AlertDialog.Builder(this, Ui.DIALOG_THEME)
                .setTitle("Use an ISO in place")
                .setItems(names, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        File file = isos.get(which);
                        prefs.edit()
                                .putString(GameConfig.KEY_ISO_PATH, file.getAbsolutePath())
                                .putString(GameConfig.KEY_ISO_LABEL, file.getName())
                                .apply();
                        rebuild();
                        toast("Using the ISO directly from shared storage");
                    }
                })
                .show();
    }

    private void confirmIsoCopy(final Uri uri) {
        final String displayName = StorageUtil.displayName(this, uri);
        long size = StorageUtil.fileSize(this, uri);
        String sizeText = size > 0 ? String.format(Locale.US, " (%.2f GB)", size / 1073741824.0)
                : "";
        new AlertDialog.Builder(this, Ui.DIALOG_THEME)
                .setTitle("Copy ISO into the app?")
                .setMessage(displayName + sizeText + "\n\nIt will be stored at "
                        + new File(getFilesDir(), "game.iso").getAbsolutePath()
                        + ". Make sure there is enough free space.")
                .setPositiveButton("Copy", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        startIsoCopy(uri, displayName);
                    }
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private void startIsoCopy(final Uri uri, final String displayName) {
        final File destination = new File(getFilesDir(), "game.iso");
        final boolean[] cancelled = {false};
        final ProgressDialog dialog = new ProgressDialog(this, Ui.DIALOG_THEME);
        dialog.setTitle("Copying ISO");
        dialog.setMessage(displayName);
        dialog.setProgressStyle(ProgressDialog.STYLE_HORIZONTAL);
        dialog.setMax(100);
        dialog.setCancelable(false);
        dialog.setButton(DialogInterface.BUTTON_NEGATIVE, "Cancel",
                new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        cancelled[0] = true;
                    }
                });
        dialog.show();

        new Thread(new Runnable() {
            @Override
            public void run() {
                final boolean success = StorageUtil.copyToFile(SettingsActivity.this, uri,
                        destination, new StorageUtil.Progress() {
                            @Override
                            public boolean onProgress(long copied, long total) {
                                if (dialog.isShowing()) {
                                    final int percent = total > 0
                                            ? (int) (copied * 100 / total) : 0;
                                    final String text = String.format(Locale.US,
                                            "%.0f MB copied", copied / 1048576.0);
                                    runOnUiThread(new Runnable() {
                                        @Override
                                        public void run() {
                                            dialog.setProgress(percent);
                                            dialog.setMessage(text);
                                        }
                                    });
                                }
                                return !cancelled[0];
                            }
                        });
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        if (dialog.isShowing()) {
                            dialog.dismiss();
                        }
                        if (success) {
                            prefs.edit()
                                    .putString(GameConfig.KEY_ISO_PATH,
                                            destination.getAbsolutePath())
                                    .putString(GameConfig.KEY_ISO_LABEL, displayName)
                                    .apply();
                            GameConfig.writeToml(SettingsActivity.this);
                            rebuild();
                            toast("ISO copied. Ready to play.");
                        } else {
                            //noinspection ResultOfMethodCallIgnored
                            destination.delete();
                            toast("ISO copy cancelled or failed");
                        }
                    }
                });
            }
        }).start();
    }

    // ---------------------------------------------------------------- Advanced

    private void buildAdvanced() {
        LinearLayout card = group("Native renderer");
        bool(card, "Threaded command processing", "Reads the game's GPU commands on a "
                + "separate thread so the game never waits for ring space. Turn off if you "
                + "see glitches.", GameConfig.KEY_THREADED_CP, true);
        bool(card, "Precise GPU barriers", "GPU waits only for the work that used an image. "
                + "Turn off if you see flickering or stale areas.",
                GameConfig.KEY_PRECISE_BARRIERS, true);
        bool(card, "Clear with load operations", "Clears only the area the game asks for. "
                + "Turn off if you see garbage in mirrors, shadows or glow.",
                GameConfig.KEY_CLEAR_LOAD_OP, true);
        bool(card, "Skip redundant resolves", "Skips copies whose result is already in place.",
                GameConfig.KEY_SKIP_RESOLVES, true);
        bool(card, "Detailed profiling", "Logs per-draw CPU time, GPU timestamps and per-pass "
                + "costs. Costs a little performance; for troubleshooting only.",
                GameConfig.KEY_GPU_PROFILE, false);

        boolean xenos = "xenos".equals(prefs.getString(GameConfig.KEY_RENDERER,
                GameConfig.DEFAULT_RENDERER));
        card = group("Xenos emulation");
        if (!xenos) {
            TextView note = Ui.text(this, "Only used when Display > Renderer is set to Xenos "
                    + "emulation.", 12.5f, Ui.WARN);
            note.setPadding(0, dp(10), 0, dp(4));
            card.addView(note, Ui.matchWrap());
        }
        LinearLayout xenosRows = Ui.vertical(this);
        int msaa = prefs.getInt(GameConfig.KEY_MSAA_SAMPLES, GameConfig.DEFAULT_MSAA_SAMPLES);
        final int[] msaaValues = {0, 2, 4};
        Ui.segmentedRow(this, xenosRows, "Anti-aliasing (MSAA)", null,
                new String[]{"Off", "2x", "4x"}, msaa >= 4 ? 2 : (msaa >= 2 ? 1 : 0),
                new Ui.IntChoice() {
                    @Override
                    public void onChoice(int index) {
                        prefs.edit().putInt(GameConfig.KEY_MSAA_SAMPLES, msaaValues[index]).apply();
                    }
                });
        LinearLayout saved = card;
        card = xenosRows;
        strings(card, "Post-process anti-aliasing", null,
                new String[]{"None", "FXAA", "FXAA Extreme"},
                new String[]{"none", "fxaa", "fxaa_extreme"}, GameConfig.KEY_FXAA,
                GameConfig.DEFAULT_FXAA, null);
        bool(card, "Single-pass scene", "Faster; needs MSAA off and 720p or lower.",
                GameConfig.KEY_SINGLE_PASS, true);
        bool(card, "Occlusion queries", "Sun flares and light glows.", GameConfig.KEY_OCCLUSION,
                true);
        saved.addView(xenosRows, Ui.matchWrap());
        Ui.setEnabledDeep(xenosRows, xenos);

        card = group("Reset");
        card.setPadding(dp(16), dp(14), dp(16), dp(14));
        card.addView(Ui.labels(this, "Restore default settings", "Every setting except your "
                + "game data and GPU driver choice. Touch layouts are kept."), Ui.matchWrap());
        TextView reset = Ui.secondaryButton(this, "Reset settings", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                confirm("Reset all settings?", "Graphics, display and renderer settings go "
                        + "back to their defaults.", "Reset", new Runnable() {
                    @Override
                    public void run() {
                        GameConfig.resetSettings(prefs);
                        rebuild();
                        toast("Settings reset");
                    }
                });
            }
        });
        reset.setTextColor(Ui.DANGER);
        LinearLayout.LayoutParams rp = Ui.matchWrap();
        rp.topMargin = dp(12);
        card.addView(reset, rp);
    }

    // ------------------------------------------------------------------ About

    private void buildAbout() {
        String version = "";
        try {
            version = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception ignored) {
        }
        LinearLayout card = group(null);
        card.setPadding(dp(18), dp(16), dp(18), dp(16));
        TextView nfs = Ui.text(this, "NEED FOR SPEED", 12f, Ui.ACCENT);
        nfs.setTypeface(Ui.CONDENSED);
        nfs.setLetterSpacing(0.3f);
        card.addView(nfs, Ui.matchWrap());
        TextView carbon = Ui.text(this, "CARBON", 34f, Color.WHITE);
        carbon.setTypeface(Ui.CONDENSED_ITALIC);
        card.addView(carbon, Ui.matchWrap());
        card.addView(Ui.text(this, "Version " + version, 13f, Ui.MUTED), Ui.matchWrap());
        TextView about = Ui.text(this, "The Xbox 360 game running natively on Android through "
                + "static recompilation. Bring your own ISO or extracted game folder.", 13.5f,
                Ui.TEXT);
        about.setPadding(0, dp(12), 0, 0);
        about.setLineSpacing(0, 1.15f);
        card.addView(about, Ui.matchWrap());

        card = group("Rendering");
        card.setPadding(dp(16), dp(12), dp(16), dp(12));
        card.addView(Ui.text(this, "Native Vulkan renderer (the carbon GPU plugin), written for "
                + "this project. Xenos emulation (the xenos plugin) stays available as a "
                + "fallback.", 13f, Ui.TEXT), Ui.matchWrap());

        card = group("Credits");
        card.setPadding(dp(16), dp(12), dp(16), dp(12));
        card.addView(Ui.text(this, "ReXGlue SDK: recompilation runtime.", 13f, Ui.TEXT),
                Ui.matchWrap());
        TextView adreno = Ui.text(this, "libadrenotools by Billy Laws (BSD-2-Clause): loading "
                + "downloaded Adreno / turnip drivers.", 13f, Ui.TEXT);
        adreno.setPadding(0, dp(6), 0, 0);
        card.addView(adreno, Ui.matchWrap());
    }

    // ----------------------------------------------------------------- Helpers

    private void confirm(String title, String message, String action, final Runnable onYes) {
        new AlertDialog.Builder(this, Ui.DIALOG_THEME)
                .setTitle(title)
                .setMessage(message)
                .setPositiveButton(action, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        onYes.run();
                    }
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private int dp(float value) {
        return Ui.dp(this, value);
    }

    private int parseInt(String text, int fallback, int min, int max) {
        try {
            int value = Integer.parseInt(text.trim());
            return Math.max(min, Math.min(max, value));
        } catch (Exception e) {
            return fallback;
        }
    }

    private void toast(String text) {
        Toast.makeText(this, text, Toast.LENGTH_LONG).show();
    }

    // ------------------------------------------------------------ Activity glue

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == StorageUtil.REQUEST_ALL_FILES) {
            if (pendingIsoScan && StorageUtil.hasAllFilesAccess(this)) {
                pendingIsoScan = false;
                scanForIsos();
            } else {
                pendingIsoScan = false;
                toast("All files access was not granted");
            }
            return;
        }
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        Uri uri = data.getData();
        if (requestCode == StorageUtil.REQUEST_PICK_ISO) {
            confirmIsoCopy(uri);
        } else if (requestCode == StorageUtil.REQUEST_PICK_DRIVER) {
            importDriver(uri);
        }
    }

    private void importDriver(final Uri uri) {
        final String displayName = StorageUtil.displayName(this, uri);
        new Thread(new Runnable() {
            @Override
            public void run() {
                final DriverStore.Driver driver = DriverStore.importFromUri(
                        SettingsActivity.this, uri, displayName);
                runOnUiThread(new Runnable() {
                    @Override
                    public void run() {
                        if (driver == null) {
                            toast("Import failed: no .so file found in the package");
                            return;
                        }
                        prefs.edit()
                                .putString(GameConfig.KEY_DRIVER_LOADER,
                                        driver.loader.getAbsolutePath())
                                .putString(GameConfig.KEY_DRIVER_LABEL, driver.label)
                                .apply();
                        rebuild();
                        toast(driver.isFullLoader ? "Imported " + driver.label
                                : "Imported " + driver.label + " (used on the next game start)");
                    }
                });
            }
        }).start();
    }
}
