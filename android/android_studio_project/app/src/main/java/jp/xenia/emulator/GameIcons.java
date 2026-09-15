package jp.xenia.emulator;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.util.AtomicFile;
import android.util.LruCache;
import android.view.View;
import android.widget.ImageView;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.ref.WeakReference;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** Serialized, bounded artwork extraction. Game files are only ever opened for reading. */
final class GameIcons implements AutoCloseable {
    static native int compatibilityNative(int title);

    private final Context context;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    // Only touched on the UI thread. Weak targets don't retain discarded screens.
    private final Map<String, List<Target>> pending = new HashMap<>();
    // Only touched by the worker; fingerprint changes invalidate both cache layers.
    private final LruCache<String, Bitmap> memory = new LruCache<String, Bitmap>(8 * 1024 * 1024) {
        @Override
        protected int sizeOf(String key, Bitmap value) {
            return value.getByteCount();
        }
    };
    private boolean closed;
    private int cacheWrites;

    GameIcons(Context context) {
        this.context = context.getApplicationContext();
    }

    static native byte[] extractNative(String path);
    static native int[] metadataNative(String path);
    static int[] readMetadata(String path) {
        return Native.metadata(path);
    }
    private static class Native {
        static int[] metadata(String path) {
            return metadataNative(path);
        }
        static {
            System.loadLibrary("xenia-app");
        }
        static byte[] read(String path) {
            return extractNative(path);
        }
    }
    private static class Target {
        final WeakReference<ImageView> image;
        final WeakReference<View> fallback;
        Target(ImageView image, View fallback) {
            this.image = new WeakReference<>(image);
            this.fallback = new WeakReference<>(fallback);
        }
    }
    void bind(String path, ImageView image, View fallback) {
        if (closed)
            return;
        image.setTag(path);
        Target target = new Target(image, fallback);
        List<Target> targets = pending.get(path);
        if (targets != null) {
            targets.add(target);
            return;
        }
        targets = new ArrayList<>();
        targets.add(target);
        pending.put(path, targets);
        worker.execute(() -> {
            Bitmap bitmap = load(path);
            main.post(() -> {
                List<Target> waiting = pending.remove(path);
                if (closed || waiting == null || bitmap == null)
                    return;
                for (Target t : waiting) {
                    ImageView view = t.image.get();
                    View placeholder = t.fallback.get();
                    if (view != null && path.equals(view.getTag())) {
                        view.setImageBitmap(bitmap);
                        if (placeholder != null)
                            placeholder.setVisibility(View.INVISIBLE);
                    }
                }
            });
        });
    }
    private Bitmap load(String path) {
        try {
            File game = path.startsWith("content:") ? AndroidStorage.file(context, Uri.parse(path))
                                                    : new File(path);
            if (!game.isFile() || !game.canRead())
                return null;
            String fingerprint = "1\n" + game.getCanonicalPath() + "\n" + game.length() + "\n"
                    + game.lastModified();
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(
                    fingerprint.getBytes(StandardCharsets.UTF_8));
            StringBuilder key = new StringBuilder();
            for (byte b : digest) key.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
            String id = key.toString();
            Bitmap bitmap = memory.get(id);
            if (bitmap != null)
                return bitmap;
            File dir = new File(context.getCacheDir(), "game-icons");
            File cached = new File(dir, id + ".png");
            if (cached.isFile() && cached.length() <= 4 * 1024 * 1024) {
                byte[] data = Files.readAllBytes(cached.toPath());
                if (data.length == 0)
                    return null; // Cached absence of embedded artwork.
                bitmap = decode(data);
                if (bitmap != null) {
                    memory.put(id, bitmap);
                    return bitmap;
                }
            }
            byte[] data = Native.read(game.getAbsolutePath());
            bitmap = data == null ? null : decode(data);
            if (bitmap != null)
                memory.put(id, bitmap);
            // Cache only validated images or absence. Cache failures do not hide an icon.
            try {
                if (dir.isDirectory() || dir.mkdirs()) {
                    if ((cacheWrites++ & 31) == 0)
                        trim(dir);
                    AtomicFile file = new AtomicFile(cached);
                    FileOutputStream output = null;
                    try {
                        output = file.startWrite();
                        if (bitmap != null)
                            output.write(data);
                        file.finishWrite(output);
                    } catch (Exception e) {
                        file.failWrite(output);
                    }
                }
            } catch (Exception ignored) {
            }
            return bitmap;
        } catch (Exception | LinkageError e) {
            return null;
        }
    }
    static Bitmap decode(byte[] data) {
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(data, 0, data.length, bounds);
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0 || bounds.outWidth > 2048
                || bounds.outHeight > 2048)
            return null;
        bounds.inJustDecodeBounds = false;
        bounds.inSampleSize = 1;
        while (Math.max(bounds.outWidth, bounds.outHeight) / bounds.inSampleSize > 256)
            bounds.inSampleSize *= 2;
        return BitmapFactory.decodeByteArray(data, 0, data.length, bounds);
    }
    private static void trim(File dir) {
        File[] files = dir.listFiles();
        if (files == null)
            return;
        long bytes = 0;
        for (File file : files) bytes += file.length();
        if (bytes < 32 * 1024 * 1024 && files.length < 1024)
            return;
        java.util.Arrays.sort(files, java.util.Comparator.comparingLong(File::lastModified));
        int count = files.length;
        for (File file : files) {
            long size = file.length();
            if (file.delete()) {
                bytes -= size;
                --count;
            }
            if (bytes < 24 * 1024 * 1024 && count < 768)
                break;
        }
    }
    @Override
    public void close() {
        closed = true;
        pending.clear();
        worker.shutdownNow();
    }
}
