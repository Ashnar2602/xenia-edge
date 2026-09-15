package jp.xenia.emulator;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import java.util.Arrays;

/** One physical controller; touch is merged into player one when publishing. */
final class AndroidGamepad {
    static boolean opensMenu(int key, int action, int repeat, boolean enabled) {
        return enabled && key == KeyEvent.KEYCODE_BUTTON_MODE
                && action == KeyEvent.ACTION_DOWN && repeat == 0;
    }
    int deviceId = -1;
    ControllerMappings mappings;
    int buttons, hatButtons;
    boolean leftTriggerKey, rightTriggerKey;
    final float[] axes = new float[6];

    void clear() {
        deviceId = -1;
        buttons = hatButtons = 0;
        leftTriggerKey = rightTriggerKey = false;
        Arrays.fill(axes, 0);
    }

    private boolean accept(int id, int source) {
        if ((source & InputDevice.SOURCE_GAMEPAD) != InputDevice.SOURCE_GAMEPAD
                && (source & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK)
            return false;
        if (deviceId != -1 && deviceId != id)
            return false;
        deviceId = id;
        return true;
    }

    boolean key(KeyEvent event) {
        int code = mappings == null ? event.getKeyCode() : mappings.key(event.getKeyCode());
        int mask = keyMask(code);
        boolean lt = code == KeyEvent.KEYCODE_BUTTON_L2;
        boolean rt = code == KeyEvent.KEYCODE_BUTTON_R2;
        if ((mask == 0 && !lt && !rt) || event.getAction() == KeyEvent.ACTION_MULTIPLE
                || !accept(event.getDeviceId(), event.getSource()))
            return false;
        boolean down = event.getAction() == KeyEvent.ACTION_DOWN;
        if (lt)
            leftTriggerKey = down;
        else if (rt)
            rightTriggerKey = down;
        else if (down)
            buttons |= mask;
        else
            buttons &= ~mask;
        return true;
    }

    private static float axis(MotionEvent e, int axis) {
        float value = e.getAxisValue(axis);
        InputDevice.MotionRange range =
                e.getDevice() == null ? null : e.getDevice().getMotionRange(axis, e.getSource());
        return Math.abs(value) > (range == null ? .15f : range.getFlat()) ? value : 0;
    }

    boolean motion(MotionEvent e) {
        if (e.getActionMasked() != MotionEvent.ACTION_MOVE
                || (e.getSource() & InputDevice.SOURCE_JOYSTICK) != InputDevice.SOURCE_JOYSTICK
                || !accept(e.getDeviceId(), e.getSource()))
            return false;
        for (int i=0;i<6;i++) {
            float value = axis(e, mappings == null ? ControllerMappings.AXES[i] : mappings.axis(i));
            if (mappings != null) value *= mappings.sign(i);
            axes[i] = i == 1 || i == 3 ? -value : i >= 4 ? Math.max(0, value) : value;
        }
        if (mappings == null || mappings.axis(4) == MotionEvent.AXIS_LTRIGGER)
            axes[4] = Math.max(axes[4], axis(e, MotionEvent.AXIS_BRAKE));
        if (mappings == null || mappings.axis(5) == MotionEvent.AXIS_RTRIGGER)
            axes[5] = Math.max(axes[5], axis(e, MotionEvent.AXIS_GAS));
        hatButtons = 0;
        float hx = axis(e, MotionEvent.AXIS_HAT_X), hy = axis(e, MotionEvent.AXIS_HAT_Y);
        if (hx < -.5)
            hatButtons |= 4;
        if (hx > .5)
            hatButtons |= 8;
        if (hy < -.5)
            hatButtons |= 1;
        if (hy > .5)
            hatButtons |= 2;
        return true;
    }

    float axis(int index) {
        if ((index == 4 && leftTriggerKey) || (index == 5 && rightTriggerKey))
            return 1;
        return axes[index];
    }

    static int keyMask(int key) {
        switch (key) {
            case KeyEvent.KEYCODE_DPAD_CENTER:
            case KeyEvent.KEYCODE_BUTTON_A:
                return 0x1000;
            case KeyEvent.KEYCODE_BUTTON_B:
                return 0x2000;
            case KeyEvent.KEYCODE_BUTTON_X:
                return 0x4000;
            case KeyEvent.KEYCODE_BUTTON_Y:
                return 0x8000;
            case KeyEvent.KEYCODE_DPAD_UP:
                return 1;
            case KeyEvent.KEYCODE_DPAD_DOWN:
                return 2;
            case KeyEvent.KEYCODE_DPAD_LEFT:
                return 4;
            case KeyEvent.KEYCODE_DPAD_RIGHT:
                return 8;
            case KeyEvent.KEYCODE_BUTTON_START:
                return 0x10;
            case KeyEvent.KEYCODE_BUTTON_SELECT:
                return 0x20;
            case KeyEvent.KEYCODE_BUTTON_THUMBL:
                return 0x40;
            case KeyEvent.KEYCODE_BUTTON_THUMBR:
                return 0x80;
            case KeyEvent.KEYCODE_BUTTON_L1:
                return 0x100;
            case KeyEvent.KEYCODE_BUTTON_R1:
                return 0x200;
            default:
                return 0;
        }
    }
}
