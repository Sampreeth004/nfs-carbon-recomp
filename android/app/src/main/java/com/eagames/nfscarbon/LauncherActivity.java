package com.eagames.nfscarbon;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.os.Bundle;
import android.view.View;
import android.widget.TextView;

import java.io.File;
import java.util.Locale;

public class LauncherActivity extends Activity {
    public static final String EXTRA_OPEN_CONTROLS = "open_controls";

    private static final int COLOR_OK = 0xFF2ED47A;
    private static final int COLOR_WARN = 0xFFFFB020;
    private static final int COLOR_INFO = 0xFF19C6E6;

    private TextView playTitle;
    private TextView playSub;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_launcher);
        playTitle = findViewById(R.id.play_title);
        playSub = findViewById(R.id.play_sub);

        findViewById(R.id.button_play).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                play();
            }
        });
        findViewById(R.id.button_settings).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                startActivity(new Intent(LauncherActivity.this, SettingsActivity.class));
            }
        });
        findViewById(R.id.button_controls).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                Intent intent = new Intent(LauncherActivity.this, SettingsActivity.class);
                intent.putExtra(EXTRA_OPEN_CONTROLS, true);
                startActivity(intent);
            }
        });
        TextView version = findViewById(R.id.launcher_version);
        try {
            String name = getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
            version.setText("v" + name);
        } catch (Exception e) {
            version.setText("");
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshStatus();
    }

    private void play() {
        if (!GameConfig.isValidGameData(this)) {
            // Nothing to launch yet: take the player straight to game data setup.
            startActivity(new Intent(this, SettingsActivity.class));
            return;
        }
        GameConfig.writeToml(this);
        startActivity(new Intent(this, GameActivity.class));
    }

    private void bindCard(int cardId, String title, String value, int dotColor) {
        View card = findViewById(cardId);
        ((TextView) card.findViewById(R.id.status_title)).setText(title);
        ((TextView) card.findViewById(R.id.status_value)).setText(value);
        card.findViewById(R.id.status_dot).setBackgroundTintList(ColorStateList.valueOf(dotColor));
    }

    private void refreshStatus() {
        SharedPreferences prefs = GameConfig.prefs(this);

        boolean ready = GameConfig.isValidGameData(this);
        String isoPath = prefs.getString(GameConfig.KEY_ISO_PATH, "");
        String isoLabel = prefs.getString(GameConfig.KEY_ISO_LABEL, "");
        String game;
        if (isoPath != null && !isoPath.isEmpty()) {
            File file = new File(isoPath);
            String size = file.isFile()
                    ? String.format(Locale.US, "  (%.2f GB)", file.length() / 1073741824.0) : "";
            game = (isoLabel == null || isoLabel.isEmpty() ? file.getName() : isoLabel) + size;
        } else if (ready) {
            game = "Extracted game folder";
        } else {
            game = "Not set up yet";
        }
        bindCard(R.id.card_game, "Game data", game, ready ? COLOR_OK : COLOR_WARN);

        String renderer = prefs.getString(GameConfig.KEY_RENDERER, GameConfig.DEFAULT_RENDERER);
        boolean nativeRenderer = !"xenos".equals(renderer);
        bindCard(R.id.card_renderer, "Renderer",
                nativeRenderer ? "Native Vulkan" : "Xenos emulation (fallback)",
                nativeRenderer ? COLOR_OK : COLOR_INFO);

        String driver = prefs.getString(GameConfig.KEY_DRIVER_LOADER, "");
        boolean customDriver = driver != null && !driver.isEmpty();
        String driverName = "System Vulkan driver";
        if (customDriver) {
            File parent = new File(driver).getParentFile();
            driverName = parent != null ? parent.getName() : driver;
        }
        bindCard(R.id.card_driver, "GPU driver", driverName, customDriver ? COLOR_INFO : COLOR_OK);

        int width = prefs.getInt(GameConfig.KEY_WIDTH, GameConfig.DEFAULT_WIDTH);
        int height = prefs.getInt(GameConfig.KEY_HEIGHT, GameConfig.DEFAULT_HEIGHT);
        boolean vsync = prefs.getBoolean(GameConfig.KEY_VSYNC, true);
        bindCard(R.id.card_display, "Display", width + " x " + height + (vsync ? "  -  VSync on" : "  -  VSync off"),
                COLOR_OK);

        boolean touch = prefs.getBoolean(GameConfig.KEY_TOUCH_ENABLED, true);
        String layoutName = TouchLayout.LAYOUT_DRIVING.equals(TouchLayout.load(this).active)
                ? "Driving" : "Gamepad";
        bindCard(R.id.card_controls, "Controls",
                touch ? "Touch overlay  -  " + layoutName + " layout" : "Touch overlay off (controller)",
                touch ? COLOR_OK : COLOR_INFO);

        if (ready) {
            playTitle.setText(R.string.play);
            playSub.setText("Ready to race");
        } else {
            playTitle.setText("SET UP");
            playSub.setText("Choose your game data to get started");
        }
        playTitle.setTextColor(Color.WHITE);
    }
}
