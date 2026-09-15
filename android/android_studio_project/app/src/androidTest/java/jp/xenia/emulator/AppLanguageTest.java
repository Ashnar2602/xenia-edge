package jp.xenia.emulator;

import static org.junit.Assert.*;

import android.content.Context;
import android.content.ContextWrapper;
import android.content.res.Configuration;
import android.content.res.Resources;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.nio.file.Files;
import java.util.Comparator;
import java.util.Locale;
import java.util.UUID;
import org.junit.Test;

public class AppLanguageTest {
    @Test
    public void globalLocalePersistsAndResolvesResources() throws Exception {
        Context base = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File root = new File(base.getCacheDir(), "locale-test-" + UUID.randomUUID());
        assertTrue(root.mkdir());
        Context context = new ContextWrapper(base) {
            @Override
            public File getFilesDir() {
                return root;
            }
            @Override
            public android.content.SharedPreferences getSharedPreferences(String name, int mode) {
                return base.getSharedPreferences(root.getName(), mode);
            }
        };
        try {
            assertEquals("", AppLanguage.selected(context));
            GameSettings settings = new GameSettings(context, 0);
            settings.save("volume", "65");
            settings.save("ui_locale", "it");
            assertEquals("it", AppLanguage.selected(context));
            assertEquals("I tuoi giochi", localized(context, "it").getString(R.string.your_games));
            assertEquals("Your games", localized(context, "en").getString(R.string.your_games));
            assertEquals("Your games", localized(context, "zz-ZZ").getString(R.string.your_games));
            assertEquals("Tus juegos", localized(context, "es-MX").getString(R.string.your_games));
            assertEquals("Vos jeux", localized(context, "fr-CA").getString(R.string.your_games));
            assertEquals(
                    "Deine Spiele", localized(context, "de-AT").getString(R.string.your_games));
            for (String tag : new String[] {"it", "es", "fr", "de"}) {
                Context translated = localized(context, tag);
                assertTrue(translated.getString(R.string.launch_error, 0xC0000001)
                                .contains("0xC0000001"));
                assertTrue(translated.getString(R.string.import_progress, 1, "test.zar")
                                .contains("test.zar"));
                assertNotEquals(
                        translated.getResources().getQuantityString(R.plurals.game_count, 1, 1),
                        translated.getResources().getQuantityString(R.plurals.game_count, 2, 2));
                assertEquals(9,
                        translated.getResources().getStringArray(R.array.controller_types).length);
            }
            assertEquals("zh-TW", AppLanguage.locales(context, "zh_TW").get(0).toLanguageTag());
            assertEquals(Locale.ENGLISH, AppLanguage.locales(context, "fr").get(1));
            // This shared label uses the translated printf %zu -> Android %d.
            String disc = localized(context, "fr").getString(R.string.disc_label, 2);
            assertTrue(disc.contains("2"));
            assertFalse(disc.contains("%"));
            GameSettings title = new GameSettings(context, 0x12345678);
            for (String[] row : title.load()) assertNotEquals("ui_locale", row[0]);
            assertThrows(java.io.IOException.class, () -> title.save("ui_locale", "de"));
            settings.save("ui_locale", null);
            assertEquals("", AppLanguage.selected(context));
            assertEquals(Resources.getSystem().getConfiguration().getLocales(),
                    AppLanguage.locales(context, ""));
            assertTrue(new String(
                    Files.readAllBytes(new File(root, "xenia-edge.config.toml").toPath()),
                    java.nio.charset.StandardCharsets.UTF_8)
                            .contains("65"));
            String[] build = GameSettings.buildNative();
            assertEquals(4, build.length);
            assertTrue(build[1].matches("[0-9a-fA-F]{7,40}"));
            assertTrue(build[3].startsWith("https://github.com/has207/xenia-edge/"));
        } finally {
            base.deleteSharedPreferences(root.getName());
            try (java.util.stream.Stream<java.nio.file.Path> paths = Files.walk(root.toPath())) {
                for (java.nio.file.Path path : (Iterable<java.nio.file.Path>) paths.sorted(
                             Comparator.reverseOrder())::iterator)
                    Files.delete(path);
            }
        }
    }

