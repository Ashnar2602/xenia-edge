# Android preview

This target builds the current emulator core, AArch64 CPU backend and Vulkan
backend into an Android ARM64 APK. It reuses the existing Android window and JNI
code. The Android frontend provides a game library, search,
video settings, game launching, player profiles, touch controls and gamepad input.
Audio uses the desktop SDL backend; Android forwards controller vibration.
Compatibility remains experimental. See [the source-level parity audit](android-feature-parity.md).

The Android frontend follows the device language by default. Settings →
Interface language can override it using the desktop `UI.ui_locale` TOML key;
System language clears the override. All 34 selectable Android language catalogs
are complete, combining shared desktop labels and Android-specific resources.
Unsupported system languages fall back to English. Android's file picker and permission settings retain the system
language. Language changes do not restart a running native session.

Desktop targets retain their entry points; Android CI is added alongside the
desktop release workflow. No code from an external Android port is needed.

## Requirements

- Git, Python 3 and a native C++20 compiler (Visual Studio 2022 on Windows).
- CMake 3.22.1 and Ninja on `PATH`.
- JDK 17 (`JAVA_HOME`).
- Android SDK (`ANDROID_HOME`), platform 35, build-tools 35.0.0 and NDK
  **29.0.14206865**. The Gradle wrapper supplies Gradle 8.11.1.
- An ARM64 Android 11 / API 30 or newer device with Vulkan for startup testing.

Install SDK components with Android Studio's SDK Manager or `sdkmanager`.
Accept their licenses before building. The helper downloads the pinned source
dependencies, Slang and the usual Xenia data on its first run.

## Build and install

From the repository root:

```sh
python android/build.py --config Debug --jobs 6
adb install -r android/android_studio_project/app/build/outputs/apk/github/debug/app-github-debug.apk
adb shell am start -W -a android.intent.action.MAIN -c android.intent.category.LAUNCHER -n jp.xenia.emulator.github.debug/jp.xenia.emulator.LauncherActivity
```

The debug APK uses Android's development signing key and a separate application
ID, `jp.xenia.emulator.github.debug`. For several connected devices, add
`-s SERIAL` to each `adb` command. App-private config and content live under
the activity's `files` directory. The existing Android logger writes to Logcat:

```sh
adb logcat -s xenia
```

`--config Release` produces an unsigned release APK; distribution signing and CI
publishing are separate steps. The initial target supports `arm64-v8a` only.

## Build integration

`android/build.py` first builds the existing `tools/build/shader_cc.cc` as a native
host executable with the small `tools/build/CMakeLists.txt` project. Gradle then
cross-compiles `xenia-app` through the root CMake project, passing the host shader
compiler as `XENIA_HOST_SHADER_CC`. Shader generation runs on the build machine;
the resulting library and shaders target Android.

For an already prepared checkout, the equivalent Gradle invocation from
`android/android_studio_project` is:

```sh
./gradlew assembleGithubDebug -PxeniaHostShaderCompiler=/absolute/path/to/xenia-shader-cc -PxeniaBuildJobs=6
```

On Windows use `gradlew.bat` and the native `.exe` path. Android-specific target
selection is in `src/xenia/app/android.cmake`; desktop executables retain their
existing entry points and dependencies.

## Player profiles

Before the first game starts, Android asks for a local Gamertag using the system
keyboard. This form uses portrait orientation so landscape keyboards cannot hide
the input; the player's previous orientation is restored on completion. It calls
the same `ProfileManager::IsGamertagValid`, `CreateProfile`
and `Login` used by the desktop, including the 15-character limit. Player 1 must
be signed in before loading; cancelling returns to the library. If accounts
already exist but none is signed in to player 1, a selection dialog is shown.

The encrypted Account, GPDs and saves use the desktop content layout under
`files/content`. Login persists `logged_profile_slot_0_xuid` immediately in
`files/xenia-edge.config.toml`, so the next session signs in automatically.
The first-profile flow also offers the desktop legacy-data migration when
existing content is found. No separate Android profile database or game-media
copy is introduced.

