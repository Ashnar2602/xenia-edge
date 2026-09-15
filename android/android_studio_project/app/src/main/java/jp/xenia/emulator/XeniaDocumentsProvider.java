package jp.xenia.emulator;

import android.app.Activity;
import android.content.Intent;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.file.Files;

/** Gives Android's Files UI access to emulator data, without exposing arbitrary paths. */
public class XeniaDocumentsProvider extends DocumentsProvider {
    private static final String[] ROOT_COLUMNS = {Root.COLUMN_ROOT_ID, Root.COLUMN_DOCUMENT_ID,
            Root.COLUMN_TITLE, Root.COLUMN_FLAGS, Root.COLUMN_ICON, Root.COLUMN_MIME_TYPES};
    private static final String[] DOCUMENT_COLUMNS = {Document.COLUMN_DOCUMENT_ID,
            Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE, Document.COLUMN_FLAGS,
            Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED};
    private static final String[] FOLDERS = {
            "content", "config", "patches", "cache_host", "screenshots", "plugins", "traces"};
    static void open(Activity activity, String folder) {
        for (String root : FOLDERS) {
            try {
                AndroidStorage.root(activity, root).mkdirs();
            } catch (IOException ignored) {
            }
        }
        android.net.Uri uri = DocumentsContract.buildDocumentUri(
                activity.getPackageName() + ".documents", folder);
        Intent intent =
                new Intent(Intent.ACTION_VIEW)
                        .setDataAndType(folder.equals("root")
                                        ? DocumentsContract.buildRootUri(
                                                  activity.getPackageName() + ".documents", "xenia")
                                        : uri,
                                folder.equals("root") ? Root.MIME_TYPE_ITEM
                                                      : Document.MIME_TYPE_DIR)
                        .putExtra(DocumentsContract.EXTRA_INITIAL_URI, uri)
                        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION
                                | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        android.content.pm.ResolveInfo files = activity.getPackageManager().resolveActivity(
                new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE),
                android.content.pm.PackageManager.MATCH_SYSTEM_ONLY);
        if (files != null)
            intent.setPackage(files.activityInfo.packageName);
        try {
            activity.startActivity(intent);
        } catch (android.content.ActivityNotFoundException e) {
            activity.startActivity(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
                            .putExtra(DocumentsContract.EXTRA_INITIAL_URI, uri));
        }
    }
    @Override
    public boolean onCreate() {
        return true;
    }
    @Override
    public android.os.Bundle call(String method, String argument, android.os.Bundle extras) {
        if (!"register_disc".equals(method))
            return super.call(method, argument, extras);
        if (android.os.Binder.getCallingUid() != android.os.Process.myUid())
            throw new SecurityException("Only Xenia may register games");
        try {
            File file = new File(argument).getCanonicalFile();
            String format = GameLibrary.format(file);
            if (format == null)
                throw new IllegalArgumentException("Unsupported game file");
            // Run in the library process: SharedPreferences is not multiprocess-safe.
            new GameLibrary(getContext())
                    .add(new GameLibrary.Game(file.getName(), file.getPath(), format));
            return android.os.Bundle.EMPTY;
        } catch (IOException e) {
            throw new IllegalArgumentException(e.getMessage(), e);
        }
    }
    private File resolve(String id) throws FileNotFoundException {
        File root = getContext().getFilesDir();
        if (id.equals("root"))
            return root;
        for (String part : id.split("/", -1))
            if (part.isEmpty() || part.equals(".") || part.equals("..") || part.contains("\\"))
                throw new FileNotFoundException("Invalid document path");
        String top = id.split("/", 2)[0];
        if (id.equals("xenia-edge.config.toml"))
            return new File(root, id);
        if (id.equals("app_log")) {
            try {
                return AndroidStorage.root(getContext(), id);
            } catch (IOException e) {
                throw new FileNotFoundException(e.getMessage());
            }
        }
        if (!java.util.Arrays.asList(FOLDERS).contains(top))
            throw new FileNotFoundException("Unknown root");
        File result;
        try {
            root = AndroidStorage.root(getContext(), top).getCanonicalFile();
            if (id.equals(top))
                return root;
            result = new File(root, id.substring(top.length() + 1));
            // Parent must stay inside private storage. A final symlink is read-only
            // content registered by Xenia; never traverse linked directories.
            if (!result.toPath().normalize().startsWith(root.toPath())
                    || !result.getParentFile().getCanonicalPath().startsWith(
                               root.getCanonicalPath() + File.separator)
                            && !result.getParentFile().getCanonicalFile().equals(
                                    root.getCanonicalFile()))
                throw new FileNotFoundException("Invalid document path");
            if (Files.isSymbolicLink(result.toPath()) && result.isDirectory())
                throw new FileNotFoundException("Linked folders are not exposed");
            for (File parent = result.getParentFile(); !parent.equals(root);
                    parent = parent.getParentFile())
                if (Files.isSymbolicLink(parent.toPath()))
                    throw new FileNotFoundException("Linked folders are not exposed");
            return result;
        } catch (IOException e) {
            throw new FileNotFoundException(e.getMessage());
        }
    }
    private String mime(File file) {
        if (file.isDirectory())
            return Document.MIME_TYPE_DIR;
        if (file.getName().endsWith(".png"))
            return "image/png";
        if (file.getName().endsWith(".toml"))
            return "text/plain";
        return "application/octet-stream";
    }
    private void row(MatrixCursor cursor, String id) throws FileNotFoundException {
        File file = resolve(id);
        int flags = 0;
        if (!id.equals("root")) {
            if (file.isDirectory())
                flags |= Document.FLAG_DIR_SUPPORTS_CREATE;
            else if (!id.equals("app_log") && !Files.isSymbolicLink(file.toPath()))
                flags |= Document.FLAG_SUPPORTS_WRITE;
            if (id.contains("/"))
                flags |= Document.FLAG_SUPPORTS_DELETE | Document.FLAG_SUPPORTS_RENAME;
        }
        cursor.newRow()
                .add(Document.COLUMN_DOCUMENT_ID, id)
                .add(Document.COLUMN_DISPLAY_NAME,
                        id.equals("root") ? "Xenia Edge" : file.getName())
                .add(Document.COLUMN_MIME_TYPE, mime(file))
                .add(Document.COLUMN_FLAGS, flags)
                .add(Document.COLUMN_SIZE, file.length())
                .add(Document.COLUMN_LAST_MODIFIED, file.lastModified());
    }
    @Override
    public Cursor queryRoots(String[] projection) {
        MatrixCursor result = new MatrixCursor(projection == null ? ROOT_COLUMNS : projection);
        result.newRow()
                .add(Root.COLUMN_ROOT_ID, "xenia")
                .add(Root.COLUMN_DOCUMENT_ID, "root")
                .add(Root.COLUMN_TITLE, "Xenia Edge")
                .add(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_IS_CHILD)
                .add(Root.COLUMN_ICON, R.mipmap.ic_launcher)
                .add(Root.COLUMN_MIME_TYPES, "*/*");
        return result;
    }
    @Override
    public Cursor queryDocument(String id, String[] projection) throws FileNotFoundException {
        MatrixCursor result = new MatrixCursor(projection == null ? DOCUMENT_COLUMNS : projection);
        row(result, id);
        return result;
    }
    @Override
    public Cursor queryChildDocuments(String id, String[] projection, String order)
            throws FileNotFoundException {
        MatrixCursor result = new MatrixCursor(projection == null ? DOCUMENT_COLUMNS : projection);
        File parent = resolve(id);
        if (id.equals("root")) {
            for (String folder : FOLDERS) {
                resolve(folder).mkdirs();
                row(result, folder);
            }
            for (String file : new String[] {"xenia-edge.config.toml", "app_log"})
                if (resolve(file).isFile())
                    row(result, file);
        } else {
            File[] children = parent.listFiles();
            if (children != null)
                for (File child : children) {
                    try {
                        row(result, id + "/" + child.getName());
                    } catch (FileNotFoundException ignored) {
                    }
                }
        }
        return result;
    }
    @Override
    public DocumentsContract.Path findDocumentPath(String parent, String child)
            throws FileNotFoundException {
        String start = parent == null ? "root" : parent;
        if (!resolve(start).exists() || !resolve(child).exists()
                || !start.equals(child) && !isChildDocument(start, child))
            throw new FileNotFoundException("Document is outside the requested tree");
        java.util.LinkedList<String> path = new java.util.LinkedList<>();
        for (String id = child;;) {
            path.addFirst(id);
            if (id.equals(start))
                break;
            int slash = id.lastIndexOf('/');
            id = slash < 0 ? "root" : id.substring(0, slash);
        }
        return new DocumentsContract.Path(parent == null ? "xenia" : null, path);
    }
    @Override
    public boolean isChildDocument(String parent, String child) {
        try {
            resolve(parent);
            resolve(child);
            return !parent.equals(child)
                    && (parent.equals("root") || child.startsWith(parent + "/"));
        } catch (FileNotFoundException e) {
            return false;
        }
    }
    @Override
    public ParcelFileDescriptor openDocument(String id, String mode, CancellationSignal signal)
            throws FileNotFoundException {
        File file = resolve(id);
        if (file.isDirectory()
                || !mode.equals("r")
                        && (id.equals("app_log") || Files.isSymbolicLink(file.toPath())))
            throw new FileNotFoundException("Linked game content is read-only");
        if (mode.equals("r"))
            return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
        DataLock lock = null;
        try {
            lock = new DataLock(getContext().getFilesDir());
            final DataLock held = lock;
            return ParcelFileDescriptor.open(file, ParcelFileDescriptor.parseMode(mode),
                    new android.os.Handler(android.os.Looper.getMainLooper()), error -> {
                        try {
                            held.close();
                        } catch (IOException ignored) {
                        }
                    });
        } catch (IOException e) {
            if (lock != null)
                try {
                    lock.close();
                } catch (IOException ignored) {
                }
            throw new FileNotFoundException(e.getMessage());
        }
    }
    private File newChild(String parent, String name) throws FileNotFoundException {
        if (parent.equals("root") || name.isEmpty() || !new File(name).getName().equals(name)
                || name.equals(".") || name.equals(".."))
            throw new FileNotFoundException("Invalid name");
        File result = resolve(parent + "/" + name);
        if (Files.exists(result.toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS))
            throw new FileNotFoundException("File already exists");
        return result;
    }
    @Override
    public String createDocument(String parent, String type, String name)
            throws FileNotFoundException {
        File file = newChild(parent, name);
        try (DataLock lock = new DataLock(getContext().getFilesDir())) {
            if (Document.MIME_TYPE_DIR.equals(type))
                Files.createDirectory(file.toPath());
            else
                Files.createFile(file.toPath());
            return parent + "/" + name;
        } catch (IOException e) {
            throw new FileNotFoundException(e.getMessage());
        }
    }
    @Override
    public String renameDocument(String id, String name) throws FileNotFoundException {
        if (!id.contains("/"))
            throw new FileNotFoundException("Cannot rename root");
        String parent = id.substring(0, id.lastIndexOf('/'));
        File source = resolve(id), destination = newChild(parent, name);
        try (DataLock lock = new DataLock(getContext().getFilesDir())) {
            Files.move(source.toPath(), destination.toPath());
            return parent + "/" + name;
        } catch (IOException e) {
            throw new FileNotFoundException(e.getMessage());
        }
    }
    @Override
    public void deleteDocument(String id) throws FileNotFoundException {
        if (!id.contains("/"))
            throw new FileNotFoundException("Cannot delete root");
        File target = resolve(id);
        try (DataLock lock = new DataLock(getContext().getFilesDir())) {
            // Files.walk does not follow symlinks; deleting a linked package only detaches it.
            try (java.util.stream.Stream<java.nio.file.Path> paths = Files.walk(target.toPath())) {
                java.util.Iterator<java.nio.file.Path> it =
                        paths.sorted(java.util.Comparator.reverseOrder()).iterator();
                while (it.hasNext()) Files.delete(it.next());
            }
        } catch (IOException e) {
            throw new FileNotFoundException(e.getMessage());
        }
    }
}
