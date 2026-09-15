package jp.xenia.emulator;

import static org.junit.Assert.*;

import android.content.Context;
import android.content.ContextWrapper;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.*;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class GameSettingsTest {
    private static String[] option(String[][] rows, String name) {
        for (String[] row : rows)
            if (row[0].equals(name))
                return row;
        throw new AssertionError("Missing option: " + name);
    }
    @Test
    public void registryTypesEnumsAndFiltering() {
        String[][] rows = GameSettings.listNative("", "", 2, 60);
        assertTrue(rows.length > 100);
        for (String[] row : rows) {
            String key = (row[0] + row[1]).toLowerCase(Locale.ROOT);
            assertFalse(key.contains("d3d") || key.contains("dxgi") || key.contains("metal")
                    || key.contains("x64"));
            assertNotEquals("Profiles", row[1]);
            assertFalse(key.contains("moltenvk"));
        }
        assertEquals("2", option(rows, "draw_resolution_scale_x")[4]);
        assertEquals("60", option(rows, "framerate_limit")[4]);
        assertEquals("integer", option(rows, "volume")[3]);
        assertEquals("UI", option(rows, "guide_button")[1]);
        assertEquals("true", option(rows, "guide_button")[4]);
        assertEquals("true", option(rows, "enable_early_precompilation")[4]);
        for (String[] row : rows)
            assertFalse(row[0].equals("controller_hotkeys") || row[0].equals("headless"));
        String guide = GameSettings.editNative("", "guide_button", "false");
        assertEquals("false", option(GameSettings.listNative("", guide, -1, -1), "guide_button")[4]);
        assertTrue(option(rows, "spirv_version_override").length > 8);
        String document = GameSettings.editNative(
                "[Unknown]\nkeep = 'untouched'\n", "user_language", "Italian");
        document = GameSettings.editNative(document, "framerate_limit", "30");
        document = GameSettings.editNative(document, "guest_display_refresh_cap", "false");
        rows = GameSettings.listNative("", document, 2, 60);
        assertEquals("Italian", option(rows, "user_language")[4]);
        assertEquals("30", option(rows, "framerate_limit")[4]);
        assertEquals("60", option(rows, "framerate_limit")[5]);
        assertEquals("true", option(rows, "framerate_limit")[6]);
        assertEquals("false", option(rows, "guest_display_refresh_cap")[4]);
        assertTrue(document.contains("untouched"));
        document = GameSettings.editNative(document, "framerate_limit", null);
        assertEquals(
                "60", option(GameSettings.listNative("", document, 2, 60), "framerate_limit")[4]);
        for (String invalid : new String[] {"-1", "4294967296", "1.5", "false", "1\nother=2"}) {
            assertThrows(IllegalArgumentException.class,
                    () -> GameSettings.editNative("", "framerate_limit", invalid));
        }
        assertThrows(
                IllegalArgumentException.class, () -> GameSettings.editNative("", "gpu", "d3d12"));
        assertThrows(IllegalArgumentException.class,
                () -> GameSettings.editNative("broken = [", "volume", "50"));
        assertThrows(IllegalArgumentException.class,
                () -> GameSettings.listNative("", "broken = [", 1, 0));
        assertThrows(IllegalArgumentException.class,
                () -> GameSettings.editNative("", "readback_resolve", "invalid"));
    }

    @Test
    public void titleFilesAreIsolatedAndPreserveOtherSettings() throws Exception {
        Context base = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File root = new File(base.getCacheDir(), "settings-test-" + UUID.randomUUID());
        assertTrue(root.mkdirs());
        Context context = new ContextWrapper(base) {
            @Override
            public File getFilesDir() {
                return root;
            }
        };
        try {
            GameSettings first = new GameSettings(context, 0x12345678);
            GameSettings second = new GameSettings(context, 0x12345679);
            first.save("volume", "45");
            second.save("volume", "80");
            first.save("readback_resolve", "all");
            assertEquals("45", option(new GameSettings(context, 0x12345678).load(), "volume")[4]);
            assertEquals("80", option(second.load(), "volume")[4]);
            first.save("volume", null);
            assertEquals("false", option(first.load(), "volume")[6]);
            assertEquals("all", option(first.load(), "readback_resolve")[4]);
            File file = new File(root, "config/12345678.config.toml");
            byte[] original = Files.readAllBytes(file.toPath());
            assertThrows(IllegalArgumentException.class, () -> first.save("volume", "-10"));
            assertArrayEquals(original, Files.readAllBytes(file.toPath()));
            Files.write(file.toPath(), "bad = [".getBytes(StandardCharsets.UTF_8));
            assertThrows(IllegalArgumentException.class, () -> first.save("volume", "50"));
            assertEquals("bad = [",
                    new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8));
        } finally {
            delete(root);
        }
    }

    @Test
    public void resetPreservesProfilesDesktopOptionsAndOtherTitles() throws Exception {
        Context base = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String fixture = "settings-reset-" + UUID.randomUUID();
        File root = new File(base.getCacheDir(), fixture);
        assertTrue(root.mkdirs());
        Context context = new ContextWrapper(base) {
            @Override
            public File getFilesDir() {
                return root;
            }
            @Override
            public android.content.SharedPreferences getSharedPreferences(String name, int mode) {
                return base.getSharedPreferences(fixture, mode);
            }
        };
        try {
            GameSettings global = new GameSettings(context, 0);
            GameSettings first = new GameSettings(context, 0x12345678);
            GameSettings second = new GameSettings(context, 0x12345679);
            File globalFile = new File(root, "xenia-edge.config.toml");
            String preserved = "[Profiles]\nlogged_profile_slot_0 = 'E030000000000001'\n"
                    + "[D3D12]\nunknown_desktop_option = 'keep-desktop'\n";
            Files.write(globalFile.toPath(), preserved.getBytes(StandardCharsets.UTF_8));
            global.save("volume", "65");
            global.save("ui_locale", "fr");
            first.save("volume", "45");
            second.save("volume", "80");
            File titleFile = new File(root, "config/12345678.config.toml");
            String original =
                    new String(Files.readAllBytes(titleFile.toPath()), StandardCharsets.UTF_8);
            original = GameSettings.editNative(original, "ui_locale", "it");
            Files.write(titleFile.toPath(),
                    (original + "\n[Unknown]\nkey = 'keep-title'\n")
                            .getBytes(StandardCharsets.UTF_8));
            first.reset();
            assertEquals("65", option(first.load(), "volume")[4]);
            assertEquals("false", option(first.load(), "volume")[6]);
            String resetTitle =
                    new String(Files.readAllBytes(titleFile.toPath()), StandardCharsets.UTF_8);
            assertTrue(resetTitle.contains("keep-title"));
            assertEquals(
                    "it", option(GameSettings.listNative("", resetTitle, -1, -1), "ui_locale")[4]);
            assertEquals("80", option(second.load(), "volume")[4]);
            context.getSharedPreferences("android_settings", 0)
                    .edit()
                    .putInt("resolution", 2)
                    .putInt("fps_limit", 29)
                    .commit();
            byte[] before = Files.readAllBytes(globalFile.toPath());
            try (DataLock lock = new DataLock(root)) {
                assertThrows(java.io.IOException.class, global::reset);
            }
            assertArrayEquals(before, Files.readAllBytes(globalFile.toPath()));
            global.reset();
            for (String[] row : global.load()) assertEquals(row[0], "false", row[6]);
            assertFalse(context.getSharedPreferences("android_settings", 0).contains("resolution"));
            assertFalse(context.getSharedPreferences("android_settings", 0).contains("fps_limit"));
            String resetGlobal =
                    new String(Files.readAllBytes(globalFile.toPath()), StandardCharsets.UTF_8);
            assertTrue(resetGlobal.contains("E030000000000001"));
            assertTrue(resetGlobal.contains("keep-desktop"));
            assertEquals("80", option(second.load(), "volume")[4]);
            Files.write(titleFile.toPath(), "bad = [".getBytes(StandardCharsets.UTF_8));
            assertThrows(IllegalArgumentException.class, first::reset);
            assertEquals("bad = [",
                    new String(Files.readAllBytes(titleFile.toPath()), StandardCharsets.UTF_8));
            assertThrows(IllegalArgumentException.class,
                    () -> GameSettings.resetNative("", new String[] {"volume", "missing_option"}));
            assertThrows(IllegalArgumentException.class,
                    () -> GameSettings.resetNative("", new String[] {"logged_profile_slot_0"}));
        } finally {
            delete(root);
            base.deleteSharedPreferences(fixture);
        }
    }

    @Test
    public void optionsScreenSearchAndValidation() throws Exception {
        String name = InstrumentationRegistry.getArguments().getString("settings_game");
        org.junit.Assume.assumeNotNull(name);
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        GameLibrary.Game selected = null;
        for (GameLibrary.Game game : new GameLibrary(context).games)
            if (game.name.equals(name))
                selected = game;
        assertNotNull(selected);
        androidx.test.uiautomator.UiDevice device = androidx.test.uiautomator.UiDevice.getInstance(
                InstrumentationRegistry.getInstrumentation());
        String pkg = context.getPackageName();
        context.startActivity(new android.content.Intent(context, GameSettingsActivity.class)
                        .putExtra("game_path", selected.path)
                        .putExtra("game_name", selected.name)
                        .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK));
        assertTrue(device.wait(androidx.test.uiautomator.Until.hasObject(
                                       androidx.test.uiautomator.By.res(pkg, "options_search")),
                10000));
        device.findObject(androidx.test.uiautomator.By.res(pkg, "options_search"))
                .setText("framerate_limit");
        assertTrue(device.wait(androidx.test.uiautomator.Until.hasObject(
                                       androidx.test.uiautomator.By.res("android", "text1")
                                               .text("framerate_limit")),
                20000));
        device.findObject(
                      androidx.test.uiautomator.By.res("android", "text1").text("framerate_limit"))
                .click();
        assertTrue(device.wait(androidx.test.uiautomator.Until.hasObject(
                                       androidx.test.uiautomator.By.res(pkg, "option_value")),
                5000));
        device.findObject(androidx.test.uiautomator.By.res(pkg, "option_value")).setText("-1");
        device.findObject(androidx.test.uiautomator.By.res("android", "button1")).click();
        assertTrue(device.wait(androidx.test.uiautomator.Until.hasObject(
                                       androidx.test.uiautomator.By.textContains("Invalid value")),
                5000));
        device.takeScreenshot(
                new File(context.getExternalFilesDir(null), "settings-validation.png"));
        device.findObject(androidx.test.uiautomator.By.res("android", "button2")).click();
        device.wait(androidx.test.uiautomator.Until.findObject(
                            androidx.test.uiautomator.By.res(pkg, "options_search")),
                      5000)
                .setText("vulkan");
        assertTrue(device.wait(androidx.test.uiautomator.Until.gone(
                                       androidx.test.uiautomator.By.res("android", "text1")
                                               .text("framerate_limit")),
                5000));
        device.takeScreenshot(new File(context.getExternalFilesDir(null), "settings-vulkan.png"));
        device.dumpWindowHierarchy(
                new File(context.getExternalFilesDir(null), "settings-vulkan.xml"));
    }
    @Test
    public void titleOverridesApplyBeforeCoreInitialization() throws Exception {
        String name = InstrumentationRegistry.getArguments().getString("settings_game");
        org.junit.Assume.assumeNotNull(name);
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        GameLibrary.Game selected = null;
        for (GameLibrary.Game game : new GameLibrary(context).games)
            if (game.name.equals(name))
                selected = game;
        assertNotNull(selected);
        String path = selected.path.startsWith("content://")
                ? AndroidStorage.file(context, android.net.Uri.parse(selected.path))
                          .getAbsolutePath()
                : selected.path;
        int titleId = GameIcons.metadataNative(path)[0];
        String id = UUID.randomUUID().toString();
        File root = new File(context.getCacheDir(), "profile-test-" + id);
        assertTrue(new File(root, "config").mkdirs());
        File config =
                new File(root, String.format(Locale.ROOT, "config/%08X.config.toml", titleId));
        String overrides = GameSettings.editNative("", "time_scalar", "0.5");
        overrides = GameSettings.editNative(overrides, "draw_resolution_scale_x", "2");
        overrides = GameSettings.editNative(overrides, "framerate_limit", "29");
        Files.write(config.toPath(), overrides.getBytes(StandardCharsets.UTF_8));
        androidx.test.uiautomator.UiDevice device = androidx.test.uiautomator.UiDevice.getInstance(
                InstrumentationRegistry.getInstrumentation());
        try {
            context.startActivity(new android.content.Intent()
                            .setClassName(context.getPackageName(),
                                    "jp.xenia.emulator.ProfileTestActivity")
                            .putExtra("test_id", id)
                            .putExtra("test_game_path", path)
                            .addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK));
            assertTrue("Core clock and video must use the title settings",
                    device.wait(androidx.test.uiautomator.Until.hasObject(
                                        androidx.test.uiautomator.By.text("SETTINGS 0.5 2 29")),
                            20000));
        } finally {
            String pids =
                    device.executeShellCommand("pidof " + context.getPackageName() + ":profiletest")
                            .trim();
            if (!pids.isEmpty())
                for (String pid : pids.split("\\s+"))
                    android.os.Process.killProcess(Integer.parseInt(pid));
            delete(root);
        }
    }

    private static void delete(File file) {
        File[] children = file.listFiles();
        if (children != null)
            for (File child : children) delete(child);
        file.delete();
    }
}
