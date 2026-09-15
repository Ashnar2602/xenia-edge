package jp.xenia.emulator;

import static org.junit.Assert.*;

import android.content.Context;
import android.content.Intent;
import android.os.SystemClock;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.uiautomator.By;
import androidx.test.uiautomator.UiDevice;
import androidx.test.uiautomator.Until;
import java.io.File;
import org.junit.Assume;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class AndroidLaunchTest {
    private static Context uiContext() {
        Context base = InstrumentationRegistry.getInstrumentation().getTargetContext();
        android.content.res.Configuration config =
                new android.content.res.Configuration(base.getResources().getConfiguration());
        config.setLocales(AppLanguage.locales(base, AppLanguage.selected(base)));
        return base.createConfigurationContext(config);
    }

    @org.junit.Before
    public void setTestOrientation() throws Exception {
        UiDevice.getInstance(InstrumentationRegistry.getInstrumentation()).setOrientationNatural();
    }

    @org.junit.After
    public void restoreOrientation() throws Exception {
        UiDevice.getInstance(InstrumentationRegistry.getInstrumentation()).unfreezeRotation();
    }

    @Test
    public void libraryNavigation() throws Exception {
        Context context = uiContext();
        UiDevice device = UiDevice.getInstance(InstrumentationRegistry.getInstrumentation());
        context.startActivity(new Intent(context, LauncherActivity.class)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        String pkg = context.getPackageName();
        assertTrue(device.wait(Until.hasObject(By.res(pkg, "library_settings")), 10000));
        device.findObject(By.res(pkg, "library_settings")).click();
        assertTrue(device.wait(
                Until.hasObject(By.pkg(pkg).text(context.getString(R.string.global_options))),
                5000));
        device.findObject(By.pkg(pkg).desc(context.getString(R.string.back_library))).click();
        assertTrue(device.wait(
                Until.hasObject(By.pkg(pkg).text(context.getString(R.string.add_games))), 5000));
        device.findObject(By.res(pkg, "library_add_games")).click();
        assertTrue(device.wait(
                Until.hasObject(By.text(context.getString(R.string.single_game))), 5000));
        device.findObject(By.text(context.getString(R.string.game_folder))).click();
        assertTrue(device.wait(
                Until.hasObject(By.text(context.getString(R.string.import_explanation))), 5000));
        device.findObject(By.res("android", "button2")).click();
        device.waitForIdle();
        device.takeScreenshot(new File(context.getExternalFilesDir(null), "library-compact.png"));
        device.dumpWindowHierarchy(
                new File(context.getExternalFilesDir(null), "library-compact.xml"));
    }

    // Opt-in: uses a game the user has already granted to the library, without
    // downloading media or changing its source. CI without media skips this.
    @Test
    public void launchSelectedGame() throws Exception {
        String name = InstrumentationRegistry.getArguments().getString("game_name");
        Assume.assumeNotNull(name);
        Context context = uiContext();
        GameLibrary.Game selected = null;
        for (GameLibrary.Game game : new GameLibrary(context).games)
            if (game.name.equals(name))
                selected = game;
        assertNotNull("Add the selected title to the library first", selected);
        File original = selected.path.startsWith("content://")
                ? AndroidStorage.file(context, android.net.Uri.parse(selected.path))
                : new File(selected.path);
        assertTrue("Original image is not readable", original.isFile() && original.canRead());
        assertFalse("Game must not be staged in app storage",
                original.getCanonicalPath().startsWith(
                        context.getExternalFilesDir(null).getCanonicalPath()));
        UiDevice device = UiDevice.getInstance(InstrumentationRegistry.getInstrumentation());
        String pkg = context.getPackageName();
        context.startActivity(new Intent(context, LauncherActivity.class)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        assertTrue(device.wait(
                Until.hasObject(By.pkg(pkg).text(context.getString(R.string.add_games))), 10000));
        device.findObject(By.pkg(pkg).clazz("android.widget.EditText")).setText(name);
        androidx.test.uiautomator.UiObject2 card = device.wait(
                Until.findObject(By.pkg(pkg).desc(context.getString(R.string.play_named, name))),
                10000);
        assertNotNull("Selected game is missing from the library", card);
        card.click();
        if ("true".equals(
                    InstrumentationRegistry.getArguments().getString("expect_profile_prompt"))) {
            boolean prompted = device.wait(Until.hasObject(By.res(pkg, "profile_gamertag")), 20000);
            device.takeScreenshot(
                    new File(context.getExternalFilesDir(null), "profile-prompt.png"));
            device.dumpWindowHierarchy(
                    new File(context.getExternalFilesDir(null), "profile-prompt.xml"));
            assertTrue("First launch must ask for a profile", prompted);
            device.findObject(By.res("android", "button2")).click();
            assertTrue("Cancelling must return to the library",
                    device.wait(Until.hasObject(
                                        By.pkg(pkg).text(context.getString(R.string.add_games))),
                            20000));
            return;
        }
        try {
            assertTrue("Player did not open",
                    device.wait(Until.hasObject(By.res(java.util.regex.Pattern.compile(
                                        java.util.regex.Pattern.quote(pkg)
                                        + ":id/game_(loading|menu)"))),
                            15000));
            assertTrue("Title did not finish loading",
                    device.wait(Until.gone(By.res(pkg, "game_loading")), 90000));
            assertTrue("Player exited while loading", device.hasObject(By.res(pkg, "game_menu")));
            // Keep the 30-second stability check by default. A shorter opt-in
            // run isolates navigation regressions from emulation memory pressure.
            long runSeconds = Long.parseLong(
                    InstrumentationRegistry.getArguments().getString("game_run_seconds", "30"));
            SystemClock.sleep(runSeconds * 1000);
            assertTrue("Player exited after launch", device.hasObject(By.res(pkg, "game_menu")));
        } finally {
            File dir = context.getExternalFilesDir(null);
            device.takeScreenshot(new File(dir, "launch-test.png"));
            device.dumpWindowHierarchy(new File(dir, "launch-test.xml"));
        }
        if ("true".equals(InstrumentationRegistry.getArguments().getString("check_game_"
                                                                             + "options"))) {
            device.findObject(By.res(pkg, "game_menu")).click();
            assertTrue(device.wait(
                    Until.hasObject(By.text(context.getString(R.string.game_options))), 5000));
            device.findObject(By.text(context.getString(R.string.game_options))).click();
            assertTrue(device.wait(Until.hasObject(By.res(pkg, "options_search")), 10000));
            device.findObject(By.text("‹  " + context.getString(R.string.game_options))).click();
            assertTrue(device.wait(Until.hasObject(By.res(pkg, "game_menu")), 10000));
        }
        if ("true".equals(InstrumentationRegistry.getArguments().getString("check_tool_shortcuts"))) {
            for (int key : new int[] {android.view.KeyEvent.KEYCODE_F6,
                    android.view.KeyEvent.KEYCODE_F7, android.view.KeyEvent.KEYCODE_F8}) {
                device.pressKeyCode(key);
                assertTrue("Shortcut did not open its panel",
                        device.wait(Until.gone(By.res(pkg, "game_menu")), 5000));
                device.pressKeyCode(key);
                assertNotNull("Shortcut did not close its panel",
                        device.wait(Until.findObject(By.res(pkg, "game_menu")), 5000));
            }
        }
        device.findObject(By.res(pkg, "game_menu")).click();
        assertTrue("Pause menu did not open",
                device.wait(Until.hasObject(By.pkg(pkg).text(context.getString(R.string.resume))),
                        10000));
        device.findObject(By.pkg(pkg).text(context.getString(R.string.resume))).click();
        device.findObject(By.res(pkg, "game_menu")).click();
        // Stop is the last row. Scroll to the end so its full touch target is
        // visible; scrollIntoView may stop with only a clipped edge on screen.
        new androidx.test.uiautomator.UiScrollable(new androidx.test.uiautomator.UiSelector()
                .className("android.widget.ListView")).scrollToEnd(10);
        device.waitForIdle();
        assertTrue(device.wait(
                Until.hasObject(By.pkg(pkg).text(context.getString(R.string.stop_game))), 10000));
        androidx.test.uiautomator.UiObject2 confirmation = null;
        for (int attempt = 0; attempt < 2 && confirmation == null; attempt++) {
            // Some devices consume the first tap to stop the scroll animation.
            // Retry only the original list row, never the confirmation button.
            androidx.test.uiautomator.UiObject2 stopRow = device.findObject(
                    By.res("android:id/text1").text(context.getString(R.string.stop_game)));
            assertNotNull("Stop menu row disappeared", stopRow);
            stopRow.click();
            confirmation = device.wait(Until.findObject(
                    By.text(context.getString(R.string.stop_confirmation))), 3000);
        }
        if (confirmation == null) {
            device.takeScreenshot(new File(context.getExternalFilesDir(null), "stop-confirm-test.png"));
            device.dumpWindowHierarchy(new File(context.getExternalFilesDir(null), "stop-confirm-test.xml"));
        }
        assertNotNull(confirmation);
        assertNotNull(device.wait(Until.findObject(By.res("android:id/button1")), 3000));
        device.findObject(By.res("android:id/button1")).click();
        assertTrue("Player did not return to library",
                device.wait(
                        Until.hasObject(By.pkg(pkg).text(context.getString(R.string.add_games))),
                        20000));
    }
}
