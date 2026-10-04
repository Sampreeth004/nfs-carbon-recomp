package com.eagames.nfscarbon;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.OpenableColumns;
import android.provider.Settings;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

public final class StorageUtil {
    public static final int REQUEST_ALL_FILES = 1001;
    public static final int REQUEST_PICK_ISO = 1002;
    public static final int REQUEST_PICK_DRIVER = 1003;

    public interface Progress {
        /** @return false to cancel the copy. */
        boolean onProgress(long copied, long total);
    }

    private StorageUtil() {
    }

    public static boolean hasAllFilesAccess(Context context) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            return Environment.isExternalStorageManager();
        }
        return true;
    }

    public static void requestAllFilesAccess(Activity activity) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R
                || Environment.isExternalStorageManager()) {
            return;
        }
        Intent appIntent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                Uri.parse("package:" + activity.getPackageName()));
        try {
            activity.startActivityForResult(appIntent, REQUEST_ALL_FILES);
        } catch (Exception e) {
            activity.startActivityForResult(
                    new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION),
                    REQUEST_ALL_FILES);
        }
    }

    public static String displayName(Context context, Uri uri) {
        String name = null;
        if ("content".equals(uri.getScheme())) {
            try (Cursor cursor = context.getContentResolver()
                    .query(uri, null, null, null, null)) {
                if (cursor != null && cursor.moveToFirst()) {
                    int index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                    if (index >= 0) {
                        name = cursor.getString(index);
                    }
                }
            } catch (Exception ignored) {
            }
        }
        if (name == null) {
            name = uri.getLastPathSegment();
        }
        return name == null ? "file" : name;
    }

    public static long fileSize(Context context, Uri uri) {
        if ("content".equals(uri.getScheme())) {
            try (Cursor cursor = context.getContentResolver()
                    .query(uri, null, null, null, null)) {
                if (cursor != null && cursor.moveToFirst()) {
                    int index = cursor.getColumnIndex(OpenableColumns.SIZE);
                    if (index >= 0 && !cursor.isNull(index)) {
                        return cursor.getLong(index);
                    }
                }
            } catch (Exception ignored) {
            }
        }
        try {
            File file = new File(uri.getPath() == null ? "" : uri.getPath());
            if (file.exists()) {
                return file.length();
            }
        } catch (Exception ignored) {
        }
        return -1;
    }

    public static boolean copyToFile(Context context, Uri uri, File destination, Progress progress) {
        InputStream input = null;
        FileOutputStream output = null;
        try {
            input = context.getContentResolver().openInputStream(uri);
            if (input == null) {
                return false;
            }
            long total = fileSize(context, uri);
            File parent = destination.getParentFile();
            if (parent != null && !parent.exists()) {
                //noinspection ResultOfMethodCallIgnored
                parent.mkdirs();
            }
            output = new FileOutputStream(destination);
            byte[] buffer = new byte[256 * 1024];
            long copied = 0;
            int read;
            while ((read = input.read(buffer)) > 0) {
                output.write(buffer, 0, read);
                copied += read;
                if (progress != null && !progress.onProgress(copied, total)) {
                    return false;
                }
            }
            output.flush();
            return true;
        } catch (Exception e) {
            return false;
        } finally {
            try {
                if (output != null) {
                    output.close();
                }
            } catch (IOException ignored) {
            }
            try {
                if (input != null) {
                    input.close();
                }
            } catch (IOException ignored) {
            }
        }
    }

    public static List<File> findIsos(File root, int maxDepth, int limit) {
        List<File> results = new ArrayList<>();
        scan(root, maxDepth, limit, results);
        return results;
    }

    private static void scan(File dir, int depth, int limit, List<File> results) {
        if (dir == null || depth < 0 || results.size() >= limit) {
            return;
        }
        File[] children = dir.listFiles();
        if (children == null) {
            return;
        }
        for (File child : children) {
            if (results.size() >= limit) {
                return;
            }
            if (child.isDirectory()) {
                if (!child.getName().startsWith(".")) {
                    scan(child, depth - 1, limit, results);
                }
            } else if (child.getName().toLowerCase(Locale.ROOT).endsWith(".iso")) {
                results.add(child);
            }
        }
    }

    public static boolean deleteRecursive(File file) {
        if (file == null || !file.exists()) {
            return true;
        }
        if (file.isDirectory()) {
            File[] children = file.listFiles();
            if (children != null) {
                for (File child : children) {
                    deleteRecursive(child);
                }
            }
        }
        return file.delete();
    }
}
