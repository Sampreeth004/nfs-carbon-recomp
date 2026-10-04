package com.eagames.nfscarbon;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;

import java.io.File;

public class LauncherActivity extends Activity {
    private TextView statusGame;
    private TextView statusDriver;
    private TextView statusGraphics;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_launcher);
        statusGame = findViewById(R.id.status_game);
        statusDriver = findViewById(R.id.status_driver);
        statusGraphics = findViewById(R.id.status_graphics);
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
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshStatus();
    }

    private void play() {
        if (!GameConfig.isValidGameData(this)) {
            new AlertDialog.Builder(this)
                    .setTitle(R.string.game_data)
                    .setMessage("No game data is configured yet. Pick your Need for Speed: "
                            + "Carbon ISO (or an extracted game folder) in Settings first.")
                    .setPositiveButton(R.string.settings, new DialogInterface.OnClickListener() {
                        @Override
                        public void onClick(DialogInterface dialog, int which) {
                            startActivity(new Intent(LauncherActivity.this,
                                    SettingsActivity.class));
                        }
                    })
                    .setNegativeButton(android.R.string.cancel, null)
                    .show();
            return;
        }
        GameConfig.writeToml(this);
        startActivity(new Intent(this, GameActivity.class));
    }

    private void refreshStatus() {
        SharedPreferences prefs = GameConfig.prefs(this);
        String isoPath = prefs.getString(GameConfig.KEY_ISO_PATH, "");
        String isoLabel = prefs.getString(GameConfig.KEY_ISO_LABEL, "");
        if (isoPath != null && !isoPath.isEmpty()) {
            File file = new File(isoPath);
            String size = file.isFile() ? String.format(" (%.2f GB)", file.length() / 1073741824.0) : "";
            statusGame.setText("Game data: " + (isoLabel == null || isoLabel.isEmpty()
                    ? file.getName() : isoLabel) + size);
        } else if (GameConfig.isValidGameData(this)) {
            statusGame.setText("Game data: extracted folder");
        } else {
            statusGame.setText("Game data: not configured");
        }

        String driver = prefs.getString(GameConfig.KEY_DRIVER_LOADER, "");
        statusDriver.setText("GPU driver: " + (driver == null || driver.isEmpty()
                ? "System Vulkan" : new File(driver).getParentFile().getName()));

        int width = prefs.getInt(GameConfig.KEY_WIDTH, GameConfig.DEFAULT_WIDTH);
        int height = prefs.getInt(GameConfig.KEY_HEIGHT, GameConfig.DEFAULT_HEIGHT);
        boolean vsync = prefs.getBoolean(GameConfig.KEY_VSYNC, true);
        statusGraphics.setText("Graphics: " + width + "x" + height
                + (vsync ? " | VSync" : ""));
    }
}
