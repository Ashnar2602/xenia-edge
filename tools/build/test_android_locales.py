import unittest
import contextlib
import io
import shutil
import tempfile
from pathlib import Path
import xml.etree.ElementTree as ET
from android_locales import android_label, generate, portable_format, profile_arrays, validate_catalog


class PortableFormatTest(unittest.TestCase):
    def test_every_selectable_locale_requires_android_specific_text(self):
        source = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory() as temporary:
            repo = Path(temporary)
            header = repo / "src/xenia/ui/profile_options.h"
            header.parent.mkdir(parents=True)
            shutil.copyfile(source / "src/xenia/ui/profile_options.h", header)
            po = repo / "assets/locale/ar/xenia.po"
            po.parent.mkdir(parents=True)
            shutil.copyfile(source / "assets/locale/ar/xenia.po", po)
            res = repo / "android/android_studio_project/app/src/main/res"
            (res / "values").mkdir(parents=True)
            (res / "values/strings.xml").write_text(
                '<resources><string name="android_only">Android-only test message</string></resources>',
                encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Missing ar resources: android_only"):
                generate(repo, repo / "output")
            (res / "values-ar").mkdir()
            (res / "values-ar/strings.xml").write_text(
                '<resources><string name="android_only">رسالة اختبار Android</string></resources>',
                encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()):
                generate(repo, repo / "output")
            self.assertTrue((repo / "output/values-ar/desktop.xml").is_file())
            (repo / "assets/locale/tl").mkdir()
            shutil.copyfile(po, repo / "assets/locale/tl/xenia.po")
            (res / "values-tl").mkdir()
            shutil.copyfile(res / "values-ar/strings.xml", res / "values-tl/strings.xml")
            with contextlib.redirect_stdout(io.StringIO()):
                generate(repo, repo / "output")
            alias = {e.get("name"): e for e in ET.parse(
                repo / "output/values-b+fil/desktop.xml").getroot()}
            self.assertIn("android_only", alias)
            self.assertIn("profile_languages", alias)

    def test_desktop_mnemonics_and_field_punctuation_are_removed(self):
        self.assertEqual(android_label("国(&C):", "Country"), "国")
        self.assertEqual(android_label("控制器（&O）", "Controllers"), "控制器")
        self.assertEqual(android_label("&Close Game", "Close game"), "Close Game")
        self.assertEqual(android_label("値： %1$s", "Value: %1$s"), "値： %1$s")

    def test_profile_choices_preserve_console_indices(self):
        arrays = {e.get("name"): [i.text for i in e]
                  for e in profile_arrays(Path(__file__).resolve().parents[2])}
        self.assertEqual(arrays["profile_languages"][0], "@string/default_value")
        self.assertEqual(arrays["profile_languages"][2], "Japanese")
        self.assertEqual(arrays["profile_countries"][16:19], ["Canada", "", "Switzerland"])
        self.assertEqual(arrays["profile_subscriptions"],
                         ["None", "", "", "Silver", "", "", "Gold", "", "", "Family"])

    def test_printf_sizes_and_reordering(self):
        self.assertEqual(portable_format("Disc %zu"), ("Disc %1$d", {1: "d"}))
        self.assertEqual(portable_format("%2$s: %1$08X"),
                         ("%2$s: %1$08X", {2: "s", 1: "X"}))
        self.assertEqual(portable_format("%s: %08X"),
                         ("%1$s: %2$08X", {1: "s", 2: "X"}))
        self.assertEqual(portable_format("%.2f%%"), ("%1$.2f%%", {1: "f"}))

    def test_unsafe_or_inconsistent_arguments_are_not_reused(self):
        for value in ["%n", "%*s", "%1$s %1$d", "%0$s", "%", "100% complete"]:
            with self.subTest(value=value):
                self.assertIsNone(portable_format(value))

    def test_complete_catalog_rejects_runtime_format_regressions(self):
        def catalog(xml):
            return {e.get("name"): e for e in ET.fromstring("<resources>" + xml + "</resources>")}
        source = catalog('<string name="error">%1$s: %2$08X</string>'
                         '<string-array name="choices"><item>A</item><item>B</item></string-array>'
                         '<plurals name="games"><item quantity="one">%d game</item>'
                         '<item quantity="other">%d games</item></plurals>')
        valid = catalog('<string name="error">%2$08X : %1$s</string>'
                        '<string-array name="choices"><item>Uno</item><item>Dos</item></string-array>'
                        '<plurals name="games"><item quantity="one">%d juego</item>'
                        '<item quantity="other">%d juegos</item></plurals>')
        validate_catalog(source, valid, "es")
        for original, replacement in [('%2$08X', '%2$s'), ('%1$s', ''),
                                       ('<item>Dos</item>', ''),
                                       ('<item quantity="other">%d juegos</item>', ''),
                                       ('Uno', '')]:
            with self.subTest(replacement=replacement):
                changed = {name: ET.fromstring(ET.tostring(e, encoding="unicode")
                                              .replace(original, replacement))
                           for name, e in valid.items()}
                with self.assertRaises(ValueError):
                    validate_catalog(source, changed, "es")
        with self.assertRaises(ValueError):
            validate_catalog(source, {}, "es")


if __name__ == "__main__":
    unittest.main()
