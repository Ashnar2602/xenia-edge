# Android feature parity

Baseline: desktop frontend and optional Vulkan tools at upstream `edge`
`e987fd7f5`, integrated with the Android port. This tracks accessible features,
not game compatibility.

**Total parity is not certified.** Earlier implementation passes include complete
Android resource coverage, bulk reset, profile metadata and title-info corrections.
The latest source audit found five additional portable differences. The follow-up
below implements them; real-content and hardware qualifications still apply. An optional
Android Discord backend is prepared, but its SDK-enabled build and behavior still
require the official AAR. Hardware and real-game
validation remain separate from code and synthetic-fixture coverage.

## Upstream synchronization (2026-09-17)

Integrated four commits from upstream `edge`, `04b3b63d9` through `e987fd7f5`:
synchronous file I/O dispatcher waits, storage request timing, local patch
listing, and the sequential-read seek correction. The one merge conflict was
the host-thread signal guard in `XObject`; upstream now provides the same
protection, so its implementation replaces the Android branch's equivalent fix.

Android lists and edits local-only patch files using the new shared enumerator.
Saved copies of bundled patches appear once, and title ownership follows the
TOML contents. The existing feature fixture now covers local patch edits,
duplicate suppression, wrong-title rejection and invalid paths.

`Storage.storage_request_timing` keeps upstream's default (`true`) and is
automatically exposed by Android's settings registry. It models console drive
latency for the launched title's storage; it may affect loading and streaming
measurements. Disabling it uses host read speed. This update does not establish
game compatibility or performance gains.

Pre-existing uncommitted Vulkan/build optimizations and dirty submodules were
preserved separately from this merge. Backup files and build logs are under
`build/upstream-sync-20260917-152638/`; the previous committed branch tip is
`backup/android-before-sync-20260917`.

Validation: Android `assembleGithubRelease`, `lintGithubRelease` and
`compileGithubDebugJavaWithJavac` passed. This local build includes the preserved
uncommitted optimizations. The added device fixture compiled but was not run;
no game or performance result is claimed. Windows C++ compilation of the kernel,
patcher and VFS targets passed with no errors or warnings. The full Windows
target build stopped earlier in unchanged Boost.Context MASM sources
(`BOOST_CONTEXT_EXPORT` / `FRAME`); desktop linking and the other platforms
still require CI validation.

## Upstream synchronization (2026-09-15)

The Android branch now includes all 19 commits from `06623d245` through
`8205e00e8` on upstream `edge`. Shared changes include parallel precompilation
(now enabled by default), asynchronous reads, timer/APC and scheduler fixes,
host-clock alignment, physical-page invalidation and profiler fixes.

The local port was reapplied with one textual conflict in the Vulkan trace
viewer: Android still waits for its surface, while desktop retains the upstream
UTF-8 path conversion. Android's existing launch worker fits the new split
between UI preparation and worker-thread precompilation.

Android now defines `UI.guide_button` with the same meaning and default as
desktop. Guide opens the menu when enabled; Back/Select remains guest input.
The duplicate controller chord implementation was removed to match upstream's
removal of `controller_hotkeys`. The removed `headless` option is also absent
from the shared settings registry. Existing Android menu tools remain available.

Before integration, the original working tree was saved in
`build/upstream-sync-20260915/working-tree.zip`, with a manifest and binary patch.
The `backup/android-before-sync-20260915` branch retains the previous base and
the `android-before-upstream-20260915` stash retains the uncommitted port.
Dirty third-party submodules were preserved.

Validation for this synchronization (`build/upstream-sync-20260915/`):

- Debug APK build and lint pass; the updated APK is installed on the connected
  ARM64 phone. `services.log` contains 14 passing device tests, including Guide
  semantics, cvar defaults/filtering, library/native services and all 34 locales.
- Release build, R8 and lint also pass (`release-build.log`); unsigned APK:
  27,083,075 bytes. Installed Debug APK: 45,583,813 bytes.
- `runtime.log`: After Burner Climax passes the launch, options, panel shortcuts,
  pause/resume and stop/library smoke test. The native log confirms early
  precompilation enabled: 12,314 discovered functions in 4,932 ms, followed by
  179 static initializers and callees (536 functions) in 12 ms.
