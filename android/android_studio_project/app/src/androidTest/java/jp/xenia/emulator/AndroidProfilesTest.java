package jp.xenia.emulator;

import static org.junit.Assert.*;

import android.content.Context;
import android.content.Intent;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.uiautomator.By;
import androidx.test.uiautomator.UiDevice;
import androidx.test.uiautomator.Until;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.UUID;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public class AndroidProfilesTest {
    @Test
    public void createCancelAndPersistAcrossProcesses() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        UiDevice device = UiDevice.getInstance(InstrumentationRegistry.getInstrumentation());
        String pkg = context.getPackageName();
        String id = UUID.randomUUID().toString();
        File root = new File(context.getCacheDir(), "profile-test-" + id);
        Intent intent = new Intent()
                                .setClassName(pkg, "jp.xenia.emulator.ProfileTestActivity")
                                .putExtra("test_id", id)
                                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            context.startActivity(intent);
            assertTrue(device.wait(Until.hasObject(By.res(pkg, "profile_gamertag")), 20000));
            device.findObject(By.res("android", "button2")).click();
            assertTrue(device.wait(Until.hasObject(By.text("PROFILE_CANCELLED")), 5000));
            assertEquals(0, accounts(root));
            close(device, "PROFILE_CANCELLED");

            context.startActivity(intent);
            assertTrue(device.wait(Until.hasObject(By.res(pkg, "profile_gamertag")), 20000));
            device.findObject(By.res(pkg, "profile_gamertag")).setText("1Invalid");
            device.findObject(By.res("android", "button1")).click();
            assertTrue(device.wait(
                    Until.hasObject(By.text(context.getString(R.string.profile_invalid))), 5000));
            assertEquals(0, accounts(root));
            device.findObject(By.res(pkg, "profile_gamertag")).setText("Android Test");
            device.findObject(By.res("android", "button1")).click();
            assertTrue(device.wait(Until.hasObject(By.text("PROFILE_READY")), 20000));
            assertEquals(1, accounts(root));
            File config = new File(root, "xenia-edge.config.toml");
            String saved = new String(Files.readAllBytes(config.toPath()), StandardCharsets.UTF_8);
            assertTrue(saved.matches("(?s).*logged_profile_slot_0_xuid = \"[0-9A-Fa-f]{16}\".*"));
            close(device, "PROFILE_READY");

            // A fresh native core must decrypt the saved Account and auto-login.
            context.startActivity(intent);
            assertTrue(device.wait(Until.hasObject(By.text("PROFILE_READY")), 20000));
            assertFalse(device.hasObject(By.res(pkg, "profile_gamertag")));
            assertEquals(1, accounts(root));
            close(device, "PROFILE_READY");

            // Existing accounts with no automatic login must offer selection.
            String signedOut = saved.replaceAll(
                    "logged_profile_slot_0_xuid = \"[^\"]*\"", "logged_profile_slot_0_xuid = \"\"");
            Files.write(config.toPath(), signedOut.getBytes(StandardCharsets.UTF_8));
            context.startActivity(intent);
            assertTrue(device.wait(Until.hasObject(By.text("Android Test")), 20000));
            device.findObject(By.text("Android Test")).click();
            assertTrue(device.wait(Until.hasObject(By.text("PROFILE_READY")), 20000));
            assertEquals(1, accounts(root));
            close(device, "PROFILE_READY");
        } finally {
            device.takeScreenshot(new File(context.getExternalFilesDir(null), "profile-test.png"));
            device.dumpWindowHierarchy(
                    new File(context.getExternalFilesDir(null), "profile-test.xml"));
            // Only this test's UUID directory, never real profiles or game media.
            String pids = device.executeShellCommand("pidof " + pkg + ":profiletest").trim();
            if (!pids.isEmpty())
                for (String pid : pids.split("\\s+"))
                    android.os.Process.killProcess(Integer.parseInt(pid));
            delete(root);
        }
    }

    private static void close(UiDevice device, String state) throws Exception {
        device.findObject(By.text(state)).click();
        assertTrue(device.wait(Until.gone(By.text(state)), 10000));
        String process =
                InstrumentationRegistry.getInstrumentation().getTargetContext().getPackageName()
                + ":profiletest";
        long deadline = android.os.SystemClock.uptimeMillis() + 10000;
        while (!device.executeShellCommand("pidof " + process).trim().isEmpty()) {
            assertTrue("Previous native process did not exit",
                    android.os.SystemClock.uptimeMillis() < deadline);
            android.os.SystemClock.sleep(50);
        }
    }

    private static long accounts(File root) throws Exception {
        try (java.util.stream.Stream<java.nio.file.Path> files = Files.walk(root.toPath())) {
            return files.filter(p -> p.getFileName().toString().equals("Account")).count();
        }
    }

    private static void delete(File file) {
        File[] children = file.listFiles();
        if (children != null)
            for (File child : children) delete(child);
        file.delete();
    }
}
