package com.eagames.nfscarbon;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.DisplayMetrics;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.view.WindowManager;

import java.util.List;

/**
 * Layout canvas for the touch controls. It keeps the phone's real screen aspect
 * ratio and draws the controls with the overlay's size and opacity, so what is
 * edited here is what appears in game. Drag to move, pinch to resize. In preview
 * mode it is a read-only thumbnail.
 */
public class TouchEditorView extends View {
    public interface Listener {
        void onSelectionChanged(TouchLayout.Control control);

        /** Called before a gesture or edit changes the layout (for undo). */
        void onEditStarted();

        void onLayoutEdited();
    }

    private static final float GRID_STEP = 0.025f;
    private static final float MIN_SIZE = 0.03f;
    private static final float MAX_SIZE = 0.4f;

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint guide = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final ScaleGestureDetector scaleDetector;
    private final float screenAspect;
    private TouchLayout layout;
    private String layoutName = TouchLayout.LAYOUT_XBOX;
    private String selectedId;
    private float pressX;
    private float pressY;
    private boolean moved;
    private boolean scaling;
    private boolean snap = true;
    private boolean preview;
    private boolean fillParent;
    private boolean guideX;
    private boolean guideY;
    private Listener listener;

    public TouchEditorView(Context context) {
        super(context);
        DisplayMetrics m = new DisplayMetrics();
        ((WindowManager) context.getSystemService(Context.WINDOW_SERVICE)).getDefaultDisplay()
                .getRealMetrics(m);
        float longSide = Math.max(m.widthPixels, m.heightPixels);
        float shortSide = Math.max(1, Math.min(m.widthPixels, m.heightPixels));
        screenAspect = longSide / shortSide;
        guide.setStyle(Paint.Style.STROKE);
        guide.setStrokeWidth(Ui.dp(context, 1.2f));
        guide.setColor(0xCC19C6E6);
        guide.setPathEffect(new DashPathEffect(new float[]{Ui.dp(context, 6), Ui.dp(context, 5)}, 0));
        scaleDetector = new ScaleGestureDetector(context,
                new ScaleGestureDetector.SimpleOnScaleGestureListener() {
                    @Override
                    public boolean onScaleBegin(ScaleGestureDetector detector) {
                        if (getSelected() == null) {
                            select(hit(detector.getFocusX(), detector.getFocusY()));
                        }
                        if (getSelected() == null) {
                            return false;
                        }
                        if (!moved && listener != null) {
                            listener.onEditStarted();
                        }
                        moved = true;
                        scaling = true;
                        return true;
                    }

                    @Override
                    public boolean onScale(ScaleGestureDetector detector) {
                        TouchLayout.Control c = getSelected();
                        if (c != null) {
                            applySize(c, c.size * detector.getScaleFactor());
                        }
                        return true;
                    }
                });
        setBackgroundColor(0xFF0A0F17);
    }

    public void setListener(Listener listener) {
        this.listener = listener;
    }

    /** Read-only thumbnail: no touch handling. */
    public void setPreview(boolean preview) {
        this.preview = preview;
    }

    /** Use the whole view (full-screen editor) instead of fitting the screen aspect. */
    public void setFillParent(boolean fill) {
        this.fillParent = fill;
        requestLayout();
    }

    public void setSnap(boolean snap) {
        this.snap = snap;
        invalidate();
    }

    public boolean isSnap() {
        return snap;
    }

    public void setLayout(TouchLayout layout, String layoutName) {
        this.layout = layout;
        this.layoutName = layoutName;
        selectedId = null;
        invalidate();
    }

    public String getLayoutName() {
        return layoutName;
    }

    public TouchLayout.Control getSelected() {
        if (layout == null || selectedId == null) {
            return null;
        }
        return layout.find(layoutName, selectedId);
    }

    public void select(TouchLayout.Control control) {
        selectedId = control == null ? null : control.id;
        invalidate();
        if (listener != null) {
            listener.onSelectionChanged(control);
        }
    }

    public void setControlSize(float size) {
        TouchLayout.Control control = getSelected();
        if (control != null) {
            applySize(control, size);
        }
    }

    public void setControlAction(int bit, String axis, String label) {
        TouchLayout.Control control = getSelected();
        if (control != null) {
            control.bit = bit;
            control.axis = axis;
            control.label = label;
            invalidate();
            if (listener != null) {
                listener.onLayoutEdited();
            }
        }
    }

    private void applySize(TouchLayout.Control control, float size) {
        control.size = Math.max(MIN_SIZE, Math.min(MAX_SIZE, size));
        invalidate();
        if (listener != null) {
            listener.onLayoutEdited();
        }
    }

