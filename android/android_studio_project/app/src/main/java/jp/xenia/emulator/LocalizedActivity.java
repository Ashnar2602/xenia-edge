package jp.xenia.emulator;

import android.app.Activity;
import android.content.Context;
import android.content.res.Configuration;

/** Applies the desktop UI.ui_locale setting without a second UI framework. */
public class LocalizedActivity extends Activity {
    private String appliedLanguage;

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