`AndroidProfilesTest` exercises the real native core and Android dialogs with a
UUID-scoped cache directory and a separate debug-only process: cancellation,
invalid names, creation, restart/autologin and selection of a signed-out account.
It does not create a profile in the user's content directory.

```sh
adb shell am instrument -w -e class jp.xenia.emulator.AndroidProfilesTest jp.xenia.emulator.github.debug.test/androidx.test.runner.AndroidJUnitRunner
```

Device validation on the HONOR BVL-N49 passes the complete profile test and the
real player's first-launch prompt/cancel path using After Burner Climax. The
profile fixture is deleted after testing; no account is added to user storage.
Evidence: `build/android-profile-device-test.txt`,
`build/android-profile-launch-test.txt`, `build/android-profile-prompt.png`.
The APK build and Android Lint pass (0 errors, 71 warnings).

## Per-game options

Long-press a library title and choose **Game options**, or open it from the
player's pause menu. The portrait editor offers category filtering, text search,
the core's descriptions and typed controls, including the desktop enum choices.
All registered Android options, including advanced settings, are available except
unsupported backend/architecture settings (D3D/DXGI/DXIL, Metal/MoltenVK, x64, XAudio,
XInput), backend selectors fixed by the Android frontend, and app-managed storage,
profile, window and internal configuration controls. Guest ImGui font options
remain available. Descriptions retain their upstream wording.

Values are overrides for the title ID, written to the existing
`files/config/<TITLEID>.config.toml` format. **Reset** removes that one override
and inherits the general value. Unrelated and unknown TOML keys are preserved;
invalid values or unreadable TOML are rejected without replacing the file.
Android's `AtomicFile` protects writes, and the settings screens share one IO
worker. Neither the game media nor the running emulator's cvars are changed.

Changes apply on the next game launch. Android now loads title overrides before
core initialization, so CPU/kernel options take effect too. The general Android
resolution/FPS controls supply base values instead of command-line overrides,
allowing per-title values to take precedence.

`GameSettingsTest` checks filtering, enum conversion, numeric ranges, malformed
TOML, preservation/reset and file isolation. With `-e settings_game 'TITLE'`, it
also checks the real editor and boots an isolated core against temporary config
to verify the guest clock and video settings. These tests never save options into
the user's title config. Device results: `build/android-settings-device-test.txt`.
All four tests pass on the HONOR BVL-N49, as does the After Burner Climax smoke
test (8 seconds plus opening options, returning to the player, pause/resume and
returning to the library). The APK build and lint pass with 0 errors (73 warnings).
Launch evidence: `build/android-settings-launch-test.txt`.

## Games and storage

The frontend requests Android's **All files access** before adding or launching
games. Enable it on the system settings screen opened by the app. For ADB testing:

```sh
adb shell appops set --uid jp.xenia.emulator.github.debug MANAGE_EXTERNAL_STORAGE allow
```

ISO/ZAR images are opened at their original local paths. Adding an XEX/GOD folder
scans that folder and stores references to the executables; sibling data files
remain in place. **No game images or folders are copied or staged**, and removing
a library entry does not delete the source. Moving a source requires adding it
again. App-private saves, configuration and generated shader caches are separate
from game media.

Library artwork is extracted before launch from XEX resources (SPA/XDBF), from
`default.xex` inside ISO/ZAR, and from the title thumbnail in STFS/GOD/CON/LIVE/PIRS
headers. The reader reuses the existing container readers, AES keys/decryption
and LZX decoder without initializing the emulator. Encrypted retail/devkit and
uncompressed, basic-compressed and LZX-compressed executables are supported.
Executable decoding uses bounded transient memory (128 MiB per image); no game
media is written. Missing, malformed or unsupported resources retain the placeholder.
ELF/homebrew without embedded title artwork and standalone delta patches do not
provide an icon through this reader.

