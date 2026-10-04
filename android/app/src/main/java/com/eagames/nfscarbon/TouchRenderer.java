package com.eagames.nfscarbon;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;

import java.util.List;

/** Shared geometry and drawing for the live overlay and the layout editor. */
public final class TouchRenderer {
    public static final int COLOR_FILL = 0x30FFFFFF;
    public static final int COLOR_FILL_PRESSED = 0x99FFC107;
    public static final int COLOR_PLATE = 0x55FFFFFF;
    public static final int COLOR_KNOB = 0xAAFFFFFF;
    public static final int COLOR_STROKE = 0xCCFFFFFF;
    public static final int COLOR_TEXT = 0xFFFFFFFF;
    public static final int COLOR_SELECTED = 0xFFFF5252;
    public static final int COLOR_ACCENT = 0xCCFFC107;

    private TouchRenderer() {
    }

    public static RectF boundsOf(TouchLayout.Control control, float width, float height,
                                 float scale) {
        if ("wheel".equals(control.kind)) {
            float halfWidth = control.w * width * 0.5f;
            float halfHeight = control.h * height * 0.5f;
            return new RectF(control.x * width - halfWidth, control.y * height - halfHeight,
                    control.x * width + halfWidth, control.y * height + halfHeight);
        }
        float radius = control.size * Math.min(width, height) * scale;
        return new RectF(control.x * width - radius, control.y * height - radius,
                control.x * width + radius, control.y * height + radius);
    }

    public static TouchLayout.Control hitTest(List<TouchLayout.Control> controls, float width,
                                              float height, float scale, float x, float y) {
        for (int i = controls.size() - 1; i >= 0; i--) {
            TouchLayout.Control control = controls.get(i);
            if (boundsOf(control, width, height, scale).contains(x, y)) {
                return control;
            }
        }
        return null;
    }

    public static void drawControl(Canvas canvas, Paint paint, TouchLayout.Control control,
                                   RectF bounds, float alpha, boolean pressed, boolean selected,
                                   float knobX, float knobY) {
        drawControl(canvas, paint, control, bounds, alpha, pressed, selected, knobX, knobY, null);
    }

