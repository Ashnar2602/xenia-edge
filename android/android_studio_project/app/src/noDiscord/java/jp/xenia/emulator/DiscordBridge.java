package jp.xenia.emulator;

import android.app.Activity;

/** Keeps the optional SDK out of ordinary builds. */
final class DiscordBridge {
    DiscordBridge(Activity activity) {}
    void playing(String title) {}
    void poll() {}
    void close() {}
}