    private List<TouchLayout.Control> controls() {
        List<TouchLayout.Control> controls = layout.layouts.get(layoutName);
        return controls != null ? controls : layout.activeControls();
    }

    private TouchLayout.Control hit(float x, float y) {
        return TouchRenderer.hitTest(controls(), getWidth(), getHeight(), layout.scale, x, y);
    }

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int width = MeasureSpec.getSize(widthMeasureSpec);
        int height = MeasureSpec.getSize(heightMeasureSpec);
        if (fillParent) {
            setMeasuredDimension(width, height);
            return;
        }
        int mode = MeasureSpec.getMode(heightMeasureSpec);
        int targetHeight = Math.round(width / screenAspect);
        if (mode != MeasureSpec.UNSPECIFIED && targetHeight > height) {
            targetHeight = height;
            width = Math.round(height * screenAspect);
        }
        setMeasuredDimension(width, targetHeight);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (layout == null) {
            return;
        }
        float w = getWidth();
        float h = getHeight();
        if (!preview && snap) {
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(1f);
            paint.setColor(0x0FFFFFFF);
            for (float f = GRID_STEP * 2; f < 1f; f += GRID_STEP * 2) {
                canvas.drawLine(f * w, 0, f * w, h, paint);
                canvas.drawLine(0, f * h, w, f * h, paint);
            }
        }
        if (!preview) {
            // Centre lines and the 16:9 frame the game renders in when widescreen is off.
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(Ui.dp(getContext(), 1));
            paint.setColor(0x1FFFFFFF);
            float frame = h * 16f / 9f;
            if (frame < w) {
                float x0 = (w - frame) / 2f;
                canvas.drawRect(x0, 0, x0 + frame, h, paint);
            }
        }
        float alpha = Math.max(0.35f, Math.min(1f, layout.opacity));
        for (TouchLayout.Control control : controls()) {
            RectF bounds = TouchRenderer.boundsOf(control, w, h, layout.scale);
            boolean selected = control.id.equals(selectedId);
            TouchRenderer.drawControl(canvas, paint, control, bounds, selected ? 1f : alpha,
                    false, selected, 0f, 0f);
        }
        if (guideX) {
            canvas.drawLine(w / 2f, 0, w / 2f, h, guide);
        }
        if (guideY) {
            canvas.drawLine(0, h / 2f, w, h / 2f, guide);
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (preview) {
            return super.onTouchEvent(event);  // a click opens the editor
        }
        if (layout == null) {
            return false;
        }
        scaleDetector.onTouchEvent(event);
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: {
                moved = false;
                scaling = false;
                select(hit(event.getX(), event.getY()));
                pressX = event.getX();
                pressY = event.getY();
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                TouchLayout.Control selected = getSelected();
                if (selected == null || scaling || event.getPointerCount() > 1) {
                    return true;
                }
                float dx = (event.getX() - pressX) / Math.max(1, getWidth());
                float dy = (event.getY() - pressY) / Math.max(1, getHeight());
                if (!moved && Math.hypot(event.getX() - pressX, event.getY() - pressY)
                        < Ui.dp(getContext(), 4)) {
                    return true;
                }
                if (!moved && listener != null) {
                    listener.onEditStarted();
                }
                moved = true;
                float nx = clamp(selected.x + dx, 0.02f, 0.98f);
                float ny = clamp(selected.y + dy, 0.02f, 0.98f);
                pressX = event.getX();
                pressY = event.getY();
                // Store the unsnapped position in the press reference so slow drags still move.
                float sx = nx;
                float sy = ny;
                if (snap) {
                    sx = Math.round(nx / GRID_STEP) * GRID_STEP;
                    sy = Math.round(ny / GRID_STEP) * GRID_STEP;
                }
                guideX = Math.abs(nx - 0.5f) < 0.012f;
                guideY = Math.abs(ny - 0.5f) < 0.012f;
                if (guideX) sx = 0.5f;
                if (guideY) sy = 0.5f;
                pressX += (sx - nx) * getWidth();
                pressY += (sy - ny) * getHeight();
                selected.x = clamp(sx, 0.02f, 0.98f);
                selected.y = clamp(sy, 0.02f, 0.98f);
                invalidate();
                if (listener != null) {
                    listener.onLayoutEdited();
                }
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                guideX = guideY = false;
                scaling = false;
                invalidate();
                return true;
            default:
                return true;
        }
    }

    private static float clamp(float value, float min, float max) {
        return Math.max(min, Math.min(max, value));
    }
}
