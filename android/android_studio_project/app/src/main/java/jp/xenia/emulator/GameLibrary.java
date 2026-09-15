package jp.xenia.emulator;

import android.content.Context;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;
import org.json.JSONArray;
import org.json.JSONObject;

final class GameLibrary {
    static final class Game {
        String id, name, path, format;
        long played, metadataStamp;
        int titleId, version, disc, compatibility, mediaId, baseVersion, discCount;
        boolean preferred, metadataCached;
        android.content.Intent identify(android.content.Intent intent) {
            return intent.putExtra("game_path", path).putExtra("game_name", name)
                    .putExtra("title_id", titleId).putExtra("version", version);
        }
        void metadata(int[] data, long stamp) {
            titleId = data[0];
            mediaId = data[1];
            version = data[2];
            baseVersion = data[3];
            disc = data[4];
            discCount = data[5];
            metadataStamp = stamp;
            metadataCached = true;
        }
        String releaseKey() {
            return titleId == 0 ? id : titleId + ":" + version;
        }
        Game(String name, String path, String format) {
            id = UUID.randomUUID().toString();
            this.name = name;
            this.path = path;
            this.format = format;
        }
    }
    private final SharedPreferences prefs;
    final List<Game> games = new ArrayList<>();
    GameLibrary(Context c) {
        prefs = c.getSharedPreferences("android_library", Context.MODE_PRIVATE);
        try {
            JSONArray data = new JSONArray(prefs.getString("games", "[]"));
            for (int i = 0; i < data.length(); ++i) {
                JSONObject o = data.getJSONObject(i);
                Game g = new Game(o.getString("name"), o.getString("path"), o.getString("format"));
                g.id = o.getString("id");
                g.played = o.optLong("played");
                g.titleId = o.optInt("title_id");
                g.version = o.optInt("version");
                g.disc = o.optInt("disc");
                g.mediaId = o.optInt("media_id");
                g.baseVersion = o.optInt("base_version");
                g.discCount = o.optInt("disc_count");
                g.metadataCached = o.optBoolean("metadata_cached");
                g.compatibility = o.optInt("compatibility");
                g.metadataStamp = o.optLong("metadata_stamp");
                g.preferred = o.optBoolean("preferred");
                games.add(g);
            }
        } catch (org.json.JSONException ignored) {
        }
    }
    void save() {
        JSONArray data = new JSONArray();
        try {
            for (Game g : games) {
                JSONObject o = new JSONObject();
                o.put("id", g.id);
                o.put("name", g.name);
                o.put("path", g.path);
                o.put("format", g.format);
                o.put("played", g.played);
                o.put("title_id", g.titleId);
                o.put("version", g.version);
                o.put("disc", g.disc);
                o.put("media_id", g.mediaId);
                o.put("base_version", g.baseVersion);
                o.put("disc_count", g.discCount);
                o.put("metadata_cached", g.metadataCached);
                o.put("compatibility", g.compatibility);
                o.put("metadata_stamp", g.metadataStamp);
                o.put("preferred", g.preferred);
                data.put(o);
            }
        } catch (org.json.JSONException e) {
            throw new IllegalStateException(e);
        }
        prefs.edit().putString("games", data.toString()).apply();
    }
    void add(Game game) {
        addAll(java.util.Collections.singletonList(game));
    }
    void addAll(List<Game> additions) {
        java.util.HashSet<String> paths = new java.util.HashSet<>();
        for (Game game : games) paths.add(game.path);
        boolean changed = false;
        for (Game game : additions)
            if (paths.add(game.path)) {
                games.add(game);
                changed = true;
            }
        if (changed)
            save();
    }
    java.util.Map<String, List<Game>> groups() {
        java.util.Map<String, List<Game>> groups = new java.util.LinkedHashMap<>();
        for (Game game : games)
            groups.computeIfAbsent(game.releaseKey(), key -> new ArrayList<>()).add(game);
        return groups;
    }
    List<Game> group(Game game) {
        List<Game> result = new ArrayList<>();
        for (Game g : games)
            if (g.releaseKey().equals(game.releaseKey()))
                result.add(g);
        result.sort(
                java.util.Comparator.comparingInt((Game g) -> g.disc).thenComparing(g -> g.path));
        return result;
    }
    List<Game> releases() {
        java.util.LinkedHashMap<String, Game> result = new java.util.LinkedHashMap<>();
        for (Game game : games) {
            Game current = result.get(game.releaseKey());
            if (current == null || game.preferred
                    || (!current.preferred && game.disc < current.disc))
                result.put(game.releaseKey(), game);
        }
        return new ArrayList<>(result.values());
    }
    long lastPlayed(Game game) {
        long played = 0;
        for (Game g : games)
            if (g.releaseKey().equals(game.releaseKey()))
                played = Math.max(played, g.played);
        // Dashboard timestamps identify a title, not a particular release.
        if (game.titleId != 0 && games.stream().noneMatch(
                    g -> g.titleId == game.titleId && g.version != game.version))
            played = Math.max(played, prefs.getLong("history_" + game.titleId, 0));
        return played;
    }
    void replace(Game game, String path, String format, int[] metadata, long stamp) {
        // A newly selected path may already be indexed. Keep one disc entry and
        // retain the user's name, history and default selection.
        for (Game other : games)
            if (other != game && other.path.equals(path)) {
                game.played = Math.max(game.played, other.played);
                game.preferred |= other.preferred;
            }
        games.removeIf(other -> other != game && other.path.equals(path));
        game.path = path;
        game.format = format;
        if (metadata != null && metadata.length >= 6) game.metadata(metadata, stamp);
        else game.metadataCached = false;
        save();
    }
    void history(long[] rows) {
        SharedPreferences.Editor edit = prefs.edit();
        for (String key : prefs.getAll().keySet())
            if (key.startsWith("history_")) edit.remove(key);
        for (int i = 0; i + 1 < rows.length; i += 2)
            edit.putLong("history_" + (int) rows[i], rows[i + 1]);
        edit.apply();
    }
    static long metadataStamp(File file) {
        return file.lastModified() ^ file.length();
    }
    static String format(File file) throws IOException {
        String name = file.getName(), lower = name.toLowerCase(Locale.ROOT), format = null;
        if (lower.endsWith(".xex"))
            format = "XEX";
        else if (lower.endsWith(".elf"))
            format = "ELF";
        else if (lower.endsWith(".iso"))
            format = "ISO";
        else if (lower.endsWith(".zar"))
            format = "ZAR";
        else {
            try (FileInputStream in = new FileInputStream(file)) {
                byte[] magic = new byte[4];
                if (in.read(magic) == 4) {
                    String s = new String(magic, java.nio.charset.StandardCharsets.US_ASCII);
                    if (s.equals("LIVE") || s.equals("PIRS") || s.equals("CON "))
                        format = "STFS / GOD";
                }
            }
        }
        return format;
    }
    static String displayName(Context c, Uri uri) throws IOException {
        try (Cursor cursor = c.getContentResolver().query(
                     uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst())
                return cursor.getString(0);
        }
        throw new IOException(c.getString(R.string.file_unavailable));
    }
    interface Progress {
        void update(String name, long bytes);
    }

