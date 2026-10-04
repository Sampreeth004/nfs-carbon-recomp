package com.eagames.nfscarbon;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import java.util.HashMap;
import java.util.Map;

public final class TouchIcons {
    private static final String[] NAMES = {
            "steering", "accelerate", "brake", "handbrake", "pause", "rear_view", "view360"
    };

    private TouchIcons() {
    }

    public static Map<String, Bitmap> load(Context context) {
        Map<String, Bitmap> icons = new HashMap<>();
        for (String name : NAMES) {
            int id = context.getResources().getIdentifier(
                    "touch_icon_" + name, "drawable", context.getPackageName());
            if (id == 0) {
                continue;
            }
            Bitmap bitmap = BitmapFactory.decodeResource(context.getResources(), id);
            if (bitmap != null) {
                icons.put(name, bitmap);
            }
        }
        return icons;
    }
}
