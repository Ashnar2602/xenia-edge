package jp.xenia.emulator;

import static org.junit.Assert.*;

import android.content.Context;
import android.content.Intent;
import androidx.test.platform.app.InstrumentationRegistry;
import androidx.test.uiautomator.*;
import java.io.File;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import org.junit.Test;

public class AndroidStandaloneTest {
    @Test
    public void sessionExitReturnsToLibrary() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        UiDevice device = UiDevice.getInstance(InstrumentationRegistry.getInstrumentation());
        context.startActivity(new Intent(context, SessionActivity.class)
                        .putExtra("previous_pid", 0)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        assertNotNull(device.wait(
                Until.findObject(By.res(context.getPackageName(), "library_settings")), 10000));
    }
    @Test
    public void vulkanViewerOpensTraceAndRejectsWrongVersion() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        android.content.res.Configuration config =
                new android.content.res.Configuration(context.getResources().getConfiguration());
        config.setLocales(AppLanguage.locales(context, AppLanguage.selected(context)));
        context = context.createConfigurationContext(config);
        UiDevice device = UiDevice.getInstance(InstrumentationRegistry.getInstrumentation());
        File trace = File.createTempFile("viewer-test-", ".xtr", context.getCacheDir());
        String pkg = context.getPackageName();
        try {
            for (boolean valid : new boolean[] {false, true}) {
                ByteBuffer bytes = ByteBuffer.allocate(52).order(ByteOrder.LITTLE_ENDIAN);
                bytes.putInt(0, valid ? 1 : 2);
                bytes.putInt(48, 1); // PrimaryBufferEnd, one empty frame.
                Files.write(trace.toPath(), bytes.array());
                context.startActivity(new Intent(context, TraceActivity.class)
                                .putExtra("trace_path", trace.getAbsolutePath())
                                .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
                if (valid) {
                    assertNotNull(device.wait(Until.findObject(By.pkg(pkg).desc(context.getString(R.string.open_trace))), 45000));
                    assertFalse(
                            device.hasObject(By.text(context.getString(R.string.trace_failed))));
                    assertFalse("Trace process died",
                            device.executeShellCommand("pidof " + pkg + ":trace").trim().isEmpty());
                    assertTrue(device.executeShellCommand("dumpsys activity activities")
                                    .contains("TraceActivity"));
                    android.os.SystemClock.sleep(700);
                    assertTrue(device.takeScreenshot(new File(context.getCacheDir(), "trace-viewer-smoke.png")));
                    device.pressBack();
                } else {
                    assertNotNull(device.wait(
                            Until.findObject(By.text(context.getString(R.string.trace_failed))),
                            45000));
                    device.findObject(By.res("android:id/button1")).click();
                }
                long deadline = android.os.SystemClock.uptimeMillis() + 10000;
                while (!device.executeShellCommand("pidof " + pkg + ":trace").trim().isEmpty()) {
                    assertTrue(android.os.SystemClock.uptimeMillis() < deadline);
                    android.os.SystemClock.sleep(100);
                }
            }
        } finally {
            for (String pid :
                    device.executeShellCommand("pidof " + pkg + ":trace").trim().split("\\s+"))
                if (!pid.isEmpty())
                    android.os.Process.killProcess(Integer.parseInt(pid));
            trace.delete();
        }
    }
}
