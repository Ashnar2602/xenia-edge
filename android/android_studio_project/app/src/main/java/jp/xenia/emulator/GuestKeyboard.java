package jp.xenia.emulator;

import android.content.Context;
import android.text.InputType;
import android.view.KeyEvent;
import android.view.View;
import android.view.inputmethod.*;

/** Android IME adapter for the existing guest ImGui text fields. */
final class GuestKeyboard extends View {
    interface Listener {
        void input(String text, int key);
    }
    private final Listener listener;
    GuestKeyboard(Context context, Listener listener) {
        super(context);
        this.listener = listener;
        setFocusableInTouchMode(true);
        setAlpha(0);
        setImportantForAccessibility(IMPORTANT_FOR_ACCESSIBILITY_NO);
    }
    @Override
    public boolean onCheckIsTextEditor() {
        return true;
    }
    @Override
    public InputConnection onCreateInputConnection(EditorInfo info) {
        info.inputType = InputType.TYPE_CLASS_TEXT;
        info.imeOptions = EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI;
        return new BaseInputConnection(this, false) {
            private String composing = "";
            @Override
            public boolean setComposingText(CharSequence text, int cursor) {
                composing = text.toString();
                return true;
            }
            @Override
            public boolean finishComposingText() {
                if (!composing.isEmpty())
                    listener.input(composing, 0);
                composing = "";
                return true;
            }
            @Override
            public boolean commitText(CharSequence text, int cursor) {
                composing = "";
                listener.input(text.toString(), 0);
                return true;
            }
            @Override
            public boolean deleteSurroundingText(int before, int after) {
                if (!composing.isEmpty())
                    composing = composing.substring(0, Math.max(0, composing.length() - before));
                else
                    for (int i = 0; i < Math.min(before, 128); i++) listener.input("", 1);
                return true;
            }
            @Override
            public boolean performEditorAction(int action) {
                finishComposingText();
                listener.input("", 2);
                return true;
            }
            @Override
            public boolean sendKeyEvent(KeyEvent event) {
                return key(event);
            }
        };
    }
    boolean key(KeyEvent event) {
        if (event.getAction() != KeyEvent.ACTION_DOWN)
            return false;
        if (event.getKeyCode() == KeyEvent.KEYCODE_DEL)
            listener.input("", 1);
        else if (event.getKeyCode() == KeyEvent.KEYCODE_ENTER)
            listener.input("", 2);
        else {
            int code = event.getUnicodeChar();
            if (code == 0 || !Character.isValidCodePoint(code))
                return false;
            listener.input(new String(Character.toChars(code)), 0);
        }
        return true;
    }
    void show(boolean enabled) {
        InputMethodManager ime = getContext().getSystemService(InputMethodManager.class);
        if (enabled) {
            requestFocus();
            ime.showSoftInput(this, InputMethodManager.SHOW_IMPLICIT);
        } else {
            ime.hideSoftInputFromWindow(getWindowToken(), 0);
            clearFocus();
        }
    }
}
