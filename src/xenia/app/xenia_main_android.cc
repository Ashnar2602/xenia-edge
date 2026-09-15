#include "xenia/app/game_compat_db.h"
#include "xenia/app/game_icon.h"
#include "xenia/app/game_library.h"
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Developers. All rights reserved.                      *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <jni.h>
#include <memory>

#include <SDL3/SDL.h>
#include "third_party/imgui/imgui.h"
#include "xenia/app/android_audio.h"
#include "xenia/app/android_input.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform_arm64.h"
#include "xenia/base/profiling.h"
#include "xenia/config.h"
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/helper/sdl/sdl_helper.h"
#include "xenia/hid/keyboard/keyboard_input_driver.h"
#include "xenia/hid/portal/hardware_portal.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/patcher/patcher.h"
#include "xenia/patcher/plugin_loader.h"
#include "xenia/ui/display_config.h"
#include "xenia/ui/graphics_provider.h"
#include "xenia/ui/imgui_audio_dialog.h"
#include "xenia/ui/imgui_debug_dialog.h"
#include "xenia/ui/imgui_drawer.h"
#include "xenia/ui/imgui_performance_dialog.h"
#include "xenia/ui/imgui_postprocessing_dialog.h"
#include "xenia/ui/profile_dialogs.h"
#include "xenia/ui/window.h"
#include "xenia/ui/window_android.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context_android.h"

DEFINE_path(storage_root, "", "Android app data directory.", "Storage");
DEFINE_string(ui_locale, "",
              "UI locale as an ISO code. Empty selects the system default.",
              "UI");
DEFINE_path(content_root, "",
            "Guest content directory; empty uses app storage.", "Storage");
DEFINE_path(cache_root, "", "Host cache directory; empty uses app storage.",
            "Storage");
DEFINE_bool(guide_button, true,
            "Toggle the context menu with the guide button.", "UI");
DECLARE_uint32(launch_flags);
DECLARE_string(launch_data);
DEFINE_transient_path(android_game_path, "", "Title to prepare.", "Config");
DEFINE_CVar(android_resolution, -1, "Legacy Android general resolution.",
            "Config", true, int32_t);
DEFINE_CVar(android_fps_limit, -1, "Legacy Android general FPS limit.",
            "Config", true, int32_t);

namespace xe::app {

// The Android activity owns one session. Graphics are initialized once the
// SurfaceView is ready; loading/termination run on its serial worker thread.
class AndroidEmulatorApp final : public ui::WindowedApp {
 public:
  static std::unique_ptr<ui::WindowedApp> Create(
      ui::WindowedAppContext& context) {
    return std::unique_ptr<ui::WindowedApp>(new AndroidEmulatorApp(context));
  }

