package jp.xenia.emulator;

import static org.junit.Assert.*;

import android.app.Activity;
import android.content.Intent;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.Test;

public class AndroidInputAudioTest {
    static {
        System.loadLibrary("xenia-app");
    }
    private static native String checkInputNative();
    private static native String checkAudioNative();

    @Test
    public void nativeButtonsAnalogsRepeatsAndTapQueue() {
        assertEquals("", checkInputNative());
    }

    private KeyEvent key(int action, int code) {
        return new KeyEvent(0, 0, action, code, 0, 0, 42, 0, 0, InputDevice.SOURCE_GAMEPAD);
    }
    @Test
    public void physicalButtonsTriggersAndReset() {
        AndroidGamepad pad = new AndroidGamepad();
        assertTrue(pad.key(key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_A)));
        assertEquals(0x1000, pad.buttons);
        assertTrue(pad.key(key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_START)));
        assertEquals(0x1010, pad.buttons);
        pad.key(key(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_A));
        assertEquals(0x10, pad.buttons);
        pad.key(key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_L2));
        pad.key(key(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_R2));
        assertEquals(1, pad.axis(4), 0);
        assertEquals(1, pad.axis(5), 0);
        pad.clear();
        assertEquals(0, pad.buttons);
        assertEquals(0, pad.axis(4), 0);
        assertEquals(0, pad.axis(5), 0);
        assertEquals(-1, pad.deviceId);
        assertFalse(pad.key(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_A)));
    }

    @Test
    public void touchAAndStartAndCancel() {
        final int[] buttons = {0};
        final float[] axes = new float[6];
        InstrumentationRegistry.getInstrumentation().runOnMainSync(() -> {
            TouchController pad = new TouchController(
                    InstrumentationRegistry.getInstrumentation().getTargetContext(),
                    (b, lx, ly, rx, ry, lt, rt) -> {
                        buttons[0] = b;
                        axes[0] = lx;
                        axes[1] = ly;
                        axes[2] = rx;
                        axes[3] = ry;
                        axes[4] = lt;
                        axes[5] = rt;
                    });
            pad.layout(0, 0, 1600, 720);
            MotionEvent down = MotionEvent.obtain(0, 0, MotionEvent.ACTION_DOWN, 1408, 468, 0);
            pad.onTouchEvent(down);
            down.recycle();
            assertEquals(0x1000, buttons[0]);
            // A held while a second finger presses Start; release A only.
            MotionEvent.PointerProperties[] properties = new MotionEvent.PointerProperties[2];
            MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[2];
            for (int i = 0; i < 2; ++i) {
                properties[i] = new MotionEvent.PointerProperties();
                properties[i].id = i;
                properties[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
                coords[i] = new MotionEvent.PointerCoords();
                coords[i].pressure = 1;
                coords[i].size = 1;
            }
            coords[0].x = 1408;
            coords[0].y = 468;
            coords[1].x = 880;
            coords[1].y = 619;
            MotionEvent second = MotionEvent.obtain(0, 1,
                    MotionEvent.ACTION_POINTER_DOWN | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT),
                    2, properties, coords, 0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
            pad.onTouchEvent(second);
            second.recycle();
            assertEquals(0x1010, buttons[0]);
            MotionEvent up = MotionEvent.obtain(0, 2, MotionEvent.ACTION_POINTER_UP, 2, properties,
                    coords, 0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
            pad.onTouchEvent(up);
            up.recycle();
            assertEquals(0x10, buttons[0]);
            MotionEvent cancel = MotionEvent.obtain(0, 3, MotionEvent.ACTION_CANCEL, 880, 619, 0);
            pad.onTouchEvent(cancel);
            cancel.recycle();
            assertEquals(0, buttons[0]);
            assertArrayEquals(new float[6], axes, 0);
        });
    }

    @Test
    public void sdlAudioConsumesGuestFramesAndResumes() {
        android.app.Instrumentation instrumentation = InstrumentationRegistry.getInstrumentation();
        Activity activity = instrumentation.startActivitySync(
                new Intent(instrumentation.getTargetContext(), LauncherActivity.class)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        instrumentation.runOnMainSync(() -> {
            org.libsdl.app.SDL.setupJNI();
            org.libsdl.app.SDL.initialize();
            org.libsdl.app.SDL.setContext(activity);
        });
        assertEquals("", checkAudioNative());
    }
}