Android loads artwork on one background worker and deduplicates pending requests.
Only thumbnails are cached: 8 MiB of decoded images in memory and a periodically
trimmed disk cache targeting 32 MiB/1,024 entries. Cache keys include the source
path, size and modification time; unavailable files are retried when displayed
again. PNG dimensions and resource offsets are checked before decoding.

Game cards show the complete embedded artwork in a square `FIT_CENTER` viewport
and the title. Tapping the card launches the game; long-pressing opens Information,
Play and Remove from list. Information is a separate, scrollable Android screen
with title/media IDs, version, disc, last launch, format, file size and source
path. Unavailable metadata and unverified compatibility are identified explicitly.
The metadata path shares the artwork reader's container validation, but reads only
XEX headers (including inside ZAR), without decrypting or decompressing the image.
Seven artwork/metadata tests passed on the device, including all seven library
titles and malformed execution-info offsets. Cards, the context menu and the
information screen were also checked visually on-device.

Local primary storage and mounted SD/USB volumes are supported through the
Android storage-volume APIs. Cloud/virtual documents without a readable local
path are rejected. This permission preserves ordinary native file and mapping
operations, including ZArchive's `std::ifstream`, without changing desktop readers.
See [Android all-files access](https://developer.android.com/training/data-storage/manage-all-files).

The player runs in a separate `:emulation` process so each title starts with fresh
emulator globals while the library remains independent. The menu offers pause,
resume, touch-control visibility and closing the title. Cooperative guest threads
are paused through `XThread::Suspend/Resume` rather than host-thread handles.
Closing the title navigates to `LauncherActivity` before destroying the isolated
player process. This also recreates the library if Android discarded it in the
background. Android does not call `KernelState::TerminateTitle` for this action:
that routine calls `quick_exit`, so Java navigation after it would never run.
Audio uses the existing `apu::sdl::SDLAudioSystem` / `SDLAudioDriver`, including
its guest 5.1 conversion and stereo downmix. SDL's own Android JNI classes are
compiled directly from the existing submodule; no separate Android mixer or media
copy is introduced. The player requests game audio focus and pauses guest input
on focus loss. A small Android lifecycle adapter pauses the SDL output streams
without fencing the cooperative guest workers. The phone volume keys control media volume. Optional SDL HID/BLE
code is not initialized; its permission lint findings are ignored only in those
upstream source files, without requesting unused Bluetooth permissions.

Android now shares `hid::GamepadKeystroke` with the desktop SDL driver: face and
shoulder buttons, Start/Back, stick presses, D-pad, trigger thresholds, eight-way
stick navigation and held-key repeats. Key releases take priority over repeats.
The Android bridge retains up to 64 state transitions so a short tap is not lost
between guest keystroke polls. Physical input supports one Android gamepad, digital
or analog triggers, separate hat/button state and reset on unplug or pause.
Controller vibration is still unsupported. The same desktop ImGui renderer and
input-system connection handle guest confirmation/sign-in dialogs on Android.

## Device tests

```sh
./gradlew assembleGithubDebugAndroidTest
adb install -r app/build/outputs/apk/androidTest/github/debug/app-github-debug-androidTest.apk
adb shell am instrument -w -e class jp.xenia.emulator.AndroidLaunchTest jp.xenia.emulator.github.debug.test/androidx.test.runner.AndroidJUnitRunner
```

Run these commands from `android/android_studio_project` after building/installing
the app. The library-navigation test needs no game. Supplying `-e game_name
"TITLE AS SHOWN IN THE LIBRARY"` enables the launch test using an already-added
local title. It checks direct source access, loading completion and player survival
for 30 seconds, then pause, resume, close and return to the library; it does not
establish gameplay correctness. The test writes
`launch-test.png` and `launch-test.xml` under the app's external files directory.
It sets the display to its natural orientation for library navigation and releases
the rotation lock afterward. `-e game_run_seconds 5` selects a short navigation
regression run; it does not replace the default 30-second stability check.

The artwork tests use synthetic XEX/ISO/ZAR/STFS files, including retail/devkit
encryption, all three compression modes, invalid offsets, truncated resources
and corrupt archive metadata:

```sh
adb shell am instrument -w -e class jp.xenia.emulator.GameIconsTest jp.xenia.emulator.github.debug.test/androidx.test.runner.AndroidJUnitRunner
```

Add `-e check_library_icons true` to also require readable embedded icons for
every title currently in the library and check that source size/mtime do not
change. On reconnection, all six artwork tests passed with the final image-base
override and XEX1 cases, including all seven ZAR titles then present in the
library. Their seven cached PNGs total about 62 KiB. Android Lint reports
0 errors and 29 warnings. The three new/affected
shared translation units also passed MSVC C++20 syntax checking; this is not a
full desktop build.
Android's process-exit history attributes the earlier `Process crashed` run to
low memory. A subsequent run loaded the game and passed pause/resume, but
exposed the `quick_exit` navigation issue described above. The launch regression
now opens the selected title through its library card.

After the Android-only close fix, the two navigation/lifecycle tests passed with
`game_run_seconds=5` (17.539 seconds total), including pause, resume, close and
return to the library. The updated APK is installed on the test phone.
**The 30-second stability check still fails:** both the instrumented run and a
separate host-driven ADB run encountered low-memory kills after roughly 30–40
seconds of emulation at 1x resolution. System logs confirm the low-memory killer
terminating the foreground emulator with about 516 MiB available. The underlying
memory consumption still needs investigation; a passing short navigation test
does not establish sustained game stability. Evidence is saved locally under
`build/android-closure-tests.txt`, `build/android-return-library-exit-info.txt`,
`build/android-external-exit-info.txt` and `build/android-low-memory-events.txt`.

## Input and audio checks

`AndroidInputAudioTest` tests touch multitouch/cancel, physical button/trigger
state, all native XInput keys, analog thresholds/directions, repeats, fast taps
and bounded backlog. Its audio test opens the actual SDL device, submits quiet
440 Hz guest-format frames, and verifies consumption plus driver pause/resume.
Native test entry points are included only in Debug builds.

```sh
adb shell am instrument -w -e class jp.xenia.emulator.AndroidInputAudioTest jp.xenia.emulator.github.debug.test/androidx.test.runner.AndroidJUnitRunner
```

The native input checks also passed on the Windows host with a deterministic
clock, and the shared helper plus the desktop SDL input driver passed MSVC and Linux Clang syntax
validation. Device audio and gameplay validation must be recorded separately;
a successful build or input unit check is not evidence of audible game output.
The updated Debug app and instrumentation APK build successfully; Android Lint
reports 0 errors (69 warnings, including the newly referenced SDL Java sources).
The updated APK is installed on the HONOR BVL-N49. All four
`AndroidInputAudioTest` checks pass on the phone, including SDL consuming actual
guest-format frames and driver pause/resume.

A startup regression occurred when the first guest audio client also initialized
SDL audio. Initializing SDL audio on the Java-owned preparation worker before
launching guest fibers fixes the immediate SIGSEGV in both AC2 and After Burner
Climax. The same SDL backend remains in use. After Burner then exposed a separate
Debug assertion rejecting zero-stride vertex fetches, although the SPIR-V backend
already handles these as constant-address fetches. That assertion was removed,
and the trace viewer avoids dividing by a zero stride. These shared shader changes
pass MSVC and Linux Clang syntax validation.

After the fixes, the launch/navigation tests pass for AC2 (12 seconds) and After
Burner Climax (15 seconds), including pause/resume/return to the library. Captures
show their startup logos, not just the Android overlay. Evidence:
`build/android-fixed-ac2-test.txt`, `build/android-fixed-afterburner-test.txt`,
`build/android-device-input-audio.txt`, and `build/android-launch-fix-build.log`.
These short checks do not establish complete gameplay or long-session stability.

## Initial validation

Based on upstream `edge` commit `8205e00e8a1ed1b8d9921a1e98987e4df0179a50`.
The signed Debug APK was built with the helper on Windows and installed through
ADB on an HONOR BVL-N49 running Android API 36 (Adreno 750). Assassin's Creed II
was opened directly from its original 5.3 GB ZAR, Vulkan initialized and
`Android title launched` was reached. The initial NOP audio registration failure
was replaced with the Android silent driver. A one-condition correction in
`GetNonCoherentMappedRange` allows the valid `VK_WHOLE_SIZE` sentinel in Debug
assertions; Release behavior is unchanged. Navigation and the 30-second launch
test including pause/resume/return to the library passed, with the game visibly
reaching **PRESS START**. Android Lint passed with 0 errors (19 warnings).
Full gameplay is
not yet validated.
Native Android Release also linked successfully with LTO off.

Windows and Linux CMake configuration and `xenia-base` compilation passed. The
platform source selection was compared with upstream for x64 and ARM64 and was
identical on both desktop platforms. Full desktop release builds have not been
validated: the Windows dependency build encounters a MASM `FRAME` error in the
unchanged Boost.Context source.

Full game compatibility, sustained audio playback and broader device/controller
testing remain separate from frontend feature coverage.

## Desktop features on Android

The Android frontend now exposes the shared services through these entry points:

| Entry point | Features |
| --- | --- |
| Library settings | Full global cvar editor, profiles, Android Files access |
| Game information | DLC/title updates, saves, bundled patch toggles, achievement totals and desktop compatibility reports |
| In-game menu | Four controller assignments, desktop post-processing/performance/debug/audio/profile dialogs, screenshot, GPU cache clearing and optional profiler |
| Guest dialogs | Android soft keyboard and direct touch input; native disc picker with zero-copy file selection |

Global and per-title settings use the same registry and TOML files as desktop.
The previous Android resolution/FPS preferences migrate when a global setting
is saved. Unsupported backends remain filtered. English is the resource fallback,
with Italian selected automatically by Android; shared native dialogs and cvar
descriptions retain their desktop wording.

Profiles use the existing ProfileManager and UserTracker, including four login
slots, encrypted accounts, gamer name/motto/bio/zone, country/language, Live and
subscription fields, and profile images. First-profile creation uses the same
migration prompt as game launch. Management runs in its own process with a serial
worker and an exclusive file lock. A running game, another management process or
an external writer cannot concurrently change the same data.

DLC and title updates are **linked, not copied**, into the core's content folders.
Title and update-release checks match desktop. Detaching a package preserves the
original. Android Files exposes content, per-title configuration, patches, host
cache, screenshots, plugins and traces for import/export and file management. Linked files are
read-only and linked directories cannot be traversed. Large game files remain
at their original paths.

The desktop GameLibrary records disc metadata for Android titles and serves the
core's disc-change dialog. An unknown disc can be selected with Android's file
picker; its selection is registered through the provider in the library process
so it survives catalog synchronization. The Android launcher groups registered
paths by title and version, with per-disc launch, removal and a default disc. Metadata is cached and scanned off the UI
thread. Release names and sorting preferences persist.

The input driver exposes four XInput users; Android keeps physical device
assignments by stable descriptor and combines touch with player one. Rumble uses
the selected controller's vibrator, or the phone for touch player one, and stops
when the session loses focus. Motor strengths are combined for Android's generic
vibrator API. Physical multi-controller and rumble behavior still need testing
on the relevant hardware.

Post-processing configuration and profile choice tables are extracted from the
desktop frontend into shared helpers. Existing desktop entry points forward to
them. The runtime dialogs take Emulator directly, avoiding an Android copy of
their implementations. Android uses density-independent sizing and constrained
dialog bounds. Touch presses and releases are queued through ImGui so taps
between frames are retained.

Like desktop, the profiler is an optional build feature. Build Debug with
`-PxeniaProfiler=ON` to enable it; the menu only displays it when the native
profiler UI is compiled. Normal builds retain upstream's disabled default.

`AndroidFeaturesTest` uses temporary UUID storage to exercise account/GPD
persistence, four sign-in slots, profile icons, patch persistence and rejected
edits, DLC/TU links and removal, provider path boundaries and data locks, and
global settings migration. Synthetic content headers test management operations;
they do not establish that a real DLC or update executes correctly in a game.

## Android CI and release signing

`CI.yml` calls `android.yml` alongside the existing desktop build. The Android
job uses Java 17, SDK/build-tools 35, NDK 29.0.14206865 and CMake 3.22.1 on Windows,
builds the native shader compiler and Release APK, runs lint, and uploads the
unsigned APK for review. The release build also exercises R8 shrinking.

For upstream pushes to `edge`, configure these repository secrets to enable
signed APKs:

- `ANDROID_KEYSTORE_BASE64`: Base64-encoded release keystore.
- `ANDROID_KEYSTORE_PASSWORD`: Keystore password.
- `ANDROID_KEY_ALIAS`: Signing key alias.
- `ANDROID_KEY_PASSWORD`: Signing key password.

The workflow signs and verifies with Android's
[apksigner](https://developer.android.com/tools/apksigner), then a separate job
attaches `xenia_edge_android.apk` to the matching desktop release. Signing secrets
are not placed in the build environment or used for PR builds. Without them,
the unsigned review artifact remains available. Desktop release creation does
not depend on Android success. No release key is generated or committed here.
CI version codes use the commit timestamp and version names use the short SHA;
local builds default to development version 1.

Earlier validation (before the second review) on the connected HONOR BVL-N49 (Android 16, Adreno 750)
completed Debug and Release builds, both lint variants and the instrumentation
suite without failures (`OK (22 tests)` after the portable-feature follow-up). Optional tests requiring explicit
game arguments retain their assumptions; this result is not a compatibility
count. The unsigned Release APK is approximately 25 MiB. Local logs are in
`build/android-features-*` and `build/android-parity-*`.

Manual checks with After Burner Climax covered opening the shared runtime
dialogs, touch taps, Back navigation, soft-keyboard text entry and PNG screenshot
capture. Real DLC/update execution, multi-disc transitions, physical controller
assignments and rumble remain device/game validation work.

Windows MSVC and Linux Clang syntax checks cover the changed shared frontend
translation units, including the desktop window and emulator. These checks do
not replace full desktop build/runtime CI or long-term Android compatibility
testing. Remote GitHub Actions and release signing need verification after the
branch is published and maintainer signing secrets are configured.

## Portable feature follow-up

The shared keyboard driver now provides configurable keyboard-to-gamepad input
and raw passthrough (`keyboard_passthrough`). Choose Keyboard in a controller
slot for gamepad emulation; changing bindings in settings takes effect on the
next session. Controller subtype overrides persist per slot. Guide opens the
menu when `UI.guide_button` is enabled (the default). Back/Select remains guest
input. The main menu also exposes time control, rumble and GPU cache clearing.
The old controller chords were removed together with upstream's
`controller_hotkeys` option.

Content import accepts multiple packages. Games, DLC and updates stay in place.
Packed profiles and saves require writable extracted data, so the import UI asks
before using the desktop extractor. Extraction is staged; neither source files
nor existing installed data are overwritten. Manage plugins opens the same
`files/plugins` directory consumed by the native loader.

Native Gamercard image picking uses the asynchronous FilePicker API; a callback
for a closed dialog is discarded. Shared native folder buttons route through an
internal Android activity and the scoped document provider. Android Files may
use its system picker as a fallback. Path cvars also have a file/folder browser.


## Second feature review

Global options expose portable guest mounts and `content_root` / `cache_root`.
Relative roots are resolved under app storage. These roots and `log_file` are
app-wide; per-title settings do not relocate shared data. Changing roots does
not migrate existing profiles/saves. Zero-copy package linking needs a filesystem
that supports symlinks; unsupported external filesystems report an error instead
of copying. Grant local-storage access before using external paths.

File logging is opt-in: set global `Logging.log_file` to a filename or use its
save-file chooser. Logcat remains available. The document provider exposes the
global config and configured log (log read-only), and follows relocated roots.
Separate-process title relaunch preserves launch arguments and appends the log.

Controller assignments now distinguish Virtual controller and Disconnected.
Choose Controller mappings for an assigned physical device to override buttons,
axes and inversion, or reset to Android's defaults. `UI.guide_button` controls
whether the mapped Guide button opens the menu, with no Back-button fallback.

Open Vulkan trace in launcher settings starts the existing trace viewer in its
own process. Texture/EDRAM previews retain upstream Vulkan TODOs. USB portals use
Android's device permission prompt and the same libusb backend/VID-PID pairs as
desktop (1430:1F17, 24C6:FA00); no root access or USB permission bypass is used.

The build reuses matching labels and supported printf messages from upstream PO catalogs through
`tools/build/android_locales.py`. Android-specific XML completes all 34 selectable
languages; the build rejects missing merged resources. English remains the default
for unsupported device languages. See [Android localization](android-localization.md)
for catalog maintenance, aliases, scope and translation provenance.
See [feature parity](android-feature-parity.md) for remaining qualifications and
current validation, including the separate Discord SDK dependency.


Current builds and lint results are recorded in
`build/android-parity4-final-build.log`; management fixtures use a result file
and close their own Activity before restarting, so AOD does not prevent testing
native services. Their disposable storage is separate from user profiles.
The latest Debug APK was installed over ADB on 2026-09-15. Profile UI checks
passed with disposable accounts; targeted launch/pause/Stop and standalone
viewer/navigation reruns also passed. See `build/android-parity2-stop-final.log`,
`build/android-parity2-viewer-final.log` and the parity report for the distinction
between verified code paths and pending hardware/game checks.

The latest follow-up also verifies locale persistence/fallback, native build
metadata and shader-preloading notifications (`build/android-parity3-final-services.log`),
plus the F6/F7/F8 panel toggles and pause/Stop flow on a game
(`build/android-parity3-runtime-test.log`). About now links to the build commit/PR
and license; the runtime menu reports applied patches and loaded plugins from
the core's existing status flags.

## Optional Discord Social SDK (not yet validated with the official AAR)

Normal Android builds have no Discord dependency. An optional Android backend
is prepared using the same application ID, presence fields and `discord` setting
as desktop. It uses unauthenticated RPC presence from Social SDK 1.10 or newer.
Obtain the official `discord_partner_sdk.aar` from the Discord Developer Portal;
the SDK is not redistributed in this repository.

```powershell
python android/build.py --config Release --discord-sdk C:/SDKs/discord_partner_sdk.aar
```

For direct Gradle invocations, pass
`-PxeniaDiscordSdk=C:/SDKs/discord_partner_sdk.aar` (an absolute path is recommended).
The same property can be supplied to CI with
`ORG_GRADLE_PROJECT_xeniaDiscordSdk` after provisioning the official AAR on the runner.
This enables Prefab, links the Android backend, uses the shared C++ runtime and
adds the SDK Java bridge. SDK voice permissions are removed by an opt-in manifest
overlay; no OAuth redirect, login flow or voice session is added.

The bridge is initialized on the first title launch if `discord` is enabled,
pumped by the existing UI poll, and cleared before title handoff, Stop or Activity
destruction. Build-time SDK validation currently checks the local AAR path only.
The actual SDK version, native ABI/STL compatibility, consumer R8 rules, merged
manifest and presence lifecycle still need verification with the official package
and an installed Discord client. The upstream Discord application's Social SDK
configuration also needs verification by its owner.

Official references: [Android integration](https://docs.discord.com/developers/discord-social-sdk/development-guides/account-linking-on-mobile)
and [1.10 Android RPC release notes](https://discord.com/developers/docs/social-sdk/release_notes.html).
