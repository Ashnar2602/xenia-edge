# Android localization

The Android frontend covers all **34 selectable languages**: English plus the
33 desktop PO catalogs. Each locale resolves all 197 Android resource entries
(strings, three arrays and the game-count plural), plus four generated
profile-choice arrays. System language detection and an English default remain.

| Desktop tag | Language |
| --- | --- |
| en | English |
| ar | Arabic |
| bn | Bengali |
| cs | Czech |
| da | Danish |
| de | German |
| el | Greek |
| es | Spanish |
| fa | Persian |
| fi | Finnish |
| fr | French |
| hi | Hindi |
| hr | Croatian |
| hu | Hungarian |
| id | Indonesian |
| it | Italian |
| ja | Japanese |
| ko | Korean |
| nl | Dutch |
| pl | Polish |
| pt_BR | Portuguese (Brazil) |
| ru | Russian |
| sk | Slovak |
| sr | Serbian (Cyrillic) |
| sv | Swedish |
| ta | Tamil |
| th | Thai |
| tl | Tagalog / Filipino |
| tr | Turkish |
| uk | Ukrainian |
| ur | Urdu |
| vi | Vietnamese |
| zh_CN | Chinese (Simplified) |
| zh_TW | Chinese (Traditional) |

## Sources and maintenance

- `tools/build/android_locales.py` reuses matching translations in
  `assets/locale/*/xenia.po`. Android-specific text lives in
  `android/android_studio_project/app/src/main/res/values-*/`.
- The 29 newly completed catalogs used Google Translate to prepare initial
  drafts from the English Android strings, followed by contextual corrections
  for account profiles, the performance profiler, processing states,
  compatibility, library/play actions and switch labels. Plural forms were supplied separately.
  Technical identifiers and format arguments were protected or corrected.
  This is complete resource coverage, **not a native-speaker certification**
  of every translation; linguistic review remains welcome.
- The later relocation/cancellation labels were translated with assistant help
  into the same 34 catalogs; they have the same linguistic-review qualification.
- Shared country, language, subscription and gamer-zone choices are generated
  from `src/xenia/ui/profile_options.h` using the desktop translations. Sparse
  enum indices and reserved values never change.
- No online service, translation library or model is used by the app or build.
  Translation preparation scripts/caches are local ignored build artifacts.
  Normal builds use only the checked-in XML and PO sources.
- For new text, add the English resource, then provide the Android-specific
  translation for every locale where no equivalent PO label exists. The build
  rejects incomplete merged catalogs, empty values, incompatible placeholders,
  changed array lengths and missing required plural alternatives.
- Desktop keyboard mnemonics and redundant trailing field colons are removed
  when adapting shared labels for Android. Error wrappers, loading messages,
  confirmations, content import and controller menus are included.

## Locale behavior

Desktop config identifiers are retained. Indonesian uses the Android `in`
resource qualifier. Tagalog retains `tl` in the catalog picker and TOML, while
Java locale resolution and generated `values-b+fil` use `fil`. The same source
catalog also produces complete `tl` resources for dependency/lint compatibility.
This also handles a device
configured as `fil-PH`; the original `values-tl` resource lookup fell back to
English on the test device.

Android handles regional/script matching, including `zh-Hans-SG` and
`zh-Hant-HK`, and right-to-left layout for Arabic, Persian and Urdu. The library
heading and game cards use relative margins so their spacing mirrors correctly;
the Latin XENIA EDGE wordmark retains its left-to-right order.
See Android's [locale resolution documentation](https://developer.android.com/guide/topics/resources/multilingual-support)
and [Tagalog/Filipino compatibility notes in the framework](https://android.googlesource.com/platform/frameworks/base/+/b6ad99651e5f/core/java/com/android/internal/inputmethod/SubtypeLocaleUtils.java).

Names entered by users, game content, paths, cvar identifiers/descriptions and
diagnostic details supplied by the native core or OS retain their source text.
The shared native ImGui dialogs retain upstream localization behavior; this
change completes the Android frontend catalogs rather than translating the
emulator's diagnostic output.

## Validation

The generator validates every merged catalog on every build.
`python tools/build/test_android_locales.py` covers missing Android-only text in
a selectable language, format conversion, placeholder safety, array/plural
structure, shared enum indices and desktop mnemonic adaptation.

`AppLanguageTest` resolves all 34 languages on Android and checks formatted
errors, multiline build information, game counts, array sizes, RTL direction,
regional Chinese fallback, Tagalog/Filipino aliases and unsupported-language
English fallback. Current build/device results are recorded in
[the feature parity document](android-feature-parity.md).
