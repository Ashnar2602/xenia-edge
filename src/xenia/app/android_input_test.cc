// Device smoke tests, linked only in the debug Android app.
#include "xenia/app/android_input.h"
#include <jni.h>
#include <array>
#include <cmath>
#include <string>
#include "xenia/app/android_features.h"
#include "xenia/apu/sdl/sdl_audio_driver.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/emulator.h"
#include "xenia/hid/input_system.h"
#include "xenia/hid/keyboard/keyboard_input_driver.h"
#include "xenia/kernel/xam/content_manager.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/ui/window_android.h"
#include "xenia/ui/windowed_app_context_android.h"

DECLARE_int32(draw_resolution_scale_x);
DECLARE_uint32(framerate_limit);
DECLARE_bool(keyboard_passthrough);

extern "C" JNIEXPORT jbyteArray JNICALL
Java_jp_xenia_emulator_FeatureChecks_historyFixtureNative(JNIEnv* env, jclass,
                                                          jint title,
                                                          jlong seconds) {
  xe::kernel::xam::GpdInfoProfile gpd;
  gpd.AddNewTitle(uint32_t(title), u"History fixture");
  auto* info = gpd.GetTitleInfo(uint32_t(title));
  info->last_played = xe::kernel::X_FILETIME(time_t(seconds));
  auto bytes = gpd.Serialize();
  auto result = env->NewByteArray(jsize(bytes.size()));
  if (result) {
    env->SetByteArrayRegion(result, 0, jsize(bytes.size()),
                            reinterpret_cast<const jbyte*>(bytes.data()));
  }
  return result;
}

