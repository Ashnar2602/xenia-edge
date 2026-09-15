package jp.xenia.emulator;

import static org.junit.Assert.*;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.SharedPreferences;
import android.view.KeyEvent;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.Test;

public class AndroidParityTest {
    @Test
    public void releaseGroupingAndPreferencesSurviveReload() {
        Context base = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String namespace = "parity-" + java.util.UUID.randomUUID();
        Context isolated = new ContextWrapper(base) {
            @Override
            public SharedPreferences getSharedPreferences(String name, int mode) {
                return base.getSharedPreferences(namespace, mode);
            }
        };
        try {
            GameLibrary library = new GameLibrary(isolated);
            GameLibrary.Game disc2 = new GameLibrary.Game("Title", "/disc2.zar", "ZAR");
            GameLibrary.Game disc1 = new GameLibrary.Game("Title", "/disc1.iso", "ISO");
            GameLibrary.Game otherVersion = new GameLibrary.Game("Title", "/other.iso", "ISO");
            disc1.titleId = disc2.titleId = otherVersion.titleId = 123;
            disc1.version = disc2.version = 1;
            otherVersion.version = 2;
            disc1.disc = 1;
            disc2.disc = 2;
            disc2.played = 100;
            library.add(disc2);
            library.add(otherVersion);
            library.add(disc1);
            library.add(new GameLibrary.Game("Unknown 1", "/unknown1", "XEX"));
            library.add(new GameLibrary.Game("Unknown 2", "/unknown2", "XEX"));
            assertEquals(4, library.releases().size());
            assertSame(disc1, library.releases().get(0));
            assertEquals(100, library.lastPlayed(disc1));
            disc2.preferred = true;
            for (GameLibrary.Game game : library.group(disc1)) game.name = "Renamed release";
            library.save();
            GameLibrary reloaded = new GameLibrary(isolated);
            assertEquals("/disc2.zar", reloaded.releases().get(0).path);
            assertEquals("Renamed release", reloaded.releases().get(0).name);
            assertEquals("Title", reloaded.releases().get(1).name);
            reloaded.games.remove(reloaded.releases().get(0));
            reloaded.save();
            assertEquals("/disc1.iso", new GameLibrary(isolated).releases().get(1).path);
        } finally {
            base.deleteSharedPreferences(namespace);
        }
    }
    @Test
    public void keyboardKeyTranslation() {
        assertEquals('W', KeyboardKeys.virtualKey(KeyEvent.KEYCODE_W));
        assertEquals(0xA3, KeyboardKeys.virtualKey(KeyEvent.KEYCODE_CTRL_RIGHT));
        assertEquals(0xDE, KeyboardKeys.virtualKey(KeyEvent.KEYCODE_APOSTROPHE));
        assertEquals(0x7B, KeyboardKeys.virtualKey(KeyEvent.KEYCODE_F12));
        assertEquals(0, KeyboardKeys.virtualKey(KeyEvent.KEYCODE_VOLUME_UP));
    }

    @Test
    public void guideOpensMenuOnlyWhenEnabledWithoutBackFallback() {
        assertTrue(AndroidGamepad.opensMenu(KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.ACTION_DOWN, 0, true));
        assertFalse(AndroidGamepad.opensMenu(KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.ACTION_DOWN, 0, false));
        assertFalse(AndroidGamepad.opensMenu(KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.ACTION_UP, 0, true));
        assertFalse(AndroidGamepad.opensMenu(KeyEvent.KEYCODE_BUTTON_MODE, KeyEvent.ACTION_DOWN, 1, true));
        assertFalse(AndroidGamepad.opensMenu(KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.ACTION_DOWN, 0, true));
        assertFalse(AndroidGamepad.opensMenu(KeyEvent.KEYCODE_BUTTON_A, KeyEvent.ACTION_DOWN, 0, true));
    }
    @Test
    public void controllerOverridesAreCachedAndDeviceSpecific() {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String name = "mapping-test-" + java.util.UUID.randomUUID();
        SharedPreferences prefs = context.getSharedPreferences(name, 0);
        try {
            prefs.edit()
                    .putInt("mapping.test.key.97", 96)
                    .putInt("mapping.test.axis.0", 11)
                    .putBoolean("mapping.test.invert.0", true)
                    .commit();
            ControllerMappings map = new ControllerMappings(prefs, "test");
            assertEquals(96, map.key(97));
            assertEquals(11, map.axis(0));
            assertEquals(-1f, map.sign(0), 0);
            ControllerMappings other = new ControllerMappings(prefs, "other");
            assertEquals(97, other.key(97));
            assertEquals(0, other.axis(0));
            prefs.edit().clear().commit();
            assertEquals(96, map.key(97));
            assertEquals(97, new ControllerMappings(prefs, "test").key(97));
        } finally {
            context.deleteSharedPreferences(name);
        }
    }
    @Test
    public void stfsImportUsesSignatureAndPortalFilterMatchesDesktop() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        java.io.File file = java.io.File.createTempFile("stfs-", ".con", context.getCacheDir());
        try {
            for (String magic : new String[] {"CON ", "LIVE", "PIRS"}) {
                java.nio.file.Files.write(
                        file.toPath(), magic.getBytes(java.nio.charset.StandardCharsets.US_ASCII));
                assertEquals("STFS / GOD", GameLibrary.format(file));
            }
            java.nio.file.Files.write(file.toPath(), new byte[] {1, 2, 3});
            assertNull(GameLibrary.format(file));
        } finally {
            file.delete();
        }
        assertTrue(UsbPortal.supported(0x1430, 0x1F17));
        assertTrue(UsbPortal.supported(0x24C6, 0xFA00));
        assertFalse(UsbPortal.supported(0x1430, 0xFA00));
    }
}