- `standalone-localized.log`: two passing tests for session exit and synthetic
  Vulkan traces (valid input and wrong-version rejection). The initial test
  expected system-language text despite an app-language override; its resource
  context now follows the same locale selection as the UI.
- Locale generation's six Python tests pass. No new translations were needed.
- This is Android build and smoke coverage, not extended game compatibility or
  a fresh Windows/Linux build. Previous real-content/hardware qualifications
  below still apply.

## Library and installer follow-up (2026-09-15)

- **Management without original media:** library navigation now forwards cached
  title ID/version to settings and management activities. Probing remains the
  fallback for entries with no known identity. Information also uses cached
  Media ID, version, base version and disc counts when media is unavailable.
- **Relocation:** a missing/unreadable file offers a replacement picker. The
  replacement is read in place and checked against the known title/release.
  The entry retains its ID, custom name, play history and default selection;
  duplicate paths are consolidated. Missing-file attempts no longer update Last
  Played. Cancelling or choosing a different release leaves the entry untouched.
- **Imported history:** a small read-only JNI call uses the shared dashboard GPD
  parser for the four configured profile slots, without constructing an emulator.
  The launcher reads under the existing data lock on its IO worker. Timestamp
  caching is separate from local launch history, and profile times are ignored
  when more than one release of a title is indexed, as on desktop.
- **Install progress and cancellation:** the existing install entry's atomic byte
  counters and cancellation flag are exposed to the management UI. The dialog
  shows the current package, batch position and extracted bytes. Cancel stops
  pending packages and requests cooperative cancellation of the active extraction;
  cleanup completes before the dialog closes. Completed packages remain installed.
  Generation IDs prevent a late cancellation from affecting a subsequent batch.
  Immutable packages still use zero-copy links and mutable extraction still stages
  data before publishing it; occupied destinations remain protected.
- **Multidisc information:** indexing now retains each file's Media ID, base
  version and disc count. The existing disc list displays Media ID and marks the
  effective default, including an automatically selected one. Existing entries
  are upgraded on the next successful metadata read.
- Three new UI strings are supplied in every one of the 34 language catalogs.
  System-language selection and English fallback are unchanged.

Validation:

- Debug APK build and lint pass (`build/android-parity7-complete-debug.log`),
  with zero lint errors. The final Debug APK (45,590,461 bytes) is installed over
  ADB. Release packaging, R8 and lint also pass
  (`build/android-parity7-release.log`); unsigned Release APK: 27,080,339 bytes.
- `build/android-parity7-complete-services.log`: **8 passing device tests**.
  Covers persisted disc metadata, offline settings/content screens, relocation
  entry preservation/deduplication, actual serialized GPD history, configured slots
  including slot 3, release ambiguity, cleared profile history,
  cancellation before import and late-cancellation isolation, existing native
  services and all 34 locale resource catalogs. Profile/content fixtures are
  disposable and do not replace the user's profile or games.
- `build/android-parity7-complete-runtime.log`: **1 passing device smoke test**
  with After Burner Climax, game options, F6/F7/F8, pause/resume, confirmed Stop
  and library return. This is not extended compatibility validation.
- `python tools/build/test_android_locales.py`: **6 passing tests**.
- The disc list was visually inspected on the phone: correct Media ID and
  automatically selected default marker (`build/android-parity7-discs.png`).
  This check caught and corrected an omitted display-format change.
- The first UI run failed with the phone asleep. It was rerun awake and the UI
  test now wakes the device. Earlier failing runs are not counted as passes.
- A native binding compile error was corrected. Release compilation encountered
  shader-tool errors and later ran out of host disk space. Cleaning only Ninja's
  generated outputs in the two old build configurations freed about 5 GB; the
  native Release build then passed with four parallel jobs
  (`build/android-parity7-release-resume.log`). The final Gradle Release pass is
  recorded in `build/android-parity7-release.log`.
- Cancellation during extraction of a real large mutable package and a complete
  system-picker relocation remain manual integration checks; the tests above
  verify their data/JNI components, not those entire real-content flows.

The five findings that motivated this work are retained as the pre-change audit
record below.

## Source audit record: differences before this follow-up (2026-09-15)

Scope: the local desktop checkout identified above, compared with Android Java
entry points, JNI services and shared native implementations. This is a source
audit, not a new device test or a comparison with a newer upstream revision.

