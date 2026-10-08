package com.eagames.nfscarbon;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.RadialGradient;
import android.graphics.Rect;
import android.graphics.Shader;
import android.graphics.drawable.Drawable;

/** Night-city backdrop: dark gradient, two colour glows and angled light streaks. */
public final class SpeedBackground extends Drawable {
    // Streaks as (start x, y, length, thickness, colour) in fractions of the width.
    private static final float[][] STREAKS = {
            {0.05f, 0.18f, 0.55f, 0.0016f}, {0.30f, 0.30f, 0.80f, 0.0011f},
            {0.55f, 0.12f, 0.45f, 0.0020f}, {0.10f, 0.62f, 0.70f, 0.0012f},
            {0.45f, 0.78f, 0.60f, 0.0018f}, {0.70f, 0.48f, 0.40f, 0.0010f},
    };
    private static final int[] STREAK_COLORS = {
            0x5519C6E6, 0x336A3DFF, 0x4419C6E6, 0x2E6A3DFF, 0x3D19C6E6, 0x2AFFFFFF
    };

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);

    @Override
    public void draw(Canvas canvas) {
        Rect b = getBounds();
        float w = b.width();
        float h = b.height();
        paint.setShader(new LinearGradient(0, 0, w, h,
                new int[]{0xFF05070C, 0xFF09111F, 0xFF140C26}, null, Shader.TileMode.CLAMP));
        canvas.drawRect(b, paint);
        // Glows.
        float g = Math.max(w, h);
        paint.setShader(new RadialGradient(w * 0.08f, h * 0.05f, g * 0.55f, 0x3319C6E6,
                0x0019C6E6, Shader.TileMode.CLAMP));
        canvas.drawRect(b, paint);
        paint.setShader(new RadialGradient(w * 0.95f, h * 1.0f, g * 0.6f, 0x336A3DFF,
                0x006A3DFF, Shader.TileMode.CLAMP));
        canvas.drawRect(b, paint);
        // Streaks, all at the same shallow angle.
        canvas.save();
        canvas.rotate(-14f, w / 2f, h / 2f);
        for (int i = 0; i < STREAKS.length; i++) {
            float[] s = STREAKS[i];
            float x0 = s[0] * w - w * 0.2f;
            float y = s[1] * h;
            float x1 = x0 + s[2] * w;
            int c = STREAK_COLORS[i];
            paint.setShader(new LinearGradient(x0, 0, x1, 0, new int[]{c & 0x00FFFFFF, c,
                    c & 0x00FFFFFF}, null, Shader.TileMode.CLAMP));
            float t = Math.max(1f, s[3] * w);
            canvas.drawRect(x0, y - t, x1, y + t, paint);
        }
        canvas.restore();
        paint.setShader(null);
    }

    @Override
    public void setAlpha(int alpha) {
        paint.setAlpha(alpha);
    }

    @Override
    public void setColorFilter(ColorFilter colorFilter) {
        paint.setColorFilter(colorFilter);
    }

    @Override
    public int getOpacity() {
        return PixelFormat.OPAQUE;
    }
}
