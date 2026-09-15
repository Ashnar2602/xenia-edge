package jp.xenia.emulator;

import static org.junit.Assert.*;
import android.content.*;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.nio.file.Files;
import java.util.UUID;
import org.junit.Test;

public class LibraryParityTest {
    @Test public void managementOpensWithoutOriginalMedia() throws Exception {
        Context base = InstrumentationRegistry.getInstrumentation().getTargetContext();
        androidx.test.uiautomator.UiDevice device = androidx.test.uiautomator.UiDevice.getInstance(
                InstrumentationRegistry.getInstrumentation());
        device.wakeUp();
        android.content.res.Configuration config = new android.content.res.Configuration(
                base.getResources().getConfiguration());
        config.setLocales(AppLanguage.locales(base, AppLanguage.selected(base)));
        Context localized = base.createConfigurationContext(config);
        base.startActivity(new Intent(base, GameSettingsActivity.class)
                .putExtra("game_path", "/missing/parity-game.zar").putExtra("title_id", 0x1234ABCD)
                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        assertTrue(device.wait(androidx.test.uiautomator.Until.hasObject(
                androidx.test.uiautomator.By.res(base.getPackageName(), "options_reset_all").enabled(true)), 15000));
        device.pressBack();
        base.startActivity(new Intent(base, ToolsActivity.class).putExtra("page", "content")
                .putExtra("game_path", "/missing/parity-game.zar").putExtra("title_id", 0x1234ABCD)
                .putExtra("version", 1).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        try {
            assertTrue(device.wait(androidx.test.uiautomator.Until.hasObject(
                    androidx.test.uiautomator.By.text(localized.getString(R.string.add_content))), 20000));
        } finally {
            device.pressBack();
        }
    }
    @Test public void offlineIdentityRelocationAndDiscMetadata() throws Exception {
        Context base = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String prefix = UUID.randomUUID().toString();
        Context context = new ContextWrapper(base) {
            @Override public SharedPreferences getSharedPreferences(String name, int mode) {
                return base.getSharedPreferences(prefix + name, mode);
            }
        };
        try {
            GameLibrary library = new GameLibrary(context);
            GameLibrary.Game game = new GameLibrary.Game("Custom name", "/missing/game.zar", "ZAR");
            game.metadata(new int[] {0xFF001122, 22, 5, 4, 2, 3}, 100);
            game.played = 500;
            game.preferred = true;
            library.add(game);
            GameLibrary.Game copy = new GameLibrary.Game("Other name", "/new/game.zar", "ZAR");
            copy.played = 900;
            library.add(copy);
            library.replace(game, copy.path, "ZAR", new int[] {0xFF001122, 23, 5, 4, 2, 3}, 101);
            GameLibrary loaded = new GameLibrary(context);
            assertEquals(1, loaded.games.size());
            GameLibrary.Game saved = loaded.games.get(0);
            assertEquals(game.id, saved.id);
            assertEquals("Custom name", saved.name);
            assertEquals(900, saved.played);
            assertTrue(saved.preferred && saved.metadataCached);
            assertEquals(23, saved.mediaId);
            assertEquals(3, saved.discCount);
            assertEquals(4, saved.baseVersion);
            Intent navigation = saved.identify(new Intent());
            assertEquals(0xFF001122, navigation.getIntExtra("title_id", 0));
            assertEquals(5, navigation.getIntExtra("version", 0));
            assertFalse(new File(saved.path).exists());
        } finally {
            base.deleteSharedPreferences(prefix + "android_library");
        }
    }

    @Test public void importedHistoryHonorsProfilesAndReleaseAmbiguity() throws Exception {
        System.loadLibrary("xenia-app");
        Context base = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String prefix = UUID.randomUUID().toString();
        File root = new File(base.getCacheDir(), "history-" + prefix);
        Context context = new ContextWrapper(base) {
            @Override public SharedPreferences getSharedPreferences(String name, int mode) {
                return base.getSharedPreferences(prefix + name, mode);
            }
        };
        try {
            String[] ids = {"E000000000000001", "E000000000000002", "E000000000000003"};
            long seconds = 1700000000;
            for (int i = 0; i < ids.length; ++i) {
                File gpd = new File(root, ids[i] + "/FFFE07D1/00010000/" + ids[i] + "/FFFE07D1.gpd");
                assertTrue(gpd.getParentFile().mkdirs());
                Files.write(gpd.toPath(), FeatureChecks.historyFixtureNative(0xFF001122, seconds + i));
            }
            String document = "[Profiles]\nlogged_profile_slot_0_xuid = '" + ids[0]
                    + "'\nlogged_profile_slot_3_xuid = '" + ids[1] + "'\n";
            long[] history = GameSettings.historyNative(root.getAbsolutePath(), document);
            assertArrayEquals(new long[] {0xFF001122L, (seconds + 1) * 1000}, history);
            GameLibrary library = new GameLibrary(context);
            GameLibrary.Game first = new GameLibrary.Game("Title", "/missing/disc1", "ZAR");
            first.titleId = 0xFF001122; first.version = 5; first.played = 100;
            GameLibrary.Game second = new GameLibrary.Game("Title", "/missing/disc2", "ZAR");
            second.titleId = first.titleId; second.version = 5; second.played = 200;
            library.add(first); library.add(second); library.history(history);
            assertEquals((seconds + 1) * 1000, library.lastPlayed(first));
            second.version = 6;
            assertEquals(100, library.lastPlayed(first));
            second.version = 5;
            library.history(GameSettings.historyNative(root.getAbsolutePath(), ""));
            assertEquals(200, library.lastPlayed(first));
            assertThrows(IllegalArgumentException.class,
                    () -> GameSettings.historyNative(root.getAbsolutePath(), "[broken"));
        } finally {
            base.deleteSharedPreferences(prefix + "android_library");
            try (java.util.stream.Stream<java.nio.file.Path> paths = Files.walk(root.toPath())) {
                for (java.nio.file.Path path : (Iterable<java.nio.file.Path>) paths.sorted(
                        java.util.Comparator.reverseOrder())::iterator) Files.delete(path);
            }
        }
    }
}
