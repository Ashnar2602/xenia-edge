package jp.xenia.emulator;

import android.view.KeyEvent;

/** Android key codes to the shared frontend's Windows-compatible virtual keys. */
final class KeyboardKeys {
    static int virtualKey(int key) {
        if (key >= KeyEvent.KEYCODE_A && key <= KeyEvent.KEYCODE_Z)
            return 'A' + key - KeyEvent.KEYCODE_A;
        if (key >= KeyEvent.KEYCODE_0 && key <= KeyEvent.KEYCODE_9)
            return '0' + key - KeyEvent.KEYCODE_0;
        if (key >= KeyEvent.KEYCODE_F1 && key <= KeyEvent.KEYCODE_F12)
            return 0x70 + key - KeyEvent.KEYCODE_F1;
        if (key >= KeyEvent.KEYCODE_NUMPAD_0 && key <= KeyEvent.KEYCODE_NUMPAD_9)
            return 0x60 + key - KeyEvent.KEYCODE_NUMPAD_0;
        switch (key) {
            case KeyEvent.KEYCODE_DEL:
                return 8;
            case KeyEvent.KEYCODE_TAB:
                return 9;
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_NUMPAD_ENTER:
                return 13;
            case KeyEvent.KEYCODE_ESCAPE:
                return 27;
            case KeyEvent.KEYCODE_SPACE:
                return 32;
            case KeyEvent.KEYCODE_PAGE_UP:
                return 33;
            case KeyEvent.KEYCODE_PAGE_DOWN:
                return 34;
            case KeyEvent.KEYCODE_MOVE_END:
                return 35;
            case KeyEvent.KEYCODE_MOVE_HOME:
                return 36;
            case KeyEvent.KEYCODE_DPAD_LEFT:
                return 37;
            case KeyEvent.KEYCODE_DPAD_UP:
                return 38;
            case KeyEvent.KEYCODE_DPAD_RIGHT:
                return 39;
            case KeyEvent.KEYCODE_DPAD_DOWN:
                return 40;
            case KeyEvent.KEYCODE_INSERT:
                return 45;
            case KeyEvent.KEYCODE_FORWARD_DEL:
                return 46;
            case KeyEvent.KEYCODE_NUMPAD_MULTIPLY:
                return 106;
            case KeyEvent.KEYCODE_NUMPAD_ADD:
                return 107;
            case KeyEvent.KEYCODE_NUMPAD_SUBTRACT:
                return 109;
            case KeyEvent.KEYCODE_NUMPAD_DOT:
                return 110;
            case KeyEvent.KEYCODE_NUMPAD_DIVIDE:
                return 111;
            case KeyEvent.KEYCODE_NUM_LOCK:
                return 144;
            case KeyEvent.KEYCODE_SCROLL_LOCK:
                return 145;
            case KeyEvent.KEYCODE_SHIFT_LEFT:
                return 160;
            case KeyEvent.KEYCODE_SHIFT_RIGHT:
                return 161;
            case KeyEvent.KEYCODE_CTRL_LEFT:
                return 162;
            case KeyEvent.KEYCODE_CTRL_RIGHT:
                return 163;
            case KeyEvent.KEYCODE_ALT_LEFT:
                return 164;
            case KeyEvent.KEYCODE_ALT_RIGHT:
                return 165;
            case KeyEvent.KEYCODE_META_LEFT:
                return 91;
            case KeyEvent.KEYCODE_META_RIGHT:
                return 92;
            case KeyEvent.KEYCODE_CAPS_LOCK:
                return 20;
            case KeyEvent.KEYCODE_BREAK:
                return 19;
            case KeyEvent.KEYCODE_SEMICOLON:
                return 186;
            case KeyEvent.KEYCODE_EQUALS:
                return 187;
            case KeyEvent.KEYCODE_COMMA:
                return 188;
            case KeyEvent.KEYCODE_MINUS:
                return 189;
            case KeyEvent.KEYCODE_PERIOD:
                return 190;
            case KeyEvent.KEYCODE_SLASH:
                return 191;
            case KeyEvent.KEYCODE_GRAVE:
                return 192;
            case KeyEvent.KEYCODE_LEFT_BRACKET:
                return 219;
            case KeyEvent.KEYCODE_BACKSLASH:
                return 220;
            case KeyEvent.KEYCODE_RIGHT_BRACKET:
                return 221;
            case KeyEvent.KEYCODE_APOSTROPHE:
                return 222;
            default:
                return 0;
        }
    }
}