  bool OnInitialize() override {
    Profiler::Initialize();
    for (size_t i = 1; i < input_.size(); ++i) {
      input_[i].connected = false;
    }
    Profiler::ThreadEnter("Main");
    arm64::InitFeatureFlags();
    const auto storage = cvars::storage_root;
    if (storage.empty()) {
      XELOGE("Android storage_root was not supplied by the activity");
      return false;
    }
    config::SetupConfig(storage);
    // Storage roots belong to the app, shared with Tools and DocumentsProvider.
    const auto content_root = cvars::content_root;
    const auto cache_root = cvars::cache_root;
    EnableAndroidFileLogging(storage);
    // General Android video choices are defaults, not command-line overrides:
    // the title config must win, and CPU/kernel settings must precede Setup.
    toml::value<int64_t> scale(std::clamp(cvars::android_resolution, 1, 2));
    toml::value<int64_t> fps(std::max(cvars::android_fps_limit, 0));
    if (cvars::android_resolution >= 0) {
      cvar::ConfigVars->at("draw_resolution_scale_x")->LoadConfigValue(&scale);
      cvar::ConfigVars->at("draw_resolution_scale_y")->LoadConfigValue(&scale);
    }
    if (cvars::android_fps_limit >= 0) {
      cvar::ConfigVars->at("framerate_limit")->LoadConfigValue(&fps);
    }
    if (!cvars::android_game_path.empty()) {
      config::LoadGameConfigForFile(cvars::android_game_path);
    }
    window_ = ui::Window::Create(app_context(), "Xenia Edge", 1280, 720);
    if (!window_ || !window_->Open()) {
      return false;
    }
    imgui_drawer_ = std::make_unique<ui::ImGuiDrawer>(window_.get(), 10);
    emulator_ = std::make_unique<Emulator>(
        "", storage,
        content_root.empty()
            ? storage / "content"
            : std::filesystem::absolute(storage / content_root),
        cache_root.empty() ? storage / "cache_host"
                           : std::filesystem::absolute(storage / cache_root));
    const auto result = emulator_->Setup(
        window_.get(), imgui_drawer_.get(), true,
        [](cpu::Processor* processor) {
          return std::make_unique<AndroidAudioSystem>(processor);
        },
        []() { return std::make_unique<gpu::vulkan::VulkanGraphicsSystem>(); },
        [this](ui::Window* window) {
          std::vector<std::unique_ptr<hid::InputDriver>> drivers;
          drivers.emplace_back(
              std::make_unique<AndroidInputDriver>(window, input_));
          drivers.emplace_back(
              std::make_unique<hid::keyboard::KeyboardInputDriver>(window, 0));
          return drivers;
        });
    if (XFAILED(result)) {
      XELOGE("Android core initialization failed: {:08X}", result);
      return false;
    }
    XELOGI("Android core initialized");
    {
      auto* input = emulator_->input_system();
      auto lock = input->lock();
      for (uint32_t slot = 0; slot < 4; ++slot) {
        auto* driver = input->GetSlotBinding(slot).driver;
        if (dynamic_cast<hid::keyboard::KeyboardInputDriver*>(driver)) {
          input->UnbindSlot(slot);
        }
      }
    }
    emulator_->set_on_launch_new_title(
        [this](const std::string& path, const std::string& module,
               uint32_t flags, const std::string& data) {
          SessionEvent(0, {path, module, std::to_string(flags), data});
        });
    emulator_->set_on_exit_to_dashboard([this]() {
      SessionEvent(1, {});
      return false;  // Kernel terminates this isolated process after
                     // navigation.
    });
    emulator_->on_launch.AddListener([this](uint32_t title,
                                            std::string_view name) {
      auto xam =
          emulator_->kernel_state()->GetKernelModule<kernel::xam::XamModule>(
              "xam.xex");
      SessionEvent(2, {std::string(name), std::to_string(title),
                       xam ? xam->loader_data().host_path : ""});
    });
    emulator_->set_on_disc_swap(
        [this](uint8_t disc) { SessionEvent(3, {std::to_string(disc)}); });
    emulator_->on_shader_storage_initialization.AddListener(
        [this](bool initializing) {
          SessionEvent(5, {initializing ? "1" : "0"});
        });
    game_library_ = std::make_unique<GameLibrary>(storage / "library");
    game_library_->Load();
    emulator_->set_disc_provider([this](uint32_t title) {
      std::vector<Emulator::TitleDisc> discs;
      for (const auto& entry : game_library_->entries()) {
        if (entry.title_id != title) {
          continue;
        }
        for (const auto& item : entry.paths) {
          discs.push_back({fmt::format("Disc {} · {}", item.disc_number,
                                       xe::path_to_utf8(item.path.filename())),
                           item.path});
        }
      }
      return discs;
    });
    emulator_->set_disc_recorder(
        [this](uint32_t title, const std::filesystem::path& path) {
          game_library_->AddDisc(title, "", path);
        });
    emulator_->set_disc_picker([this](const std::string&) {
      auto request = std::make_shared<DiscRequest>();
      {
        std::lock_guard<std::mutex> lock(disc_mutex_);
        if (disc_request_) {
          return std::filesystem::path();
        }
        disc_request_ = request;
      }
      kernel::GuestScheduler::WaitOnFence(request->ready);
      return request->path;
    });
    return true;
  }

