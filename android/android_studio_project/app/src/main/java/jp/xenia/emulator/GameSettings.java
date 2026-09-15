package jp.xenia.emulator;

import android.content.Context;
import android.util.AtomicFile;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/** Desktop-compatible per-title TOML; called only on the settings IO worker. */
final class GameSettings {
    static {
        System.loadLibrary("xenia-app");
    }
    static final java.util.concurrent.ExecutorService IO =
            java.util.concurrent.Executors.newSingleThreadExecutor();
    static native String[][] listNative(String global, String game, int scale, int fps);
    static native String[] rootsNative(String document);
    static native long[] historyNative(String contentRoot, String document);
    static native String[] buildNative();
    static native String editNative(String document, String name, String value);
    static native String resetNative(String document, String[] names);
    private final Context context;
    private final AtomicFile file;
    private final boolean global;
    GameSettings(Context context, int titleId) {
        this.context = context;
        global = titleId == 0;
        file = new AtomicFile(new File(context.getFilesDir(),
                global ? "xenia-edge.config.toml"
                       : String.format(Locale.ROOT, "config/%08X.config.toml", titleId)));
    }
    private static String read(AtomicFile file) throws java.io.IOException {
        if (!file.getBaseFile().exists()
                && !new File(file.getBaseFile().getPath() + ".bak").exists())
            return "";
        return new String(file.readFully(), StandardCharsets.UTF_8);
    }
    String[][] load() throws java.io.IOException {
        android.content.SharedPreferences prefs =
                context.getSharedPreferences("android_settings", 0);
        String base =
                read(new AtomicFile(new File(context.getFilesDir(), "xenia-edge.config.toml")));
        if (global) {
            base = legacyVideo(base);
            return listNative("", base, -1, -1);
        }
        return java.util.Arrays
                .stream(listNative(base, read(file), prefs.getInt("resolution", -1),
                        prefs.getInt("fps_limit", -1)))
                .filter(row -> !globalOnly(row[0]))
                .toArray(String[][] ::new);
    }
    private String legacyVideo(String document) {
        android.content.SharedPreferences prefs =
                context.getSharedPreferences("android_settings", 0);
        if (prefs.contains("resolution")) {
            String scale =
                    Integer.toString(Math.max(1, Math.min(2, prefs.getInt("resolution", 1))));
            document = editNative(document, "draw_resolution_scale_x", scale);
            document = editNative(document, "draw_resolution_scale_y", scale);
        }
        if (prefs.contains("fps_limit"))
            document = editNative(document, "framerate_limit",
                    Integer.toString(Math.max(0, prefs.getInt("fps_limit", 0))));
        return document;
    }
    private static boolean globalOnly(String name) {
        return name.equals("content_root") || name.equals("cache_root") || name.equals("log_file")
                || name.equals("ui_locale");
    }
    void save(String name, String value) throws java.io.IOException {
        if (!global && globalOnly(name))
            throw new java.io.IOException(context.getString(R.string.global_only_option));
        write(document -> editNative(document, name, value));
    }
    void reset() throws java.io.IOException {
        write(document
                -> resetNative(document,
                        java.util.Arrays.stream(listNative("", document, -1, -1))
                                .map(row -> row[0])
                                .filter(name -> global || !globalOnly(name))
                                .toArray(String[] ::new)));
    }
    private void write(java.util.function.UnaryOperator<String> edit) throws java.io.IOException {
        try (DataLock lock = global ? new DataLock(context.getFilesDir()) : null) {
            // Read-modify-write preserves other options, including unknown desktop
            // entries. AtomicFile keeps the old file if writing fails.
            String document = read(file);
            String updated = edit.apply(global ? legacyVideo(document) : document);
            FileOutputStream stream = null;
            try {
                stream = file.startWrite();
                stream.write(updated.getBytes(StandardCharsets.UTF_8));
                file.finishWrite(stream);
                if (global)
                    context.getSharedPreferences("android_settings", 0)
                            .edit()
                            .remove("resolution")
                            .remove("fps_limit")
                            .commit();
            } catch (java.io.IOException e) {
                file.failWrite(stream);
                throw e;
            }
        }
    }
}
