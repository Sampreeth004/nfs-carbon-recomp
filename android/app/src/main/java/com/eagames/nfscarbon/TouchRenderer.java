package com.eagames.nfscarbon;

import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;

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
    private static final int COLOR_BACKDROP = 0x66000000;

    private static final int TINT_A = 0xFF4CAF50;
    private static final int TINT_B = 0xFFE53935;
    private static final int TINT_X = 0xFF1E88E5;
    private static final int TINT_Y = 0xFFFDD835;

    private static final Path scratchPath = new Path();

    private TouchRenderer() {
    }

    /** Shoulder, trigger and menu buttons are drawn as horizontal pills. */
    public static boolean isPill(TouchLayout.Control control) {
        switch (control.id) {
            case "lb":
            case "rb":
            case "lt":
            case "rt":
            case "start":
            case "back":
                return true;
            default:
                return false;
        }
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
        if (isPill(control)) {
            float halfWidth = radius * ("start".equals(control.id) || "back".equals(control.id)
                    ? 1.5f : 1.8f);
            float halfHeight = radius * 0.8f;
            return new RectF(control.x * width - halfWidth, control.y * height - halfHeight,
                    control.x * width + halfWidth, control.y * height + halfHeight);
        }
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
        int strokeColor = selected ? COLOR_SELECTED : COLOR_STROKE;
        float strokeWidth = dp(canvas, selected ? 3f : 2f);
        float radius = bounds.width() * 0.5f;
        float inset = dp(canvas, 1.5f);

        String kind = control.kind;
        boolean isStick = !("button".equals(kind) || "trigger".equals(kind) || "pedal".equals(kind)
                || "dpad".equals(kind) || "wheel".equals(kind));

        if (icon != null) {
            if (!isStick && !"wheel".equals(kind)) {
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(withAlpha(pressed ? COLOR_FILL_PRESSED : COLOR_BACKDROP, alpha));
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

        if (isPill(control)) {
            drawPill(canvas, paint, control, bounds, alpha, pressed, strokeColor, strokeWidth);
        } else if ("button".equals(kind) || "trigger".equals(kind)) {
            drawRoundButton(canvas, paint, control, bounds, alpha, pressed, strokeColor,
                    strokeWidth);
        } else if ("pedal".equals(kind)) {
            float corner = radius * 0.5f;
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(withAlpha(COLOR_BACKDROP, alpha));
            canvas.drawRoundRect(bounds, corner, corner, paint);
            paint.setColor(withAlpha(pressed ? COLOR_FILL_PRESSED : COLOR_FILL, alpha));
            canvas.drawRoundRect(bounds, corner, corner, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(strokeWidth);
            paint.setColor(withAlpha(strokeColor, alpha));
            canvas.drawRoundRect(bounds, corner, corner, paint);
            drawLabel(canvas, paint, control.label, bounds.centerX(), bounds.centerY(),
                    radius * 0.6f, alpha);
        } else if ("dpad".equals(kind)) {
            drawDpad(canvas, paint, bounds, alpha, pressed, strokeColor, strokeWidth);
        } else if ("wheel".equals(kind)) {
            float corner = bounds.height() * 0.5f;
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(withAlpha(COLOR_BACKDROP, alpha));
            canvas.drawRoundRect(bounds, corner, corner, paint);
            paint.setColor(withAlpha(pressed ? COLOR_FILL_PRESSED : COLOR_FILL, alpha));
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
            // Analog stick: dark base, outer and inner ring, floating knob.
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(withAlpha(COLOR_BACKDROP, alpha));
            canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius - inset, paint);
            paint.setColor(withAlpha(COLOR_FILL, alpha));
            canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius - inset, paint);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(strokeWidth);
            paint.setColor(withAlpha(strokeColor, alpha));
            canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius - inset, paint);
            paint.setStrokeWidth(dp(canvas, 1f));
            paint.setColor(withAlpha(COLOR_PLATE, alpha));
            canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius * 0.55f, paint);
            drawKnob(canvas, paint, bounds, alpha, pressed, strokeColor, knobX, knobY);
        }
    }

    private static int faceTint(TouchLayout.Control control) {
        switch (control.id) {
            case "a":
                return TINT_A;
            case "b":
                return TINT_B;
            case "x":
                return TINT_X;
            case "y":
                return TINT_Y;
            default:
                return 0;
        }
    }

    private static int withChannelAlpha(int color, int a) {
        return (color & 0x00FFFFFF) | ((a & 0xFF) << 24);
    }

    private static void drawRoundButton(Canvas canvas, Paint paint, TouchLayout.Control control,
                                        RectF bounds, float alpha, boolean pressed,
                                        int strokeColor, float strokeWidth) {
        float radius = bounds.width() * 0.5f * (pressed ? 1.06f : 1f);
        float inset = dp(canvas, 1.5f);
        int tint = faceTint(control);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(withAlpha(COLOR_BACKDROP, alpha));
        canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius, paint);
        int fill;
        int stroke = strokeColor;
        if (tint != 0) {
            fill = withChannelAlpha(tint, pressed ? 0xDD : 0x55);
            if (strokeColor != COLOR_SELECTED) {
                stroke = withChannelAlpha(tint, 0xE6);
            }
        } else {
            fill = pressed ? COLOR_FILL_PRESSED : COLOR_FILL;
        }
        paint.setColor(withAlpha(fill, alpha));
        canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(strokeWidth);
        paint.setColor(withAlpha(stroke, alpha));
        canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius - inset, paint);
        drawLabel(canvas, paint, control.label, bounds.centerX(), bounds.centerY(),
                radius * 0.8f, alpha);
    }

    private static void drawPill(Canvas canvas, Paint paint, TouchLayout.Control control,
                                 RectF bounds, float alpha, boolean pressed, int strokeColor,
                                 float strokeWidth) {
        float corner = bounds.height() * 0.5f;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(withAlpha(COLOR_BACKDROP, alpha));
        canvas.drawRoundRect(bounds, corner, corner, paint);
        paint.setColor(withAlpha(pressed ? COLOR_FILL_PRESSED : COLOR_FILL, alpha));
        canvas.drawRoundRect(bounds, corner, corner, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(strokeWidth);
        paint.setColor(withAlpha(strokeColor, alpha));
        canvas.drawRoundRect(bounds, corner, corner, paint);
        drawLabel(canvas, paint, control.label, bounds.centerX(), bounds.centerY(),
                bounds.height() * 0.46f, alpha);
    }

    private static void drawDpad(Canvas canvas, Paint paint, RectF bounds, float alpha,
                                 boolean pressed, int strokeColor, float strokeWidth) {
        float radius = bounds.width() * 0.5f;
        float inset = dp(canvas, 1.5f);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(withAlpha(COLOR_BACKDROP, alpha));
        canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius - inset, paint);
        paint.setColor(withAlpha(COLOR_FILL, alpha));
        canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius - inset, paint);

        float arm = radius * 0.30f;
        float edge = radius * 0.22f;
        paint.setColor(withAlpha(pressed ? COLOR_FILL_PRESSED : COLOR_PLATE, alpha));
        canvas.drawRoundRect(new RectF(bounds.centerX() - arm, bounds.top + edge,
                bounds.centerX() + arm, bounds.bottom - edge), arm * 0.5f, arm * 0.5f, paint);
        canvas.drawRoundRect(new RectF(bounds.left + edge, bounds.centerY() - arm,
                bounds.right - edge, bounds.centerY() + arm), arm * 0.5f, arm * 0.5f, paint);

        // Direction arrows.
        paint.setColor(withAlpha(COLOR_TEXT, alpha));
        float tip = radius * 0.78f;
        float base = radius * 0.52f;
        float half = radius * 0.13f;
        float cx = bounds.centerX(), cy = bounds.centerY();
        for (int d = 0; d < 4; d++) {
            float dx = d == 2 ? -1 : (d == 3 ? 1 : 0);
            float dy = d == 0 ? -1 : (d == 1 ? 1 : 0);
            scratchPath.reset();
            scratchPath.moveTo(cx + dx * tip, cy + dy * tip);
            scratchPath.lineTo(cx + dx * base - dy * half, cy + dy * base + dx * half);
            scratchPath.lineTo(cx + dx * base + dy * half, cy + dy * base - dx * half);
            scratchPath.close();
            canvas.drawPath(scratchPath, paint);
        }

        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(strokeWidth);
        paint.setColor(withAlpha(strokeColor, alpha));
        canvas.drawCircle(bounds.centerX(), bounds.centerY(), radius - inset, paint);
    }

    private static void drawKnob(Canvas canvas, Paint paint, RectF bounds, float alpha,
                                 boolean pressed, int strokeColor, float knobX, float knobY) {
        float radius = bounds.width() * 0.5f;
        float inset = dp(canvas, 1.5f);
        float knobRadius = radius * 0.40f;
        float kx = bounds.centerX() + knobX * (radius - knobRadius - inset);
        float ky = bounds.centerY() + knobY * (radius - knobRadius - inset);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(withAlpha(COLOR_BACKDROP, alpha));
        canvas.drawCircle(kx, ky + dp(canvas, 2f), knobRadius, paint);
        paint.setColor(withAlpha(pressed ? COLOR_FILL_PRESSED : COLOR_KNOB, alpha));
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
        paint.setTypeface(Typeface.DEFAULT_BOLD);
        Paint.FontMetrics metrics = paint.getFontMetrics();
        float baseline = centerY - (metrics.ascent + metrics.descent) * 0.5f;
        canvas.drawText(label, centerX, baseline, paint);
        paint.setTypeface(Typeface.DEFAULT);
    }

    public static int withAlpha(int color, float alpha) {
        int a = Math.round(((color >>> 24) & 0xFF) * Math.max(0f, Math.min(1f, alpha)));
        return (color & 0x00FFFFFF) | (a << 24);
    }

    static float dp(Canvas canvas, float value) {
        return value * canvas.getDensity() / 160f;
    }
}
