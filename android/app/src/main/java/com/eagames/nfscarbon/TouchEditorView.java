package com.eagames.nfscarbon;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.MotionEvent;
import android.view.View;

import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** 16:9 preview canvas used by the settings screen to move controls around. */
public class TouchEditorView extends View {
    public interface Listener {
        void onSelectionChanged(TouchLayout.Control control);

        void onLayoutEdited();
    }

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private TouchLayout layout;
    private String layoutName = TouchLayout.LAYOUT_XBOX;
    private String selectedId;
    private float pressX;
    private float pressY;
    private Listener listener;

    public TouchEditorView(Context context) {
        super(context);
        setBackgroundColor(0xFF10151C);
    }

    public void setListener(Listener listener) {
        this.listener = listener;
    }

    public void setLayout(TouchLayout layout, String layoutName) {
        this.layout = layout;
        this.layoutName = layoutName;
        selectedId = null;
        requestLayout();
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

    public void setControlSize(float size) {
        TouchLayout.Control control = getSelected();
        if (control != null) {
            control.size = Math.max(0.03f, Math.min(0.4f, size));
            invalidate();
            if (listener != null) {
                listener.onLayoutEdited();
            }
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

    @Override
    protected void onMeasure(int widthMeasureSpec, int heightMeasureSpec) {
        int width = MeasureSpec.getSize(widthMeasureSpec);
        int height = MeasureSpec.getSize(heightMeasureSpec);
        int mode = MeasureSpec.getMode(heightMeasureSpec);
        int targetHeight = Math.round(width * 9f / 16f);
        if (mode != MeasureSpec.UNSPECIFIED && targetHeight > height) {
            targetHeight = height;
            width = Math.round(height * 16f / 9f);
        }
        setMeasuredDimension(width, targetHeight);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (layout == null) {
            return;
        }
        RectF viewport = new RectF(0, 0, getWidth(), getHeight());
        paint.setStyle(Paint.Style.STROKE);
        paint.setStrokeWidth(2f);
        paint.setColor(0x33FFFFFF);
        canvas.drawRect(viewport, paint);

        List<TouchLayout.Control> controls = layout.layouts.get(layoutName);
        if (controls == null) {
            controls = layout.activeControls();
        }
        for (TouchLayout.Control control : controls) {
            RectF bounds = TouchRenderer.boundsOf(control, getWidth(), getHeight(), 1f);
            boolean selected = control.id.equals(selectedId);
            TouchRenderer.drawControl(canvas, paint, control, bounds, 1f, false, selected,
                    0f, 0f);
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (layout == null) {
            return false;
        }
        List<TouchLayout.Control> controls = layout.layouts.get(layoutName);
        if (controls == null) {
            controls = layout.activeControls();
        }
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: {
                TouchLayout.Control hit = TouchRenderer.hitTest(controls, getWidth(), getHeight(),
                        1f, event.getX(), event.getY());
                selectedId = hit == null ? null : hit.id;
                pressX = event.getX();
                pressY = event.getY();
                invalidate();
                if (listener != null) {
                    listener.onSelectionChanged(hit);
                }
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                TouchLayout.Control selected = getSelected();
                if (selected != null) {
                    float dx = (event.getX() - pressX) / Math.max(1, getWidth());
                    float dy = (event.getY() - pressY) / Math.max(1, getHeight());
                    selected.x = clamp(selected.x + dx, 0.02f, 0.98f);
                    selected.y = clamp(selected.y + dy, 0.02f, 0.98f);
                    pressX = event.getX();
                    pressY = event.getY();
                    invalidate();
                    if (listener != null) {
                        listener.onLayoutEdited();
                    }
                }
                return true;
            }
            default:
                return true;
        }
    }

    private static float clamp(float value, float min, float max) {
        return Math.max(min, Math.min(max, value));
    }
}
