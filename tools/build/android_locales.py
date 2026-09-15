#!/usr/bin/env python3
"""Reuse desktop catalogs for matching Android strings; English is the fallback.

Never invent a translation or replace a locale's hand-written Android resources.
Generated resources live in the Gradle build directory, not the source tree.
"""
import copy
import json
import re
import sys
from pathlib import Path
import xml.etree.ElementTree as ET
from compile_po import _parse_po


# Equivalent desktop labels whose wording differs on the Android frontend.
# Keep this small and explicit: fuzzy matching may translate the wrong action.
DESKTOP_LABELS = {
    "gamer_name": "Gamer Name:", "gamer_motto": "Gamer Motto:",
    "gamer_bio": "Gamer Bio:", "sign_in": "Login", "sign_out": "Logout",
    "signed_out": "Not logged in", "profile_subscription": "Subscription Tier:",
    "options_search": "Filter cvars", "options_all": "All",
    "options_saved": "Saved. Takes effect on next launch.",
    "options_reset": "Reset to Default", "unlimited": "Uncapped",
    "search_games": "Search games...", "info_last_played": "Last Played",
    "clear_image": "Clear Icon", "help_faq": "FA&Q...",
    "stop_game": "&Stop Game",
}


def normalized(text):
    return text.replace("&", "").replace("…", "...").rstrip(".:").strip().casefold()


def android_label(text, original):
    # Desktop keyboard mnemonics have no meaning in Android touch menus.
    text = re.sub(r"[\(（]&[A-Za-z][\)）]", "", text).replace("&", "").strip()
    if not original.rstrip().endswith((":", "：")):
        text = text.rstrip(":：").rstrip()
    return text


def profile_arrays(repo):
    """Read the shared sparse console enums; never renumber their null entries."""
    source = (repo / "src/xenia/ui/profile_options.h").read_text(encoding="utf-8")
    result = []
    for symbol, name in (("kLanguageNames", "profile_languages"),
                         ("kCountryNames", "profile_countries"),
                         ("kSubscriptionTierNames", "profile_subscriptions"),
                         ("kGamerZoneNames", "profile_zones")):
        match = re.search(r"\b" + symbol + r"\[\]\s*=\s*\{([^}]+)\}", source)
        if not match:
            raise ValueError("Missing shared profile choices: " + symbol)
        body = match[1]
        tokens = re.findall(r'nullptr|"(?:\\.|[^"\\])*"', body)
        if re.sub(r'nullptr|"(?:\\.|[^"\\])*"|[\s,]', "", body):
            raise ValueError("Unsupported profile choice syntax: " + symbol)
        array = ET.Element("string-array", name=name)
        for index, token in enumerate(tokens):
            ET.SubElement(array, "item").text = (
                ("@string/default_value" if index == 0 else "")
                if token == "nullptr" else json.loads(token))
        result.append(array)
    return result