1. **Per-title management requires readable original media.** Desktop
   `GameInfoPanel` builds configuration/content pages from its stored `LibraryKey`
   (title ID and version). Android `GameSettingsActivity.onCreate` and
   `ToolsActivity.onCreate` reopen `game_path` and require successful metadata
   extraction before loading options, patches, content, saves or statistics.
   Thus a disconnected drive or moved file blocks management even when the
   indexed identity and managed data are available. The information page's cached
   title ID/compatibility fallback does not fix these entry points; it also leaves
   version/disc fields unavailable despite some of those values being cached.
   Minimal approach: forward cached title ID/version through internal navigation,
   probe the source only when identity is missing, and use cached information
   consistently. Keep normal content/title/version validation.

2. **No missing-media relocation flow.** Desktop
   `GameListPanel::LaunchOrPrompt` offers a file picker for a missing path and
   removes the stale path after a replacement is selected. Android
   `LauncherActivity.launchGame` starts the player directly; `EmulatorActivity`
   reports unavailable media, with no replacement action. Re-adding the file and
   removing the old entry is possible manually. Android also updates `played`
   before checking file availability, unlike the desktop missing-file path.
   Minimal approach: add a zero-copy replacement picker, validate the selected
   title/release, update the affected disc entry and preserve existing library
   metadata; do not count missing-file attempts as launches.

3. **Profile history does not feed Last Played.** Desktop
   `GameListPanel::LoadTimestampsFromProfiles` merges newer dashboard GPD times
   from signed-in profiles, only for titles with a single release because the
   GPD cannot identify the release. Android `GameLibrary.lastPlayed` only merges
   locally recorded `played` values across discs. The native statistics service
   returns achievement/gamerscore information but does not supply these timestamps
   to the library. Imported profile history therefore does not affect Android's
   Last Played display or sorting. Minimal approach: expose timestamps through
   the existing management service and apply the same single-release rule.

4. **Content installation has no progress/cancellation controls.** Desktop
   `InstallProgressDialog::Tick` displays per-package state and byte progress;
   `OnCancel` sets `ContentInstallEntry::cancelled_`, checked by the shared
   installer during extraction. Android `ToolsActivity.importPackages` shows a
   generic working state and reports results after the batch. The JNI
   `packageNative` keeps the install entry local and exposes neither progress nor
   cancellation. Back backgrounds the task while busy; it does not cancel it.
   Minimal approach: expose the existing entry's counters/cancellation flag for
   the active operation, retaining staged extraction and cleanup. This is separate
   from the previously recorded need to validate real mutable packages.

5. **Incomplete per-disc metadata presentation (minor).** Desktop
   `GameInfoPanel::ReloadDiscs` shows each path's Media ID and marks the effective
   default. Android `LauncherActivity.manageDiscs` shows disc number/path and marks
   only an explicitly preferred disc; an automatically selected default has no
   marker. `GameLibrary.Game` does not store Media ID and `indexMetadata` discards
   that field. The information page can show it for the chosen file while readable,
   but the disc list cannot show it for all entries. Launch/default/remove actions
   already exist. Minimal approach: retain Media ID during indexing and display
   it alongside the effective default in the existing disc list.

### Other areas traced in this audit

| Desktop surface | Android route / finding |
| --- | --- |
| Main menus, configuration categories, toolbar and runtime context menu (`emulator_window.cc`) | Launcher/settings and `EmulatorActivity.showMenu`, linked through `AndroidEmulatorApp::ShowTool`; portable actions present. Desktop window chrome is excluded. |
| Global/title options and reset (`config_panel_wx.cc`) | Registry-based `GameSettings` and settings activity; source-media dependency is item 1 above. Different layouts/presets do not by themselves mean the underlying option is missing. |
| Library search, sort, releases and discs (`game_list_panel_wx.cc`, `game_info_panel_wx.cc`) | Launcher/library equivalents present; missing-media flow, profile timestamps and disc metadata are items 2, 3 and 5. |
| Profile editor, sign-in slots and achievements | Management activity/native services and shared guide/profile dialog paths cover the reviewed fields and actions. Android saves individual edits instead of staging a whole desktop form. |
| DLC/TU/save management and patches | Existing content manager/patch editor reused; title/type and known TU release checks present. Installation controls are item 4. |
| Runtime audio, post-processing, performance, debug, speed, cache, screenshots and profiler | Shared native panels/actions are reachable. GPU trace capture is also available through the shared debug panel, not only a physical keyboard shortcut. Backend/build-dependent controls retain their conditions. |
| Controllers, keyboard and portals | Existing Android input/assignment/mapping and shared backend paths present; physical hardware qualifications below still apply. |
| Storage, logs, plugins, trace viewer, About/help and locale selection | Existing implementations remain; real plugin/trace operation, optional Discord and build/release validation remain qualified below. |

