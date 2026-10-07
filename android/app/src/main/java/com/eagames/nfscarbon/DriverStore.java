package com.eagames.nfscarbon;

import android.content.Context;
import android.net.Uri;

import org.json.JSONObject;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

public final class DriverStore {
    public static final String SYSTEM_ID = "";

    public static final class Driver {
        public final String id;
        public final File directory;
        public final String label;
        public final File loader;
        public final boolean isFullLoader;

        Driver(String id, File directory, String label, File loader, boolean isFullLoader) {
            this.id = id;
            this.directory = directory;
            this.label = label;
            this.loader = loader;
            this.isFullLoader = isFullLoader;
        }

        @Override
        public String toString() {
            return label;
        }
    }

    private DriverStore() {
    }

    public static File driversDir(Context context) {
        return new File(context.getFilesDir(), "drivers");
    }

    public static List<Driver> list(Context context) {
        List<Driver> drivers = new ArrayList<>();
        File[] dirs = driversDir(context).listFiles();
        if (dirs == null) {
            return drivers;
        }
        for (File dir : dirs) {
            if (!dir.isDirectory()) {
                continue;
            }
            File loader = findLoader(dir);
            if (loader != null) {
                String label = metaName(loader.getParentFile());
                if (label == null || label.isEmpty()) {
                    label = dir.getName();
                }
                drivers.add(new Driver(dir.getName(), dir, label, loader,
                        exportsVulkanLoaderEntryPoint(loader)));
            }
        }
        return drivers;
    }

    public static File findLoader(File dir) {
        File found = findLoaderIn(dir);
        if (found != null) {
            return found;
        }
        // Some packages wrap their files in a single folder.
        File[] children = dir.listFiles();
        if (children != null) {
            for (File child : children) {
                if (child.isDirectory() && !child.getName().equals("tmp")) {
                    found = findLoaderIn(child);
                    if (found != null) {
                        return found;
                    }
                }
            }
        }
        return null;
    }

    private static File findLoaderIn(File dir) {
        File preferred = new File(dir, "libvulkan.so");
        if (preferred.isFile()) {
            return preferred;
        }
        File meta = new File(dir, "meta.json");
        String metaLibrary = metaLibraryName(meta);
        if (metaLibrary != null) {
            File candidate = new File(dir, metaLibrary);
            if (candidate.isFile()) {
                return candidate;
            }
        }
        File[] files = dir.listFiles();
        if (files == null) {
            return null;
        }
        for (File file : files) {
            if (file.isFile() && file.getName().toLowerCase(Locale.ROOT).endsWith(".so")) {
                return file;
            }
        }
        return null;
    }

    public static Driver importFromUri(Context context, Uri uri, String displayName) {
        String safe = sanitize(displayName);
        if (safe.isEmpty()) {
            safe = "driver";
        }
        File destination = new File(driversDir(context), safe + "-" + System.currentTimeMillis());
        //noinspection ResultOfMethodCallIgnored
        destination.mkdirs();

        boolean isZip = safe.toLowerCase(Locale.ROOT).endsWith(".zip");
        boolean ok;
        if (isZip) {
            ok = extractZip(context, uri, destination);
        } else {
            ok = copySingle(context, uri, new File(destination,
                    safe.toLowerCase(Locale.ROOT).endsWith(".so") ? safe : safe + ".so"));
        }

        File loader = findLoader(destination);
        if (!ok || loader == null) {
            StorageUtil.deleteRecursive(destination);
            return null;
        }
        String label = metaName(loader.getParentFile());
        if (label == null || label.isEmpty()) {
            label = destination.getName();
        }
        return new Driver(destination.getName(), destination, label, loader,
                exportsVulkanLoaderEntryPoint(loader));
    }

    public static void remove(Driver driver) {
        StorageUtil.deleteRecursive(driver.directory);
    }

