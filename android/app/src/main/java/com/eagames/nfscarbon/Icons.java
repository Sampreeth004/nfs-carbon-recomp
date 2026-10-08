package com.eagames.nfscarbon;

import android.graphics.Canvas;
import android.graphics.ColorFilter;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.RectF;
import android.graphics.drawable.Drawable;

/** Line icons drawn on a 24-unit grid, so no image assets are needed. */
public final class Icons extends Drawable {
    public static final int PLAY = 0;
    public static final int GAUGE = 1;
    public static final int DISPLAY = 2;
    public static final int GAMEPAD = 3;
    public static final int CHIP = 4;
    public static final int DISC = 5;
    public static final int TUNE = 6;
    public static final int INFO = 7;
    public static final int BACK = 8;
    public static final int CHECK = 9;
    public static final int UNDO = 10;
    public static final int GRID = 11;
    public static final int RESET = 12;
    public static final int CLOSE = 13;
    public static final int PLUS = 14;
    public static final int MINUS = 15;
    public static final int EDIT = 16;
    public static final int TRASH = 17;
    public static final int CHEVRON = 18;
    public static final int LAYERS = 19;

    private final int glyph;
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final RectF r = new RectF();

    public Icons(int glyph, int color) {
        this.glyph = glyph;
        stroke.setStyle(Paint.Style.STROKE);
        stroke.setStrokeCap(Paint.Cap.ROUND);
        stroke.setStrokeJoin(Paint.Join.ROUND);
        stroke.setColor(color);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(color);
    }

