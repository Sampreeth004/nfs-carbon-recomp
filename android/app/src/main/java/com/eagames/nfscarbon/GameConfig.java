package com.eagames.nfscarbon;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * SharedPreferences-backed settings that are serialized to the flat TOML file
 * read by the ReXGlue runtime at startup (&lt;files&gt;/nfscarbon.toml).
 */
public final class GameConfig {
    public static final String PREFS = "nfscarbon_prefs";
    public static final String TOML_NAME = "nfscarbon.toml";

    // GPU plugin: "carbon" (native Vulkan renderer) or "xenos" (Xenos emulation).
    public static final String KEY_RENDERER = "gpu_plugin";
    public static final String KEY_WIDTH = "video_mode_width";
    public static final String KEY_HEIGHT = "video_mode_height";
    public static final String KEY_VSYNC = "vsync";
    public static final String KEY_BLOOM = "bloom";
    public static final String KEY_FPS_CAP = "fps_cap";
    public static final String KEY_RENDER_SCALE = "render_scale";
    public static final String KEY_REFLECTIONS = "reflection_faces";
    public static final String KEY_MIRROR_HALF = "mirror_half_rate";
    public static final String KEY_MSAA = "native_2x_msaa";
    public static final String KEY_MSAA_SAMPLES = "msaa_samples";
    public static final String KEY_FXAA = "swap_post_effect";
    public static final String KEY_UPSCALER = "present_effect";
    public static final String KEY_LETTERBOX = "present_letterbox";
    public static final String KEY_OCCLUSION = "occlusion_query_enable";
    public static final String KEY_ANISO = "anisotropic_override";
    public static final String KEY_SINGLE_PASS = "nfsmw_una_pasada";
    public static final String KEY_FINE_PACING = "fine_frame_pacing";

    public static final String KEY_ISO_PATH = "iso_path";
    public static final String KEY_ISO_LABEL = "iso_label";

    public static final String KEY_DRIVER_LOADER = "driver_loader";
    public static final String KEY_DRIVER_LABEL = "driver_label";

    public static final String KEY_SHOW_FPS = "show_fps";
    public static final String KEY_TOUCH_ENABLED = "touch_enabled";
    public static final String KEY_TOUCH_LAYOUT = "touch_layout";
    public static final String KEY_TOUCH_OPACITY = "touch_opacity";
    public static final String KEY_TOUCH_SCALE = "touch_scale";
    public static final String KEY_TOUCH_DEADZONE = "touch_deadzone";

    public static final int DEFAULT_WIDTH = 1280;
    public static final int DEFAULT_HEIGHT = 720;
    public static final int DEFAULT_MSAA_SAMPLES = 0;
    public static final String DEFAULT_FXAA = "none";
    public static final String DEFAULT_RENDERER = "carbon";
    public static final String DEFAULT_UPSCALER = "bilinear";
    public static final String DEFAULT_LAYOUT = "xbox";
    public static final float DEFAULT_OPACITY = 0.55f;
    public static final float DEFAULT_SCALE = 1.0f;
    public static final float DEFAULT_DEADZONE = 0.08f;

    private GameConfig() {
    }

    public static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    public static File tomlFile(Context context) {
        return new File(context.getFilesDir(), TOML_NAME);
    }

    public static String fxaaLabel(String value) {
        return "fxaa_extreme".equals(value) ? "FXAA Extreme" : ("fxaa".equals(value) ? "FXAA" : "None");
    }