    private static String sanitize(String name) {
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < name.length(); i++) {
            char c = name.charAt(i);
            if (Character.isLetterOrDigit(c) || c == '.' || c == '_' || c == '-') {
                out.append(c);
            } else if (c == ' ') {
                out.append('_');
            }
        }
        return out.toString();
    }

    private static boolean extractZip(Context context, Uri uri, File destination) {
        try (InputStream input = context.getContentResolver().openInputStream(uri);
             ZipInputStream zip = new ZipInputStream(input)) {
            if (input == null) {
                return false;
            }
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                String name = entry.getName().replace('\\', '/');
                if (name.contains("..") || entry.isDirectory()) {
                    continue;
                }
                File out = new File(destination, name);
                File parent = out.getParentFile();
                if (parent != null && !parent.exists()) {
                    //noinspection ResultOfMethodCallIgnored
                    parent.mkdirs();
                }
                try (FileOutputStream output = new FileOutputStream(out)) {
                    byte[] buffer = new byte[256 * 1024];
                    int read;
                    while ((read = zip.read(buffer)) > 0) {
                        output.write(buffer, 0, read);
                    }
                }
            }
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private static boolean copySingle(Context context, Uri uri, File destination) {
        try (InputStream input = context.getContentResolver().openInputStream(uri);
             FileOutputStream output = new FileOutputStream(destination)) {
            if (input == null) {
                return false;
            }
            byte[] buffer = new byte[256 * 1024];
            int read;
            while ((read = input.read(buffer)) > 0) {
                output.write(buffer, 0, read);
            }
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    private static String metaName(File dir) {
        try {
            File meta = new File(dir, "meta.json");
            if (!meta.isFile()) {
                return null;
            }
            byte[] data = new byte[(int) Math.min(meta.length(), 16384)];
            try (InputStream input = new java.io.FileInputStream(meta)) {
                int offset = 0;
                int read;
                while (offset < data.length && (read = input.read(data, offset,
                        data.length - offset)) > 0) {
                    offset += read;
                }
            }
            JSONObject json = new JSONObject(new String(data, "UTF-8"));
            String name = json.optString("name", "");
            if (!name.isEmpty()) {
                return name;
            }
            return json.optString("libraryName", null);
        } catch (Exception e) {
            return null;
        }
    }

    private static String metaLibraryName(File meta) {
        try {
            if (!meta.isFile()) {
                return null;
            }
            byte[] data = new byte[(int) Math.min(meta.length(), 16384)];
            try (InputStream input = new java.io.FileInputStream(meta)) {
                int offset = 0;
                int read;
                while (offset < data.length && (read = input.read(data, offset,
                        data.length - offset)) > 0) {
                    offset += read;
                }
            }
            JSONObject json = new JSONObject(new String(data, "UTF-8"));
            String library = json.optString("libraryName", "");
            if (library.isEmpty()) {
                library = json.optString("library_path", "");
            }
            return library.isEmpty() ? null : library;
        } catch (Exception e) {
            return null;
        }
    }

    /**
     * True when the library is a full Vulkan loader (exports vkGetInstanceProcAddr)
     * and can be dlopen'ed directly. Turnip/Adreno packages are ICD modules instead;
     * those are opened through libadrenotools (arm64 only).
     */
    public static boolean exportsVulkanLoaderEntryPoint(File library) {
        RandomAccessFile file = null;
        try {
            file = new RandomAccessFile(library, "r");
            byte[] header = new byte[64];
            if (file.read(header) != 64) {
                return true;
            }
            if (header[0] != 0x7F || header[1] != 'E' || header[2] != 'L' || header[3] != 'F') {
                return true;
            }
            if (header[4] != 2 || header[5] != 1) {
                return true;
            }
            long sectionOffset = readLong(header, 40);
            int sectionEntrySize = readShort(header, 58);
            int sectionCount = readShort(header, 60);
            if (sectionEntrySize < 64 || sectionCount <= 0) {
                return true;
            }

            long dynsymOffset = -1;
            long dynsymSize = 0;
            long dynsymEntrySize = 24;
            int dynsymLink = 0;
            byte[] section = new byte[sectionEntrySize];
            for (int i = 0; i < sectionCount; i++) {
                file.seek(sectionOffset + (long) i * sectionEntrySize);
                if (file.read(section) != sectionEntrySize) {
                    return true;
                }
                int type = (int) readInt(section, 4);
                if (type == 11) {
                    dynsymOffset = readLong(section, 24);
                    dynsymSize = readLong(section, 32);
                    dynsymLink = (int) readInt(section, 40);
                    dynsymEntrySize = readLong(section, 56);
                    break;
                }
            }
            if (dynsymOffset < 0 || dynsymSize <= 0) {
                return false;
            }
            if (dynsymEntrySize <= 0) {
                dynsymEntrySize = 24;
            }

            file.seek(sectionOffset + (long) dynsymLink * sectionEntrySize);
            if (file.read(section) < sectionEntrySize) {
                return true;
            }
            long stringOffset = readLong(section, 24);
            long stringSize = readLong(section, 32);
            if (stringSize <= 0 || stringSize > 16 * 1024 * 1024) {
                return true;
            }
            byte[] strings = new byte[(int) stringSize];
            file.seek(stringOffset);
            if (file.read(strings) != strings.length) {
                return true;
            }

            long symbolCount = dynsymSize / dynsymEntrySize;
            byte[] symbol = new byte[(int) dynsymEntrySize];
            for (long i = 0; i < symbolCount; i++) {
                file.seek(dynsymOffset + i * dynsymEntrySize);
                if (file.read(symbol) != symbol.length) {
                    return true;
                }
                int nameOffset = (int) readInt(symbol, 0);
                if (nameOffset <= 0 || nameOffset >= strings.length) {
                    continue;
                }
                int end = nameOffset;
                while (end < strings.length && strings[end] != 0) {
                    end++;
                }
                String name = new String(strings, nameOffset, end - nameOffset, "US-ASCII");
                if ("vkGetInstanceProcAddr".equals(name)) {
                    return true;
                }
            }
            return false;
        } catch (Exception e) {
            return true;
        } finally {
            if (file != null) {
                try {
                    file.close();
                } catch (Exception ignored) {
                }
            }
        }
    }

    private static long readLong(byte[] data, int offset) {
        return ((long) (data[offset] & 0xFF))
                | ((long) (data[offset + 1] & 0xFF) << 8)
                | ((long) (data[offset + 2] & 0xFF) << 16)
                | ((long) (data[offset + 3] & 0xFF) << 24)
                | ((long) (data[offset + 4] & 0xFF) << 32)
                | ((long) (data[offset + 5] & 0xFF) << 40)
                | ((long) (data[offset + 6] & 0xFF) << 48)
                | ((long) (data[offset + 7] & 0xFF) << 56);
    }

    private static long readInt(byte[] data, int offset) {
        return ((long) (data[offset] & 0xFF))
                | ((long) (data[offset + 1] & 0xFF) << 8)
                | ((long) (data[offset + 2] & 0xFF) << 16)
                | ((long) (data[offset + 3] & 0xFF) << 24);
    }

    private static int readShort(byte[] data, int offset) {
        return (data[offset] & 0xFF) | ((data[offset + 1] & 0xFF) << 8);
    }
}