extern "C" JNIEXPORT jstring JNICALL
Java_jp_xenia_emulator_FeatureChecks_keyboardNative(JNIEnv* env, jclass,
                                                    jlong context) {
  using namespace xe;
  auto* app = reinterpret_cast<xe::ui::AndroidWindowedAppContext*>(context);
  std::string result = "OK";
  app->CallInUIThreadSynchronous([&]() {
    auto* input = xe::app::AndroidEmulator(context)->input_system();
    auto* window = app->GetActivityWindow();
    auto lock = input->lock();
    auto original = input->GetSlotBinding(0);
    xe::hid::InputDriver* keyboard = nullptr;
    for (const auto& device : input->EnumerateDevices()) {
      if (device.info.stable_id == "keyboard") {
        keyboard = device.driver;
        input->BindSlot(0, device.driver, 0, "keyboard", "Keyboard");
      }
    }
    if (!keyboard || !window) {
      result = "Keyboard driver missing";
      return;
    }
    window->UpdateFocus(true);
    input->SetSlotSubtypeOverride(0, 2);
    xe::hid::X_INPUT_CAPABILITIES caps{};
    if (input->GetCapabilities(0, xe::hid::Controller, &caps) !=
            X_ERROR_SUCCESS ||
        caps.sub_type != 2) {
      result = "Controller subtype override";
    }
    window->DispatchKey('W', 'w', true, 0, 0);
    xe::hid::X_INPUT_STATE state{};
    if (input->GetState(0, xe::hid::Controller, &state) != X_ERROR_SUCCESS ||
        state.gamepad.thumb_ly <= 0) {
      result = "Keyboard gamepad mapping";
    }
    window->UpdateFocus(false);
    window->UpdateFocus(true);
    input->GetState(0, xe::hid::Controller, &state);
    if (state.gamepad.thumb_ly != 0) {
      result = "Keyboard stuck after focus loss";
    }
    xe::hid::X_INPUT_KEYSTROKE stroke{};
    // GetState does not consume keystrokes. Drain the preceding gamepad-mode
    // event before testing the independent raw-keyboard mode.
    while (keyboard->GetKeystroke(0, xe::hid::Controller, &stroke) !=
           X_ERROR_EMPTY) {
    }
    cvars::keyboard_passthrough = true;
    window->DispatchKey('A', 'a', true, 0, 0);
    if (keyboard->GetKeystroke(0, xe::hid::Keyboard, &stroke) !=
            X_ERROR_SUCCESS ||
        stroke.virtual_key != 'A' || stroke.unicode != 'a') {
      result = "Raw keyboard passthrough";
    }
    window->UpdateFocus(false);
    cvars::keyboard_passthrough = false;
    if (original.driver) {
      input->BindSlot(0, original.driver, original.driver_slot,
                      original.stable_id, original.display_name);
    } else {
      input->UnbindSlot(0);
    }
    input->SetSlotSubtypeOverride(0, original.subtype_override);
  });
  return env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_jp_xenia_emulator_ProfileTestActivity_settingsNative(JNIEnv* env, jclass) {
  return env->NewStringUTF(
      fmt::format("SETTINGS {} {} {}", xe::Clock::guest_time_scalar(),
                  cvars::draw_resolution_scale_x, cvars::framerate_limit)
          .c_str());
}

namespace {
using namespace xe;
using namespace xe::hid;
#define CHECK(condition) \
  if (!(condition)) return #condition
const char* CheckInput() {
  app::AndroidInputState state;
  app::AndroidInputDriver driver(nullptr, state);
  X_INPUT_KEYSTROKE key = {};
  // All digital buttons, including Start and the face buttons, reach XInput.
  constexpr uint16_t keys[] = {0x5810, 0x5811, 0x5812, 0x5813, 0x5814, 0x5815,
                               0x5816, 0x5817, 0x5805, 0x5804, 0x5838, 0,
                               0x5800, 0x5801, 0x5802, 0x5803};
  for (unsigned i = 0; i < 16; ++i) {
    if (!keys[i]) {
      continue;
    }
    // Publish DOWN and UP before the guest has polled: neither may be lost.
    state.packet.gamepad.buttons = uint16_t(1u << i);
    state.UpdateKeystrokes();
    state.packet.gamepad.buttons = 0;
    state.UpdateKeystrokes();
    CHECK(driver.GetKeystroke(XUserIndexAny, 0, &key) == X_ERROR_SUCCESS);
    CHECK(key.virtual_key == keys[i] && key.flags == X_INPUT_KEYSTROKE_KEYDOWN);
    CHECK(driver.GetKeystroke(0, 0, &key) == X_ERROR_SUCCESS);
    CHECK(key.virtual_key == keys[i] && key.flags == X_INPUT_KEYSTROKE_KEYUP);
    CHECK(driver.GetKeystroke(0, 0, &key) == X_ERROR_EMPTY);
  }
  GamepadKeystroke repeat;
  CHECK(repeat.Get(0x1000, 0, 0, &key) == X_ERROR_SUCCESS);
  CHECK(repeat.Get(0x1000, 400, 0, &key) == X_ERROR_EMPTY);
  CHECK(repeat.Get(0x1000, 401, 0, &key) == X_ERROR_SUCCESS);
  CHECK(key.flags == (X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT));
  // Release after a repeat deadline must win over auto-repeat.
  CHECK(repeat.Get(0, 900, 0, &key) == X_ERROR_SUCCESS);
  CHECK(key.flags == X_INPUT_KEYSTROKE_KEYUP);
  CHECK(repeat.Get(0, 1000, 0, &key) == X_ERROR_EMPTY);
  X_INPUT_GAMEPAD analog = {};
  analog.left_trigger = 31;
  CHECK(GamepadKeystroke::Keyfield(analog) == 0);
  analog.left_trigger = 32;
  analog.right_trigger = 255;
  CHECK(GamepadKeystroke::Keyfield(analog) == (3ull << 16));
  constexpr int16_t xy[][2] = {{0, 32767},      {0, -32768},     {32767, 0},
                               {-32768, 0},     {-32768, 32767}, {32767, 32767},
                               {32767, -32768}, {-32768, -32768}};
  for (unsigned stick = 0; stick < 2; ++stick) {
    for (unsigned i = 0; i < 8; ++i) {
      analog = {};
      if (stick) {
        analog.thumb_rx = xy[i][0];
        analog.thumb_ry = xy[i][1];
      } else {
        analog.thumb_lx = xy[i][0];
        analog.thumb_ly = xy[i][1];
      }
      CHECK(GamepadKeystroke::Keyfield(analog) ==
            (1ull << (18 + stick * 8 + i)));
    }
  }
  state.packet.packet_number = 123;
  state.packet.gamepad = analog;
  X_INPUT_STATE packet = {};
  CHECK(driver.GetState(0, &packet) == X_ERROR_SUCCESS);
  CHECK(packet.packet_number == 123 && packet.gamepad.thumb_rx == -32768);
  CHECK(driver.GetState(1, &packet) == X_ERROR_DEVICE_NOT_CONNECTED);
  // Games which never consume keystrokes cannot grow the queue unboundedly.
  for (int i = 0; i < 1000; ++i) {
    state.packet.gamepad = {};
    state.packet.gamepad.buttons = i % 2 ? 0x1000 : 0;
    state.UpdateKeystrokes();
    CHECK(state.pending_keys.size() <= 64);
  }
  state.packet.gamepad = {};
  state.UpdateKeystrokes();
  unsigned drained = 0;
  while (driver.GetKeystroke(0, 0, &key) == X_ERROR_SUCCESS) {
    CHECK(++drained <= 128);
  }
  CHECK(state.keystroke.buttons == 0);
  std::array<app::AndroidInputState, 4> players;
  app::AndroidInputDriver multi(nullptr, players);
  CHECK(multi.EnumerateDevices().size() == 4);
  for (uint8_t slot = 0; slot < 4; slot++) {
    players[slot].packet.gamepad.buttons = 0x1000;
    players[slot].UpdateKeystrokes();
    CHECK(multi.GetKeystroke(XUserIndexAny, 0, &key) == X_ERROR_SUCCESS);
    CHECK(key.user_index == slot && key.virtual_key == 0x5800);
    CHECK(multi.GetState(slot, &packet) == X_ERROR_SUCCESS &&
          packet.gamepad.buttons == 0x1000);
    X_INPUT_VIBRATION vibration = {uint16_t(100 + slot), uint16_t(200 + slot)};
    CHECK(multi.SetState(slot, &vibration) == X_ERROR_SUCCESS);
    CHECK(players[slot].vibration.load() ==
          uint32_t(100 + slot) + (uint32_t(200 + slot) << 16));
    players[slot].connected = false;
    CHECK(multi.GetState(slot, &packet) == X_ERROR_DEVICE_NOT_CONNECTED);
    CHECK(multi.SetState(slot, &vibration) == X_ERROR_DEVICE_NOT_CONNECTED);
  }
  CHECK(multi.EnumerateDevices().empty());
  return "";
}
#undef CHECK
}  // namespace
extern "C" JNIEXPORT jstring JNICALL
Java_jp_xenia_emulator_AndroidInputAudioTest_checkInputNative(JNIEnv* env,
                                                              jclass) {
  return env->NewStringUTF(CheckInput());
}
extern "C" JNIEXPORT jstring JNICALL
Java_jp_xenia_emulator_AndroidInputAudioTest_checkAudioNative(JNIEnv* env,
                                                              jclass) {
  auto semaphore = xe::threading::Semaphore::Create(0, 16);
  xe::apu::sdl::SDLAudioDriver driver(semaphore.get());
  if (!driver.Initialize()) {
    const std::string error = SDL_GetError();
    driver.Shutdown();
    return env->NewStringUTF(error.empty() ? "SDL audio initialization failed"
                                           : error.c_str());
  }
  std::string error;
  // Same planar big-endian 5.1 input as the guest. Quiet 440 Hz tone on L/R.
  std::array<float, 6 * 256> frame = {};
  for (int block = 0; block < 120; ++block) {
    for (int i = 0; i < 256; ++i) {
      const float sample =
          .04f * std::sin((block * 256 + i) * (440.f * 6.2831853f / 48000.f));
      frame[i] = frame[256 + i] = xe::byte_swap(sample);
    }
    if (block == 60) {
      driver.Pause();
      driver.SubmitFrame(frame.data());
      if (xe::threading::Wait(semaphore.get(), false,
                              std::chrono::milliseconds(50)) !=
          xe::threading::WaitResult::kTimeout) {
        error = "Paused audio still consumed frames";
      }
      driver.Resume();
    } else {
      driver.SubmitFrame(frame.data());
    }
    if (xe::threading::Wait(semaphore.get(), false,
                            std::chrono::milliseconds(2000)) !=
        xe::threading::WaitResult::kSuccess) {
      error = "SDL did not consume a guest audio frame";
      break;
    }
  }
  driver.Shutdown();
  return env->NewStringUTF(error.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_jp_xenia_emulator_FeatureChecks_logNative(JNIEnv*, jclass) {
  xe::EnableAndroidFileLogging("/proc/xenia-fixture");
  XELOGI("Android logging fixture");
  xe::FlushLog();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_jp_xenia_emulator_FeatureChecks_servicesNative(JNIEnv*, jclass,
                                                    jlong context) {
  auto* emulator = xe::app::AndroidEmulator(context);
  emulator->on_launch_new_title()("/fixture/game.zar", "next.xex", 123, "00FF");
  if (emulator->on_exit_to_dashboard()()) {
    return false;
  }
  emulator->on_launch(0x415407D7, "Fixture title");
  emulator->on_disc_swap()(2);
  emulator->on_shader_storage_initialization(true);
  emulator->on_shader_storage_initialization(false);
  xe::kernel::xam::XCONTENT_AGGREGATE_DATA data{};
  data.title_id = 0x415407D7;
  data.content_type = xe::XContentType::kSavedGame;
  data.set_file_name("fixture-save");
  data.set_display_name(u"Friendly save");
  auto* kernel = emulator->kernel_state();
  auto root = kernel->xam_state()->profile_manager()->GetProfileContentPath(
      0, data.title_id, data.content_type);
  std::filesystem::create_directories(root / "fixture-save");
  return kernel->content_manager()->WriteContentHeaderFile(0, data) ==
         X_ERROR_SUCCESS;
}