Content replacement also differs intentionally: desktop per-title import uses
`overwrite_existing`; Android rejects an occupied destination and can detach an
existing linked package before registering another one. This preserves the
zero-copy/non-destructive import model. It is not an automatic overwrite feature;
mutable data likewise is not silently replaced. Shared file management remains
available for explicit data management.

No further missing portable entry point was identified in the surfaces traced
above. This does not certify every runtime behavior or every possible core option.
The audit itself changed only this document. The implementation follow-up above
addresses its findings; earlier build/device results remain historical evidence.

## Second review: implementation

| Area | Android implementation |
| --- | --- |
| File logging | Common `log_file` / `log_append` definitions, optional file sink attached after global config loads. Existing logcat output remains. A relative filename resolves under app storage; relaunch appends. Correction to the original audit: the old Android guards excluded these cvars entirely, rather than exposing working editor entries. |
| Storage | Portable guest mount options are exposed. Global `content_root` / `cache_root` are used by the emulator, management services and document provider. Roots and the log destination are app-wide options; per-title overrides are not offered for them. Existing data is not moved automatically. |
| Title relaunch | Native callback forwards path, module, flags and hex launch data to a main-process handoff. It waits for the previous emulation process to exit before starting the next. The default in-process path remains shared with desktop. |
| Guest exit / title / disc callbacks | Guest exit navigates to the library; title changes update runtime metadata and restore input assignments; disc changes update metadata and notify the user. |
| Content/save details | `ContentManager::ListContent` supplies package and extracted-content metadata, including sidecar names; the UI shows file/directory size. |
| Controller menu | Honors `UI.guide_button` (default true). Guide opens the menu; Back/Select remains guest input. Obsolete controller chords were removed with upstream's `controller_hotkeys` removal. |
| Disconnect | Explicit Disconnected and Virtual controller choices, separate from automatic physical assignment; empty bindings are unbound in the shared InputSystem. |
| Physical mappings | Per-device key/axis/inversion overrides, cached outside the input event loop, with reset. Android's system mapping remains the default. |
| STFS import | Unknown filename extensions are checked for CON/LIVE/PIRS magic, including single-file import and folder scans. |
| Path chooser | Save-file semantics for the log destination; existing file/folder selection for other paths. Selected media remains zero-copy. |
| Files | Root configuration and the configured log are exposed; original game directories can open in the system file manager. Configured content/cache roots retain provider path checks and write locks. |
| Stop | Running-game Stop asks for confirmation about unsaved progress and keeps the session paused until the user confirms or cancels. |
| Localization | All 34 selectable languages cover 197 Android resource entries plus four generated profile-choice arrays. Android-specific translations supplement the shared desktop PO labels; plural forms, technical identifiers and formatting are checked. Every merged catalog must be complete to build, and missing-translation lint is an error. System detection and English fallback remain. See [localization scope and provenance](android-localization.md); complete coverage does not imply native-speaker review of every sentence. |
| Bulk option reset | Global and per-title settings offer a confirmed reset using the existing cvar registry and atomic file writer. Only Android-available keys are removed. Profile login entries, desktop-only/unknown keys and other title files remain. Global reset honors the data lock and removes legacy video overrides. Per-title reset inherits global options and preserves global-only keys. |
| Profile details and choices | Online XUID and domain are selectable read-only fields, using the same account getters as the desktop editor. Country, language, subscription and gamer-zone labels are generated from shared `profile_options.h` and desktop PO catalogs; reserved console enum indices are retained. |
| Title information | Displays the actual desktop database compatibility state instead of a fixed unverified label, retaining cached status/title ID when media is unavailable. Last launched uses the latest disc in the grouped release, as in the library. |
| Interface language | Settings offers the languages present in the catalogs and System language. Uses the desktop `UI.ui_locale` TOML key, is global only, and applies to all Android activities. Normal screens refresh when the setting changes; running native sessions read it on their next launch. Regional/script matching remains Android's responsibility, with English fallback. |
| Build information | About shows the native build's branch, commit and date, with links to its commit/PR and the project license, as on desktop. |
| Runtime panel shortcuts | F6/F7/F8 now close their respective post-processing, performance and debug panels when pressed again. Reuses their desktop close callbacks; unrelated guest dialogs are not dismissed. |
| Runtime status | Shader storage initialization drives a localized loading message through the existing core event. The runtime menu reports applied patches and loaded plugins from the same core flags used by the desktop title bar. |
| USB portals | Android UsbManager grants a borrowed descriptor to the existing libusb hardware backend. Same two VID/PID pairs as desktop, with attach/detach, interface claim and descriptor lifetime handling. Requires a compatible USB host and physical portal for hardware validation. |
| Vulkan trace viewer | Separate Android activity runs the existing Vulkan trace viewer after its surface is ready. Uses the desktop trace parser/player and retains upstream Vulkan limitations, including the unimplemented texture/EDRAM previews. |
| Discord presence (unvalidated opt-in) | An AAR build option selects an Android implementation of the existing DiscordPresence interface. Uses the desktop application ID, presence fields and discord cvar, UI-thread callbacks, and cleanup before title exit/Stop/process teardown. Ordinary builds do not contain the SDK or its cvar. This backend has not been compiled against the official AAR or tested with Discord. |