def quoted(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("'", "\\'").replace("\n", "\\n") + '"'


FORMAT = re.compile(r"%(?:(\d+)\$)?([-+ #0]*)(\d*)(?:\.(\d+))?(?:hh|ll|[hlzjt])?([diuoxXfFeEgGsc%])")


def portable_format(text):
    """Convert supported printf placeholders to numbered Android placeholders.

    Reject unknown formats or conflicting argument types instead of generating
    a resource that may fail at runtime. Numbering preserves translated order.
    """
    arguments = {}
    next_argument = 1
    output = []
    end = 0
    for match in FORMAT.finditer(text):
        if "%" in text[end:match.start()]:
            return None
        output.append(text[end:match.start()])
        index, flags, width, precision, kind = match.groups()
        if kind == "%":
            if match.group() != "%%":
                return None
            output.append("%%")
        else:
            position = int(index) if index else next_argument
            if not index:
                next_argument += 1
            kind = {"i": "d", "u": "d", "F": "f"}.get(kind, kind)
            allowed_flags = "-" if kind in "sc" else "-#0" if kind in "oxX" else "-+ 0"
            if kind in "feE":
                allowed_flags += "#"
            if (any(flag not in allowed_flags for flag in flags)
                    or precision is not None and kind in "doxXc"):
                return None
            if position < 1 or arguments.get(position, kind) != kind:
                return None
            arguments[position] = kind
            output.append(f"%{position}${flags}{width}"
                          + ("." + precision if precision is not None else "") + kind)
        end = match.end()
    if "%" in text[end:]:
        return None
    output.append(text[end:])
    return "".join(output), arguments


def validate_catalog(defaults, translated, locale):
    """Fail the build if a complete catalog loses text or changes format types."""
    required = {name for name, element in defaults.items()
                if element.get("translatable") != "false"}
    missing = required - translated.keys()
    if missing:
        raise ValueError(f"Missing {locale} resources: " + ", ".join(sorted(missing)))
    for name in required:
        source, target = defaults[name], translated[name]
        if source.tag != target.tag:
            raise ValueError(f"{locale}/{name}: resource type changed")
        if source.tag == "plurals":
            originals = {e.get("quantity"): e for e in source}
            quantities = {e.get("quantity") for e in target}
            if not {"other", *originals}.issubset(quantities):
                raise ValueError(f"{locale}/{name}: missing plural quantities")
            pairs = [(originals.get(e.get("quantity"), originals["other"]), e)
                     for e in target]
        elif source.tag == "string-array":
            if len(source) != len(target):
                raise ValueError(f"{locale}/{name}: array length changed")
            pairs = zip(source, target)
        else:
            pairs = [(source, target)]
        for before, after in pairs:
            first = portable_format("".join(before.itertext()))
            second = portable_format("".join(after.itertext()))
            if (not (after.text or "").strip().strip('"') or first is None
                    or second is None or first[1] != second[1]):
                raise ValueError(f"{locale}/{name}: empty text or incompatible placeholders")


def generate(repo, destination):
    resources = repo / "android/android_studio_project/app/src/main/res"
    defaults = {}
    for path in (resources / "values").glob("*.xml"):
        for element in ET.parse(path).getroot():
            if element.tag in ("string", "string-array", "plurals"):
                defaults[element.get("name")] = element
    profiles = profile_arrays(repo)
    counts = {}
    for path in (repo / "assets/locale").glob("*/xenia.po"):
        locale = path.parent.name
        qualifier = locale.replace("_", "-r")
        qualifier = {"id": "in", "he": "iw"}.get(qualifier, qualifier)
        existing = {}
        directory = resources / ("values-" + qualifier)
        if directory.exists():
            for xml in directory.glob("*.xml"):
                for element in ET.parse(xml).getroot():
                    name = element.get("name")
                    if name in existing:
                        raise ValueError(f"{locale}/{name}: duplicate resource")
                    existing[name] = element
        translations = {}
        for source, translated in _parse_po(path):
            if "\x04" in source or not translated:
                continue
            before, after = portable_format(source), portable_format(translated)
            if before is None or after is None or before[1] != after[1]:
                continue
            key = normalized(before[0])
            candidate = after[0]
            if key in translations and translations[key] != candidate:
                translations[key] = None  # Ambiguous normalized message.
            else:
                translations[key] = candidate
        root = ET.Element("resources")
        for name, element in defaults.items():
            if (name in existing or element.get("translatable") == "false"
                    or element.tag == "plurals"):
                continue
            output = copy.deepcopy(element)
            changed = False
            for text in ([output] if output.tag == "string" else list(output)):
                original = (text.text or "").replace("\\n", "\n")
                if list(text) or "\\" in original:
                    continue
                parsed = portable_format(original)
                label = DESKTOP_LABELS.get(name, parsed[0]) if parsed else None
                translated = translations.get(normalized(label)) if label else None
                checked = portable_format(translated) if translated else None
                if checked is None or checked[1] != parsed[1]:
                    continue
                if translated and translated != original:
                    # Android resource parser handles quoted strings, then XML escaping.
                    text.text = quoted(android_label(translated, original))
                    changed = True
            if changed:
                root.append(output)
        # The same choices as desktop, localized with its PO catalogs. Always
        # retain the English item if the catalog does not contain a translation.
        for array in profiles:
            output = copy.deepcopy(array)
            for item in output:
                if not item.text.startswith("@"):
                    translated = translations.get(normalized(item.text))
                    if item.text and not translated:
                        raise ValueError(f"{locale}: missing profile choice {item.text}")
                    item.text = quoted(android_label(translated, item.text) if translated else "")
            root.append(output)
        # Every selectable desktop locale must cover the complete Android UI.
        # Reuse PO strings where possible; Android-only text lives in source XML.
        validate_catalog(defaults, {**existing, **{e.get("name"): e for e in root}}, locale)
        target = destination / ("values-" + qualifier) / "desktop.xml"
        target.parent.mkdir(parents=True, exist_ok=True)
        ET.indent(root)
        ET.ElementTree(root).write(target, encoding="utf-8", xml_declaration=True)
        if locale == "tl":
            # Android uses fil, while desktop and some Android dependencies use
            # tl. Generate both from one catalog; no duplicate source text.
            alias = ET.Element("resources")
            alias.extend(copy.deepcopy(list(existing.values()) + list(root)))
            ET.indent(alias)
            alias_path = destination / "values-b+fil" / "desktop.xml"
            alias_path.parent.mkdir(parents=True, exist_ok=True)
            ET.ElementTree(alias).write(alias_path, encoding="utf-8", xml_declaration=True)
        counts[locale] = len(root)
    # A single build-time source of truth for the in-app locale picker.
    root = ET.Element("resources")
    for array in profiles:
        output = copy.deepcopy(array)
        for item in output:
            if not item.text.startswith("@"):
                item.text = quoted(item.text)
        root.append(output)
    tags = ET.SubElement(root, "string-array", name="ui_locale_tags", translatable="false")
    for locale in ["en", *sorted(counts)]:
        ET.SubElement(tags, "item").text = locale.replace("_", "-")
    target = destination / "values" / "locales.xml"
    target.parent.mkdir(parents=True, exist_ok=True)
    ET.indent(root)
    ET.ElementTree(root).write(target, encoding="utf-8", xml_declaration=True)
    print("Desktop Android locale overlays:", counts)


if __name__ == "__main__":
    generate(Path(sys.argv[1]), Path(sys.argv[2]))
