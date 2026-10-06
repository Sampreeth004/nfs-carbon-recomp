package com.eagames.nfscarbon;

import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.RelativeLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.util.Locale;

public class GameActivity extends NfsCarbonActivity {
    private TouchControlsView touchControls;
    private Button layoutButton;
    private Button visibilityButton;
    private TextView fpsView;

    private final Handler fpsHandler = new Handler(Looper.getMainLooper());
    private final Runnable fpsTick = new Runnable() {
        @Override
        public void run() {
            if (fpsView == null) {
                return;
            }
            float fps = GameBridge.getGuestFps();
            if (fps > 0.5f) {
                fpsView.setText(String.format(Locale.US, "%.0f fps  %.1f ms  worst %.0f ms",
                        fps, GameBridge.getGuestFrameMs(), GameBridge.getGuestWorstMs()));
            } else {
                fpsView.setText("-- fps");
            }
            fpsHandler.postDelayed(this, 500);
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // The manifest already asks for sensor landscape, but SDL derives the
        // orientation from its window size at startup and can request portrait;
        // asking here is what actually sticks.
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
        super.onCreate(savedInstanceState);
        if (mBrokenLibraries || mLayout == null) {
            return;
        }
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        // The game never needs more than 60 Hz: asking the display for 60 Hz halves the
        // compositing work (and heat) on a 120 Hz panel.
        if (GameConfig.prefs(this).getInt(GameConfig.KEY_FPS_CAP, 60) != 0) {
            WindowManager.LayoutParams attributes = getWindow().getAttributes();
            attributes.preferredRefreshRate = 60.0f;
            getWindow().setAttributes(attributes);
        }
        createOverlay();
    }

    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        // SDL calls this when its window size suggests a different orientation.
        // The game is always landscape; the overlay and letterbox assume it.
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    @Override
    protected void onDestroy() {
        fpsHandler.removeCallbacks(fpsTick);
        GameBridge.setTouchPadEnabled(false);
        super.onDestroy();
    }

    private void createOverlay() {
        touchControls = new TouchControlsView(this);
        RelativeLayout.LayoutParams overlayParams = new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.MATCH_PARENT,
                RelativeLayout.LayoutParams.MATCH_PARENT);
        mLayout.addView(touchControls, overlayParams);

        layoutButton = createFloatingButton();
        layoutButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                touchControls.toggleLayout();
                updateLayoutButton();
                Toast.makeText(GameActivity.this,
                        "Touch layout: " + touchControls.getActiveLayout(),
                        Toast.LENGTH_SHORT).show();
            }
        });
        RelativeLayout.LayoutParams layoutParams = new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.WRAP_CONTENT, dp(34));
        layoutParams.addRule(RelativeLayout.ALIGN_PARENT_TOP);
        layoutParams.addRule(RelativeLayout.ALIGN_PARENT_LEFT);
        layoutParams.topMargin = dp(6);
        layoutParams.leftMargin = dp(6);
        mLayout.addView(layoutButton, layoutParams);

        visibilityButton = createFloatingButton();
        visibilityButton.setText("HIDE");
        visibilityButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                boolean hidden = touchControls.getVisibility() != View.VISIBLE;
                touchControls.setVisibility(hidden ? View.VISIBLE : View.GONE);
                touchControls.setPadActive(hidden);
                visibilityButton.setText(hidden ? "HIDE" : "SHOW");
            }
        });
        RelativeLayout.LayoutParams visibilityParams = new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.WRAP_CONTENT, dp(34));
        visibilityParams.addRule(RelativeLayout.ALIGN_PARENT_TOP);
        visibilityParams.addRule(RelativeLayout.RIGHT_OF, layoutButton.getId());
        visibilityParams.topMargin = dp(6);
        visibilityParams.leftMargin = dp(6);
        mLayout.addView(visibilityButton, visibilityParams);

        boolean touchEnabled = GameConfig.prefs(this)
                .getBoolean(GameConfig.KEY_TOUCH_ENABLED, true);
        touchControls.setVisibility(touchEnabled ? View.VISIBLE : View.GONE);
        touchControls.setPadActive(touchEnabled);
        visibilityButton.setText(touchEnabled ? "HIDE" : "SHOW");
        updateLayoutButton();

        fpsView = new TextView(this);
        fpsView.setText("-- fps");
        fpsView.setTextColor(Color.WHITE);
        fpsView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        fpsView.setTypeface(Typeface.MONOSPACE);
        fpsView.setBackgroundColor(0x66000000);
        fpsView.setPadding(dp(10), dp(3), dp(10), dp(3));
        RelativeLayout.LayoutParams fpsParams = new RelativeLayout.LayoutParams(
                RelativeLayout.LayoutParams.WRAP_CONTENT,
                RelativeLayout.LayoutParams.WRAP_CONTENT);
        fpsParams.addRule(RelativeLayout.ALIGN_PARENT_TOP);
        fpsParams.addRule(RelativeLayout.CENTER_HORIZONTAL);
        fpsParams.topMargin = dp(6);
        mLayout.addView(fpsView, fpsParams);
        if (GameConfig.prefs(this).getBoolean(GameConfig.KEY_SHOW_FPS, true)) {
            fpsHandler.post(fpsTick);
        } else {
            fpsView.setVisibility(View.GONE);
        }
    }

    private void updateLayoutButton() {
        layoutButton.setText(touchControls.getActiveLayout().toUpperCase());
    }

    private Button createFloatingButton() {
        Button button = new Button(this);
        button.setAllCaps(false);
        button.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f);
        button.setTextColor(Color.WHITE);
        button.setBackgroundColor(0x551F6FEB);
        button.setPadding(dp(10), 0, dp(10), 0);
        button.setMinWidth(0);
        button.setMinimumWidth(0);
        button.setMinHeight(0);
        button.setMinimumHeight(0);
        button.setAlpha(0.8f);
        button.setId(View.generateViewId());
        return button;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    @Override
    protected void onPause() {
        GameBridge.setGamePaused(true);
        super.onPause();
    }

    @Override
    protected void onResume() {
        super.onResume();
        GameBridge.setGamePaused(false);
    }
}
