package com.eagames.nfscarbon;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;

import java.util.List;

/**
 * Geometry and drawing of the on-screen controls, shared by the live overlay and
 * the layout editor. Everything is drawn from shapes; there are no bitmaps.
 */
public final class TouchRenderer {
    // Palette: dark glass with a thin light edge, cyan when pressed.
    private static final int GLASS = 0x8C0B1118;
    private static final int GLASS_PRESSED = 0xB319C6E6;
    private static final int EDGE = 0xB3FFFFFF;
    private static final int EDGE_SOFT = 0x40FFFFFF;
    private static final int ACCENT = 0xFF19C6E6;
    private static final int TEXT = 0xFFFFFFFF;
    private static final int SELECTED = 0xFFFF5252;
    private static final int GAS = 0xFF2ED47A;
    private static final int BRAKE = 0xFFFF5A5A;

    private static final int FACE_A = 0xFF4CD964;
    private static final int FACE_B = 0xFFFF4D4D;
    private static final int FACE_X = 0xFF3D9BFF;
    private static final int FACE_Y = 0xFFFFD233;

    private static final Path path = new Path();
    private static final RectF scratch = new RectF();

    private TouchRenderer() {
    }

    /** Shoulder, trigger and menu buttons are horizontal pills. */
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
        float cx = control.x * width;
        float cy = control.y * height;
        if ("wheel".equals(control.kind)) {
            float hw = control.w * width * 0.5f;
            float hh = control.h * height * 0.5f;
            return new RectF(cx - hw, cy - hh, cx + hw, cy + hh);
        }
        float r = control.size * Math.min(width, height) * scale;
        if ("pedal".equals(control.kind)) {
            return new RectF(cx - r * 0.72f, cy - r * 1.25f, cx + r * 0.72f, cy + r * 1.25f);
        }
        if ("steer".equals(control.kind)) {
            return new RectF(cx - r * 0.95f, cy - r * 1.05f, cx + r * 0.95f, cy + r * 1.05f);
        }
        if (isPill(control)) {
            float hw = r * ("start".equals(control.id) || "back".equals(control.id) ? 1.3f : 1.75f);
            float hh = r * 0.78f;
            return new RectF(cx - hw, cy - hh, cx + hw, cy + hh);
        }
        return new RectF(cx - r, cy - r, cx + r, cy + r);
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

    /**
     * knobX/knobY: stick deflection (-1..1), steering position (knobX), or pedal and
     * trigger pressure (knobY, 0..1).
     */
    public static void drawControl(Canvas canvas, Paint paint, TouchLayout.Control control,
                                   RectF bounds, float alpha, boolean pressed, boolean selected,
                                   float knobX, float knobY) {
        String kind = control.kind;
        if ("steer".equals(kind)) {
            drawSteerArrow(canvas, paint, control, bounds, alpha, pressed, selected);
        } else if ("wheel".equals(kind)) {
            drawSteering(canvas, paint, bounds, alpha, pressed, selected, knobX);
        } else if ("pedal".equals(kind)) {
            drawPedal(canvas, paint, control, bounds, alpha, pressed, selected, knobY);
        } else if ("dpad".equals(kind)) {
            drawDpad(canvas, paint, bounds, alpha, pressed, selected);
        } else if ("stick".equals(kind)) {
            drawStick(canvas, paint, bounds, alpha, pressed, selected, knobX, knobY);
        } else if (isPill(control)) {
            drawPill(canvas, paint, control, bounds, alpha, pressed, selected, knobY);
        } else {
            drawRound(canvas, paint, control, bounds, alpha, pressed, selected);
        }
    }

    // ------------------------------------------------------------------ pieces