The former content audit did not establish that extracted save directories were
omitted: `ListFiles` includes directories. The actual correction is metadata and
size parity through ContentManager.

## Previously integrated features

- Library icons and metadata for shared formats; title/release grouping, preferred
  disc, rename, sorting, per-disc actions and disc registration.
- Profiles/GamerTags, four sign-in slots, profile editing/icons, achievements and
  shared native guide/dialog entry points.
- Global/per-title registry-based settings, patches, DLC/title-update management,
  save/content browsing, cache actions, screenshots and GPU trace capture.
- Shared SDL audio, touch/physical gamepads, keyboard driver and raw passthrough,
  controller subtype and assignment, runtime guest-time and diagnostic controls.
- General content installer: immutable packages are linked; writable profile/save
  extraction is explicit and staged, preserving sources and existing destinations.
- Plugins directory, asynchronous native file picker, native folder navigation,
  FAQ, compatibility and version/about links.
- Android build workflow and optional release-signing/publication jobs alongside
  desktop CI. Remote workflow and signing are not yet validated.

## Remaining qualifications

- **Discord presence:** the optional backend is implemented in source, but
  the official Android Social SDK artifact and application setup remain required.
  The normal APK does not enable Discord. No SDK-enabled compile, R8, ABI,
  manifest, presence update or cleanup result is claimed.
  Social SDK 1.10 added Android RPC presence without OAuth; the official SDK
  artifact is still required ([release notes](https://discord.com/developers/docs/social-sdk/release_notes.html)).
- **Translations:** all 34 Android catalogs are complete. Native-speaker review
  of the newly translated text is still welcome. Shared native ImGui dialogs,
  cvar descriptions and diagnostic details retain their upstream language behavior.
- **Debugging:** current desktop host debugger initializes x86-64 Capstone and
  x64 stepping; GDB frontend is Windows-guarded. These have not been represented
  as a completed portable guest debugger.
- **Platform exclusions:** D3D/Metal, host-x64 options, desktop window chrome and
  OS-only helpers such as GameMode, MangoHUD, Rosetta and Game Bar. Removing the
  Library/Recent navigation was an explicit user decision.
- Real disc/title transitions, complete mutable-package extraction, physical
  gamepad mappings/rumble, USB portal I/O and plugin execution need suitable
  hardware/content. Synthetic fixtures cannot establish these runtime outcomes.
- Windows/Linux syntax checks do not replace full build/runtime CI. No changes
  have been published, signed with maintainer keys or tested in remote Actions.

## Validation

- All 34 Android locale catalogs are complete in the latest follow-up.
  `build/android-parity6-complete-build.log` records successful Debug/Release
  builds, R8 and both lint variants with missing translations treated as errors.
  No runtime translation service or new dependency is included.
- `python tools/build/test_android_locales.py` passes `OK (6 tests)`.
  `build/android-parity6-complete-services.log` records `OK (7 tests)` on the
  phone, including all 34 resource locales, numeric/formatted errors, multiline
  strings, plural counts, RTL direction, Chinese script/region matching,
  Tagalog/Filipino aliases, English fallback and existing native/settings fixtures.
- The first locale run exposed Tagalog falling back to English. Using Android's
  `fil` locale fixed runtime resolution; generating both `fil` and `tl` from the
  same catalog also resolved lint errors caused by dependencies using `tl`.
  Those earlier failing runs are not counted as passes.
- Arabic Settings/library and Traditional Chinese Settings were visually
  inspected. Relative card margins and the Latin wordmark were corrected for RTL,
  and the previously selected English override was restored. Screenshots:
  `build/android-parity6-arabic-settings.png`,
  `build/android-parity6-arabic-library.png`,
  `build/android-parity6-chinese-settings.png`.
- The final Debug APK (45,773,425 bytes) was installed over ADB. This completes
  Android resource coverage; native-speaker review of every translation and
  the other qualifications above remain separate.
- `build/android-parity6-runtime.log` records `OK (1 test)` for the final APK:
  After Burner Climax launch, panel toggles, pause/resume, confirmed Stop and
  return to the library. This is a brief smoke test, not compatibility certification.

- The latest follow-up builds Debug and Release (including R8), with both lint
  variants passing: `build/android-parity5-build.log`. Python locale tests pass
  `OK (4 tests)`, including sparse console enum indices.
- `build/android-parity5-services.log` records `OK (7 tests)` on the phone.
  Disposable data verifies global/title reset, preserved profiles and unknown
  desktop settings, lock rejection, legacy override removal, unchanged files
  after invalid TOML, read-only profile metadata and persistence. Resource
  resolution checks cover all 34 selectable locale tags and invalid/reserved
  profile enum fallback.
- The reset confirmation was visually inspected and cancelled on real user
  settings. Profile metadata was inspected without editing the account.
  After Burner Climax's information page now displays the database's Gameplay
  status, explicitly labeled as a desktop report. Screenshots:
  `build/android-parity5-reset-confirmation.png`,
  `build/android-parity5-profile.png` and `build/android-parity5-info.png`.
- The installed Debug APK is 45,193,701 bytes. Gradle's package task was rerun
  to discard unused space left by incremental ZIP updates, without changing
  the compiled payload (`build/android-parity5-package.log`).
- `build/android-parity5-runtime.log` records `OK (1 test)` after reinstalling
  that APK: After Burner Climax launches for five seconds, F6/F7/F8 toggle their
  panels, pause/resume works and confirmed Stop returns to the library. This
  remains a smoke test, not a long-running compatibility result.

- The next follow-up adds the three complete catalogs, validated desktop-label
  aliases and the optional Discord SDK integration. Final ordinary Debug/Release
  builds and both lint variants pass in `build/android-parity4-final-build.log`.
  This build does not include the Discord AAR.
- `build/android-parity4-services.log`: `OK (5 tests)` for native services,
  settings and localization, including es-MX/fr-CA/de-AT fallback, formatted launch
  errors, plural selection and controller array lengths.
- `build/android-parity4-runtime-localized.log`: `OK (1 test)` for After Burner
  Climax launch, F6/F7/F8, pause and confirmed Stop. Two earlier attempts failed
  before game launch because the test used the system language while the app had
  an English override; the test now uses the selected UI locale.
- French and German Settings were visually checked on the phone
  (`build/android-parity4-french.png`, `build/android-parity4-german.png`), then
  the English override observed at the start of these checks was restored.
- `python tools/build/test_android_locales.py`: 3 passing tests. The new checks
  reject missing resource entries, missing plural alternatives, changed array
  lengths, empty values and changed/missing format arguments.
- Final Debug APK (45,126,165 bytes) and test APK installed over ADB; the native
  service/localization rerun passed `OK (5 tests)` in
  `build/android-parity4-final-services.log`. Release unsigned APK: 26,579,027 bytes.
  An invalid SDK path is rejected during Gradle configuration
  (`build/android-parity4-sdk-validation.log`); this is not an SDK-enabled build.

- Follow-up implementation on 2026-09-15 adds explicit UI locale selection,
  native build information, runtime panel toggles and loading/status indicators.
  The locale/settings suite passed `OK (3 tests)` in
  `build/android-parity3-settings-test.log`, using disposable config directories.
  Checks include persistent global locale, per-title exclusion, English fallback,
  regional tags, translated integer formatting and native build metadata.
- Manual phone checks confirmed the English override in Settings and the separate
  profile-management process, then restored System language (Italian on this
  device). About was visually inspected: `build/android-parity3-about.png`.
  The commit/license URLs were checked in code; no browser navigation was required.
- `build/android-parity3-runtime-test.log` records a passing After Burner Climax
  smoke run including F6/F7/F8 open/close, pause/resume, Stop confirmation and
  library return. Real plugin loading remains outside this validation.
- `python tools/build/test_android_locales.py` passes both format-conversion
  tests, including reordered arguments and unsafe-format rejection.
- Final Debug/Release builds and both lint variants pass in
  `build/android-parity3-final-build.log`. The final Debug APK (44,995,193 bytes)
  and test APK were installed over ADB. The final locale/settings/native service
  rerun passed `OK (5 tests)` in `build/android-parity3-final-services.log`,
  including both shader-preloading event payloads through JNI.

- Debug and Release APKs build, including R8; both lint variants complete without
  errors. All merged Android locale catalogs are now required to be complete.
  Missing-translation, placeholder and extra-translation lint checks remain enabled.
- Windows MSVC and Linux Clang syntax checks pass for logging, InputSystem,
  trace viewer/reader, and the Windows hardware portal translation unit where
  applicable. These are not full desktop builds.
- Native management fixtures pass with disposable data across two processes:
  file logging and append, unwritable-log handling, all four JNI session event
  payloads, profile persistence, extracted-save sidecar names/sizes, package
  sizes/links, provider boundaries/locks, root relocation and global config.
- Consolidated non-visual suite: JUnit reports `OK (20 tests)`; 19 execute and
  the optional real-library artwork test is skipped without its opt-in flag.
  Covers native management, input/hotkeys/mapping, synthetic artwork/container
  formats and configuration. Log: `build/android-parity2-headless.log`.
- Standalone UI suite: `OK (2 tests)` after the phone became available. Checks
  session exit navigation, rejection of a mismatched trace version, and opening
  a synthetic one-frame trace through the Vulkan presenter. The viewer screenshot
  was inspected (`build/android-trace-viewer-smoke.png`). This is not replay of
  a real game's GPU command stream.
- Device follow-up on 2026-09-15: the latest Debug APK and instrumentation APK
  were installed over ADB. Profile UI passed cancellation, invalid-name rejection,
  creation, persistence, automatic login and signed-out selection across fresh
  processes, using disposable storage. This resolves the earlier interrupted
  profile UI check (`build/android-parity2-resume-ui.log`, profile test only;
  the combined run still had failures in other tests).
- After Burner Climax passed the five-second launch smoke check, pause/resume,
  Stop confirmation and return to the library: `OK (1 test)` in
  `build/android-parity2-stop-final.log`. The test scrolls the final row fully
  into view and allows one retry of that list row if the first tap only stops
  scrolling. It still requires the unsaved-progress warning and explicitly
  confirms Stop. Earlier failures left emulation open and consequently blocked
  the viewer in combined runs; they are not counted as passes.
- The final standalone rerun passed `OK (2 tests)` in
  `build/android-parity2-viewer-final.log`, including the fullscreen refinement.
  The updated viewer screenshot was inspected. These smoke checks do not prove
  real-game GPU trace replay or long-running game compatibility.
- USB hardware I/O and the SDK-enabled Discord build are not validated. No physical
  portal or Android Discord SDK was available.

Build logs: `build/android-parity2-final-build.log`,
`build/android-parity2-release.log`. Native service regression:
`build/android-parity2-services.log`. Desktop checks:
`build/parity2-windows-check.log`, `build/parity2-linux-check.log`.