  void SessionEvent(int event, std::vector<std::string> values) {
    app_context().CallInUIThreadSynchronous([this, event,
                                             values = std::move(values)]() {
      auto& context =
          static_cast<ui::AndroidWindowedAppContext&>(app_context());
      auto* env = context.ui_thread_jni_env();
      auto activity = context.activity();
      auto cls = env->GetObjectClass(activity);
      auto method =
          env->GetMethodID(cls, "sessionEvent", "(I[Ljava/lang/String;)V");
      auto strings = env->FindClass("java/lang/String");
      auto data = env->NewObjectArray(jsize(values.size()), strings, nullptr);
      for (jsize i = 0; data && i < jsize(values.size()); ++i) {
        auto text = xe::to_utf16(values[i]);
        auto item = env->NewString(reinterpret_cast<const jchar*>(text.data()),
                                   jsize(text.size()));
        env->SetObjectArrayElement(data, i, item);
        env->DeleteLocalRef(item);
      }
      if (method && data) {
        env->CallVoidMethod(activity, method, event, data);
      }
      if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
      }
      env->DeleteLocalRef(data);
      env->DeleteLocalRef(strings);
      env->DeleteLocalRef(cls);
    });
  }
  X_STATUS Prepare(const std::filesystem::path& path) {
    XELOGI("Android preparing {}", xe::path_to_utf8(path));
    auto* probe = xe::filesystem::OpenFile(path, "rb");
    if (!probe) {
      XELOGE("Android game open failed: {}", strerror(errno));
      return X_STATUS_ACCESS_DENIED;
    }
    fclose(probe);
    if (emulator_->GetFileSignature(path) ==
        Emulator::FileSignatureType::Unknown) {
      return X_STATUS_NOT_SUPPORTED;
    }
    // Initialize Android/JNI audio on the activity's native worker stack,
    // before a guest fiber first registers an audio client.
    if (!helper::sdl::SDLHelper::Prepare() ||
        !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      XELOGE("Android SDL audio initialization failed: {}", SDL_GetError());
      return X_STATUS_UNSUCCESSFUL;
    }
    audio_initialized_ = true;
    auto result = emulator_->SetupSubsystems();
    if (XFAILED(result)) {
      return result;
    }
    app_context().CallInUIThreadSynchronous([this]() {
      auto* graphics = emulator_->graphics_system();
      auto* presenter = graphics->presenter();
      window_->SetPresenter(presenter);
      ui::ApplyDisplayConfigForCvars(emulator_.get());
      // Same guest dialogs and gamepad navigation as the desktop frontend.
      immediate_drawer_ = graphics->provider()->CreateImmediateDrawer();
      if (immediate_drawer_) {
        immediate_drawer_->SetPresenter(presenter);
        imgui_drawer_->SetPresenterAndImmediateDrawer(presenter,
                                                      immediate_drawer_.get());
        Profiler::SetUserIO(11, window_.get(), presenter,
                            immediate_drawer_.get());
      }
    });
    return X_STATUS_SUCCESS;
  }

