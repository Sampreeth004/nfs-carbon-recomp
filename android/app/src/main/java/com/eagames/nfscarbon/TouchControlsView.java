package com.eagames.nfscarbon;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.View;

import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * Full-screen transparent overlay that renders the active touch layout and
 * merges all active pointers into one synthetic gamepad state.
 */
public class TouchControlsView extends View {
    public interface Listener {
        void onLayoutToggled(String active);
    }

    private static final int DPAD_UP = 0x0001;
    private static final int DPAD_DOWN = 0x0002;
    private static final int DPAD_LEFT = 0x0004;
    private static final int DPAD_RIGHT = 0x0008;

    private static final class Interaction {
        TouchLayout.Control control;
        int pointerId;
        float x;
        float y;
        float downX;
        float downY;
    }

    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Map<Integer, Interaction> pointers = new HashMap<>();
    private final Map<Integer, Integer> dpadBits = new HashMap<>();
    private final Map<String, float[]> sticks = new HashMap<>();
    private final Map<String, Float> triggers = new HashMap<>();
    private final Set<String> pressed = new HashSet<>();
    private final Handler handler = new Handler(Looper.getMainLooper());

    private TouchLayout layout;
    private boolean padActive = true;
    private float wheelValue = 0f;
    private boolean wheelHeld = false;
    // Arrow steering: pointers holding the left/right arrows, and the stick
    // value ramping towards the direction they ask for.
    private final Map<Integer, String> steerPointers = new HashMap<>();
    private boolean steerTicking = false;
    private final Runnable steerRamp = new Runnable() {
        @Override
        public void run() {
            float target = steerTarget();
            // Full lock in ~120 ms, back to centre in ~80 ms.
            float step = target == 0f ? 0.2f : 0.14f;
            if (Math.abs(target - wheelValue) <= step) {
                wheelValue = target;
            } else {
                wheelValue += Math.signum(target - wheelValue) * step;
            }
            invalidate();
            updateNative();
            if (wheelValue != target || target != 0f) {
                handler.postDelayed(this, 16);
            } else {
                steerTicking = false;
            }
        }
    };
    private Listener listener;

    private final Runnable wheelSpring = new Runnable() {
        @Override
        public void run() {
            if (wheelHeld) {
                return;
            }
            if (Math.abs(wheelValue) < 0.01f) {
                wheelValue = 0f;
            } else {
                wheelValue *= 0.82f;
                handler.postDelayed(this, 16);
            }
            invalidate();
            updateNative();
        }
    };

    public TouchControlsView(Context context) {
        super(context);
        layout = TouchLayout.load(context);
        setFocusable(false);
        setFocusableInTouchMode(false);
    }

    public void setListener(Listener listener) {
        this.listener = listener;
    }

    public String getActiveLayout() {
        return layout.active;
    }

    public TouchLayout getLayout() {
        return layout;
    }

    public void reloadLayout() {
        layout = TouchLayout.load(getContext());
        clearInteractions();
        invalidate();
    }

    public void toggleLayout() {
        layout.active = layout.otherLayout();
        layout.save(getContext());
        clearInteractions();
        if (listener != null) {
            listener.onLayoutToggled(layout.active);
        }
        invalidate();
    }

    public void setPadActive(boolean active) {
        padActive = active;
        GameBridge.setTouchPadEnabled(active && getVisibility() == VISIBLE);
        if (!active) {
            clearInteractions();
        }
    }

    @Override
    public void onVisibilityChanged(View changedView, int visibility) {
        super.onVisibilityChanged(changedView, visibility);
        GameBridge.setTouchPadEnabled(padActive && visibility == VISIBLE);
        if (visibility != VISIBLE) {
            clearInteractions();
            wheelHeld = false;
            wheelValue = 0f;
            handler.removeCallbacks(wheelSpring);
            handler.removeCallbacks(steerRamp);
            steerTicking = false;
            updateNative();
        }
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        GameBridge.setTouchPadEnabled(padActive && getVisibility() == VISIBLE);
    }

    @Override
    protected void onDetachedFromWindow() {
        handler.removeCallbacks(wheelSpring);
        GameBridge.setTouchPadEnabled(false);
        GameBridge.setTouchPad(0, 0f, 0f, 0f, 0f, 0f, 0f);
        super.onDetachedFromWindow();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        List<TouchLayout.Control> controls = layout.activeControls();
        float alpha = Math.max(0.1f, Math.min(1f, layout.opacity));
        for (TouchLayout.Control control : controls) {
            RectF bounds = TouchRenderer.boundsOf(control, getWidth(), getHeight(), layout.scale);
            float knobX = 0f;
            float knobY = 0f;
            if ("stick".equals(control.kind)) {
                float[] value = sticks.get(control.id);
                if (value != null) {
                    knobX = value[0];
                    knobY = -value[1];
                }
            } else if ("wheel".equals(control.kind)) {
                knobX = wheelValue;
            } else if ("steer".equals(control.kind)) {
                knobX = wheelValue;
            } else if ("pedal".equals(control.kind) || "trigger".equals(control.kind)) {
                Float pressure = triggers.get(control.id);
                knobY = pressure != null ? pressure : 0f;
            }
            boolean pressed = isControlPressed(control) || triggers.containsKey(control.id)
                    || ("wheel".equals(control.kind) && wheelHeld)
                    || ("steer".equals(control.kind) && steerPointers.containsValue(control.id));
            TouchRenderer.drawControl(canvas, paint, control, bounds, alpha, pressed, false,
                    knobX, knobY);
        }
    }

