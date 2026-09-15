package jp.xenia.emulator;

import static org.junit.Assert.*;

import android.content.*;
import android.content.pm.ProviderInfo;
import android.provider.DocumentsContract;
import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.uiautomator.*;
import java.io.*;
import java.nio.file.Files;
import java.util.UUID;
import org.junit.Test;

public class AndroidFeaturesTest {
    @Test
    public void desktopServicesAndPersistence() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        UiDevice device = UiDevice.getInstance(InstrumentationRegistry.getInstrumentation());
        String id = UUID.randomUUID().toString(), pkg = context.getPackageName();
        File root = new File(context.getCacheDir(), "profile-test-" + id);
        try {
            for (boolean verify : new boolean[] {false, true}) {
                context.startActivity(new Intent()
                                .setClassName(pkg, "jp.xenia.emulator.ProfileTestActivity")
                                .putExtra("test_id", id)
                                .putExtra("test_features", true)
                                .putExtra("verify_features", verify)
                                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK));
                File result = new File(root, "features.result");
                long resultDeadline = android.os.SystemClock.uptimeMillis() + 30000;
                while (!result.isFile() && android.os.SystemClock.uptimeMillis() < resultDeadline)
                    android.os.SystemClock.sleep(100);
                assertTrue("Native feature test timed out", result.isFile());
                assertEquals("FEATURES_OK",
                        new String(Files.readAllBytes(result.toPath()),
                                java.nio.charset.StandardCharsets.UTF_8));
                assertTrue(result.delete());
                long deadline = android.os.SystemClock.uptimeMillis() + 10000;
                while (!device.executeShellCommand("pidof " + pkg + ":profiletest")
                                .trim()
                                .isEmpty()) {
                    assertTrue(android.os.SystemClock.uptimeMillis() < deadline);
                    android.os.SystemClock.sleep(100);
                }
            }
        } finally {
            String pids = device.executeShellCommand("pidof " + pkg + ":profiletest").trim();
            if (!pids.isEmpty())
                for (String pid : pids.split("\\s+"))
                    android.os.Process.killProcess(Integer.parseInt(pid));
            delete(root);
        }
    }
    @Test
    public void dataProviderAndGlobalMigration() throws Exception {
        Context base = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String id = UUID.randomUUID().toString();
        File root = new File(base.getCacheDir(), "features-files-" + id);
        assertTrue(root.mkdirs());
        Context context = new ContextWrapper(base) {
            @Override
            public File getFilesDir() {
                return root;
            }
            @Override
            public SharedPreferences getSharedPreferences(String name, int mode) {
                return base.getSharedPreferences(id + name, mode);
            }
        };
        XeniaDocumentsProvider provider = new XeniaDocumentsProvider();
        ProviderInfo info = new ProviderInfo();
        info.authority = base.getPackageName() + ".test.documents";
        info.exported = true;
        info.grantUriPermissions = true;
        info.readPermission = info.writePermission = "android.permission.MANAGE_DOCUMENTS";
        provider.attachInfo(context, info);
        try {
            try (android.database.Cursor folders =
                            provider.queryChildDocuments("root", null, (String) null)) {
                assertEquals(7, folders.getCount());
            }
            String file = provider.createDocument("content", "application/octet-stream", "save");
            assertEquals("xenia", provider.findDocumentPath(null, file).getRootId());
            assertEquals(java.util.Arrays.asList("root", "content", file),
                    provider.findDocumentPath(null, file).getPath());
            assertEquals(java.util.Arrays.asList("content", file),
                    provider.findDocumentPath("content", file).getPath());
            assertThrows(
                    FileNotFoundException.class, () -> provider.findDocumentPath("plugins", file));
            File disc = new File(root, "test.xex");
            Files.write(disc.toPath(), new byte[] {0});
            provider.call("register_disc", disc.getPath(), null);
            provider.call("register_disc", disc.getPath(), null);
            GameLibrary registered = new GameLibrary(context);
            assertEquals(1, registered.games.size());
            assertEquals(disc.getCanonicalPath(), registered.games.get(0).path);
            try (android.os.ParcelFileDescriptor fd = provider.openDocument(file, "w", null);
                    OutputStream stream =
                            new android.os.ParcelFileDescriptor.AutoCloseOutputStream(fd)) {
                stream.write(42);
            }
            // The descriptor's close listener runs on the main looper.
            android.os.SystemClock.sleep(150);
            File original = new File(root, "original");
            Files.write(original.toPath(), new byte[] {7});
            Files.createSymbolicLink(new File(root, "content/link").toPath(), original.toPath());
            assertThrows(FileNotFoundException.class,
                    () -> provider.openDocument("content/link", "w", null));
            provider.deleteDocument("content/link");
            assertTrue(original.exists());
            Files.createSymbolicLink(new File(root, "content/folder-link").toPath(), root.toPath());
            assertThrows(FileNotFoundException.class,
                    () -> provider.queryDocument("content/folder-link/original", null));
            for (String path : new String[] {"content/../original", "content//save",
                         "content/../../outside", "private-key"})
                assertThrows(FileNotFoundException.class, () -> provider.queryDocument(path, null));
            try (DataLock lock = new DataLock(root)) {
                assertThrows(IOException.class, () -> new DataLock(root));
                assertThrows(FileNotFoundException.class, () -> provider.deleteDocument(file));
                assertThrows(
                        IOException.class, () -> new GameSettings(context, 0).save("volume", "65"));
            }
            assertEquals("content/renamed", provider.renameDocument(file, "renamed"));
            provider.deleteDocument("content/renamed");
            context.getSharedPreferences("android_settings", 0)
                    .edit()
                    .putInt("resolution", 2)
                    .putInt("fps_limit", 30)
                    .commit();
            GameSettings global = new GameSettings(context, 0);
            global.save("volume", "65");
            String saved = new String(
                    Files.readAllBytes(new File(root, "xenia-edge.config.toml").toPath()),
                    java.nio.charset.StandardCharsets.UTF_8);
            try (android.database.Cursor row =
                            provider.queryDocument("xenia-edge.config.toml", null)) {
                assertEquals(1, row.getCount());
            }
            try (DataLock lock = new DataLock(root)) {
                assertThrows(FileNotFoundException.class,
                        () -> provider.openDocument("xenia-edge.config.toml", "w", null));
            }
            global.save("content_root", "relocated");
            assertEquals(new File(root, "relocated").getCanonicalFile(),
                    AndroidStorage.root(context, "content"));
            assertTrue(new File(root, "relocated").mkdirs());
            String relocated =
                    provider.createDocument("content", "application/octet-stream", "new-save");
            assertTrue(new File(root, "relocated/new-save").exists());
            provider.deleteDocument(relocated);
            assertTrue(saved.contains("volume = 65"));
            assertTrue(saved.contains("draw_resolution_scale_x = 2"));
            assertTrue(saved.contains("framerate_limit = 30"));
            assertFalse(context.getSharedPreferences("android_settings", 0).contains("resolution"));
        } finally {
            provider.shutdown();
            context.getSharedPreferences("android_settings", 0).edit().clear().commit();
            context.getSharedPreferences("android_library", 0).edit().clear().commit();
            delete(root);
        }
    }
    private static void delete(File root) throws IOException {
        if (!root.exists())
            return;
        try (java.util.stream.Stream<java.nio.file.Path> paths = Files.walk(root.toPath())) {
            java.util.Iterator<java.nio.file.Path> it =
                    paths.sorted(java.util.Comparator.reverseOrder()).iterator();
            while (it.hasNext()) Files.deleteIfExists(it.next());
        }
    }
}