  X_STATUS Launch(const std::filesystem::path& path) {
    auto xam =
        emulator_->kernel_state()->GetKernelModule<kernel::xam::XamModule>(
            "xam.xex");
    auto& loader = xam->loader_data();
    loader.host_path = xe::path_to_utf8(path);
    loader.launch_flags = cvars::launch_flags;
    loader.launch_data_present =
        cvars::launch_flags != 0 || !cvars::launch_data.empty();
    loader.launch_data.clear();
    if (cvars::launch_data.size() % 2 ||
        cvars::launch_data.find_first_not_of("0123456789abcdefABCDEF") !=
            std::string::npos) {
      return X_STATUS_INVALID_PARAMETER;
    }
    for (size_t i = 0; i < cvars::launch_data.size(); i += 2) {
      loader.launch_data.push_back(
          uint8_t(std::stoul(cvars::launch_data.substr(i, 2), nullptr, 16)));
    }
    auto result = emulator_->LaunchPath(path);
    if (XSUCCEEDED(result)) {
      auto xam =
          emulator_->kernel_state()->GetKernelModule<kernel::xam::XamModule>(
              "xam.xex");
      xam->loader_data().host_path = xe::path_to_utf8(path);
      XELOGI("Android title launched: {}", emulator_->title_name());
    } else {
      XELOGE("Android title launch failed: {:08X}", result);
    }
    return result;
  }
  void Pause(bool paused) {
    static_cast<AndroidAudioSystem*>(emulator_->audio_system())
        ->SetOutputPaused(paused);
    if (!kernel::GuestScheduler::enabled()) {
      if (paused) {
        emulator_->Pause();
      } else {
        emulator_->Resume();
      }
      return;
    }
    if (paused == guest_paused_) {
      return;
    }
    guest_paused_ = paused;
    if (paused) {
      // Cooperative guests have fibers, not host thread handles. Leave the GPU
      // and audio workers available to drain outstanding guest requests.
      auto threads = emulator_->kernel_state()
                         ->object_table()
                         ->GetObjectsByType<kernel::XThread>(
                             kernel::XObject::Type::Thread);
      for (auto& thread : threads) {
        if (thread->fiber() && thread->is_running() &&
            XSUCCEEDED(thread->Suspend(nullptr))) {
          paused_threads_.push_back(thread);
        }
      }
    } else {
      for (auto& thread : paused_threads_) {
        thread->Resume(nullptr);
      }
      paused_threads_.clear();
    }
    XELOGI("Android guest {}", paused ? "paused" : "resumed");
  }
  AndroidInputState& input(uint32_t slot = 0) { return input_.at(slot); }
  void ConfigureController(uint32_t slot, bool keyboard, uint8_t subtype) {
    if (slot >= 4) {
      return;
    }
    auto* input = emulator_->input_system();
    auto lock = input->lock();
    for (const auto& device : input->EnumerateDevices()) {
      bool is_keyboard = dynamic_cast<hid::keyboard::KeyboardInputDriver*>(
                             device.driver) != nullptr;
      if (keyboard != is_keyboard ||
          (!keyboard && device.info.driver_slot != slot)) {
        continue;
      }
      const auto& bound = input->GetSlotBinding(slot);
      if (bound.driver != device.driver ||
          bound.driver_slot != device.info.driver_slot) {
        input->BindSlot(slot, device.driver, device.info.driver_slot,
                        device.info.stable_id, device.info.display_name);
      }
      input->SetSlotSubtypeOverride(slot, subtype);
      return;
    }
    input->UnbindSlot(slot);
  }
  Emulator* emulator() { return emulator_.get(); }
  void ShowTool(int tool) {
    auto* drawer = imgui_drawer_.get();
    auto* input = emulator_->input_system();
    if (tool >= 3 && tool < 5 && drawer->HasOpenDialogs()) {
      return;
    }
    switch (tool) {
      case 0:
        ToggleDialog(postprocessing_dialog_);
        break;
      case 1:
        ToggleDialog(performance_dialog_);
        break;
      case 2:
        ToggleDialog(debug_dialog_);
        break;
      case 3:
        new ui::ImGuiAudioDialog(drawer, input);
        break;
      case 4:
        new ProfileConfigDialog(drawer, emulator_.get(), input);
        break;
      case 5:
        emulator_->graphics_system()->ClearCaches();
        break;
      case 6:
        Profiler::ToggleDisplay();
        break;
      case 7:
        drawer->GetIO().AddKeyEvent(ImGuiKey_Escape, true);
        drawer->GetIO().AddKeyEvent(ImGuiKey_Escape, false);
        break;
      case 8:
      case 9:
      case 10:
        Clock::set_guest_time_scalar(
            tool == 10 ? 1.0
                       : std::clamp(Clock::guest_time_scalar() *
                                        (tool == 8 ? 0.5 : 2.0),
                                    0.03125, 32.0));
        break;
      case 11:
        input->ToggleVibration();
        break;
      case 12:
        emulator_->graphics_system()->RequestFrameTrace();
        break;
    }
    if (tool >= 8) {
      std::string value;
      if (tool <= 10) {
        value = fmt::format("{:.3g}x", Clock::guest_time_scalar());
      } else if (tool == 11) {
        value = input->GetVibrationCvar() ? "1" : "0";
      }
      SessionEvent(4, {std::to_string(tool), value});
    }
  }
  int UIState() {
    auto& io = imgui_drawer_->GetIO();
    std::lock_guard<std::mutex> lock(disc_mutex_);
    return (imgui_drawer_->HasOpenDialogs() ? 1 : 0) |
           (io.WantTextInput ? 2 : 0) | (disc_request_ ? 4 : 0) |
           (XE_OPTION_PROFILING_UI ? 8 : 0) |
           (emulator_->patcher() && emulator_->patcher()->IsAnyPatchApplied()
                ? 16
                : 0) |
           (emulator_->plugin_loader() &&
                    emulator_->plugin_loader()->IsAnyPluginLoaded()
                ? 32
                : 0);
  }
  void DiscSelected(const std::filesystem::path& path) {
    std::shared_ptr<DiscRequest> request;
    {
      std::lock_guard<std::mutex> lock(disc_mutex_);
      request = std::move(disc_request_);
    }
    if (request) {
      request->path = path;
      request->ready.Signal();
    }
  }
  void AddDisc(const std::filesystem::path& path) {
    for (const auto& entry : game_library_->entries()) {
      for (const auto& disc : entry.paths) {
        if (disc.path == path) {
          return;
        }
      }
    }
    auto info = ReadGameExecutionInfo(path);
    if (!info.empty() && info[0]) {
      game_library_->AddDisc(info[0], "", path);
    }
  }
  void SyncDiscs(const std::vector<std::filesystem::path>& paths) {
    // The Java library owns registration. Keep removed discs out of the native
    // picker, without touching any source media or unrelated content.
    auto entries = game_library_->entries();
    for (auto entry : entries) {
      auto count = entry.paths.size();
      std::erase_if(entry.paths, [&](const auto& disc) {
        return std::find(paths.begin(), paths.end(), disc.path) == paths.end();
      });
      if (entry.paths.size() != count) {
        game_library_->Upsert(std::move(entry));
      }
    }
    for (const auto& path : paths) {
      AddDisc(path);
    }
  }
  void TextInput(std::u16string_view text, int key) {
    auto& io = imgui_drawer_->GetIO();
    if (!io.WantTextInput) {
      return;
    }
    if (key) {
      auto code = key == 1 ? ImGuiKey_Backspace : ImGuiKey_Enter;
      io.AddKeyEvent(code, true);
      io.AddKeyEvent(code, false);
    } else {
      io.AddInputCharactersUTF8(xe::to_utf8(text).c_str());
    }
  }

