package com.eagames.nfscarbon;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.ProgressDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public class SettingsActivity extends Activity {
    private static final int COLOR_TEXT = 0xFFE6EDF3;
    private static final int COLOR_MUTED = 0xFF8B949E;
    private static final int COLOR_ACCENT = 0xFF19C6E6;
    private static final int COLOR_OK = 0xFF2ED47A;
    private static final int COLOR_WARN = 0xFFFFB020;
    private static final int COLOR_FIELD = 0xFF182233;

    private static final String[] ACTION_NAMES = {
            "None", "A", "B", "X", "Y", "LB", "RB", "LT (analog)", "RT (analog)",
            "Start", "Back", "L3", "R3", "Guide"
    };
    private static final int[] ACTION_BITS = {
            0, 4096, 8192, 16384, 32768, 256, 512, 0, 0, 16, 32, 64, 128, 1024
    };
    private static final String[] ACTION_AXES = {
            "", "", "", "", "", "", "", "lt", "rt", "", "", "", "", ""
    };

    private static final String[] PAGES = {
            "Graphics", "GPU driver", "Game data", "Controls", "About"
    };
    private static final int PAGE_CONTROLS = 3;

    // scale %, fps cap, bloom, reflection faces, mirror half rate, anisotropy value
    private static final String[] PRESET_NAMES = {"Battery saver", "Balanced", "Quality"};
    private static final String[] PRESET_HINTS = {
            "30 fps, 75% resolution", "60 fps, recommended", "Bloom, full reflections"
    };
    private static final int[][] PRESETS = {
            {75, 30, 0, 1, 1, 0},
            {100, 60, 0, 2, 1, 3},
            {100, 60, 1, 6, 0, 5},
    };

    private LinearLayout container;
    private LinearLayout current;
    private LinearLayout tabs;
    private ScrollView scroll;
    private SharedPreferences prefs;
    private TouchLayout touchLayout;
    private int page = 0;
    private int rowsInCard = 0;

    private TextView isoValue;
    private boolean pendingIsoScan = false;
    private Uri pendingIsoUri;
    private Uri pendingDriverUri;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_settings);
        container = findViewById(R.id.settings_container);
        tabs = findViewById(R.id.settings_tabs);
        scroll = findViewById(R.id.settings_scroll);
        prefs = GameConfig.prefs(this);
        touchLayout = TouchLayout.load(this);

        findViewById(R.id.settings_back).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                finish();
            }
        });

        // Keep the cards a readable width on wide landscape screens.
        int widthPx = getResources().getDisplayMetrics().widthPixels;
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) container.getLayoutParams();
        lp.width = Math.min(widthPx, dp(780));
        lp.gravity = Gravity.CENTER_HORIZONTAL;
        container.setLayoutParams(lp);

        boolean openControls = getIntent().getBooleanExtra(LauncherActivity.EXTRA_OPEN_CONTROLS,
                false);
        if (openControls) {
            page = PAGE_CONTROLS;
        } else if (savedInstanceState != null) {
            page = savedInstanceState.getInt("page", 0);
        }
        buildTabs();
        showPage(page);
        if (openControls) {
            container.post(new Runnable() {
                @Override
                public void run() {
                    openLayoutEditor();
                }
            });
        }
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        outState.putInt("page", page);
    }

    @Override
    protected void onPause() {
        super.onPause();
        touchLayout.save(this);
        GameConfig.writeToml(this);
    }

    // -------------------------------------------------------------------- Pages

    private void buildTabs() {
        tabs.removeAllViews();
        for (int i = 0; i < PAGES.length; i++) {
            final int index = i;
            TextView chip = new TextView(this);
            chip.setText(PAGES[i]);
            chip.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
            chip.setGravity(Gravity.CENTER);
            chip.setPadding(dp(18), dp(8), dp(18), dp(8));
            chip.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View view) {
                    showPage(index);
                }
            });
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            params.rightMargin = dp(8);
            tabs.addView(chip, params);
        }
    }

    private void updateTabs() {
        for (int i = 0; i < tabs.getChildCount(); i++) {
            TextView chip = (TextView) tabs.getChildAt(i);
            boolean selected = i == page;
            chip.setTextColor(selected ? Color.WHITE : COLOR_MUTED);
            chip.setTypeface(null, selected ? Typeface.BOLD : Typeface.NORMAL);
            if (selected) {
                GradientDrawable bg = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                        new int[]{0xFF12B5DB, 0xFF6A3DFF});
                bg.setCornerRadius(dp(20));
                chip.setBackground(bg);
            } else {
                chip.setBackground(rounded(0x33182233, 0x22FFFFFF, 20));
            }
        }
    }

    private void showPage(int index) {
        page = index;
        updateTabs();
        populatePage();
        scroll.scrollTo(0, 0);
    }

    private void populatePage() {
        container.removeAllViews();
        current = container;
        rowsInCard = 0;
        switch (page) {
            case 0:
                buildGraphics();
                break;
            case 1:
                buildDriver();
                break;
            case 2:
                buildGameData();
                break;
            case PAGE_CONTROLS:
                buildTouch();
                break;
            default:
                buildAbout();
                break;
        }
    }

    private void rebuildUi() {
        final int y = scroll.getScrollY();
        populatePage();
        scroll.post(new Runnable() {
            @Override
            public void run() {
                scroll.scrollTo(0, y);
            }
        });
    }

    // ---------------------------------------------------------------- Graphics

    private void buildGraphics() {
        buildPresets();

        // ---- Display
        beginCard("Display", null);
        final String[] rendererLabels = {
                "Native Vulkan (default)", "Xenos emulation (fallback)"
        };
        final String[] rendererValues = {"carbon", "xenos"};
        stringSpinner("Renderer",
                "Native Vulkan is much faster. Xenos emulation is a slower compatibility "
                        + "fallback.",
                rendererLabels, rendererValues, GameConfig.KEY_RENDERER,
                GameConfig.DEFAULT_RENDERER);

        int width = prefs.getInt(GameConfig.KEY_WIDTH, GameConfig.DEFAULT_WIDTH);
        int height = prefs.getInt(GameConfig.KEY_HEIGHT, GameConfig.DEFAULT_HEIGHT);
        android.util.DisplayMetrics realMetrics = new android.util.DisplayMetrics();
        getWindowManager().getDefaultDisplay().getRealMetrics(realMetrics);
        final int nativeLong = Math.max(realMetrics.widthPixels, realMetrics.heightPixels);
        final int nativeShort = Math.min(realMetrics.widthPixels, realMetrics.heightPixels);
        final int[][] dimensions = {
                {720, 480}, {1280, 720}, {1920, 1080}, {2560, 1440}, {3840, 2160},
                {nativeLong, nativeShort}
        };
        final String[] labels = {
                "480p (720x480, fastest)", "720p (1280x720)", "1080p (1920x1080)",
                "1440p (2560x1440)", "4K (3840x2160)",
                "Native (" + nativeLong + "x" + nativeShort + ")", "Custom"
        };
        int selection = 4;
        for (int i = 0; i < dimensions.length; i++) {
            if (dimensions[i][0] == width && dimensions[i][1] == height) {
                selection = i;
                break;
            }
        }

        final EditText customWidth = createEditText("Custom width", String.valueOf(width));
        final EditText customHeight = createEditText("Custom height", String.valueOf(height));
        customWidth.setVisibility(selection == 4 ? View.VISIBLE : View.GONE);
        customHeight.setVisibility(selection == 4 ? View.VISIBLE : View.GONE);

        spinnerRow("Guest resolution",
                "Resolution the game thinks it renders at. Leave at 720p-class values unless "
                        + "you know you need more.",
                labels, selection,
                new AdapterView.OnItemSelectedListener() {
                    @Override
                    public void onItemSelected(AdapterView<?> parent, View view, int position,
                                               long id) {
                        if (position < dimensions.length) {
                            prefs.edit()
                                    .putInt(GameConfig.KEY_WIDTH, dimensions[position][0])
                                    .putInt(GameConfig.KEY_HEIGHT, dimensions[position][1])
                                    .apply();
                            customWidth.setText(String.valueOf(dimensions[position][0]));
                            customHeight.setText(String.valueOf(dimensions[position][1]));
                        }
                        customWidth.setVisibility(position == dimensions.length
                                ? View.VISIBLE : View.GONE);
                        customHeight.setVisibility(position == dimensions.length
                                ? View.VISIBLE : View.GONE);
                    }

                    @Override
                    public void onNothingSelected(AdapterView<?> parent) {
                    }
                });
        addField(customWidth);
        addField(customHeight);
        customWidth.addTextChangedListener(new SimpleWatcher() {
            @Override
            public void afterTextChanged(Editable s) {
                int value = parseInt(s.toString(), GameConfig.DEFAULT_WIDTH, 640, 4095);
                prefs.edit().putInt(GameConfig.KEY_WIDTH, value).apply();
            }
        });
        customHeight.addTextChangedListener(new SimpleWatcher() {
            @Override
            public void afterTextChanged(Editable s) {
                int value = parseInt(s.toString(), GameConfig.DEFAULT_HEIGHT, 480, 4095);
                prefs.edit().putInt(GameConfig.KEY_HEIGHT, value).apply();
            }
        });

        intSpinner("Render resolution",
                "Scales the native renderer's targets. Lower is cooler and faster.",
                new String[]{"100% (1280x720, sharpest)", "85% (1088x612)",
                        "75% (960x540, lighter)", "60% (768x432)", "50% (640x360, lightest)"},
                new int[]{100, 85, 75, 60, 50}, GameConfig.KEY_RENDER_SCALE, 100);
        intSpinner("Frame rate cap",
                "Lower caps run cooler. The game is designed for 30 or 60.",
                new String[]{"30 fps (coolest, steadiest)", "45 fps", "60 fps", "Unlimited"},
                new int[]{30, 45, 60, 0}, GameConfig.KEY_FPS_CAP, 60);
        switchRow("Vertical sync", null, GameConfig.KEY_VSYNC, true);
        switchRow("Letterbox presentation", "Keep the game's aspect ratio with black bars.",
                GameConfig.KEY_LETTERBOX, true);
        switchRow("Fine frame pacing", "120 Hz timing with a 60 fps cap for smoother frames.",
                GameConfig.KEY_FINE_PACING, true);
        switchRow("Show FPS counter", "Overlay at the top of the screen while playing.",
                GameConfig.KEY_SHOW_FPS, true);

        // ---- Effects
        beginCard("Effects and quality", null);
        switchRow("Bloom / glow",
                "Heavy: heats the phone and costs fps. Off by default on Android.",
                GameConfig.KEY_BLOOM, false);
        int reflStored = prefs.getInt(GameConfig.KEY_REFLECTIONS, 2);
        intSpinnerAt("Car reflections",
                "How many reflection faces are refreshed per frame.",
                new String[]{"Full (6 faces per frame)", "Reduced (2 per frame)",
                        "Minimal (1 per frame)"},
                new int[]{6, 2, 1}, GameConfig.KEY_REFLECTIONS,
                reflStored >= 6 ? 0 : (reflStored >= 2 ? 1 : 2));
        switchRow("Rear-view mirror at half rate", "Lighter; the mirror updates every other frame.",
                GameConfig.KEY_MIRROR_HALF, true);
        intSpinner("Anisotropic filtering", "Sharper textures at angles.",
                new String[]{"Force 4x (default)", "Off (fastest)", "2x", "16x", "No override"},
                new int[]{3, 0, 2, 5, -1}, GameConfig.KEY_ANISO, 3);

        // ---- Xenos only
        beginCard("Xenos emulation only",
                "These options only apply when the renderer is set to Xenos emulation.");
        int msaa = prefs.getInt(GameConfig.KEY_MSAA_SAMPLES, GameConfig.DEFAULT_MSAA_SAMPLES);
        intSpinnerAt("Anti-aliasing (MSAA)", null,
                new String[]{"Off (fastest)", "MSAA 2x", "MSAA 4x (game default)"},
                new int[]{0, 2, 4}, GameConfig.KEY_MSAA_SAMPLES,
                msaa >= 4 ? 2 : (msaa >= 2 ? 1 : 0));
        stringSpinner("Anti-aliasing (post effect)", null,
                new String[]{"None", "FXAA", "FXAA Extreme"},
                new String[]{"none", "fxaa", "fxaa_extreme"}, GameConfig.KEY_FXAA,
                GameConfig.DEFAULT_FXAA);
        switchRow("Single-pass scene", "Faster; requires MSAA off.", GameConfig.KEY_SINGLE_PASS,
                true);
        switchRow("Occlusion queries", "Sun flares and light glows.", GameConfig.KEY_OCCLUSION,
                true);
        label("Upscaler: bilinear (this SDK build has no FidelityFX support).");
    }

    private void buildPresets() {
        beginCard("Performance preset",
                "A quick starting point. Fine-tune the options below afterwards.");
        final int active = detectPreset();
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        for (int i = 0; i < PRESETS.length; i++) {
            final int index = i;
            TextView chip = new TextView(this);
            chip.setText(PRESET_NAMES[i] + "\n" + PRESET_HINTS[i]);
            chip.setGravity(Gravity.CENTER);
            chip.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            chip.setTextColor(i == active ? Color.WHITE : COLOR_MUTED);
            chip.setTypeface(null, i == active ? Typeface.BOLD : Typeface.NORMAL);
            chip.setPadding(dp(8), dp(12), dp(8), dp(12));
            chip.setBackground(i == active
                    ? rounded(0x3319C6E6, COLOR_ACCENT, 12)
                    : rounded(COLOR_FIELD, 0x22FFFFFF, 12));
            chip.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View view) {
                    applyPreset(index);
                    rebuildUi();
                    toast(PRESET_NAMES[index] + " preset applied");
                }
            });
            LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(0,
                    ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
            params.rightMargin = i < PRESETS.length - 1 ? dp(8) : 0;
            row.addView(chip, params);
        }
        LinearLayout.LayoutParams rowParams = matchWrap();
        rowParams.topMargin = dp(8);
        current.addView(row, rowParams);
        label(active < 0 ? "Current settings: custom" : "Current settings: "
                + PRESET_NAMES[active]);
    }

    private int detectPreset() {
        int[] now = {
                prefs.getInt(GameConfig.KEY_RENDER_SCALE, 100),
                prefs.getInt(GameConfig.KEY_FPS_CAP, 60),
                prefs.getBoolean(GameConfig.KEY_BLOOM, false) ? 1 : 0,
                prefs.getInt(GameConfig.KEY_REFLECTIONS, 2),
                prefs.getBoolean(GameConfig.KEY_MIRROR_HALF, true) ? 1 : 0,
                prefs.getInt(GameConfig.KEY_ANISO, 3),
        };
        for (int i = 0; i < PRESETS.length; i++) {
            boolean same = true;
            for (int j = 0; j < now.length; j++) {
                if (now[j] != PRESETS[i][j]) {
                    same = false;
                    break;
                }
            }
            if (same) {
                return i;
            }
        }
        return -1;
    }

    private void applyPreset(int index) {
        int[] p = PRESETS[index];
        prefs.edit()
                .putInt(GameConfig.KEY_RENDER_SCALE, p[0])
                .putInt(GameConfig.KEY_FPS_CAP, p[1])
                .putBoolean(GameConfig.KEY_BLOOM, p[2] != 0)
                .putInt(GameConfig.KEY_REFLECTIONS, p[3])
                .putBoolean(GameConfig.KEY_MIRROR_HALF, p[4] != 0)
                .putInt(GameConfig.KEY_ANISO, p[5])
                .apply();
    }

    // --------------------------------------------------------------- Game data

    private void buildGameData() {
        beginCard("Game data", null);
        isoValue = label("");
        isoValue.setTextColor(COLOR_TEXT);
        isoValue.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
        updateIsoLabel();

        beginCard("Choose game data", "Copy mode is the safe default; direct paths skip the "
                + "copy but some devices cannot memory-map large files from shared storage.");
        primaryButton("Pick ISO file (copies into app storage)", new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("*/*");
                intent.putExtra(Intent.EXTRA_MIME_TYPES,
                        new String[]{"application/x-iso9660-image", "application/octet-stream"});
                try {
                    startActivityForResult(intent, StorageUtil.REQUEST_PICK_ISO);
                } catch (Exception e) {
                    toast("No document picker available");
                }
            }
        });

        button("Scan device for ISOs (direct path, needs All files access)",
                new View.OnClickListener() {
                    @Override
                    public void onClick(View view) {
                        scanForIsos();
                    }
                });

        button("Clear game data selection", new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                prefs.edit()
                        .remove(GameConfig.KEY_ISO_PATH)
                        .remove(GameConfig.KEY_ISO_LABEL)
                        .apply();
                updateIsoLabel();
                toast("Cleared; the runtime will look for an extracted game folder");
            }
        });

        label("A copied ISO lives at " + new File(getFilesDir(), "game.iso").getAbsolutePath()
                + ".");
    }

    private void updateIsoLabel() {
        String path = prefs.getString(GameConfig.KEY_ISO_PATH, "");
        String name = prefs.getString(GameConfig.KEY_ISO_LABEL, "");
        if (path == null || path.isEmpty()) {
            isoValue.setText("Game data: not set (extracted game folder fallback)");
            return;
        }
        File file = new File(path);
        String size = file.isFile()
                ? String.format(Locale.US, " (%.2f GB)", file.length() / 1073741824.0) : "";
        isoValue.setText("Game data: " + (name == null || name.isEmpty() ? path : name) + size
                + "\n" + path);
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
        new AlertDialog.Builder(this)
                .setTitle("Select ISO (direct path)")
                .setItems(names, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        File file = isos.get(which);
                        prefs.edit()
                                .putString(GameConfig.KEY_ISO_PATH, file.getAbsolutePath())
                                .putString(GameConfig.KEY_ISO_LABEL, file.getName())
                                .apply();
                        updateIsoLabel();
                        toast("Using ISO directly from shared storage");
                    }
                })
                .show();
    }

    private void confirmIsoCopy(final Uri uri) {
        final String displayName = StorageUtil.displayName(this, uri);
        long size = StorageUtil.fileSize(this, uri);
        String sizeText = size > 0
                ? String.format(Locale.US, " (%.2f GB)", size / 1073741824.0) : "";
        new AlertDialog.Builder(this)
                .setTitle("Copy ISO into app storage?")
                .setMessage(displayName + sizeText + "\n\nThe file will be stored at "
                        + new File(getFilesDir(), "game.iso").getAbsolutePath()
                        + ". Make sure there is enough free space.")
                .setPositiveButton(android.R.string.ok, new DialogInterface.OnClickListener() {
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
        final ProgressDialog dialog = new ProgressDialog(this);
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
                boolean ok = StorageUtil.copyToFile(SettingsActivity.this, uri, destination,
                        new StorageUtil.Progress() {
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
                final boolean success = ok;
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
                            updateIsoLabel();
                            GameConfig.writeToml(SettingsActivity.this);
                            toast("ISO copied");
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

    // -------------------------------------------------------------- GPU driver

    private static boolean isArm64() {
        return Build.SUPPORTED_ABIS.length > 0 && "arm64-v8a".equals(Build.SUPPORTED_ABIS[0]);
    }

    private void buildDriver() {
        final List<DriverStore.Driver> drivers = DriverStore.list(this);
        String selected = prefs.getString(GameConfig.KEY_DRIVER_LOADER, "");
        DriverStore.Driver active = null;
        for (DriverStore.Driver driver : drivers) {
            if (driver.loader.getAbsolutePath().equals(selected)) {
                active = driver;
            }
        }

        // ---- Active driver status
        beginCard("Active driver", null);
        String name;
        String detail;
        int color;
        if (active == null) {
            name = "System Vulkan driver";
            detail = "The driver built into your phone. Always works.";
            color = COLOR_OK;
        } else if (active.isFullLoader) {
            name = active.label;
            detail = "Full Vulkan loader, loaded directly.";
            color = COLOR_ACCENT;
        } else if (isArm64()) {
            name = active.label;
            detail = "Adreno driver (turnip), opened next to the system loader with "
                    + "libadrenotools. Qualcomm Adreno GPUs only.";
            color = COLOR_ACCENT;
        } else {
            name = active.label;
            detail = "Adreno driver packages need a 64-bit ARM phone. The system driver will "
                    + "be used instead.";
            color = COLOR_WARN;
        }
        LinearLayout status = new LinearLayout(this);
        status.setOrientation(LinearLayout.HORIZONTAL);
        status.setGravity(Gravity.CENTER_VERTICAL);
        View dot = new View(this);
        GradientDrawable dotShape = new GradientDrawable();
        dotShape.setShape(GradientDrawable.OVAL);
        dotShape.setColor(color);
        dot.setBackground(dotShape);
        status.addView(dot, new LinearLayout.LayoutParams(dp(12), dp(12)));
        TextView nameView = new TextView(this);
        nameView.setText(name);
        nameView.setTextColor(Color.WHITE);
        nameView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f);
        nameView.setTypeface(null, Typeface.BOLD);
        LinearLayout.LayoutParams nameParams = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        nameParams.leftMargin = dp(10);
        status.addView(nameView, nameParams);
        LinearLayout.LayoutParams statusParams = matchWrap();
        statusParams.topMargin = dp(8);
        current.addView(status, statusParams);
        label(detail);
        label("This phone: " + Build.MODEL + "  |  " + (Build.SUPPORTED_ABIS.length > 0
                ? Build.SUPPORTED_ABIS[0] : "unknown ABI") + "  |  Android "
                + Build.VERSION.RELEASE);

        // ---- Driver list
        beginCard("Available drivers", "Tap a driver to use it. Changes apply the next time "
                + "the game starts.");
        driverRow("System Vulkan driver", "Built into the phone", active == null, null,
                new View.OnClickListener() {
                    @Override
                    public void onClick(View view) {
                        prefs.edit().putString(GameConfig.KEY_DRIVER_LOADER, "").apply();
                        rebuildUi();
                    }
                }, null);
        for (final DriverStore.Driver driver : drivers) {
            String kind = driver.isFullLoader ? "Full Vulkan loader" : "Adreno driver (turnip)";
            driverRow(driver.label, kind, driver == active || (active != null
                            && driver.id.equals(active.id)), driver,
                    new View.OnClickListener() {
                        @Override
                        public void onClick(View view) {
                            prefs.edit().putString(GameConfig.KEY_DRIVER_LOADER,
                                    driver.loader.getAbsolutePath()).apply();
                            rebuildUi();
                        }
                    },
                    new View.OnClickListener() {
                        @Override
                        public void onClick(View view) {
                            confirmRemoveDriver(driver);
                        }
                    });
        }
        if (drivers.isEmpty()) {
            label("No custom drivers imported yet.");
        }

        // ---- Import
        beginCard("Import a driver", "Download a turnip / Adreno driver .zip (the kind with a "
                + "meta.json and a libvulkan_freedreno.so), or pick a full libvulkan.so loader. "
                + "It is copied into app storage.");
        primaryButton("Import driver (.zip or .so)", new View.OnClickListener() {
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
        });
        label("If a custom driver cannot be opened the game falls back to the system driver. "
                + "The reason is logged under the tag nfscarbon-driver.");
    }

    private void driverRow(String title, String subtitle, boolean selected,
                           DriverStore.Driver driver, View.OnClickListener select,
                           View.OnClickListener remove) {
        rowGap();
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(10), 0, dp(10));
        row.setOnClickListener(select);

        TextView radio = new TextView(this);
        radio.setText(selected ? "●" : "○");
        radio.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f);
        radio.setTextColor(selected ? COLOR_ACCENT : COLOR_MUTED);
        radio.setPadding(0, 0, dp(12), 0);
        row.addView(radio, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        TextView titleView = new TextView(this);
        titleView.setText(title);
        titleView.setTextColor(COLOR_TEXT);
        titleView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f);
        texts.addView(titleView, matchWrap());
        TextView sub = new TextView(this);
        sub.setText(subtitle);
        sub.setTextColor(COLOR_MUTED);
        sub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        texts.addView(sub, matchWrap());
        row.addView(texts, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        if (remove != null) {
            TextView removeView = new TextView(this);
            removeView.setText("Remove");
            removeView.setTextColor(0xFFFF7B72);
            removeView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
            removeView.setPadding(dp(12), dp(6), dp(4), dp(6));
            removeView.setOnClickListener(remove);
            row.addView(removeView, new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        }
        current.addView(row, matchWrap());
    }

    private void confirmRemoveDriver(final DriverStore.Driver driver) {
        new AlertDialog.Builder(this)
                .setTitle("Remove driver?")
                .setMessage(driver.label + " will be deleted from app storage.")
                .setPositiveButton("Remove", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        String selected = prefs.getString(GameConfig.KEY_DRIVER_LOADER, "");
                        DriverStore.remove(driver);
                        if (driver.loader.getAbsolutePath().equals(selected)) {
                            prefs.edit().putString(GameConfig.KEY_DRIVER_LOADER, "").apply();
                        }
                        rebuildUi();
                        toast("Driver removed");
                    }
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    // ----------------------------------------------------------- Touch controls

    private void buildTouch() {
        section(R.string.touch_controls);
        switchRow("Enable touch overlay", GameConfig.KEY_TOUCH_ENABLED, true);

        String active = touchLayout.active;
        final String[] layoutValues = {TouchLayout.LAYOUT_XBOX, TouchLayout.LAYOUT_DRIVING};
        final String[] layoutNames = {"Xbox gamepad", "Driving"};
        int layoutSelection = TouchLayout.LAYOUT_DRIVING.equals(active) ? 1 : 0;
        spinnerRow("Default layout", layoutNames, layoutSelection,
                new AdapterView.OnItemSelectedListener() {
                    @Override
                    public void onItemSelected(AdapterView<?> parent, View view, int position,
                                               long id) {
                        touchLayout.active = layoutValues[position];
                        touchLayout.save(SettingsActivity.this);
                    }

                    @Override
                    public void onNothingSelected(AdapterView<?> parent) {
                    }
                });

        seekRow("Overlay opacity", 0.1f, 1.0f, touchLayout.opacity,
                new ValueListener() {
                    @Override
                    public void onValue(float value) {
                        touchLayout.opacity = value;
                    }
                });
        seekRow("Control scale", 0.5f, 1.6f, touchLayout.scale, new ValueListener() {
            @Override
            public void onValue(float value) {
                touchLayout.scale = value;
            }
        });
        seekRow("Stick deadzone", 0.0f, 0.3f, touchLayout.deadzone, new ValueListener() {
            @Override
            public void onValue(float value) {
                touchLayout.deadzone = value;
            }
        });

        button("Edit touch controls...", new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                openLayoutEditor();
            }
        });
        button("Reset layouts to defaults", new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                touchLayout.resetFromDefaults(SettingsActivity.this);
                touchLayout.save(SettingsActivity.this);
                toast("Touch layouts reset");
                rebuildUi();
            }
        });
    }

    private void openLayoutEditor() {
        final LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int padding = dp(16);
        root.setPadding(padding, padding, padding, padding);

        labelInto(root, "Drag a control to move it. Use the sliders to resize and remap.");

        final String[] layoutValues = {TouchLayout.LAYOUT_XBOX, TouchLayout.LAYOUT_DRIVING};
        final String[] layoutNames = {"Xbox gamepad", "Driving"};
        final Spinner layoutPicker = new Spinner(this);
        ArrayAdapter<String> layoutAdapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, layoutNames);
        layoutAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        layoutPicker.setAdapter(layoutAdapter);
        layoutPicker.setSelection(TouchLayout.LAYOUT_DRIVING.equals(touchLayout.active) ? 1 : 0);

        final TouchEditorView editor = new TouchEditorView(this);
        editor.setLayout(touchLayout, layoutValues[layoutPicker.getSelectedItemPosition()]);
        editor.setMinimumHeight(dp(160));

        final TextView sizeLabel = new TextView(this);
        sizeLabel.setTextColor(COLOR_MUTED);
        sizeLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        final SeekBar sizeBar = new SeekBar(this);
        sizeBar.setMax(370);

        final Spinner actionSpinner = new Spinner(this);
        ArrayAdapter<String> actionAdapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, ACTION_NAMES);
        actionAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        actionSpinner.setAdapter(actionAdapter);

        final boolean[] updating = {false};

        editor.setListener(new TouchEditorView.Listener() {
            @Override
            public void onSelectionChanged(TouchLayout.Control control) {
                updating[0] = true;
                if (control != null) {
                    sizeBar.setProgress(Math.round((control.size - 0.03f) * 1000f));
                    sizeLabel.setText("Size: " + String.format(Locale.US, "%.3f", control.size));
                    int actionIndex = 0;
                    for (int i = 0; i < ACTION_NAMES.length; i++) {
                        boolean bitMatch = ACTION_BITS[i] != 0 && control.bit == ACTION_BITS[i];
                        boolean axisMatch = !ACTION_AXES[i].isEmpty()
                                && ACTION_AXES[i].equals(control.axis);
                        if (bitMatch || axisMatch
                                || (i == 0 && control.bit == 0 && control.axis.isEmpty())) {
                            actionIndex = i;
                            break;
                        }
                    }
                    actionSpinner.setSelection(actionIndex);
                }
                updating[0] = false;
            }

            @Override
            public void onLayoutEdited() {
                touchLayout.save(SettingsActivity.this);
            }
        });

        sizeBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                if (!fromUser) {
                    return;
                }
                float size = 0.03f + progress / 1000f;
                sizeLabel.setText("Size: " + String.format(Locale.US, "%.3f", size));
                editor.setControlSize(size);
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
            }
        });

        actionSpinner.post(new Runnable() {
            @Override
            public void run() {
                actionSpinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
                    @Override
                    public void onItemSelected(AdapterView<?> parent, View view, int position,
                                               long id) {
                        if (updating[0]) {
                            return;
                        }
                        editor.setControlAction(ACTION_BITS[position], ACTION_AXES[position],
                                ACTION_NAMES[position]);
                    }

                    @Override
                    public void onNothingSelected(AdapterView<?> parent) {
                    }
                });
            }
        });

        layoutPicker.post(new Runnable() {
            @Override
            public void run() {
                layoutPicker.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
                    @Override
                    public void onItemSelected(AdapterView<?> parent, View view, int position,
                                               long id) {
                        editor.setLayout(touchLayout, layoutValues[position]);
                        editor.invalidate();
                    }

                    @Override
                    public void onNothingSelected(AdapterView<?> parent) {
                    }
                });
            }
        });

        Button reset = new Button(this);
        reset.setText("Reset this layout");
        reset.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                int position = layoutPicker.getSelectedItemPosition();
                TouchLayout defaults = new TouchLayout();
                defaults.fromJson(TouchLayout.defaultJson(SettingsActivity.this));
                List<TouchLayout.Control> replacement = defaults.layouts.get(layoutValues[position]);
                if (replacement != null) {
                    List<TouchLayout.Control> copy = new ArrayList<>();
                    for (TouchLayout.Control control : replacement) {
                        copy.add(control.copy());
                    }
                    touchLayout.layouts.put(layoutValues[position], copy);
                    touchLayout.save(SettingsActivity.this);
                    editor.setLayout(touchLayout, layoutValues[position]);
                }
            }
        });

        root.addView(layoutPicker, matchWrap());
        root.addView(editor, matchWrap());
        root.addView(sizeLabel, matchWrap());
        root.addView(sizeBar, matchWrap());
        labelInto(root, "Action");
        root.addView(actionSpinner, matchWrap());
        root.addView(reset, matchWrap());

        new AlertDialog.Builder(this)
                .setTitle("Touch layout editor")
                .setView(root)
                .setPositiveButton("Done", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int which) {
                        touchLayout.save(SettingsActivity.this);
                        rebuildUi();
                    }
                })
                .show();
    }

    // -------------------------------------------------------------------- About

    private void buildAbout() {
        beginCard("NFS Carbon", null);
        String version = "";
        try {
            version = "Version " + getPackageManager().getPackageInfo(getPackageName(), 0)
                    .versionName;
        } catch (Exception ignored) {
        }
        label(version);
        label("Need for Speed: Carbon (Xbox 360) running natively on Android through a static "
                + "recompilation. Game data: your own ISO or extracted game folder.");
        beginCard("Rendering", null);
        label("Native Vulkan renderer (the carbon GPU plugin), written for this project. Xenos "
                + "emulation (the xenos plugin) stays available as a fallback.");
        beginCard("Credits", null);
        label("ReXGlue SDK: recompilation runtime.");
        label("libadrenotools by Billy Laws (BSD-2-Clause): loading downloaded Adreno / turnip "
                + "drivers.");
    }

    // ------------------------------------------------------------------ Helpers

    private interface ValueListener {
        void onValue(float value);
    }

    private abstract static class SimpleWatcher implements TextWatcher {
        @Override
        public void beforeTextChanged(CharSequence s, int start, int count, int after) {
        }

        @Override
        public void onTextChanged(CharSequence s, int start, int before, int count) {
        }
    }

    private GradientDrawable rounded(int fill, int stroke, int radiusDp) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(fill);
        drawable.setStroke(dp(1), stroke);
        drawable.setCornerRadius(dp(radiusDp));
        return drawable;
    }

    private void section(int titleRes) {
        beginCard(getString(titleRes), null);
    }

    private void beginCard(String title, String hint) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackgroundResource(R.drawable.bg_card);
        card.setPadding(dp(16), dp(14), dp(16), dp(14));
        LinearLayout.LayoutParams params = matchWrap();
        params.topMargin = dp(12);
        container.addView(card, params);
        current = card;
        rowsInCard = 0;
        if (title != null) {
            TextView view = new TextView(this);
            view.setText(title.toUpperCase(Locale.US));
            view.setTextColor(COLOR_ACCENT);
            view.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            view.setTypeface(null, Typeface.BOLD);
            view.setLetterSpacing(0.1f);
            card.addView(view, matchWrap());
        }
        if (hint != null) {
            label(hint);
        }
    }

    private void rowGap() {
        if (rowsInCard++ > 0) {
            View divider = new View(this);
            divider.setBackgroundColor(0x1AFFFFFF);
            current.addView(divider, new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, 1));
        }
    }

    private TextView label(String text) {
        return labelInto(current, text);
    }

    private TextView labelInto(LinearLayout parent, String text) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextColor(COLOR_MUTED);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        view.setPadding(0, dp(4), 0, dp(4));
        parent.addView(view, matchWrap());
        return view;
    }

    private Switch switchRow(String title, final String key, boolean defaultValue) {
        return switchRow(title, null, key, defaultValue);
    }

    private Switch switchRow(String title, String hint, final String key,
                             boolean defaultValue) {
        rowGap();
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(10), 0, dp(10));

        LinearLayout texts = new LinearLayout(this);
        texts.setOrientation(LinearLayout.VERTICAL);
        TextView titleView = new TextView(this);
        titleView.setText(title);
        titleView.setTextColor(COLOR_TEXT);
        titleView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f);
        texts.addView(titleView, matchWrap());
        if (hint != null) {
            TextView hintView = new TextView(this);
            hintView.setText(hint);
            hintView.setTextColor(COLOR_MUTED);
            hintView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            texts.addView(hintView, matchWrap());
        }
        row.addView(texts, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        final Switch toggle = new Switch(this);
        int[][] states = {{android.R.attr.state_checked}, {}};
        toggle.setThumbTintList(new ColorStateList(states,
                new int[]{COLOR_ACCENT, 0xFFB0B8C2}));
        toggle.setTrackTintList(new ColorStateList(states,
                new int[]{0x6619C6E6, 0x44FFFFFF}));
        toggle.setChecked(prefs.getBoolean(key, defaultValue));
        toggle.setOnCheckedChangeListener(
                new android.widget.CompoundButton.OnCheckedChangeListener() {
                    @Override
                    public void onCheckedChanged(android.widget.CompoundButton buttonView,
                                                 boolean isChecked) {
                        prefs.edit().putBoolean(key, isChecked).apply();
                    }
                });
        row.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                toggle.toggle();
            }
        });
        row.addView(toggle, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        current.addView(row, matchWrap());
        return toggle;
    }

    private ArrayAdapter<String> styledAdapter(String[] items) {
        return new ArrayAdapter<String>(this, android.R.layout.simple_spinner_item, items) {
            @Override
            public View getView(int position, View convertView, ViewGroup parent) {
                return tint(super.getView(position, convertView, parent), false);
            }

            @Override
            public View getDropDownView(int position, View convertView, ViewGroup parent) {
                return tint(super.getDropDownView(position, convertView, parent), true);
            }
        };
    }

    private View tint(View view, boolean dropDown) {
        if (view instanceof TextView) {
            TextView text = (TextView) view;
            text.setTextColor(COLOR_TEXT);
            text.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f);
            text.setPadding(dp(12), dp(12), dp(12), dp(12));
            if (dropDown) {
                text.setBackgroundColor(COLOR_FIELD);
            }
        }
        return view;
    }

    private Spinner spinnerRow(String title, String[] items, int selection,
                               AdapterView.OnItemSelectedListener listener) {
        return spinnerRow(title, null, items, selection, listener);
    }

    private Spinner spinnerRow(String title, String hint, String[] items, int selection,
                               final AdapterView.OnItemSelectedListener listener) {
        rowGap();
        TextView titleView = new TextView(this);
        titleView.setText(title);
        titleView.setTextColor(COLOR_TEXT);
        titleView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f);
        titleView.setPadding(0, dp(10), 0, 0);
        current.addView(titleView, matchWrap());
        if (hint != null) {
            TextView hintView = new TextView(this);
            hintView.setText(hint);
            hintView.setTextColor(COLOR_MUTED);
            hintView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
            current.addView(hintView, matchWrap());
        }
        final Spinner spinner = new Spinner(this);
        spinner.setAdapter(styledAdapter(items));
        spinner.setSelection(selection);
        spinner.setBackground(rounded(COLOR_FIELD, 0x33FFFFFF, 10));
        spinner.setPopupBackgroundDrawable(new ColorDrawable(COLOR_FIELD));
        // Attach after the first layout: Spinner emits an initial onItemSelected
        // for the programmatic selection, which must not be treated as a user
        // change (it would silently overwrite persisted settings).
        spinner.post(new Runnable() {
            @Override
            public void run() {
                spinner.setOnItemSelectedListener(listener);
            }
        });
        LinearLayout.LayoutParams params = matchWrap();
        params.topMargin = dp(6);
        params.bottomMargin = dp(8);
        current.addView(spinner, params);
        return spinner;
    }

    private Spinner intSpinner(String title, String hint, String[] labels, final int[] values,
                               final String key, int defaultValue) {
        int stored = prefs.getInt(key, defaultValue);
        int selection = 0;
        for (int i = 0; i < values.length; i++) {
            if (values[i] == defaultValue) {
                selection = i;
            }
        }
        for (int i = 0; i < values.length; i++) {
            if (values[i] == stored) {
                selection = i;
            }
        }
        return intSpinnerAt(title, hint, labels, values, key, selection);
    }

    private Spinner intSpinnerAt(String title, String hint, String[] labels, final int[] values,
                                 final String key, int selection) {
        return spinnerRow(title, hint, labels, selection,
                new AdapterView.OnItemSelectedListener() {
                    @Override
                    public void onItemSelected(AdapterView<?> parent, View view, int position,
                                               long id) {
                        prefs.edit().putInt(key, values[position]).apply();
                    }

                    @Override
                    public void onNothingSelected(AdapterView<?> parent) {
                    }
                });
    }

    private Spinner stringSpinner(String title, String hint, String[] labels,
                                  final String[] values, final String key,
                                  String defaultValue) {
        String stored = prefs.getString(key, defaultValue);
        int selection = 0;
        for (int i = 0; i < values.length; i++) {
            if (values[i].equals(stored)) {
                selection = i;
            }
        }
        return spinnerRow(title, hint, labels, selection,
                new AdapterView.OnItemSelectedListener() {
                    @Override
                    public void onItemSelected(AdapterView<?> parent, View view, int position,
                                               long id) {
                        prefs.edit().putString(key, values[position]).apply();
                    }

                    @Override
                    public void onNothingSelected(AdapterView<?> parent) {
                    }
                });
    }

    private SeekBar seekRow(String title, float min, float max, float value,
                            final ValueListener listener) {
        rowGap();
        final TextView valueLabel = new TextView(this);
        valueLabel.setTextColor(COLOR_TEXT);
        valueLabel.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f);
        valueLabel.setPadding(0, dp(10), 0, 0);
        current.addView(valueLabel, matchWrap());
        final SeekBar bar = new SeekBar(this);
        bar.setMax(1000);
        bar.setProgressTintList(ColorStateList.valueOf(COLOR_ACCENT));
        bar.setThumbTintList(ColorStateList.valueOf(COLOR_ACCENT));
        final float range = max - min;
        bar.setProgress(Math.round((value - min) / range * 1000f));
        final String titleText = title;
        valueLabel.setText(titleText + ": " + String.format(Locale.US, "%.2f", value));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                float shown = min + range * progress / 1000f;
                valueLabel.setText(titleText + ": " + String.format(Locale.US, "%.2f", shown));
                if (fromUser) {
                    listener.onValue(shown);
                }
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
            }
        });
        LinearLayout.LayoutParams params = matchWrap();
        params.bottomMargin = dp(6);
        current.addView(bar, params);
        return bar;
    }

    private EditText createEditText(String hint, String value) {
        EditText edit = new EditText(this);
        edit.setHint(hint);
        edit.setText(value);
        edit.setTextColor(COLOR_TEXT);
        edit.setHintTextColor(COLOR_MUTED);
        edit.setBackground(rounded(COLOR_FIELD, 0x33FFFFFF, 10));
        edit.setPadding(dp(12), dp(12), dp(12), dp(12));
        edit.setInputType(InputType.TYPE_CLASS_NUMBER);
        return edit;
    }

    private void addField(EditText field) {
        LinearLayout.LayoutParams params = matchWrap();
        params.bottomMargin = dp(8);
        current.addView(field, params);
    }

    private Button button(String text, View.OnClickListener listener) {
        Button button = new Button(this);
        button.setText(text);
        button.setAllCaps(false);
        button.setTextColor(Color.WHITE);
        button.setBackgroundResource(R.drawable.bg_button_secondary);
        button.setOnClickListener(listener);
        LinearLayout.LayoutParams params = matchWrap();
        params.topMargin = dp(8);
        current.addView(button, params);
        return button;
    }

    private Button primaryButton(String text, View.OnClickListener listener) {
        Button button = button(text, listener);
        button.setBackgroundResource(R.drawable.bg_play);
        button.setTypeface(null, Typeface.BOLD);
        return button;
    }

    private LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
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
            pendingIsoUri = uri;
            confirmIsoCopy(uri);
        } else if (requestCode == StorageUtil.REQUEST_PICK_DRIVER) {
            pendingDriverUri = uri;
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
                        rebuildUi();
                        if (driver.isFullLoader) {
                            toast("Imported " + driver.label);
                        } else {
                            toast("Imported " + driver.label + " (Adreno driver, used on "
                                    + "next game start)");
                        }
                    }
                });
            }
        }).start();
    }
}
