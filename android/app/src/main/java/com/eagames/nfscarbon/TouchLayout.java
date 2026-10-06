package com.eagames.nfscarbon;

import android.content.Context;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Model for the on-screen gamepad layouts stored in &lt;files&gt;/touch_layouts.json. */
public class TouchLayout {
    public static final String FILE_NAME = "touch_layouts.json";
    public static final String DEFAULT_ASSET = "touch_layouts_default.json";
    public static final String LAYOUT_XBOX = "xbox";
    public static final String LAYOUT_DRIVING = "driving";
    public static final int VERSION = 9;

    public static final class Control {
        public String id = "";
        public String kind = "button";
        public float x = 0.5f;
        public float y = 0.5f;
        public float size = 0.1f;
        public float w = 0f;
        public float h = 0f;
        public String label = "";
        public int bit = 0;
        public String axis = "";

        public Control copy() {
            Control out = new Control();
            out.id = id;
            out.kind = kind;
            out.x = x;
            out.y = y;
            out.size = size;
            out.w = w;
            out.h = h;
            out.label = label;
            out.bit = bit;
            out.axis = axis;
            return out;
        }
    }

    public int version = VERSION;
    public String active = LAYOUT_XBOX;
    public float opacity = GameConfig.DEFAULT_OPACITY;
    public float scale = GameConfig.DEFAULT_SCALE;
    public float deadzone = GameConfig.DEFAULT_DEADZONE;
    public final LinkedHashMap<String, List<Control>> layouts = new LinkedHashMap<>();

    public static File file(Context context) {
        return new File(context.getFilesDir(), FILE_NAME);
    }

    public static String defaultJson(Context context) {
        try (InputStream input = context.getAssets().open(DEFAULT_ASSET)) {
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            byte[] buffer = new byte[16 * 1024];
            int read;
            while ((read = input.read(buffer)) > 0) {
                out.write(buffer, 0, read);
            }
            return new String(out.toByteArray(), StandardCharsets.UTF_8);
        } catch (Exception e) {
            return "{}";
        }
    }

    public static TouchLayout load(Context context) {
        TouchLayout layout = new TouchLayout();
        String json = null;
        File file = file(context);
        if (file.isFile()) {
            json = readFile(file);
        }
        if (json != null && layout.fromJson(json) && layout.version >= VERSION
                && !layout.layouts.isEmpty()) {
            return layout;
        }
        // Old schema or unreadable file: replace with the current defaults.
        layout = new TouchLayout();
        if (!layout.fromJson(defaultJson(context)) || layout.layouts.isEmpty()) {
            layout.buildFallback();
        }
        layout.version = VERSION;
        layout.save(context);
        return layout;
    }

    public TouchLayout copyFrom(TouchLayout other) {
        active = other.active;
        opacity = other.opacity;
        scale = other.scale;
        deadzone = other.deadzone;
        layouts.clear();
        for (Map.Entry<String, List<Control>> entry : other.layouts.entrySet()) {
            List<Control> list = new ArrayList<>();
            for (Control control : entry.getValue()) {
                list.add(control.copy());
            }
            layouts.put(entry.getKey(), list);
        }
        return this;
    }

    public void resetFromDefaults(Context context) {
        TouchLayout defaults = new TouchLayout();
        if (!defaults.fromJson(defaultJson(context)) || defaults.layouts.isEmpty()) {
            defaults.buildFallback();
        }
        String keepActive = active;
        copyFrom(defaults);
        if (layouts.containsKey(keepActive)) {
            active = keepActive;
        }
    }

    public List<Control> activeControls() {
        List<Control> list = layouts.get(active);
        if (list == null && !layouts.isEmpty()) {
            list = layouts.values().iterator().next();
        }
        return list == null ? new ArrayList<Control>() : list;
    }