  kernel::xam::ProfileManager* profiles() {
    return emulator_->kernel_state()->xam_state()->profile_manager();
  }

  bool NeedsProfileMigration() {
    // Match the desktop's first-profile prompt and legacy save migration.
    return !profiles()->GetAccountCount() &&
           !xe::filesystem::ListDirectories(emulator_->content_root()).empty();
  }

  X_STATUS CreateProfile(const std::string& gamertag) {
    if (!kernel::xam::ProfileManager::IsGamertagValid(gamertag)) {
      return X_STATUS_INVALID_PARAMETER;
    }
    try {
      const bool migrate = NeedsProfileMigration();
      if (!profiles()->CreateProfile(gamertag, true, migrate)) {
        return X_STATUS_UNSUCCESSFUL;
      }
      if (migrate) {
        emulator_->DataMigration(0xB13EBABEBABEBABE);
      }
      return profiles()->GetProfile(uint8_t(0)) ? X_STATUS_SUCCESS
                                                : X_STATUS_UNSUCCESSFUL;
    } catch (const std::exception& e) {
      XELOGE("Android profile creation failed: {}", e.what());
      return X_STATUS_UNSUCCESSFUL;
    }
  }

 protected:
  void OnDestroy() override {
    if (window_) {
      Profiler::SetUserIO(11, window_.get(), nullptr, nullptr);
    }
    if (imgui_drawer_) {
      imgui_drawer_->SetPresenterAndImmediateDrawer(nullptr, nullptr);
    }
    immediate_drawer_.reset();
    if (window_) {
      window_->SetPresenter(nullptr);
    }
    emulator_.reset();
    if (audio_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    imgui_drawer_.reset();
    window_.reset();
    Profiler::Shutdown();
  }

 private:
  template <typename T>
  void ToggleDialog(T*& dialog) {
    if (dialog) {
      dialog->CloseDialog();
    } else if (!imgui_drawer_->HasOpenDialogs()) {
      dialog = new T(imgui_drawer_.get(), emulator_.get(),
                     emulator_->input_system());
      dialog->SetOnCloseCallback([slot = &dialog]() { *slot = nullptr; });
    }
  }

  explicit AndroidEmulatorApp(ui::WindowedAppContext& context)
      : WindowedApp(context, "xenia") {}

  std::unique_ptr<ui::Window> window_;
  std::unique_ptr<ui::ImGuiDrawer> imgui_drawer_;
  ui::ImGuiPostProcessingDialog* postprocessing_dialog_ = nullptr;
  ui::ImGuiPerformanceDialog* performance_dialog_ = nullptr;
  ui::ImGuiDebugDialog* debug_dialog_ = nullptr;
  std::unique_ptr<ui::ImmediateDrawer> immediate_drawer_;
  std::array<AndroidInputState, 4> input_;
  std::unique_ptr<Emulator> emulator_;
  std::unique_ptr<GameLibrary> game_library_;
  struct DiscRequest {
    threading::Fence ready;
    std::filesystem::path path;
  };
  std::mutex disc_mutex_;
  std::shared_ptr<DiscRequest> disc_request_;
  bool guest_paused_ = false;
  bool audio_initialized_ = false;
  std::vector<kernel::object_ref<kernel::XThread>> paused_threads_;
};

}  // namespace xe::app

