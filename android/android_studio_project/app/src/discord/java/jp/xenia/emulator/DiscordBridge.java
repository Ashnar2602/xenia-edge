package jp.xenia.emulator;

import android.app.Activity;
import com.discord.socialsdk.DiscordSocialSdkInit;

/** UI-thread-only RPC presence; no account tokens, OAuth or voice session. */
final class DiscordBridge {
    private final Activity activity;
    private boolean started;

    DiscordBridge(Activity activity) {
        this.activity = activity;
    }

    void playing(String title) {
        if (!enabledNative()) {
            close();
            return;
        }
        if (!started) {
            DiscordSocialSdkInit.setEngineActivity(activity);
            started = true;
        }
        updateNative(title);
    }

    void poll() {
        if (started)
            pumpNative();
    }

    void close() {
        if (started) {
            closeNative();
            started = false;
        }
    }

    private static native boolean enabledNative();
    private static native void updateNative(String title);
    private static native void pumpNative();
    private static native void closeNative();
}
