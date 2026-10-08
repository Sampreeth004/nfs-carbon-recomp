package com.eagames.nfscarbon;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.CompoundButton;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.util.Locale;

/**
 * Home screen: one big PLAY button, the status of everything the game needs (each
 * tile opens its settings page), and the settings people change most often.
 */
public class LauncherActivity extends Activity {
    private SharedPreferences prefs;
    private boolean landscape;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = GameConfig.prefs(this);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Rebuilt every time: settings, drivers and game data may have changed.
        landscape = getResources().getConfiguration().orientation
                == Configuration.ORIENTATION_LANDSCAPE;
        setContentView(build());
    }

    private void play() {
        if (!GameConfig.isValidGameData(this)) {
            openSettings(SettingsActivity.PAGE_GAME_DATA);
            return;
        }
        GameConfig.writeToml(this);
        startActivity(new Intent(this, GameActivity.class));
    }

    private void openSettings(int page) {
        Intent intent = new Intent(this, SettingsActivity.class);
        intent.putExtra(SettingsActivity.EXTRA_PAGE, page);
        startActivity(intent);
    }

    // -------------------------------------------------------------------- Layout

    private View build() {
        FrameLayout root = new FrameLayout(this);
        root.setBackground(new SpeedBackground());
        if (landscape) {
            LinearLayout row = Ui.horizontal(this);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(dp(36), dp(12), dp(20), dp(12));
            LinearLayout left = Ui.vertical(this);
            left.setGravity(Gravity.CENTER_VERTICAL);
            buildHero(left);
            row.addView(left, new LinearLayout.LayoutParams(0,
                    ViewGroup.LayoutParams.MATCH_PARENT, 1f));

            ScrollView rightScroll = new ScrollView(this);
            rightScroll.setVerticalScrollBarEnabled(false);
            rightScroll.setFillViewport(true);
            LinearLayout right = Ui.vertical(this);
            right.setGravity(Gravity.CENTER_VERTICAL);
            right.setPadding(dp(16), dp(8), dp(16), dp(8));
            buildStatus(right);
            buildQuick(right);
            rightScroll.addView(right, Ui.matchWrap());
            row.addView(rightScroll, new LinearLayout.LayoutParams(0,
                    ViewGroup.LayoutParams.MATCH_PARENT, 1.15f));
            root.addView(row, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.MATCH_PARENT));
        } else {
            ScrollView scroll = new ScrollView(this);
            scroll.setFillViewport(true);
            LinearLayout column = Ui.vertical(this);
            column.setPadding(dp(22), dp(40), dp(22), dp(24));
            buildHero(column);
            buildStatus(column);
            buildQuick(column);
            scroll.addView(column, Ui.matchWrap());
            root.addView(scroll);
        }
        return root;
    }

    private void buildHero(LinearLayout parent) {
        TextView nfs = Ui.text(this, "NEED FOR SPEED", 13f, Ui.ACCENT);
        nfs.setTypeface(Ui.CONDENSED);
        nfs.setLetterSpacing(0.34f);
        parent.addView(nfs, Ui.matchWrap());
        TextView carbon = Ui.text(this, "CARBON", landscape ? 58f : 52f, Color.WHITE);
        carbon.setTypeface(Ui.CONDENSED_ITALIC);
        carbon.setLetterSpacing(0.04f);
        carbon.setShadowLayer(dp(14), 0, 0, 0x6619C6E6);
        carbon.setIncludeFontPadding(false);
        LinearLayout.LayoutParams cp = Ui.matchWrap();
        cp.topMargin = -dp(2);
        parent.addView(carbon, cp);
        TextView sub = Ui.text(this, "Xbox 360 recompilation  ·  native Vulkan renderer", 12.5f,
                Ui.MUTED);
        LinearLayout.LayoutParams sp = Ui.matchWrap();
        sp.topMargin = dp(4);
        parent.addView(sub, sp);

        // PLAY
        boolean ready = GameConfig.isValidGameData(this);
        LinearLayout playButton = Ui.horizontal(this);
        playButton.setGravity(Gravity.CENTER_VERTICAL);
        playButton.setPadding(dp(18), 0, dp(22), 0);
        playButton.setBackground(Ui.ripple(Ui.gradient(this, 18, 0xFF12B5DB, 0xFF5B44F2),
                0x66FFFFFF));
        playButton.setElevation(dp(6));
        playButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                play();
            }
        });
        FrameLayout disc = new FrameLayout(this);
        disc.setBackground(Ui.rounded(this, 0x33FFFFFF, 0, 22));
        ImageView glyph = new ImageView(this);
        glyph.setImageDrawable(new Icons(ready ? Icons.PLAY : Icons.DISC, Color.WHITE));
        disc.addView(glyph, new FrameLayout.LayoutParams(dp(24), dp(24), Gravity.CENTER));
        playButton.addView(disc, new LinearLayout.LayoutParams(dp(44), dp(44)));
        LinearLayout playTexts = Ui.vertical(this);
        playTexts.setPadding(dp(14), 0, 0, 0);
        TextView playTitle = Ui.text(this, ready ? "PLAY" : "SET UP", 26f, Color.WHITE);
        playTitle.setTypeface(Ui.CONDENSED_ITALIC);
        playTitle.setLetterSpacing(0.16f);
        playTitle.setIncludeFontPadding(false);
        playTexts.addView(playTitle, Ui.wrap());
        playTexts.addView(Ui.text(this, ready ? "Ready to race" : "Choose your game data first",
                12.5f, 0xDDFFFFFF), Ui.wrap());
        playButton.addView(playTexts, Ui.weight(1));
        ImageView chevron = Ui.icon(this, Icons.CHEVRON, 0xCCFFFFFF, 22);
        playButton.addView(chevron);
        LinearLayout.LayoutParams pp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(72));
        pp.topMargin = dp(landscape ? 18 : 24);
        parent.addView(playButton, pp);

        LinearLayout buttons = Ui.horizontal(this);
        buttons.addView(iconButton(Icons.TUNE, "Settings", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                openSettings(SettingsActivity.PAGE_PERFORMANCE);
            }
        }), Ui.weight(1));
        View spacer = new View(this);
        buttons.addView(spacer, new LinearLayout.LayoutParams(dp(10), 1));
        buttons.addView(iconButton(Icons.GAMEPAD, "Edit controls", new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                startActivity(new Intent(LauncherActivity.this, ControlsEditorActivity.class));
            }
        }), Ui.weight(1));
        LinearLayout.LayoutParams bp = Ui.matchWrap();
        bp.topMargin = dp(10);
        parent.addView(buttons, bp);

        String version = "";
        try {
            version = "v" + getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception ignored) {
        }
        TextView versionView = Ui.text(this, version, 11f, Ui.FAINT);
        versionView.setPadding(dp(2), dp(12), 0, landscape ? 0 : dp(8));
        parent.addView(versionView, Ui.matchWrap());
    }

    private View iconButton(int glyph, String label, View.OnClickListener l) {
        LinearLayout b = Ui.horizontal(this);
        b.setGravity(Gravity.CENTER);
        b.setPadding(dp(12), dp(13), dp(12), dp(13));
        b.setBackground(Ui.ripple(Ui.rounded(this, 0xCC172234, 0x33FFFFFF, 14), 0x33FFFFFF));
        b.addView(Ui.icon(this, glyph, Ui.TEXT, 20));
        TextView t = Ui.text(this, label, 14.5f, Ui.TEXT);
        t.setTypeface(Ui.MEDIUM);
        LinearLayout.LayoutParams tp = Ui.wrap();
        tp.leftMargin = dp(10);
        b.addView(t, tp);
        b.setOnClickListener(l);
        return b;
    }

    // -------------------------------------------------------------------- Status

    private void buildStatus(LinearLayout parent) {
        TextView label = Ui.overline(this, "Status");
        label.setPadding(dp(4), dp(landscape ? 4 : 22), 0, dp(8));
        parent.addView(label, Ui.matchWrap());

        boolean ready = GameConfig.isValidGameData(this);
        String isoPath = prefs.getString(GameConfig.KEY_ISO_PATH, "");
        String isoLabel = prefs.getString(GameConfig.KEY_ISO_LABEL, "");
        String game;
        if (isoPath != null && !isoPath.isEmpty()) {
            File file = new File(isoPath);
            game = isoLabel == null || isoLabel.isEmpty() ? file.getName() : isoLabel;
            if (file.isFile()) {
                game += String.format(Locale.US, "  ·  %.1f GB", file.length() / 1073741824.0);
            }
        } else {
            game = ready ? "Extracted game folder" : "Tap to choose your ISO";
        }

        boolean nativeRenderer = !"xenos".equals(prefs.getString(GameConfig.KEY_RENDERER,
                GameConfig.DEFAULT_RENDERER));
        String driver = prefs.getString(GameConfig.KEY_DRIVER_LOADER, "");
        boolean customDriver = driver != null && !driver.isEmpty();
        String driverName = "System driver";
        if (customDriver) {
            File parentDir = new File(driver).getParentFile();
            driverName = parentDir != null ? parentDir.getName() : driver;
        }

        int preset = GameConfig.detectPreset(prefs);
        int cap = prefs.getInt(GameConfig.KEY_FPS_CAP, 60);
        int scale = prefs.getInt(GameConfig.KEY_RENDER_SCALE, 100);
        String perf = (preset >= 0 ? GameConfig.PRESET_NAMES[preset] : "Custom") + "  ·  "
                + (cap == 0 ? "no cap" : cap + " fps") + "  ·  " + scale + "%";

        boolean touch = prefs.getBoolean(GameConfig.KEY_TOUCH_ENABLED, true);
        String layoutName = TouchLayout.LAYOUT_XBOX.equals(TouchLayout.load(this).active)
                ? "Gamepad" : "Driving";
        String controls = touch ? "Touch  ·  " + layoutName + " layout" : "Controller only";

        View gameTile = tile(Icons.DISC, "Game data", game, ready ? Ui.OK : Ui.WARN,
                SettingsActivity.PAGE_GAME_DATA);
        parent.addView(gameTile, Ui.matchWrap());
        View renderer = tile(Icons.DISPLAY, "Renderer", nativeRenderer ? "Native Vulkan" : "Xenos",
                nativeRenderer ? Ui.OK : Ui.ACCENT, SettingsActivity.PAGE_DISPLAY);
        View gpu = tile(Icons.CHIP, "GPU driver", driverName,
                customDriver ? Ui.ACCENT : Ui.OK, SettingsActivity.PAGE_DRIVER);
        View performance = tile(Icons.GAUGE, "Performance", perf, Ui.OK,
                SettingsActivity.PAGE_PERFORMANCE);
        View input = tile(Icons.GAMEPAD, "Controls", controls, touch ? Ui.OK : Ui.ACCENT,
                SettingsActivity.PAGE_CONTROLS);
        if (landscape) {
            // Two columns beside the hero; long values (performance) get a full row.
            LinearLayout row1 = Ui.horizontal(this);
            row1.addView(renderer, Ui.weight(1));
            addGap(row1);
            row1.addView(gpu, Ui.weight(1));
            parent.addView(row1, gapParams());
            parent.addView(performance, gapParams());
            parent.addView(input, gapParams());
        } else {
            for (View v : new View[]{renderer, gpu, performance, input}) {
                parent.addView(v, gapParams());
            }
        }
    }

    private View tile(int glyph, String title, String value, int color, final int page) {
        LinearLayout t = Ui.horizontal(this);
        t.setPadding(dp(12), dp(10), dp(10), dp(10));
        t.setBackground(Ui.ripple(Ui.rounded(this, Ui.SURFACE, Ui.STROKE, 14), 0x33FFFFFF));
        FrameLayout badge = new FrameLayout(this);
        badge.setBackground(Ui.rounded(this, (color & 0x00FFFFFF) | 0x22000000, 0, 10));
        ImageView icon = new ImageView(this);
        icon.setImageDrawable(new Icons(glyph, color));
        badge.addView(icon, new FrameLayout.LayoutParams(dp(20), dp(20), Gravity.CENTER));
        t.addView(badge, new LinearLayout.LayoutParams(dp(34), dp(34)));
        LinearLayout texts = Ui.vertical(this);
        texts.setPadding(dp(10), 0, 0, 0);
        TextView titleView = Ui.text(this, title.toUpperCase(Locale.US), 10.5f, Ui.MUTED);
        titleView.setTypeface(Ui.CONDENSED);
        titleView.setLetterSpacing(0.1f);
        titleView.setSingleLine(true);
        texts.addView(titleView, Ui.matchWrap());
        TextView valueView = Ui.text(this, value, 13.5f, Ui.TEXT);
        valueView.setSingleLine(true);
        valueView.setEllipsize(android.text.TextUtils.TruncateAt.END);
        texts.addView(valueView, Ui.matchWrap());
        t.addView(texts, Ui.weight(1));
        t.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                openSettings(page);
            }
        });
        return t;
    }

    private void addGap(LinearLayout row) {
        row.addView(new View(this), new LinearLayout.LayoutParams(dp(8), 1));
    }

    private LinearLayout.LayoutParams gapParams() {
        LinearLayout.LayoutParams p = Ui.matchWrap();
        p.topMargin = dp(8);
        return p;
    }

    // ------------------------------------------------------------ Quick settings

    private void buildQuick(LinearLayout parent) {
        TextView label = Ui.overline(this, "Quick settings");
        label.setPadding(dp(4), dp(16), 0, dp(8));
        parent.addView(label, Ui.matchWrap());
        LinearLayout card = Ui.card(this);
        card.setPadding(dp(14), dp(4), dp(14), dp(4));
        parent.addView(card, Ui.matchWrap());

        final int preset = GameConfig.detectPreset(prefs);
        Ui.segmentedRow(this, card, "Performance preset",
                preset < 0 ? "Custom settings. Pick a preset to replace them." : null,
                new String[]{"Battery", "Balanced", "Quality"}, preset, new Ui.IntChoice() {
                    @Override
                    public void onChoice(int index) {
                        GameConfig.applyPreset(prefs, index);
                        setContentView(build());
                    }
                });
        card.addView(Ui.divider(this));
        int cap = prefs.getInt(GameConfig.KEY_FPS_CAP, 60);
        final int[] caps = {30, 45, 60, 0};
        int capIndex = -1;
        for (int i = 0; i < caps.length; i++) {
            if (caps[i] == cap) capIndex = i;
        }
        Ui.segmentedRow(this, card, "Frame rate cap", null,
                new String[]{"30", "45", "60", "Unlimited"}, capIndex, new Ui.IntChoice() {
                    @Override
                    public void onChoice(int index) {
                        prefs.edit().putInt(GameConfig.KEY_FPS_CAP, caps[index]).apply();
                        setContentView(build());
                    }
                });
        card.addView(Ui.divider(this));
        Ui.switchRow(this, card, "Touch controls", null,
                prefs.getBoolean(GameConfig.KEY_TOUCH_ENABLED, true),
                new CompoundButton.OnCheckedChangeListener() {
                    @Override
                    public void onCheckedChanged(CompoundButton b, boolean checked) {
                        prefs.edit().putBoolean(GameConfig.KEY_TOUCH_ENABLED, checked).apply();
                    }
                });
        card.addView(Ui.divider(this));
        Ui.switchRow(this, card, "Frame rate counter", null,
                prefs.getBoolean(GameConfig.KEY_SHOW_FPS, true),
                new CompoundButton.OnCheckedChangeListener() {
                    @Override
                    public void onCheckedChanged(CompoundButton b, boolean checked) {
                        prefs.edit().putBoolean(GameConfig.KEY_SHOW_FPS, checked).apply();
                    }
                });
    }

    private int dp(float value) {
        return Ui.dp(this, value);
    }
}