    private static void glass(Canvas canvas, Paint paint, RectF r, float corner, float alpha,
                              boolean pressed, boolean selected, int edgeColor) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(withAlpha(pressed ? GLASS_PRESSED : GLASS, alpha));
        canvas.drawRoundRect(r, corner, corner, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(canvas, selected ? 3f : 1.5f));
        paint.setColor(withAlpha(selected ? SELECTED : edgeColor, alpha));
        float inset = paint.getStrokeWidth() * 0.5f;
        scratch.set(r.left + inset, r.top + inset, r.right - inset, r.bottom - inset);
        canvas.drawRoundRect(scratch, corner, corner, paint);
    }

    private static void drawRound(Canvas canvas, Paint paint, TouchLayout.Control control,
                                  RectF b, float alpha, boolean pressed, boolean selected) {
        int face = faceColor(control.id);
        float r = b.width() * 0.5f;
        float cx = b.centerX(), cy = b.centerY();
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(withAlpha(pressed ? (face != 0 ? withAlpha(face, 0.85f) : GLASS_PRESSED)
                : GLASS, alpha));
        canvas.drawCircle(cx, cy, r, paint);
        paint.setStyle(Paint.Style.STROKE);
        float ring = dp(canvas, selected ? 3f : (face != 0 ? 2.5f : 1.5f));
        paint.setStrokeWidth(ring);
        paint.setColor(withAlpha(selected ? SELECTED : (face != 0 ? face : EDGE), alpha));
        canvas.drawCircle(cx, cy, r - ring * 0.5f, paint);
        int textColor = pressed || face == 0 ? TEXT : face;
        drawText(canvas, paint, control.label, cx, cy, r * (control.label.length() > 1 ? 0.5f : 0.9f),
                textColor, alpha, r * 1.5f);
    }

    private static void drawPill(Canvas canvas, Paint paint, TouchLayout.Control control, RectF b,
                                 float alpha, boolean pressed, boolean selected, float pressure) {
        float corner = b.height() * 0.5f;
        glass(canvas, paint, b, corner, alpha, pressed, selected, EDGE);
        if ("lt".equals(control.id) || "rt".equals(control.id)) {
            // Analog triggers: a fill bar along the bottom.
            if (pressure > 0f) {
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(withAlpha(ACCENT, alpha));
                float inset = dp(canvas, 4f);
                float w = (b.width() - inset * 2f) * Math.min(1f, pressure);
                canvas.drawRoundRect(new RectF(b.left + inset, b.bottom - inset - dp(canvas, 3f),
                        b.left + inset + w, b.bottom - inset), dp(canvas, 2f), dp(canvas, 2f), paint);
            }
        }
        float cx = b.centerX(), cy = b.centerY();
        if ("start".equals(control.id) && control.label.isEmpty()) {
            // Three lines: menu.
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeCap(Paint.Cap.ROUND);
            paint.setStrokeWidth(dp(canvas, 2f));
            paint.setColor(withAlpha(TEXT, alpha));
            float hw = b.height() * 0.24f, gap = b.height() * 0.16f;
            for (int i = -1; i <= 1; i++) {
                canvas.drawLine(cx - hw, cy + i * gap, cx + hw, cy + i * gap, paint);
            }
            paint.setStrokeCap(Paint.Cap.BUTT);
        } else if ("back".equals(control.id) && control.label.isEmpty()) {
            // Two overlapping windows: view.
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(dp(canvas, 1.8f));
            paint.setColor(withAlpha(TEXT, alpha));
            float s = b.height() * 0.22f, o = b.height() * 0.08f;
            canvas.drawRoundRect(new RectF(cx - s - o, cy - s + o, cx + s - o, cy + s + o),
                    dp(canvas, 2f), dp(canvas, 2f), paint);
            canvas.drawRoundRect(new RectF(cx - s + o, cy - s - o, cx + s + o, cy + s - o),
                    dp(canvas, 2f), dp(canvas, 2f), paint);
        } else {
            drawText(canvas, paint, control.label, cx, cy, b.height() * 0.42f, TEXT, alpha,
                    b.width() * 0.8f);
        }
    }

    private static void drawStick(Canvas canvas, Paint paint, RectF b, float alpha,
                                  boolean pressed, boolean selected, float kx, float ky) {
        float r = b.width() * 0.5f;
        float cx = b.centerX(), cy = b.centerY();
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(withAlpha(GLASS, alpha));
        canvas.drawCircle(cx, cy, r, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(canvas, selected ? 3f : 1.5f));
        paint.setColor(withAlpha(selected ? SELECTED : EDGE, alpha));
        canvas.drawCircle(cx, cy, r - dp(canvas, 1f), paint);
        paint.setStrokeWidth(dp(canvas, 1f));
        paint.setColor(withAlpha(EDGE_SOFT, alpha));
        canvas.drawCircle(cx, cy, r * 0.62f, paint);
        // Direction ticks.
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(withAlpha(EDGE_SOFT, alpha));
        for (int d = 0; d < 4; d++) {
            triangle(canvas, paint, cx, cy, d, r * 0.88f, r * 0.76f, r * 0.07f);
        }
        float knob = r * 0.42f;
        float px = cx + kx * (r - knob), py = cy + ky * (r - knob);
        paint.setColor(withAlpha(0x66000000, alpha));
        canvas.drawCircle(px, py + dp(canvas, 2f), knob, paint);
        paint.setColor(withAlpha(pressed ? ACCENT : 0xE6E8EEF4, alpha));
        canvas.drawCircle(px, py, knob, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(canvas, 1f));
        paint.setColor(withAlpha(0x33000000, alpha));
        canvas.drawCircle(px, py, knob * 0.7f, paint);
    }

    private static void drawDpad(Canvas canvas, Paint paint, RectF b, float alpha,
                                 boolean pressed, boolean selected) {
        float cx = b.centerX(), cy = b.centerY();
        float r = b.width() * 0.5f;
        float arm = r * 0.36f;
        path.reset();
        path.addRoundRect(new RectF(cx - arm, b.top, cx + arm, b.bottom), arm * 0.45f, arm * 0.45f,
                Path.Direction.CW);
        Path horizontal = new Path();
        horizontal.addRoundRect(new RectF(b.left, cy - arm, b.right, cy + arm), arm * 0.45f,
                arm * 0.45f, Path.Direction.CW);
        path.op(horizontal, Path.Op.UNION);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(withAlpha(pressed ? GLASS_PRESSED : GLASS, alpha));
        canvas.drawPath(path, paint);
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(canvas, selected ? 3f : 1.5f));
        paint.setColor(withAlpha(selected ? SELECTED : EDGE, alpha));
        canvas.drawPath(path, paint);
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(withAlpha(TEXT, alpha));
        for (int d = 0; d < 4; d++) {
            triangle(canvas, paint, cx, cy, d, r * 0.82f, r * 0.58f, r * 0.15f);
        }
    }

    private static void drawSteering(Canvas canvas, Paint paint, RectF b, float alpha,
                                     boolean pressed, boolean selected, float position) {
        float corner = Math.min(b.width(), b.height()) * 0.22f;
        glass(canvas, paint, b, corner, alpha, false, selected, EDGE);
        float cx = b.centerX(), cy = b.centerY();
        float half = b.width() * 0.5f;
        // Centre notch.
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(dp(canvas, 1.5f));
        paint.setColor(withAlpha(EDGE_SOFT, alpha));
        canvas.drawLine(cx, b.top + b.height() * 0.22f, cx, b.bottom - b.height() * 0.22f, paint);
        // Chevrons on each side, brighter towards the edge and in the steered direction.
        float chev = Math.min(b.height() * 0.16f, half * 0.12f);
        for (int side = -1; side <= 1; side += 2) {
            for (int i = 0; i < 3; i++) {
                float x = cx + side * half * (0.30f + i * 0.20f);
                float strength = 0.35f + i * 0.2f;
                if (position * side > 0f) {
                    strength = Math.min(1f, strength + Math.abs(position) * 0.6f);
                }
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeCap(Paint.Cap.ROUND);
                paint.setStrokeJoin(Paint.Join.ROUND);
                paint.setStrokeWidth(dp(canvas, 3f));
                paint.setColor(withAlpha(position * side > 0.05f ? ACCENT : TEXT, alpha * strength));
                path.reset();
                path.moveTo(x - side * chev * 0.6f, cy - chev);
                path.lineTo(x + side * chev * 0.6f, cy);
                path.lineTo(x - side * chev * 0.6f, cy + chev);
                canvas.drawPath(path, paint);
            }
        }
        paint.setStrokeCap(Paint.Cap.BUTT);
        // Steering position marker.
        float mx = cx + position * half * 0.9f;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(withAlpha(pressed ? ACCENT : 0xCCFFFFFF, alpha));
        float mw = dp(canvas, 5f);
        canvas.drawRoundRect(new RectF(mx - mw, b.top + b.height() * 0.14f, mx + mw,
                b.bottom - b.height() * 0.14f), mw, mw, paint);
    }

    private static void drawSteerArrow(Canvas canvas, Paint paint, TouchLayout.Control control,
                                       RectF b, float alpha, boolean pressed, boolean selected) {
        float corner = Math.min(b.width(), b.height()) * 0.24f;
        glass(canvas, paint, b, corner, alpha, pressed, selected, EDGE);
        int side = "steer_left".equals(control.id) ? -1 : 1;
        float cx = b.centerX(), cy = b.centerY();
        float w = b.width() * 0.22f, h = b.height() * 0.24f;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(withAlpha(TEXT, alpha));
        path.reset();
        path.moveTo(cx + side * w, cy);
        path.lineTo(cx - side * w * 0.55f, cy - h);
        path.lineTo(cx - side * w * 0.55f, cy + h);
        path.close();
        canvas.drawPath(path, paint);
    }

    private static void drawPedal(Canvas canvas, Paint paint, TouchLayout.Control control,
                                  RectF b, float alpha, boolean pressed, boolean selected,
                                  float pressure) {
        boolean gas = "rt".equals(control.axis);
        int color = gas ? GAS : BRAKE;
        float corner = b.width() * 0.22f;
        glass(canvas, paint, b, corner, alpha, false, selected, withAlpha(color, 0.9f));
        // Pressure fill from the bottom.
        if (pressure > 0f) {
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(withAlpha(withAlpha(color, 0.55f), alpha));
            float top = b.bottom - b.height() * Math.min(1f, pressure);
            canvas.save();
            path.reset();
            path.addRoundRect(b, corner, corner, Path.Direction.CW);
            canvas.clipPath(path);
            canvas.drawRect(b.left, top, b.right, b.bottom, paint);
            canvas.restore();
        }
        // Grip ridges.
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeCap(Paint.Cap.ROUND);
        paint.setStrokeWidth(dp(canvas, 2f));
        paint.setColor(withAlpha(pressed ? TEXT : EDGE_SOFT, alpha));
        float inset = b.width() * 0.26f;
        for (int i = 0; i < 5; i++) {
            float y = b.top + b.height() * (0.16f + i * 0.11f);
            canvas.drawLine(b.left + inset, y, b.right - inset, y, paint);
        }
        paint.setStrokeCap(Paint.Cap.BUTT);
        drawText(canvas, paint, control.label, b.centerX(), b.bottom - b.height() * 0.17f,
                b.width() * 0.26f, pressed ? TEXT : color, alpha);
    }

    // ----------------------------------------------------------------- helpers

    private static int faceColor(String id) {
        switch (id) {
            case "a":
                return FACE_A;
            case "b":
                return FACE_B;
            case "x":
                return FACE_X;
            case "y":
                return FACE_Y;
            default:
                return 0;
        }
    }

    /** d: 0 up, 1 down, 2 left, 3 right. */
    private static void triangle(Canvas canvas, Paint paint, float cx, float cy, int d, float tip,
                                 float base, float half) {
        float dx = d == 2 ? -1 : (d == 3 ? 1 : 0);
        float dy = d == 0 ? -1 : (d == 1 ? 1 : 0);
        path.reset();
        path.moveTo(cx + dx * tip, cy + dy * tip);
        path.lineTo(cx + dx * base - dy * half, cy + dy * base + dx * half);
        path.lineTo(cx + dx * base + dy * half, cy + dy * base - dx * half);
        path.close();
        canvas.drawPath(path, paint);
    }

    private static void drawText(Canvas canvas, Paint paint, String text, float cx, float cy,
                                 float size, int color, float alpha) {
        drawText(canvas, paint, text, cx, cy, size, color, alpha, 0f);
    }

    private static void drawText(Canvas canvas, Paint paint, String text, float cx, float cy,
                                 float size, int color, float alpha, float maxWidth) {
        if (text == null || text.isEmpty()) {
            return;
        }
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(withAlpha(color, alpha));
        paint.setTextAlign(Paint.Align.CENTER);
        paint.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));
        paint.setTextSize(size);
        if (maxWidth > 0f) {
            float w = paint.measureText(text);
            if (w > maxWidth) {
                paint.setTextSize(size * maxWidth / w);
            }
        }
        Paint.FontMetrics m = paint.getFontMetrics();
        canvas.drawText(text, cx, cy - (m.ascent + m.descent) * 0.5f, paint);
        paint.setTypeface(Typeface.DEFAULT);
    }

    public static int withAlpha(int color, float alpha) {
        int a = Math.round(((color >>> 24) & 0xFF) * Math.max(0f, Math.min(1f, alpha)));
        return (color & 0x00FFFFFF) | (a << 24);
    }

    static float dp(Canvas canvas, float value) {
        // Hardware-accelerated canvases report no density; use the display's.
        return value * android.content.res.Resources.getSystem().getDisplayMetrics().density;
    }
}