    @Test
    public void everySelectableLanguageFormatsAndroidUi() {
        Context base = InstrumentationRegistry.getInstrumentation().getTargetContext();
        String[] tags = base.getResources().getStringArray(R.array.ui_locale_tags);
        assertEquals(34, tags.length);
        for (String tag : tags) {
            Context context = localized(base, tag);
            Locale locale = context.getResources().getConfiguration().getLocales().get(0);
            assertFalse(tag, context.getString(R.string.your_games).isEmpty());
            if (!tag.equals("en"))
                assertNotEquals(tag, "Your games", context.getString(R.string.your_games));
            assertTrue(
                    tag, context.getString(R.string.launch_error, 0xC0000001).contains("C0000001"));
            assertTrue(tag,
                    context.getString(R.string.import_progress, 2, "game.zar")
                            .contains("game.zar"));
            assertTrue(tag, context.getString(R.string.options_error, "DETAIL").contains("DETAIL"));
            assertTrue(tag, context.getString(R.string.play_named, "Game").contains("Game"));
            String build = context.getString(R.string.build_details, "branch", "commit", "date");
            assertEquals(tag, 3, build.split("\n").length);
            assertTrue(tag,
                    build.contains("branch") && build.contains("commit") && build.contains("date"));
            for (int count : new int[] {0, 1, 2, 3, 5, 11, 21, 101, 1000000}) {
                String text = context.getResources().getQuantityString(
                        R.plurals.game_count, count, count);
                assertFalse(tag, text.contains("%"));
                assertTrue(tag + ": " + text, text.contains(String.format(locale, "%d", count)));
            }
            for (int id : new int[] {R.string.storage_explanation, R.string.profile_invalid,
                         R.string.profile_error, R.string.reset_global_confirmation,
                         R.string.reset_game_confirmation}) {
                assertFalse(tag, context.getString(id).isEmpty());
            }
            assertEquals(
                    tag, 9, context.getResources().getStringArray(R.array.controller_types).length);
            assertEquals(tag, 6, context.getResources().getStringArray(R.array.game_sorts).length);
            assertEquals(
                    tag, 5, context.getResources().getStringArray(R.array.compat_states).length);
            if (java.util.Arrays.asList("ar", "fa", "ur").contains(tag))
                assertEquals(tag, android.view.View.LAYOUT_DIRECTION_RTL,
                        context.getResources().getConfiguration().getLayoutDirection());
        }
        assertEquals(localized(base, "zh-CN").getString(R.string.your_games),
                localized(base, "zh-Hans-SG").getString(R.string.your_games));
        assertEquals(localized(base, "zh-TW").getString(R.string.your_games),
                localized(base, "zh-Hant-HK").getString(R.string.your_games));
        assertEquals("Your games", localized(base, "zz-ZZ").getString(R.string.your_games));
        assertEquals("Iyong mga laro", localized(base, "tl").getString(R.string.your_games));
        assertEquals("Iyong mga laro", localized(base, "fil-PH").getString(R.string.your_games));
        assertEquals("22 игры",
                localized(base, "ru")
                        .getResources()
                        .getQuantityString(R.plurals.game_count, 22, 22));
        assertEquals("5 gier",
                localized(base, "pl").getResources().getQuantityString(R.plurals.game_count, 5, 5));
    }

    @Test
    public void profileLabelsKeepConsoleIndicesAcrossLocales() {
        Context base = InstrumentationRegistry.getInstrumentation().getTargetContext();
        for (String tag : base.getResources().getStringArray(R.array.ui_locale_tags)) {
            Context context = localized(base, tag);
            assertEquals(context.getString(R.string.default_value),
                    ProfileLabels.value(context, "country", "0", "0"));
            assertEquals("17", ProfileLabels.value(context, "country", "17", "17"));
            assertEquals("999", ProfileLabels.value(context, "country", "999", "999"));
            assertEquals("invalid", ProfileLabels.value(context, "zone", "invalid", "invalid"));
            assertEquals(
                    13, context.getResources().getStringArray(R.array.profile_languages).length);
            assertEquals(10,
                    context.getResources().getStringArray(R.array.profile_subscriptions).length);
            assertEquals(context.getResources().getStringArray(R.array.profile_subscriptions)[6],
                    ProfileLabels.value(context, "subscription", "6", "Gold"));
        }
        assertEquals("Japanese", ProfileLabels.value(localized(base, "en"), "language", "2", "2"));
        assertNotEquals(
                "Japanese", ProfileLabels.value(localized(base, "it"), "language", "2", "2"));
        assertEquals("On", ProfileLabels.value(localized(base, "en"), "live", "true", "true"));
    }

    private static Context localized(Context context, String tag) {
        Configuration config = new Configuration(context.getResources().getConfiguration());
        config.setLocales(AppLanguage.locales(context, tag));
        return context.createConfigurationContext(config);
    }
}