    public Control find(String layoutName, String id) {
        List<Control> list = layouts.get(layoutName);
        if (list == null) {
            return null;
        }
        for (Control control : list) {
            if (control.id.equals(id)) {
                return control;
            }
        }
        return null;
    }

    public String otherLayout() {
        return LAYOUT_XBOX.equals(active) ? LAYOUT_DRIVING : LAYOUT_XBOX;
    }

    public boolean fromJson(String json) {
        try {
            JSONObject root = new JSONObject(json);
            version = root.optInt("version", 1);
            String parsedActive = root.optString("active", LAYOUT_XBOX);
            opacity = (float) root.optDouble("opacity", GameConfig.DEFAULT_OPACITY);
            scale = (float) root.optDouble("scale", GameConfig.DEFAULT_SCALE);
            deadzone = (float) root.optDouble("deadzone", GameConfig.DEFAULT_DEADZONE);
            JSONObject layoutObject = root.optJSONObject("layouts");
            if (layoutObject == null) {
                return false;
            }
            layouts.clear();
            JSONArray names = layoutObject.names();
            if (names == null) {
                return false;
            }
            for (int i = 0; i < names.length(); i++) {
                String name = names.getString(i);
                JSONArray array = layoutObject.optJSONArray(name);
                if (array == null) {
                    continue;
                }
                List<Control> controls = new ArrayList<>();
                for (int j = 0; j < array.length(); j++) {
                    JSONObject item = array.getJSONObject(j);
                    Control control = new Control();
                    control.id = item.optString("id", "control" + j);
                    control.kind = item.optString("kind", "button");
                    control.x = (float) item.optDouble("x", 0.5);
                    control.y = (float) item.optDouble("y", 0.5);
                    control.size = (float) item.optDouble("size", 0.1);
                    control.w = (float) item.optDouble("w", 0.0);
                    control.h = (float) item.optDouble("h", 0.0);
                    control.label = item.optString("label", "");
                    control.bit = item.optInt("bit", 0);
                    control.axis = item.optString("axis", "");
                    controls.add(control);
                }
                layouts.put(name, controls);
            }
            if (layouts.containsKey(parsedActive)) {
                active = parsedActive;
            }
            return !layouts.isEmpty();
        } catch (Exception e) {
            return false;
        }
    }

    public String toJson() {
        try {
            JSONObject root = new JSONObject();
            root.put("version", version);
            root.put("active", active);
            root.put("opacity", (double) opacity);
            root.put("scale", (double) scale);
            root.put("deadzone", (double) deadzone);
            JSONObject layoutObject = new JSONObject();
            for (Map.Entry<String, List<Control>> entry : layouts.entrySet()) {
                JSONArray array = new JSONArray();
                for (Control control : entry.getValue()) {
                    JSONObject item = new JSONObject();
                    item.put("id", control.id);
                    item.put("kind", control.kind);
                    item.put("x", (double) control.x);
                    item.put("y", (double) control.y);
                    item.put("size", (double) control.size);
                    if (control.w > 0f) {
                        item.put("w", (double) control.w);
                    }
                    if (control.h > 0f) {
                        item.put("h", (double) control.h);
                    }
                    if (!control.label.isEmpty()) {
                        item.put("label", control.label);
                    }
                    if (control.bit != 0) {
                        item.put("bit", control.bit);
                    }
                    if (!control.axis.isEmpty()) {
                        item.put("axis", control.axis);
                    }
                    array.put(item);
                }
                layoutObject.put(entry.getKey(), array);
            }
            root.put("layouts", layoutObject);
            return root.toString(2);
        } catch (Exception e) {
            return "{}";
        }
    }