    static List<Game> importTree(Context c, Uri tree, AtomicBoolean cancel, Progress progress)
            throws IOException {
        File root = AndroidStorage.directory(c, tree);
        List<Game> result = new ArrayList<>();
        scan(root, result, 0, cancel, progress, new java.util.HashSet<>());
        if (result.isEmpty())
            throw new IOException(c.getString(R.string.no_executables));
        return result;
    }
    private static void scan(File dir, List<Game> games, int depth, AtomicBoolean cancel,
            Progress progress, java.util.Set<String> visited) throws IOException {
        if (depth > 64 || !visited.add(dir.getCanonicalPath()))
            return;
        if (cancel.get())
            throw new IOException("Scan cancelled");
        progress.update(dir.getName(), 0);
        File[] files = dir.listFiles();
        if (files == null)
            throw new IOException("Cannot read game folder");
        for (File f : files) {
            if (f.isDirectory()) {
                scan(f, games, depth + 1, cancel, progress, visited);
                continue;
            }
            String name = f.getName(), lower = name.toLowerCase(Locale.ROOT), format = format(f);
            if (format != null) {
                String title = lower.equals("default.xex") ? dir.getName()
                                                           : name.replaceFirst("\\.[^.]+$", "");
                games.add(new Game(title, f.getAbsolutePath(), format));
            }
        }
    }
}
