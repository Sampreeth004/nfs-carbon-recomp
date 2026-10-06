package com.eagames.nfscarbon;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.ProgressDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.TypedValue;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
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
    private static final int COLOR_ACCENT = 0xFF58A6FF;
    private static final int COLOR_BUTTON = 0xFF1F6FEB;

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

    private LinearLayout container;
    private SharedPreferences prefs;
    private TouchLayout touchLayout;

    private TextView isoValue;
    private LinearLayout driverContainer;
    private TextView driverWarning;
    private boolean pendingIsoScan = false;
    private Uri pendingIsoUri;
    private Uri pendingDriverUri;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_settings);
        container = findViewById(R.id.settings_container);
        prefs = GameConfig.prefs(this);
        touchLayout = TouchLayout.load(this);
        buildGraphics();
        buildGameData();
        buildDriver();
        buildTouch();
        buildAbout();
        if (getIntent().getBooleanExtra(LauncherActivity.EXTRA_OPEN_CONTROLS, false)) {
            container.post(new Runnable() {
                @Override
                public void run() {
                    openLayoutEditor();
                }
            });
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        touchLayout.save(this);
        GameConfig.writeToml(this);
    }

    // ---------------------------------------------------------------- Graphics

    private void buildGraphics() {
        section(R.string.graphics);

        final String[] rendererLabels = {
                "Native Vulkan (default)", "Xenos emulation (fallback)"
        };
        final String[] rendererValues = {"carbon", "xenos"};
        String renderer = prefs.getString(GameConfig.KEY_RENDERER, GameConfig.DEFAULT_RENDERER);
        spinnerRow("Renderer", rendererLabels, "xenos".equals(renderer) ? 1 : 0,
                new AdapterView.OnItemSelectedListener() {
                    @Override
                    public void onItemSelected(AdapterView<?> parent, View view, int position,
                                               long id) {
                        prefs.edit().putString(GameConfig.KEY_RENDERER,
                                rendererValues[position]).apply();
                    }

                    @Override
                    public void onNothingSelected(AdapterView<?> parent) {
                    }
                });
        label("MSAA, single pass, occlusion and post-effect options apply to Xenos emulation "
                + "only.");

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

        spinnerRow("Guest resolution", labels, selection,
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

        container.addView(customWidth, matchWrap());
        container.addView(customHeight, matchWrap());

        customWidth.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable s) {
                int value = parseInt(s.toString(), GameConfig.DEFAULT_WIDTH, 640, 4095);
                prefs.edit().putInt(GameConfig.KEY_WIDTH, value).apply();
            }
        });
        customHeight.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable s) {
                int value = parseInt(s.toString(), GameConfig.DEFAULT_HEIGHT, 480, 4095);
                prefs.edit().putInt(GameConfig.KEY_HEIGHT, value).apply();
            }
        });

        switchRow("Vertical sync", GameConfig.KEY_VSYNC, true);
        switchRow("Bloom / glow (heavy: heats the phone, costs fps)", GameConfig.KEY_BLOOM, false);
        final String[] scaleLabels = {"100% (1280x720, sharpest)", "85% (1088x612)",
                "75% (960x540, lighter)", "60% (768x432)", "50% (640x360, lightest)"};
        final int[] scaleValues = {100, 85, 75, 60, 50};
        int scaleStored = prefs.getInt(GameConfig.KEY_RENDER_SCALE, 100);
        int scaleSelection = 0;
        for (int i = 0; i < scaleValues.length; i++) {
            if (scaleValues[i] == scaleStored) {
                scaleSelection = i;
            }
        }
        spinnerRow("Render resolution", scaleLabels, scaleSelection,
                new AdapterView.OnItemSelectedListener() {
                    @Override
                    public void onItemSelected(AdapterView<?> parent, View view, int position,
                                               long id) {
                        prefs.edit().putInt(GameConfig.KEY_RENDER_SCALE, scaleValues[position]).apply();
                    }

                    @Override
                    public void onNothingSelected(AdapterView<?> parent) {
                    }
                });
        final String[] capLabels = {"30 fps (coolest, steadiest)", "45 fps", "60 fps", "Unlimited"};
        final int[] capValues = {30, 45, 60, 0};
        int capSelection = 2;
        int storedCap = prefs.getInt(GameConfig.KEY_FPS_CAP, 60);
        for (int i = 0; i < capValues.length; i++) {
            if (capValues[i] == storedCap) {
                capSelection = i;
            }
        }
        spinnerRow("Frame rate cap", capLabels, capSelection,
                new AdapterView.OnItemSelectedListener() {
                    @Override
                    public void onItemSelected(AdapterView<?> parent, View view, int position,
                                               long id) {
                        prefs.edit().putInt(GameConfig.KEY_FPS_CAP, capValues[position]).apply();
                    }

                    @Override
                    public void onNothingSelected(AdapterView<?> parent) {
                    }
                });
        final String[] reflLabels = {"Car reflections: full (6 faces per frame)",
                "Car reflections: reduced (2 per frame)", "Car reflections: minimal (1 per frame)"};
        final int[] reflValues = {6, 2, 1};
        int reflStored = prefs.getInt(GameConfig.KEY_REFLECTIONS, 2);
        int reflSelection = reflStored >= 6 ? 0 : (reflStored >= 2 ? 1 : 2);
        spinnerRow("Car reflections", reflLabels, reflSelection,
                new AdapterView.OnItemSelectedListener() {
                    @Override
                    public void onItemSelected(AdapterView<?> parent, View view, int position,
                                               long id) {
                        prefs.edit().putInt(GameConfig.KEY_REFLECTIONS, reflValues[position]).apply();
                    }

                    @Override
                    public void onNothingSelected(AdapterView<?> parent) {
                    }
                });
        switchRow("Rear-view mirror at half rate (lighter)", GameConfig.KEY_MIRROR_HALF, true);
        switchRow("Letterbox presentation", GameConfig.KEY_LETTERBOX, true);
        switchRow("Occlusion queries (sun flares, lights)", GameConfig.KEY_OCCLUSION, true);
        switchRow("Single-pass scene (faster; requires MSAA off)",
                GameConfig.KEY_SINGLE_PASS, true);
        switchRow("Fine frame pacing (120 Hz timing, 60 fps cap)",
                GameConfig.KEY_FINE_PACING, true);
        switchRow("Show FPS counter on screen", GameConfig.KEY_SHOW_FPS, true);

        final int[] anisoValues = {3, 0, 2, 5, -1};
        final String[] anisoLabels = {
                "Force 4x (default)", "Off (fastest)", "2x", "16x", "No override"
        };
        int aniso = prefs.getInt(GameConfig.KEY_ANISO, 3);
        int anisoSelection = 0;
        for (int i = 0; i < anisoValues.length; i++) {
            if (anisoValues[i] == aniso) {
                anisoSelection = i;
            }
        }
        spinnerRow("Anisotropic filtering", anisoLabels, anisoSelection,
                new AdapterView.OnItemSelectedListener() {
                    @Override
                    public void onItemSelected(AdapterView<?> parent, View view, int position,
                                               long id) {
                        prefs.edit().putInt(GameConfig.KEY_ANISO, anisoValues[position]).apply();
                    }

                    @Override
                    public void onNothingSelected(AdapterView<?> parent) {
                    }
                });

        final int[] msaaValues = {0, 2, 4};
        final String[] msaaLabels = {"Off (fastest)", "MSAA 2x", "MSAA 4x (game default)"};
        int msaa = prefs.getInt(GameConfig.KEY_MSAA_SAMPLES, GameConfig.DEFAULT_MSAA_SAMPLES);
        int msaaSelection = msaa >= 4 ? 2 : (msaa >= 2 ? 1 : 0);
        spinnerRow("Anti-aliasing (MSAA)", msaaLabels, msaaSelection,
                new AdapterView.OnItemSelectedListener() {
                    @Override
                    public void onItemSelected(AdapterView<?> parent, View view, int position,
                                               long id) {
                        prefs.edit()
                                .putInt(GameConfig.KEY_MSAA_SAMPLES, msaaValues[position])
                                .apply();
                    }

                    @Override
                    public void onNothingSelected(AdapterView<?> parent) {
                    }
                });

        final String[] fxaaLabels = {"None", "FXAA", "FXAA Extreme"};
        final String[] fxaaValues = {"none", "fxaa", "fxaa_extreme"};
        String fxaa = prefs.getString(GameConfig.KEY_FXAA, GameConfig.DEFAULT_FXAA);
        int fxaaSelection = 0;
        for (int i = 0; i < fxaaValues.length; i++) {
            if (fxaaValues[i].equals(fxaa)) {
                fxaaSelection = i;
            }
        }
        spinnerRow("Anti-aliasing (post effect)", fxaaLabels, fxaaSelection,
                new AdapterView.OnItemSelectedListener() {
                    @Override
                    public void onItemSelected(AdapterView<?> parent, View view, int position,
                                               long id) {
                        prefs.edit().putString(GameConfig.KEY_FXAA,
                                fxaaValues[position]).apply();
                    }

                    @Override
                    public void onNothingSelected(AdapterView<?> parent) {
                    }
                });

        label("Upscaler: Bilinear (this SDK build has no FidelityFX support)");
    }

    // --------------------------------------------------------------- Game data

    private void buildGameData() {
        section(R.string.game_data);
        isoValue = label("");
        updateIsoLabel();

        button("Pick ISO file (copies into app storage)", new View.OnClickListener() {
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

        label("Copy mode is the safe default: the ISO lives at "
                + new File(getFilesDir(), "game.iso").getAbsolutePath()
                + ". Direct paths skip the copy but some devices cannot memory-map large "
                + "files from shared storage.");
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

    private void buildDriver() {
        section(R.string.gpu_driver);
        driverContainer = new LinearLayout(this);
        driverContainer.setOrientation(LinearLayout.VERTICAL);
        container.addView(driverContainer, matchWrap());
        populateDrivers();
        button("Import driver (.so or .zip)", new View.OnClickListener() {
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
        button("Remove selected driver", new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                removeSelectedDriver();
            }
        });
        label("Only full Vulkan loader packages are supported (a libvulkan.so that exports "
                + "vkGetInstanceProcAddr, for example a Winlator-style turnip build). ICD-only "
                + "packages with a driver JSON are not supported. Imported drivers are copied "
                + "into app storage.");
    }

    private void populateDrivers() {
        driverContainer.removeAllViews();
        final List<DriverStore.Driver> drivers = DriverStore.list(this);
        List<String> names = new ArrayList<>();
        names.add("System Vulkan");
        for (DriverStore.Driver driver : drivers) {
            names.add(driver.label);
        }
        String selected = prefs.getString(GameConfig.KEY_DRIVER_LOADER, "");
        int selection = 0;
        for (int i = 0; i < drivers.size(); i++) {
            if (drivers.get(i).loader.getAbsolutePath().equals(selected)) {
                selection = i + 1;
            }
        }

        TextView title = new TextView(this);
        title.setText("Vulkan driver");
        title.setTextColor(COLOR_MUTED);
        title.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        driverContainer.addView(title, matchWrap());

        Spinner spinner = new Spinner(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, names);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinner.setAdapter(adapter);
        spinner.setSelection(selection);
        spinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                if (position == 0) {
                    prefs.edit().putString(GameConfig.KEY_DRIVER_LOADER, "").apply();
                } else {
                    DriverStore.Driver driver = drivers.get(position - 1);
                    prefs.edit()
                            .putString(GameConfig.KEY_DRIVER_LOADER,
                                    driver.loader.getAbsolutePath())
                            .apply();
                }
                showDriverWarning(drivers, position);
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {
            }
        });
        driverContainer.addView(spinner, matchWrap());
        showDriverWarning(drivers, selection);
    }

    private void showDriverWarning(List<DriverStore.Driver> drivers, int position) {
        TextView previous = driverWarning;
        if (previous != null) {
            driverContainer.removeView(previous);
            driverWarning = null;
        }
        if (position <= 0 || position > drivers.size()) {
            return;
        }
        DriverStore.Driver driver = drivers.get(position - 1);
        if (driver.isFullLoader) {
            return;
        }
        TextView warning = new TextView(this);
        warning.setText("This package contains an ICD/HAL driver, not a full Vulkan loader, "
                + "so it cannot be used on Android (the OS only loads vendor ICDs from "
                + "/vendor). The game will fall back to the system Vulkan driver. Import a "
                + "package that includes a full libvulkan.so loader instead.");
        warning.setTextColor(0xFFFFB74D);
        warning.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        warning.setPadding(0, dp(4), 0, dp(4));
        driverContainer.addView(warning, matchWrap());
        driverWarning = warning;
    }

    private void removeSelectedDriver() {
        String selected = prefs.getString(GameConfig.KEY_DRIVER_LOADER, "");
        if (selected == null || selected.isEmpty()) {
            toast("System driver cannot be removed");
            return;
        }
        for (DriverStore.Driver driver : DriverStore.list(this)) {
            if (driver.loader.getAbsolutePath().equals(selected)) {
                DriverStore.remove(driver);
                prefs.edit().putString(GameConfig.KEY_DRIVER_LOADER, "").apply();
                populateDrivers();
                toast("Driver removed");
                return;
            }
        }
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
        section(R.string.about);
        label("NFS Carbon Android launcher for the ReXGlue recompilation.\n"
                + "Game data: Need for Speed: Carbon (Xbox 360) ISO or extracted folder.\n"
                + "Rendering: native Vulkan renderer (carbon GPU plugin), or Xenos emulation "
                + "(xenos GPU plugin) as a fallback.");
    }

    // ------------------------------------------------------------------ Helpers

    private void rebuildUi() {
        container.removeAllViews();
        buildGraphics();
        buildGameData();
        buildDriver();
        buildTouch();
        buildAbout();
    }

    private interface ValueListener {
        void onValue(float value);
    }

    private TextView section(int titleRes) {
        TextView view = new TextView(this);
        view.setText(titleRes);
        view.setTextColor(COLOR_ACCENT);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f);
        view.setPadding(0, dp(22), 0, dp(6));
        container.addView(view, matchWrap());
        return view;
    }

    private TextView label(String text) {
        return labelInto(container, text);
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

    private android.widget.CheckBox switchRow(String title, final String key,
                                              boolean defaultValue) {
        android.widget.CheckBox view = new android.widget.CheckBox(this);
        view.setText(title);
        view.setTextColor(COLOR_TEXT);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f);
        view.setPadding(0, dp(10), 0, dp(10));
        view.setButtonTintList(android.content.res.ColorStateList.valueOf(0xFF58A6FF));
        view.setChecked(prefs.getBoolean(key, defaultValue));
        view.setOnCheckedChangeListener(new android.widget.CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(android.widget.CompoundButton buttonView,
                                         boolean isChecked) {
                prefs.edit().putBoolean(key, isChecked).apply();
            }
        });
        container.addView(view, matchWrap());
        return view;
    }

    private Spinner spinnerRow(String title, String[] items, int selection,
                               final AdapterView.OnItemSelectedListener listener) {
        label(title);
        final Spinner spinner = new Spinner(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_item, items);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinner.setAdapter(adapter);
        spinner.setSelection(selection);
        // Attach after the first layout: Spinner emits an initial onItemSelected
        // for the programmatic selection, which must not be treated as a user
        // change (it would silently overwrite persisted settings).
        spinner.post(new Runnable() {
            @Override
            public void run() {
                spinner.setOnItemSelectedListener(listener);
            }
        });
        container.addView(spinner, matchWrap());
        return spinner;
    }

    private SeekBar seekRow(String title, float min, float max, float value,
                            final ValueListener listener) {
        final TextView valueLabel = label("");
        final SeekBar bar = new SeekBar(this);
        bar.setMax(1000);
        final float range = max - min;
        bar.setProgress(Math.round((value - min) / range * 1000f));
        final String titleText = title;
        valueLabel.setText(titleText + ": " + String.format(Locale.US, "%.2f", value));
        bar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                float current = min + range * progress / 1000f;
                valueLabel.setText(titleText + ": " + String.format(Locale.US, "%.2f", current));
                if (fromUser) {
                    listener.onValue(current);
                }
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
            }
        });
        container.addView(bar, matchWrap());
        return bar;
    }

    private EditText createEditText(String hint, String value) {
        EditText edit = new EditText(this);
        edit.setHint(hint);
        edit.setText(value);
        edit.setTextColor(COLOR_TEXT);
        edit.setHintTextColor(COLOR_MUTED);
        edit.setBackgroundColor(0xFF161B22);
        edit.setPadding(dp(12), dp(12), dp(12), dp(12));
        edit.setInputType(InputType.TYPE_CLASS_NUMBER);
        return edit;
    }

    private Button button(String text, View.OnClickListener listener) {
        Button button = new Button(this);
        button.setText(text);
        button.setAllCaps(false);
        button.setTextColor(Color.WHITE);
        button.setBackgroundColor(COLOR_BUTTON);
        button.setOnClickListener(listener);
        LinearLayout.LayoutParams params = matchWrap();
        params.topMargin = dp(6);
        container.addView(button, params);
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
                        populateDrivers();
                        if (driver.isFullLoader) {
                            toast("Imported " + driver.label);
                        } else {
                            toast("Imported " + driver.label + " - but this is an ICD/HAL "
                                    + "package, not a full Vulkan loader; it cannot be used "
                                    + "on Android and the system driver will be used instead.");
                        }
                    }
                });
            }
        }).start();
    }
}