XE_DEFINE_WINDOWED_APP(xenia, xe::app::AndroidEmulatorApp::Create);

namespace {
xe::app::AndroidEmulatorApp* AndroidApp(jlong context) {
  return static_cast<xe::app::AndroidEmulatorApp*>(
      reinterpret_cast<xe::ui::AndroidWindowedAppContext*>(context)->app());
}
std::filesystem::path JavaPath(JNIEnv* env, jstring path) {
  const jchar* utf = env->GetStringChars(path, nullptr);
  if (!utf) {
    return {};
  }
  auto result = xe::to_path(std::u16string_view(
      reinterpret_cast<const char16_t*>(utf), env->GetStringLength(path)));
  env->ReleaseStringChars(path, utf);
  return result;
}
}  // namespace

namespace xe::app {
Emulator* AndroidEmulator(jlong context) {
  return AndroidApp(context)->emulator();
}
}  // namespace xe::app

extern "C" {
// All profile operations run on the activity's serial worker before launch.
// Null means player 1 is already signed in; otherwise return XUID/name pairs.
JNIEXPORT jobjectArray JNICALL
Java_jp_xenia_emulator_AndroidProfiles_listNative(JNIEnv* env, jclass,
                                                  jlong context) {
  auto* profiles = AndroidApp(context)->profiles();
  if (profiles->GetProfile(uint8_t(0))) {
    return nullptr;
  }
  const auto& accounts = *profiles->GetAccounts();
  auto string_class = env->FindClass("java/lang/String");
  auto result =
      env->NewObjectArray(jsize(accounts.size() * 2), string_class, nullptr);
  env->DeleteLocalRef(string_class);
  if (!result) {
    return nullptr;
  }
  jsize i = 0;
  for (const auto& [xuid, account] : accounts) {
    auto id = env->NewStringUTF(fmt::format("{:016X}", xuid).c_str());
    const auto name = xe::to_utf16(account.GetGamertagString());
    auto tag = env->NewString(reinterpret_cast<const jchar*>(name.data()),
                              jsize(name.size()));
    env->SetObjectArrayElement(result, i++, id);
    env->SetObjectArrayElement(result, i++, tag);
    env->DeleteLocalRef(id);
    env->DeleteLocalRef(tag);
  }
  return result;
}

JNIEXPORT jboolean JNICALL
Java_jp_xenia_emulator_AndroidProfiles_migrationNative(JNIEnv*, jclass,
                                                       jlong context) {
  return AndroidApp(context)->NeedsProfileMigration();
}

JNIEXPORT jint JNICALL Java_jp_xenia_emulator_AndroidProfiles_createNative(
    JNIEnv* env, jclass, jlong context, jstring gamertag) {
  using xe::X_STATUS;
  if (!gamertag) {
    return X_STATUS_INVALID_PARAMETER;
  }
  const char* chars = env->GetStringUTFChars(gamertag, nullptr);
  if (!chars) {
    return X_STATUS_UNSUCCESSFUL;
  }
  std::string name(chars);
  env->ReleaseStringUTFChars(gamertag, chars);
  return AndroidApp(context)->CreateProfile(name);
}

JNIEXPORT jboolean JNICALL Java_jp_xenia_emulator_AndroidProfiles_loginNative(
    JNIEnv*, jclass, jlong context, jlong xuid) {
  auto* profiles = AndroidApp(context)->profiles();
  profiles->Login(uint64_t(xuid), 0);
  auto* profile = profiles->GetProfile(uint8_t(0));
  return profile && profile->xuid() == uint64_t(xuid);
}

JNIEXPORT jintArray JNICALL Java_jp_xenia_emulator_GameIcons_metadataNative(
    JNIEnv* env, jclass, jstring path) {
  if (!path) {
    return nullptr;
  }
  auto info = xe::app::ReadGameExecutionInfo(JavaPath(env, path));
  if (info.empty()) {
    return nullptr;
  }
  auto result = env->NewIntArray(static_cast<jsize>(info.size()));
  if (result) {
    env->SetIntArrayRegion(result, 0, static_cast<jsize>(info.size()),
                           reinterpret_cast<const jint*>(info.data()));
  }
  return result;
}

JNIEXPORT jbyteArray JNICALL Java_jp_xenia_emulator_GameIcons_extractNative(
    JNIEnv* env, jclass, jstring path) {
  if (!path) {
    return nullptr;
  }
  auto icon = xe::app::ReadGameIcon(JavaPath(env, path));
  if (icon.empty()) {
    return nullptr;
  }
  auto result = env->NewByteArray(static_cast<jsize>(icon.size()));
  if (result) {
    env->SetByteArrayRegion(result, 0, static_cast<jsize>(icon.size()),
                            reinterpret_cast<const jbyte*>(icon.data()));
  }
  return result;
}

JNIEXPORT jint JNICALL Java_jp_xenia_emulator_EmulatorActivity_prepareNative(
    JNIEnv* env, jobject, jlong context, jstring path) {
  return AndroidApp(context)->Prepare(JavaPath(env, path));
}
JNIEXPORT jint JNICALL Java_jp_xenia_emulator_GameIcons_compatibilityNative(
    JNIEnv*, jclass, jint title) {
  return jint(xe::app::GetCompatState(uint32_t(title)));
}
JNIEXPORT jint JNICALL Java_jp_xenia_emulator_EmulatorActivity_launchNative(
    JNIEnv* env, jobject, jlong context, jstring path) {
  return AndroidApp(context)->Launch(JavaPath(env, path));
}
JNIEXPORT void JNICALL Java_jp_xenia_emulator_EmulatorActivity_pauseNative(
    JNIEnv*, jobject, jlong context, jboolean paused) {
  AndroidApp(context)->Pause(paused);
}
JNIEXPORT void JNICALL Java_jp_xenia_emulator_EmulatorActivity_inputNative(
    JNIEnv*, jobject, jlong context, jint slot, jboolean connected,
    jint buttons, jint lx, jint ly, jint rx, jint ry, jint lt, jint rt) {
  if (slot < 0 || slot >= 4) {
    return;
  }
  auto& input = AndroidApp(context)->input(slot);
  std::lock_guard<std::mutex> lock(input.mutex);
  input.connected = connected;
  if (!connected) {
    input.vibration = 0;
    input.packet = {};
    input.keystroke = {};
    input.pending_keys.clear();
    input.keyfield = 0;
    return;
  }
  input.packet.packet_number = uint32_t(input.packet.packet_number) + 1;
  auto& pad = input.packet.gamepad;
  pad.buttons = uint16_t(buttons);
  pad.thumb_lx = int16_t(lx);
  pad.thumb_ly = int16_t(ly);
  pad.thumb_rx = int16_t(rx);
  pad.thumb_ry = int16_t(ry);
  pad.left_trigger = uint8_t(lt);
  pad.right_trigger = uint8_t(rt);
  input.UpdateKeystrokes();
}
JNIEXPORT jintArray JNICALL
Java_jp_xenia_emulator_EmulatorActivity_vibrationNative(JNIEnv* env, jobject,
                                                        jlong context) {
  jint values[4];
  for (uint32_t i = 0; i < 4; ++i) {
    values[i] = AndroidApp(context)->input(i).vibration.load();
  }
  auto result = env->NewIntArray(4);
  if (result) {
    env->SetIntArrayRegion(result, 0, 4, values);
  }
  return result;
}
JNIEXPORT void JNICALL Java_jp_xenia_emulator_EmulatorActivity_toolNative(
    JNIEnv*, jobject, jlong context, jint tool) {
  AndroidApp(context)->ShowTool(tool);
}
JNIEXPORT jboolean JNICALL Java_jp_xenia_emulator_UsbPortal_attachNative(
    JNIEnv*, jclass, jlong context, jint fd) {
  auto* portal = dynamic_cast<xe::hid::HardwarePortal*>(
      AndroidApp(context)->emulator()->input_system()->GetPortal());
  return portal && portal->SetAndroidDevice(fd);
}
JNIEXPORT jint JNICALL
Java_jp_xenia_emulator_EmulatorActivity_inputOptionsNative(JNIEnv*, jobject,
                                                           jlong) {
  return cvars::guide_button ? 1 : 0;
}
JNIEXPORT void JNICALL Java_jp_xenia_emulator_EmulatorActivity_controllerNative(
    JNIEnv*, jobject, jlong context, jint slot, jboolean keyboard,
    jint subtype) {
  AndroidApp(context)->ConfigureController(uint32_t(slot), keyboard,
                                           uint8_t(subtype));
}
JNIEXPORT void JNICALL Java_jp_xenia_emulator_EmulatorActivity_keyNative(
    JNIEnv*, jobject, jlong context, jint key, jint unicode, jboolean down,
    jint repeat, jint mods) {
  auto* window = reinterpret_cast<xe::ui::AndroidWindowedAppContext*>(context)
                     ->GetActivityWindow();
  if (window) {
    window->DispatchKey(key, unicode, down, repeat, mods);
  }
}
JNIEXPORT void JNICALL Java_jp_xenia_emulator_EmulatorActivity_focusNative(
    JNIEnv*, jobject, jlong context, jboolean focused) {
  auto* window = reinterpret_cast<xe::ui::AndroidWindowedAppContext*>(context)
                     ->GetActivityWindow();
  if (window) {
    window->UpdateFocus(focused);
  }
}
JNIEXPORT jint JNICALL Java_jp_xenia_emulator_EmulatorActivity_uiStateNative(
    JNIEnv*, jobject, jlong context) {
  return AndroidApp(context)->UIState();
}
JNIEXPORT void JNICALL Java_jp_xenia_emulator_EmulatorActivity_textNative(
    JNIEnv* env, jobject, jlong context, jstring text, jint key) {
  const auto* chars = text ? env->GetStringChars(text, nullptr) : nullptr;
  AndroidApp(context)->TextInput(
      chars ? std::u16string_view(reinterpret_cast<const char16_t*>(chars),
                                  env->GetStringLength(text))
            : std::u16string_view(),
      key);
  if (chars) {
    env->ReleaseStringChars(text, chars);
  }
}
JNIEXPORT jintArray JNICALL
Java_jp_xenia_emulator_EmulatorActivity_screenshotNative(JNIEnv* env, jobject,
                                                         jlong context) {
  auto* graphics = AndroidApp(context)->emulator()->graphics_system();
  auto* presenter = graphics ? graphics->presenter() : nullptr;
  xe::ui::RawImage image;
  if (!presenter || !presenter->CaptureGuestOutput(image) || !image.width ||
      !image.height ||
      uint64_t(image.width) * image.height > 16 * 1024 * 1024 ||
      image.stride < uint64_t(image.width) * 4 ||
      uint64_t(image.height - 1) * image.stride + uint64_t(image.width) * 4 >
          image.data.size()) {
    return nullptr;
  }
  std::vector<jint> pixels(2 + size_t(image.width) * image.height);
  pixels[0] = jint(image.width);
  pixels[1] = jint(image.height);
  for (uint32_t y = 0; y < image.height; ++y) {
    for (uint32_t x = 0; x < image.width; ++x) {
      auto* p = image.data.data() + y * image.stride + x * 4;
      pixels[2 + size_t(y) * image.width + x] = jint(
          0xFF000000u | (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]);
    }
  }
  auto result = env->NewIntArray(jsize(pixels.size()));
  if (result) {
    env->SetIntArrayRegion(result, 0, jsize(pixels.size()), pixels.data());
  }
  return result;
}
JNIEXPORT void JNICALL Java_jp_xenia_emulator_EmulatorActivity_discsNative(
    JNIEnv* env, jobject, jlong context, jobjectArray paths) {
  std::vector<std::filesystem::path> selected;
  for (jsize i = 0; paths && i < env->GetArrayLength(paths); ++i) {
    auto path = static_cast<jstring>(env->GetObjectArrayElement(paths, i));
    try {
      selected.push_back(JavaPath(env, path));
    } catch (const std::exception& e) {
      XELOGW("Disc metadata: {}", e.what());
    }
    env->DeleteLocalRef(path);
  }
  try {
    AndroidApp(context)->SyncDiscs(selected);
  } catch (const std::exception& e) {
    XELOGW("Disc catalog: {}", e.what());
  }
}
JNIEXPORT void JNICALL
Java_jp_xenia_emulator_EmulatorActivity_discSelectedNative(JNIEnv* env, jobject,
                                                           jlong context,
                                                           jstring path) {
  AndroidApp(context)->DiscSelected(path ? JavaPath(env, path)
                                         : std::filesystem::path());
}
}
