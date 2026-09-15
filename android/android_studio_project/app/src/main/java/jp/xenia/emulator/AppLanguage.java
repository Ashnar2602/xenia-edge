package jp.xenia.emulator;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.res.Resources;
import android.os.LocaleList;
import android.widget.Toast;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Locale;

/** Locale choices come from the same catalogs as the generated resources. */
final class AppLanguage {
    static String selected(Context context) {
        try {
            return AndroidStorage.options(context)[3];
        } catch (java.io.IOException e) {
            android.util.Log.w("xenia", "Unable to read UI locale", e);
            return "";
        }
    }

    static LocaleList locales(Context context, String selected) {
        if (selected.isEmpty()) {
            LocaleList system = Resources.getSystem().getConfiguration().getLocales();
            Locale[] locales = new Locale[system.size()];
            for (int i = 0; i < locales.length; i++) locales[i] = androidLocale(system.get(i));
            return new LocaleList(locales);
        }
        Locale locale = androidLocale(Locale.forLanguageTag(selected.replace('_', '-')));
        if (locale.getLanguage().isEmpty())
            return new LocaleList(Locale.ENGLISH);
        // Android also resolves regional/script fallbacks (es-MX, zh-Hant, ...).
        return locale.getLanguage().equals("en") ? new LocaleList(locale)
                                                 : new LocaleList(locale, Locale.ENGLISH);
    }

    private static Locale androidLocale(Locale locale) {
        // Keep desktop's tl config/catalog identifier, but use Android's Filipino locale.
        return locale.getLanguage().equals("tl")
                ? new Locale.Builder().setLocale(locale).setLanguage("fil").build()
                : locale;
    }

    static void choose(Activity activity) {
        ArrayList<String> tags = new ArrayList<>(
                Arrays.asList(activity.getResources().getStringArray(R.array.ui_locale_tags)));
        tags.sort((a, b)
                          -> Locale.forLanguageTag(a)
                                  .getDisplayName(Locale.forLanguageTag(a))
                                  .compareToIgnoreCase(Locale.forLanguageTag(b).getDisplayName(
                                          Locale.forLanguageTag(b))));
        tags.add(0, "");
        String[] labels = new String[tags.size()];
        for (int i = 0; i < labels.length; i++) {
            Locale locale = Locale.forLanguageTag(tags.get(i));
            labels[i] = i == 0 ? activity.getString(R.string.system_language)
                               : locale.getDisplayName(locale);
        }
        String selected = selected(activity).replace('_', '-');
        new AlertDialog.Builder(activity)
                .setTitle(R.string.interface_language)
                .setSingleChoiceItems(labels, tags.indexOf(selected),
                        (dialog, which) -> {
                            dialog.dismiss();
                            GameSettings.IO.execute(() -> {
                                try {
                                    new GameSettings(activity, 0)
                                            .save("ui_locale", tags.get(which));
                                    activity.runOnUiThread(() -> {
                                        if (!activity.isFinishing() && !activity.isDestroyed())
                                            activity.recreate();
                                    });
                                } catch (Exception e) {
                                    activity.runOnUiThread(
                                            ()
                                                    -> Toast.makeText(activity,
                                                                    activity.getString(
                                                                            R.string.options_error,
                                                                            e.getMessage()),
                                                                    Toast.LENGTH_LONG)
                                                            .show());
                                }
                            });
                        })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    private AppLanguage() {}
}