    @Override
    public void draw(Canvas canvas) {
        float size = Math.min(getBounds().width(), getBounds().height());
        float s = size / 24f;
        canvas.save();
        canvas.translate(getBounds().centerX() - size / 2f, getBounds().centerY() - size / 2f);
        canvas.scale(s, s);
        stroke.setStrokeWidth(1.9f);
        path.reset();
        switch (glyph) {
            case PLAY:
                path.moveTo(8, 5);
                path.lineTo(19, 12);
                path.lineTo(8, 19);
                path.close();
                canvas.drawPath(path, fill);
                break;
            case GAUGE:
                r.set(3, 5, 21, 23);
                canvas.drawArc(r, 160, 220, false, stroke);
                canvas.drawLine(12, 14, 16.5f, 9.5f, stroke);
                canvas.drawCircle(12, 14, 1.6f, fill);
                break;
            case DISPLAY:
                r.set(3, 4, 21, 16);
                canvas.drawRoundRect(r, 2, 2, stroke);
                canvas.drawLine(9, 20, 15, 20, stroke);
                canvas.drawLine(12, 16, 12, 20, stroke);
                break;
            case GAMEPAD:
                r.set(2.5f, 7, 21.5f, 18);
                canvas.drawRoundRect(r, 5.5f, 5.5f, stroke);
                canvas.drawLine(7.5f, 10.5f, 7.5f, 14.5f, stroke);
                canvas.drawLine(5.5f, 12.5f, 9.5f, 12.5f, stroke);
                canvas.drawCircle(15.5f, 11.3f, 1.1f, fill);
                canvas.drawCircle(17.7f, 13.7f, 1.1f, fill);
                break;
            case CHIP:
                r.set(6, 6, 18, 18);
                canvas.drawRoundRect(r, 2, 2, stroke);
                r.set(9.5f, 9.5f, 14.5f, 14.5f);
                canvas.drawRect(r, stroke);
                for (int i = 0; i < 3; i++) {
                    float p = 9 + i * 3;
                    canvas.drawLine(p, 3, p, 6, stroke);
                    canvas.drawLine(p, 18, p, 21, stroke);
                    canvas.drawLine(3, p, 6, p, stroke);
                    canvas.drawLine(18, p, 21, p, stroke);
                }
                break;
            case DISC:
                canvas.drawCircle(12, 12, 9, stroke);
                canvas.drawCircle(12, 12, 2.6f, stroke);
                r.set(5.5f, 5.5f, 18.5f, 18.5f);
                canvas.drawArc(r, 200, 60, false, stroke);
                break;
            case TUNE:
                canvas.drawLine(4, 7, 20, 7, stroke);
                canvas.drawLine(4, 17, 20, 17, stroke);
                canvas.drawCircle(9, 7, 2.4f, fill);
                canvas.drawCircle(15, 17, 2.4f, fill);
                canvas.drawLine(4, 12, 20, 12, stroke);
                canvas.drawCircle(17, 12, 2.4f, fill);
                break;
            case INFO:
                canvas.drawCircle(12, 12, 9, stroke);
                canvas.drawLine(12, 11, 12, 16.5f, stroke);
                canvas.drawCircle(12, 7.6f, 1.2f, fill);
                break;
            case BACK:
                path.moveTo(14.5f, 5.5f);
                path.lineTo(8, 12);
                path.lineTo(14.5f, 18.5f);
                stroke.setStrokeWidth(2.3f);
                canvas.drawPath(path, stroke);
                break;
            case CHEVRON:
                path.moveTo(9.5f, 5.5f);
                path.lineTo(16, 12);
                path.lineTo(9.5f, 18.5f);
                stroke.setStrokeWidth(2.3f);
                canvas.drawPath(path, stroke);
                break;
            case CHECK:
                path.moveTo(5, 12.5f);
                path.lineTo(10, 17.5f);
                path.lineTo(19.5f, 7);
                stroke.setStrokeWidth(2.4f);
                canvas.drawPath(path, stroke);
                break;
            case UNDO:
                path.moveTo(9, 5);
                path.lineTo(4.5f, 9.5f);
                path.lineTo(9, 14);
                canvas.drawPath(path, stroke);
                path.reset();
                path.moveTo(4.5f, 9.5f);
                path.lineTo(14, 9.5f);
                path.cubicTo(18, 9.5f, 20, 12, 20, 14.5f);
                path.cubicTo(20, 17, 18, 19.5f, 14, 19.5f);
                path.lineTo(11, 19.5f);
                canvas.drawPath(path, stroke);
                break;
            case GRID:
                r.set(4, 4, 20, 20);
                canvas.drawRoundRect(r, 2, 2, stroke);
                canvas.drawLine(9.3f, 4, 9.3f, 20, stroke);
                canvas.drawLine(14.7f, 4, 14.7f, 20, stroke);
                canvas.drawLine(4, 9.3f, 20, 9.3f, stroke);
                canvas.drawLine(4, 14.7f, 20, 14.7f, stroke);
                break;
            case RESET:
                r.set(4.5f, 4.5f, 19.5f, 19.5f);
                canvas.drawArc(r, -60, 300, false, stroke);
                path.moveTo(15.5f, 3.5f);
                path.lineTo(16.3f, 7.8f);
                path.lineTo(12, 8.4f);
                canvas.drawPath(path, stroke);
                break;
            case CLOSE:
                stroke.setStrokeWidth(2.2f);
                canvas.drawLine(6.5f, 6.5f, 17.5f, 17.5f, stroke);
                canvas.drawLine(17.5f, 6.5f, 6.5f, 17.5f, stroke);
                break;
            case PLUS:
                stroke.setStrokeWidth(2.2f);
                canvas.drawLine(12, 5.5f, 12, 18.5f, stroke);
                canvas.drawLine(5.5f, 12, 18.5f, 12, stroke);
                break;
            case MINUS:
                stroke.setStrokeWidth(2.2f);
                canvas.drawLine(5.5f, 12, 18.5f, 12, stroke);
                break;
            case EDIT:
                path.moveTo(4.5f, 19.5f);
                path.lineTo(5.3f, 15.6f);
                path.lineTo(15.6f, 5.3f);
                path.lineTo(18.7f, 8.4f);
                path.lineTo(8.4f, 18.7f);
                path.close();
                canvas.drawPath(path, stroke);
                canvas.drawLine(13.4f, 7.5f, 16.5f, 10.6f, stroke);
                break;
            case TRASH:
                canvas.drawLine(4.5f, 7, 19.5f, 7, stroke);
                path.moveTo(6.5f, 7);
                path.lineTo(7.5f, 20);
                path.lineTo(16.5f, 20);
                path.lineTo(17.5f, 7);
                canvas.drawPath(path, stroke);
                canvas.drawLine(9.5f, 4, 14.5f, 4, stroke);
                break;
            case LAYERS:
                path.moveTo(12, 4);
                path.lineTo(21, 9);
                path.lineTo(12, 14);
                path.lineTo(3, 9);
                path.close();
                canvas.drawPath(path, stroke);
                path.reset();
                path.moveTo(3, 13.5f);
                path.lineTo(12, 18.5f);
                path.lineTo(21, 13.5f);
                canvas.drawPath(path, stroke);
                break;
            default:
                break;
        }
        canvas.restore();
    }

    @Override
    public void setAlpha(int alpha) {
        stroke.setAlpha(alpha);
        fill.setAlpha(alpha);
    }

    @Override
    public void setColorFilter(ColorFilter colorFilter) {
        stroke.setColorFilter(colorFilter);
        fill.setColorFilter(colorFilter);
    }

    @Override
    public int getOpacity() {
        return PixelFormat.TRANSLUCENT;
    }
}
