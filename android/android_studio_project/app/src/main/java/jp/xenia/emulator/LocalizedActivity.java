package jp.xenia.emulator;

import android.app.Activity;
import android.content.Context;
import android.content.res.Configuration;
import android.os.Bundle;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

/** Shared immersive window setup and the desktop UI.ui_locale setting. */
public class LocalizedActivity extends Activity {
    private String appliedLanguage;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setDecorFitsSystemWindows(false);
    }

    @Override
    public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (focused) {
            WindowInsetsController controller = getWindow().getInsetsController();
            if (controller != null) {
                controller.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
                controller.hide(WindowInsets.Type.systemBars());
            }
        }
    }

    @Override
    protected void attachBaseContext(Context base) {
        appliedLanguage = AppLanguage.selected(base);
        super.attachBaseContext(base);
        Configuration override = new Configuration();
        override.setLocales(AppLanguage.locales(base, appliedLanguage));
        applyOverrideConfiguration(override);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Native sessions own their process and must never restart for a label
        // change. Their next launch reads the new global setting.
        if (!(this instanceof WindowedAppActivity)
                && !appliedLanguage.equals(AppLanguage.selected(this)))
            recreate();
    }
}
