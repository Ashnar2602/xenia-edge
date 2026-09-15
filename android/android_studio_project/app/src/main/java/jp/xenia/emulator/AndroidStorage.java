package jp.xenia.emulator;

import android.content.Context;
import android.net.Uri;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.os.storage.StorageManager;
import android.os.storage.StorageVolume;
import android.provider.DocumentsContract;
import android.system.Os;
import java.io.File;
import java.io.IOException;

/** Resolve user-selected local storage. Never stages or copies game data. */
final class AndroidStorage {
    private static File cachedConfig;
    private static java.nio.file.attribute.FileTime cachedTime;
    private static long cachedSize;
    private static String[] cachedRoots;
    static synchronized File root(Context context, String folder) throws IOException {
        int index = folder.equals("content")  ? 0
                : folder.equals("cache_host") ? 1
                : folder.equals("app_log")    ? 2
                                              : -1;
        if (index < 0)
            return new File(context.getFilesDir(), folder);
        String value = options(context)[index];
        if (value.isEmpty())
            return new File(context.getFilesDir(), index == 2 ? "xenia.log" : folder);
        File path = new File(value);
        return (path.isAbsolute() ? path : new File(context.getFilesDir(), value))
                .getCanonicalFile();
    }

    static synchronized String[] options(Context context) throws IOException {
        android.util.AtomicFile config = new android.util.AtomicFile(
                new File(context.getFilesDir(), "xenia-edge.config.toml"));
        File file = config.getBaseFile();
        java.nio.file.attribute.BasicFileAttributes stat = file.exists()
                ? java.nio.file.Files.readAttributes(
                          file.toPath(), java.nio.file.attribute.BasicFileAttributes.class)
                : null;
        java.nio.file.attribute.FileTime time = stat == null ? null : stat.lastModifiedTime();
        long size = stat == null ? 0 : stat.size();
        if (!file.equals(cachedConfig) || !java.util.Objects.equals(time, cachedTime)
                || size != cachedSize || cachedRoots == null) {
            String text = stat == null
                    ? ""
                    : new String(config.readFully(), java.nio.charset.StandardCharsets.UTF_8);
            try {
                cachedRoots = GameSettings.rootsNative(text);
            } catch (IllegalArgumentException e) {
                throw new IOException(e.getMessage(), e);
            }
            cachedConfig = file;
            cachedTime = time;
            cachedSize = size;
        }
        return cachedRoots;
    }

    static void openDirectory(android.app.Activity activity, File folder) throws IOException {
        File dir = folder.getCanonicalFile();
        if (dir.toPath().startsWith(activity.getFilesDir().getCanonicalFile().toPath())) {
            String id = activity.getFilesDir().getCanonicalFile().equals(dir)
                    ? "root"
                    : activity.getFilesDir()
                              .getCanonicalFile()
                              .toPath()
                              .relativize(dir.toPath())
                              .toString();
            XeniaDocumentsProvider.open(activity, id);
            return;
        }
        StorageManager manager = activity.getSystemService(StorageManager.class);
        for (StorageVolume volume : manager.getStorageVolumes()) {
            File base = volume.getDirectory();
            if (base == null || !dir.toPath().startsWith(base.getCanonicalFile().toPath()))
                continue;
            String id = (volume.isPrimary() ? "primary" : volume.getUuid()) + ":"
                    + base.getCanonicalFile().toPath().relativize(dir.toPath());
            Uri uri =
                    DocumentsContract.buildDocumentUri("com.android.externalstorage.documents", id);
            activity.startActivity(new android.content.Intent(android.content.Intent.ACTION_VIEW)
                            .setDataAndType(uri, DocumentsContract.Document.MIME_TYPE_DIR)
                            .putExtra(DocumentsContract.EXTRA_INITIAL_URI, uri)
                            .addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION));
            return;
        }
        throw new IOException(activity.getString(R.string.local_storage_required));
    }

    static File file(Context context, Uri uri) throws IOException {
        requireAccess(context);
        if ("com.android.externalstorage.documents".equals(uri.getAuthority())) {
            return document(context, DocumentsContract.getDocumentId(uri));
        }
        // Downloads and other local providers may also expose a real file.
        try (ParcelFileDescriptor fd = context.getContentResolver().openFileDescriptor(uri, "r")) {
            if (fd != null) {
                File file = new File(Os.readlink("/proc/self/fd/" + fd.getFd()));
                if (file.isFile() && file.canRead())
                    return file.getCanonicalFile();
            }
        } catch (Exception e) {
            throw new IOException(context.getString(R.string.local_storage_required), e);
        }
        throw new IOException(context.getString(R.string.local_storage_required));
    }

    static File directory(Context context, Uri uri) throws IOException {
        requireAccess(context);
        if (!"com.android.externalstorage.documents".equals(uri.getAuthority()))
            throw new IOException(context.getString(R.string.local_storage_required));
        File dir = document(context, DocumentsContract.getTreeDocumentId(uri));
        if (!dir.isDirectory())
            throw new IOException(context.getString(R.string.file_unavailable));
        return dir;
    }

    private static void requireAccess(Context context) throws IOException {
        if (!Environment.isExternalStorageManager())
            throw new IOException(context.getString(R.string.storage_explanation));
    }

    private static File document(Context context, String id) throws IOException {
        int colon = id.indexOf(':');
        if (colon < 0)
            throw new IOException(context.getString(R.string.local_storage_required));
        String volumeId = id.substring(0, colon), relative = id.substring(colon + 1);
        StorageManager manager = context.getSystemService(StorageManager.class);
        for (StorageVolume volume : manager.getStorageVolumes()) {
            if (!(volume.isPrimary() && "primary".equals(volumeId))
                    && !volumeId.equalsIgnoreCase(volume.getUuid()))
                continue;
            File directory = volume.getDirectory();
            if (directory == null)
                continue;
            File root = directory.getCanonicalFile();
            File file = new File(root, relative).getCanonicalFile();
            if (!file.equals(root) && !file.getPath().startsWith(root.getPath() + File.separator))
                throw new IOException("Invalid document path");
            if (file.canRead())
                return file;
        }
        throw new IOException(context.getString(R.string.file_unavailable));
    }
}