    private boolean isControlPressed(TouchLayout.Control control) {
        if (pressed.contains(control.id)) {
            return true;
        }
        if ("dpad".equals(control.kind)) {
            for (Integer bits : dpadBits.values()) {
                if (bits != 0) {
                    return true;
                }
            }
        }
        return false;
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (!padActive || getVisibility() != VISIBLE) {
            return false;
        }
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int index = event.getActionIndex();
                handleDown(event.getPointerId(index), event.getX(index), event.getY(index));
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                for (int i = 0; i < event.getPointerCount(); i++) {
                    Interaction interaction = pointers.get(event.getPointerId(i));
                    if (interaction != null) {
                        handleMove(interaction, event.getX(i), event.getY(i));
                    }
                }
                invalidate();
                updateNative();
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP: {
                handleUp(event.getPointerId(event.getActionIndex()));
                return true;
            }
            case MotionEvent.ACTION_CANCEL: {
                clearInteractions();
                invalidate();
                updateNative();
                return true;
            }
            default:
                return true;
        }
    }

    private void handleDown(int pointerId, float x, float y) {
        if (pointers.containsKey(pointerId)) {
            return;
        }
        List<TouchLayout.Control> controls = layout.activeControls();
        TouchLayout.Control hit = TouchRenderer.hitTest(controls, getWidth(), getHeight(),
                layout.scale, x, y);
        if (hit == null || isControlInUse(hit)) {
            return;
        }
        Interaction interaction = new Interaction();
        interaction.control = hit;
        interaction.pointerId = pointerId;
        interaction.x = x;
        interaction.y = y;
        interaction.downX = x;
        interaction.downY = y;
        pointers.put(pointerId, interaction);
        if ("stick".equals(hit.kind)) {
            sticks.put(hit.id, new float[]{0f, 0f});
            updateStick(interaction);
        } else if ("trigger".equals(hit.kind) || "pedal".equals(hit.kind)) {
            triggers.put(hit.id, 1f);
        } else if ("steer".equals(hit.kind)) {
            steerPointers.put(pointerId, hit.id);
            startSteerRamp();
        } else if ("wheel".equals(hit.kind)) {
            wheelHeld = true;
            handler.removeCallbacks(wheelSpring);
            wheelValue = steerValue(TouchRenderer.boundsOf(hit, getWidth(), getHeight(),
                    layout.scale), x);
        } else if ("dpad".equals(hit.kind)) {
            dpadBits.put(pointerId, directionBits(TouchRenderer.boundsOf(hit, getWidth(),
                    getHeight(), layout.scale), x, y));
        } else {
            pressed.add(hit.id);
        }
        invalidate();
        updateNative();
    }

    private void handleMove(Interaction interaction, float x, float y) {
        interaction.x = x;
        interaction.y = y;
        TouchLayout.Control control = interaction.control;
        RectF bounds = TouchRenderer.boundsOf(control, getWidth(), getHeight(), layout.scale);
        if ("stick".equals(control.kind)) {
            updateStick(interaction);
        } else if ("trigger".equals(control.kind) || "pedal".equals(control.kind)) {
            float delta = (interaction.downY - y) / Math.max(1f, bounds.height());
            float pressure = Math.max(0f, Math.min(1f, 1f + delta));
            triggers.put(control.id, pressure);
        } else if ("steer".equals(control.kind)) {
            TouchLayout.Control over = TouchRenderer.hitTest(layout.activeControls(), getWidth(),
                    getHeight(), layout.scale, x, y);
            if (over != null && "steer".equals(over.kind) && over != control) {
                interaction.control = over;
                steerPointers.put(interaction.pointerId, over.id);
                startSteerRamp();
            }
        } else if ("wheel".equals(control.kind)) {
            wheelValue = steerValue(bounds, x);
        } else if ("dpad".equals(control.kind)) {
            dpadBits.put(interaction.pointerId, directionBits(bounds, x, y));
        }
    }

    private void handleUp(int pointerId) {
        Interaction interaction = pointers.remove(pointerId);
        if (interaction == null) {
            return;
        }
        TouchLayout.Control control = interaction.control;
        if ("stick".equals(control.kind)) {
            sticks.remove(control.id);
        } else if ("trigger".equals(control.kind) || "pedal".equals(control.kind)) {
            triggers.remove(control.id);
        } else if ("steer".equals(control.kind)) {
            steerPointers.remove(pointerId);
            startSteerRamp();
        } else if ("wheel".equals(control.kind)) {
            wheelHeld = false;
            handler.postDelayed(wheelSpring, 16);
        } else if ("dpad".equals(control.kind)) {
            dpadBits.remove(pointerId);
        } else {
            pressed.remove(control.id);
        }
        invalidate();
        updateNative();
    }

    private float steerTarget() {
        boolean left = false, right = false;
        for (String id : steerPointers.values()) {
            if ("steer_left".equals(id)) {
                left = true;
            } else if ("steer_right".equals(id)) {
                right = true;
            }
        }
        return (right ? 1f : 0f) - (left ? 1f : 0f);
    }

    private void startSteerRamp() {
        if (!steerTicking) {
            steerTicking = true;
            handler.post(steerRamp);
        }
    }

    private boolean isControlInUse(TouchLayout.Control control) {
        for (Interaction interaction : pointers.values()) {
            if (interaction.control == control) {
                return true;
            }
        }
        return false;
    }

    private void updateStick(Interaction interaction) {
        RectF bounds = TouchRenderer.boundsOf(interaction.control, getWidth(), getHeight(),
                layout.scale);
        float radius = Math.max(1f, bounds.width() * 0.5f);
        float dx = (interaction.x - bounds.centerX()) / radius;
        float dy = (interaction.y - bounds.centerY()) / radius;
        float magnitude = (float) Math.sqrt(dx * dx + dy * dy);
        if (magnitude > 1f) {
            dx /= magnitude;
            dy /= magnitude;
        }
        sticks.put(interaction.control.id, new float[]{dx, -dy});
    }

    private float steerValue(RectF bounds, float x) {
        float half = Math.max(1f, bounds.width() * 0.45f);
        return Math.max(-1f, Math.min(1f, (x - bounds.centerX()) / half));
    }

    private static int directionBits(RectF bounds, float x, float y) {
        float dx = x - bounds.centerX();
        float dy = y - bounds.centerY();
        float threshold = bounds.width() * 0.12f;
        if (Math.abs(dx) < threshold && Math.abs(dy) < threshold) {
            return 0;
        }
        if (Math.abs(dx) >= Math.abs(dy)) {
            return dx < 0 ? DPAD_LEFT : DPAD_RIGHT;
        }
        return dy < 0 ? DPAD_UP : DPAD_DOWN;
    }

    private void clearInteractions() {
        pointers.clear();
        steerPointers.clear();
        dpadBits.clear();
        sticks.clear();
        triggers.clear();
        pressed.clear();
    }

    private void updateNative() {
        int buttons = 0;
        for (String id : pressed) {
            TouchLayout.Control control = layout.find(layout.active, id);
            if (control != null) {
                buttons |= control.bit;
            }
        }
        for (Integer bits : dpadBits.values()) {
            buttons |= bits;
        }

        float leftX = 0f;
        float leftY = 0f;
        float rightX = 0f;
        float rightY = 0f;
        boolean hasLeftStick = false;
        for (Map.Entry<String, float[]> entry : sticks.entrySet()) {
            if (isRightStick(entry.getKey())) {
                rightX = entry.getValue()[0];
                rightY = entry.getValue()[1];
            } else {
                leftX = entry.getValue()[0];
                leftY = entry.getValue()[1];
                hasLeftStick = true;
            }
        }
        if (!hasLeftStick && wheelValue != 0f) {
            leftX = wheelValue;
        }

        float leftTrigger = 0f;
        float rightTrigger = 0f;
        for (Map.Entry<String, Float> entry : triggers.entrySet()) {
            TouchLayout.Control control = layout.find(layout.active, entry.getKey());
            if (control == null) {
                continue;
            }
            if ("rt".equals(control.axis)) {
                rightTrigger = entry.getValue();
            } else if ("lt".equals(control.axis)) {
                leftTrigger = entry.getValue();
            }
        }

        float[] left = applyDeadzone(leftX, leftY);
        float[] right = applyDeadzone(rightX, rightY);
        GameBridge.setTouchPad(buttons, left[0], left[1], right[0], right[1],
                leftTrigger, rightTrigger);
    }

    private float[] applyDeadzone(float x, float y) {
        float magnitude = (float) Math.sqrt(x * x + y * y);
        if (magnitude <= layout.deadzone) {
            return new float[]{0f, 0f};
        }
        float factor = Math.min(1f, (magnitude - layout.deadzone)
                / Math.max(0.0001f, 1f - layout.deadzone)) / magnitude;
        return new float[]{x * factor, y * factor};
    }

    private static boolean isRightStick(String id) {
        return "rs".equals(id) || "look".equals(id) || id.startsWith("rs_");
    }
}