    public static void writeToml(Context context) {
        SharedPreferences p = prefs(context);
        // One-time: car reflections are not visible in the native renderer, so older
        // installs that stored a non-zero value move to Off (it can be changed again).
        if (!p.getBoolean("reflections_off_migrated", false)) {
            p.edit().putInt(KEY_REFLECTIONS, 0).putBoolean("reflections_off_migrated", true)
                    .apply();
        }

        int guestWidth = p.getInt(KEY_WIDTH, DEFAULT_WIDTH);
        int guestHeight = p.getInt(KEY_HEIGHT, DEFAULT_HEIGHT);

        Map<String, String> values = new LinkedHashMap<>();
        values.put(KEY_RENDERER, quote(p.getString(KEY_RENDERER, DEFAULT_RENDERER)));
        values.put(KEY_WIDTH, String.valueOf(guestWidth));
        values.put(KEY_HEIGHT, String.valueOf(guestHeight));
        values.put(KEY_VSYNC, bool(p.getBoolean(KEY_VSYNC, true)));
        values.put("carbon_gpu_bloom", bool(p.getBoolean(KEY_BLOOM, false)));
        values.put("achievement_toasts", "false");
        values.put("carbon_gpu_render_scale", String.valueOf(p.getInt(KEY_RENDER_SCALE, 100)));
        values.put("carbon_gpu_fps_cap", String.valueOf(p.getInt(KEY_FPS_CAP, 60)));
        values.put("carbon_gpu_reflection_faces", String.valueOf(p.getInt(KEY_REFLECTIONS, 0)));
        values.put("carbon_gpu_mirror_half_rate", bool(p.getBoolean(KEY_MIRROR_HALF, true)));
        // MSAA off by default: on Adreno the Xenos path is much faster at 1
        // sample. "native_2x_msaa" only gates the host's 2x attachment support.
        int msaaSamples = p.getInt(KEY_MSAA_SAMPLES, DEFAULT_MSAA_SAMPLES);
        values.put("gpu_sin_msaa", bool(msaaSamples < 2));
        values.put("gpu_msaa_muestras",
                String.valueOf(msaaSamples >= 4 ? 4 : (msaaSamples >= 2 ? 2 : 1)));
        values.put(KEY_MSAA, "true");
        // The merged-strip single frame only fits in the 10 MB emulated EDRAM
        // without MSAA and up to 720p (1280x720x8 bytes). Above that the merged
        // surface overflows EDRAM and the emulation thrashes - measured at
        // 1080p: 19.9 fps merged vs 29.8 split. So it is only enabled for
        // guests at 720p or lower.
        boolean singlePassFits = msaaSamples < 2
                && (long) guestWidth * guestHeight <= 1280L * 720L;
        values.put(KEY_SINGLE_PASS, bool(singlePassFits
                && p.getBoolean(KEY_SINGLE_PASS, true)));
        values.put(KEY_FXAA, quote(p.getString(KEY_FXAA, DEFAULT_FXAA)));
        values.put(KEY_UPSCALER, quote(p.getString(KEY_UPSCALER, DEFAULT_UPSCALER)));
        values.put(KEY_LETTERBOX, bool(p.getBoolean(KEY_LETTERBOX, true)));
        values.put(KEY_OCCLUSION, bool(p.getBoolean(KEY_OCCLUSION, true)));
        values.put(KEY_ANISO, String.valueOf(p.getInt(KEY_ANISO, 3)));
        // A 120 Hz guest vblank grid lets late frames flip after 8.3 ms steps
        // instead of 16.7 ms ones (measured in-race: ~21 -> ~24 fps), while
        // frame_rate_limit keeps light scenes at 60 fps.
        boolean finePacing = p.getBoolean(KEY_FINE_PACING, true);
        values.put("video_mode_refresh_rate", finePacing ? "120.0" : "60.0");
        values.put("frame_rate_limit", "60.0");

        // Keep textures and render targets resident instead of re-decoding and
        // re-uploading them (defaults are 384/768/24 MB which thrash at high
        // guest resolutions).
        values.put("texture_cache_memory_limit_render_to_texture", "96");
        values.put("texture_cache_memory_limit_soft", "768");
        values.put("texture_cache_memory_limit_hard", "1536");
        values.put("texture_cache_memory_limit_soft_lifetime", "120");

        values.put("game_data_root", quote(p.getString(KEY_ISO_PATH, "")));
        String driverPath = p.getString(KEY_DRIVER_LOADER, "");
        boolean driverIsLoader = driverPath.isEmpty()
                || DriverStore.exportsVulkanLoaderEntryPoint(new java.io.File(driverPath));
        // Full loaders are dlopen'ed directly; downloaded ICD drivers (turnip) go
        // through libadrenotools.
        values.put("vulkan_loader", quote(driverIsLoader ? driverPath : ""));
        values.put("vulkan_icd_driver", quote(driverIsLoader ? "" : driverPath));

        values.put("input_backend", quote("sdl"));
        values.put("mnk_mode", "false");
        values.put("mnk_mouse", "false");
        values.put("touch_pad", bool(p.getBoolean(KEY_TOUCH_ENABLED, true)));
        values.put("thread_affinity", quote("auto"));

        mergeToml(tomlFile(context), values);
    }

    private static String bool(boolean value) {
        return value ? "true" : "false";
    }

    private static String quote(String value) {
        String escaped = value.replace("\\", "\\\\").replace("\"", "\\\"");
        return "\"" + escaped + "\"";
    }

    private static void mergeToml(File file, Map<String, String> values) {
        List<String> lines = new ArrayList<>();
        if (file.exists()) {
            try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                    new FileInputStream(file), StandardCharsets.UTF_8))) {
                String line;
                while ((line = reader.readLine()) != null) {
                    lines.add(line);
                }
            } catch (IOException ignored) {
            }
        }

        for (Map.Entry<String, String> entry : values.entrySet()) {
            Pattern pattern = Pattern.compile("^\\s*" + Pattern.quote(entry.getKey()) + "\\s*=.*$");
            boolean replaced = false;
            for (int i = 0; i < lines.size(); i++) {
                Matcher matcher = pattern.matcher(lines.get(i));
                if (matcher.matches()) {
                    lines.set(i, entry.getKey() + " = " + entry.getValue());
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                lines.add(entry.getKey() + " = " + entry.getValue());
            }
        }

        File parent = file.getParentFile();
        if (parent != null && !parent.exists()) {
            //noinspection ResultOfMethodCallIgnored
            parent.mkdirs();
        }
        try (BufferedWriter writer = new BufferedWriter(new OutputStreamWriter(
                new FileOutputStream(file, false), StandardCharsets.UTF_8))) {
            for (String line : lines) {
                writer.write(line);
                writer.newLine();
            }
        } catch (IOException ignored) {
        }
    }

    public static boolean isValidGameData(Context context) {
        String path = prefs(context).getString(KEY_ISO_PATH, "");
        if (path == null || path.isEmpty()) {
            File internalGame = new File(context.getFilesDir(), "game");
            File externalGame = context.getExternalFilesDir(null) == null
                    ? internalGame : new File(context.getExternalFilesDir(null), "game");
            return new File(internalGame, "NFS").isDirectory()
                    || new File(externalGame, "NFS").isDirectory();
        }
        return new File(path).exists();
    }
}