    public static void drawControl(Canvas canvas, Paint paint, TouchLayout.Control control,
                                   RectF bounds, float alpha, boolean pressed, boolean selected,
                                   float knobX, float knobY, Bitmap icon) {
        int fill = pressed ? COLOR_FILL_PRESSED : COLOR_FILL;
        int strokeColor = selected ? COLOR_SELECTED : COLOR_STROKE;
        float strokeWidth = dp(canvas, selected ? 3f : 2f);
        float radius = bounds.width() * 0.5f;
        float inset = dp(canvas, 1.5f);

        String kind = control.kind;
        boolean isStick = !("button".equals(kind) || "trigger".equals(kind) || "pedal".equals(kind)
                || "dpad".equals(kind) || "wheel".equals(kind));

        if (icon != null) {
            if (pressed && !isStick && !"wheel".equals(kind)) {
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(withAlpha(COLOR_FILL_PRESSED, alpha));
                canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius, paint);
            }
            if (selected) {
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(strokeWidth);
                paint.setColor(withAlpha(COLOR_SELECTED, alpha));
                canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius - inset, paint);
            }
            drawIcon(canvas, paint, icon, bounds, alpha);
            if ("wheel".equals(kind)) {
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(dp(canvas, 4f));
                paint.setColor(withAlpha(COLOR_ACCENT, alpha));
                float markerX = bounds.centerX() + knobX * bounds.width() * 0.42f;
                canvas.drawLine(markerX, bounds.centerY() - bounds.height() * 0.24f,
                        markerX, bounds.centerY() + bounds.height() * 0.24f, paint);
            } else if (isStick) {
                drawKnob(canvas, paint, bounds, alpha, pressed, strokeColor, knobX, knobY);
            }
            return;
        }

        if ("button".equals(kind) || "trigger".equals(kind) || "pedal".equals(kind)) {
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(withAlpha(fill, alpha));
            canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(strokeWidth);
            paint.setColor(withAlpha(strokeColor, alpha));
            canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius - inset, paint);
            drawLabel(canvas, paint, control.label, bounds.centerX(), bounds.centerY(),
                    radius * 0.7f, alpha);
        } else if ("dpad".equals(kind)) {
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(withAlpha(fill, alpha));
            canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius - inset, paint);

            float arm = radius * 0.30f;
            float edge = radius * 0.32f;
            paint.setColor(withAlpha(pressed ? COLOR_FILL_PRESSED : COLOR_PLATE, alpha));
            canvas.drawRoundRect(new RectF(bounds.centerX() - arm, bounds.top + edge,
                    bounds.centerX() + arm, bounds.bottom - edge),
                    arm * 0.5f, arm * 0.5f, paint);
            canvas.drawRoundRect(new RectF(bounds.left + edge, bounds.centerY() - arm,
                    bounds.right - edge, bounds.centerY() + arm),
                    arm * 0.5f, arm * 0.5f, paint);

            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(strokeWidth);
            paint.setColor(withAlpha(strokeColor, alpha));
            canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius - inset, paint);
        } else if ("wheel".equals(kind)) {
            float corner = bounds.height() * 0.5f;
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(withAlpha(fill, alpha));
            canvas.drawRoundRect(bounds, corner, corner, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(strokeWidth);
            paint.setColor(withAlpha(strokeColor, alpha));
            canvas.drawRoundRect(bounds, corner, corner, paint);
            paint.setStrokeWidth(dp(canvas, 4f));
            paint.setColor(withAlpha(COLOR_ACCENT, alpha));
            float markerX = bounds.centerX() + knobX * bounds.width() * 0.45f;
            canvas.drawLine(markerX, bounds.top + bounds.height() * 0.2f,
                    markerX, bounds.bottom - bounds.height() * 0.2f, paint);
        } else {
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(withAlpha(fill, alpha));
            canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius - inset, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(strokeWidth);
            paint.setColor(withAlpha(strokeColor, alpha));
            canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius - inset, paint);
            drawKnob(canvas, paint, bounds, alpha, pressed, strokeColor, knobX, knobY);
        }
    }

    private static void drawKnob(Canvas canvas, Paint paint, RectF bounds, float alpha,
                                 boolean pressed, int strokeColor, float knobX, float knobY) {
        float radius = bounds.width() * 0.5f;
        float inset = dp(canvas, 1.5f);
        float knobRadius = radius * 0.40f;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(withAlpha(pressed ? COLOR_FILL_PRESSED : COLOR_KNOB, alpha));
        float kx = bounds.centerX() + knobX * (radius - knobRadius - inset);
        float ky = bounds.centerY() + knobY * (radius - knobRadius - inset);
        canvas.drawCircle(kx, ky, knobRadius, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(canvas, 1.5f));
        paint.setColor(withAlpha(strokeColor, alpha));
        canvas.drawCircle(kx, ky, knobRadius, paint);
    }

    private static void drawIcon(Canvas canvas, Paint paint, Bitmap icon, RectF bounds,
                                 float alpha) {
        float inset = Math.min(bounds.width(), bounds.height()) * 0.05f;
        float availableWidth = Math.max(1f, bounds.width() - inset * 2f);
        float availableHeight = Math.max(1f, bounds.height() - inset * 2f);
        float scale = Math.min(availableWidth / icon.getWidth(), availableHeight / icon.getHeight());
        float width = icon.getWidth() * scale;
        float height = icon.getHeight() * scale;
        float left = bounds.centerX() - width * 0.5f;
        float top = bounds.centerY() - height * 0.5f;
        paint.setStyle(Paint.Style.FILL);
        paint.setFilterBitmap(true);
        paint.setAlpha(Math.round(255f * Math.max(0f, Math.min(1f, alpha))));
        canvas.drawBitmap(icon, null, new RectF(left, top, left + width, top + height), paint);
        paint.setAlpha(255);
        paint.setFilterBitmap(false);
    }

    public static void drawLabel(Canvas canvas, Paint paint, String label, float centerX,
                                 float centerY, float textSize, float alpha) {
        if (label == null || label.isEmpty()) {
            return;
        }
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(withAlpha(COLOR_TEXT, alpha));
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTextSize(textSize);
        Paint.FontMetrics metrics = paint.getFontMetrics();
        float baseline = centerY - (metrics.ascent + metrics.descent) * 0.5f;
        canvas.drawText(label, centerX, baseline, paint);
    }

    public static int withAlpha(int color, float alpha) {
        int a = Math.round(((color >>> 24) & 0xFF) * Math.max(0f, Math.min(1f, alpha)));
        return (color & 0x00FFFFFF) | (a << 24);
    }

    static float dp(Canvas canvas, float value) {
        return value * canvas.getDensity() / 160f;
    }
}