    public boolean save(Context context) {
        try (FileOutputStream output = new FileOutputStream(file(context))) {
            output.write(toJson().getBytes(StandardCharsets.UTF_8));
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private static String readFile(File file) {
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                new FileInputStream(file), StandardCharsets.UTF_8))) {
            StringBuilder builder = new StringBuilder();
            String line;
            while ((line = reader.readLine()) != null) {
                builder.append(line).append('\n');
            }
            return builder.toString();
        } catch (Exception e) {
            return null;
        }
    }

    private void buildFallback() {
        layouts.clear();
        // Driving: Carbon's Xbox 360 defaults (manual): A handbrake, B nitrous,
        // X speedbreaker, Y crew, LB reset, RB change view, Back engage event.
        List<Control> driving = new ArrayList<>();
        driving.add(control("steer_left", "steer", 0.09f, 0.72f, 0.16f));
        driving.add(control("steer_right", "steer", 0.26f, 0.72f, 0.16f));
        driving.add(pedal("brake", "BRAKE", 0.76f, 0.70f, 0.13f, "lt"));
        driving.add(pedal("gas", "GAS", 0.91f, 0.66f, 0.15f, "rt"));
        driving.add(button("a", "E-BRAKE", 0.62f, 0.79f, 0.078f, 4096));
        driving.add(button("b", "NOS", 0.80f, 0.33f, 0.075f, 8192));
        driving.add(button("x", "BREAKER", 0.645f, 0.49f, 0.07f, 16384));
        driving.add(button("y", "CREW", 0.93f, 0.31f, 0.062f, 32768));
        driving.add(button("lb", "RESET", 0.22f, 0.09f, 0.05f, 256));
        driving.add(button("rb", "VIEW", 0.93f, 0.09f, 0.05f, 512));
        driving.add(button("back", "EVENT", 0.43f, 0.16f, 0.045f, 32));
        driving.add(button("start", "", 0.57f, 0.16f, 0.045f, 16));
        layouts.put(LAYOUT_DRIVING, driving);

        List<Control> xbox = new ArrayList<>();
        xbox.add(control("ls", "stick", 0.12f, 0.70f, 0.16f));
        xbox.add(control("dpad", "dpad", 0.29f, 0.86f, 0.075f));
        xbox.add(control("rs", "stick", 0.70f, 0.83f, 0.10f));
        xbox.add(button("y", "Y", 0.88f, 0.44f, 0.063f, 32768));
        xbox.add(button("x", "X", 0.81f, 0.59f, 0.063f, 16384));
        xbox.add(button("b", "B", 0.95f, 0.59f, 0.063f, 8192));
        xbox.add(button("a", "A", 0.88f, 0.74f, 0.063f, 4096));
        xbox.add(trigger("lt", "LT", 0.22f, 0.08f, 0.06f, "lt"));
        xbox.add(button("lb", "LB", 0.22f, 0.21f, 0.06f, 256));
        xbox.add(trigger("rt", "RT", 0.93f, 0.08f, 0.06f, "rt"));
        xbox.add(button("rb", "RB", 0.93f, 0.21f, 0.06f, 512));
        xbox.add(button("back", "", 0.44f, 0.16f, 0.042f, 32));
        xbox.add(button("start", "", 0.56f, 0.16f, 0.042f, 16));
        layouts.put(LAYOUT_XBOX, xbox);
        active = LAYOUT_DRIVING;
    }

    private static Control pedal(String id, String label, float x, float y, float size,
                                 String axis) {
        Control control = control(id, "pedal", x, y, size);
        control.label = label;
        control.axis = axis;
        return control;
    }

    private static Control trigger(String id, String label, float x, float y, float size,
                                   String axis) {
        Control control = control(id, "trigger", x, y, size);
        control.label = label;
        control.axis = axis;
        return control;
    }

    private static Control control(String id, String kind, float x, float y, float size) {
        Control control = new Control();
        control.id = id;
        control.kind = kind;
        control.x = x;
        control.y = y;
        control.size = size;
        return control;
    }

    private static Control button(String id, String label, float x, float y, float size, int bit) {
        Control control = control(id, "button", x, y, size);
        control.label = label;
        control.bit = bit;
        return control;
    }
}
