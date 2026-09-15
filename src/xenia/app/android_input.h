#ifndef XENIA_APP_ANDROID_INPUT_H_
#define XENIA_APP_ANDROID_INPUT_H_

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include "xenia/base/clock.h"
#include "xenia/hid/gamepad_keystroke.h"
#include "xenia/hid/input_driver.h"

namespace xe::app {

// Java combines touch and physical-controller input before publishing a packet.
struct AndroidInputState {
  std::mutex mutex;
  hid::X_INPUT_STATE packet = {};
  hid::GamepadKeystroke keystroke;
  std::deque<uint64_t> pending_keys;
  uint64_t keyfield = 0;
  std::atomic<bool> connected{true};
  std::atomic<uint32_t> vibration{0};

  // Called under mutex after publishing the packet. Retain short taps between
  // guest polls, but bound the backlog if the title never reads keystrokes.
  void UpdateKeystrokes() {
    const auto keys = hid::GamepadKeystroke::Keyfield(packet.gamepad);
    if (keys == keyfield) {
      return;
    }
    keyfield = keys;
    if (pending_keys.size() == 64) {
      pending_keys.clear();
    }
    pending_keys.push_back(keys);
  }
};

class AndroidInputDriver final : public hid::InputDriver {
 public:
  AndroidInputDriver(ui::Window* window, AndroidInputState& state)
      : InputDriver(window, 0), states_{&state, nullptr, nullptr, nullptr} {}
  AndroidInputDriver(ui::Window* window,
                     std::array<AndroidInputState, 4>& states)
      : InputDriver(window, 0),
        states_{&states[0], &states[1], &states[2], &states[3]} {}
  X_STATUS Setup() override { return X_STATUS_SUCCESS; }
  hid::InputType GetInputType() const override { return hid::Controller; }
  std::vector<hid::InputDeviceInfo> EnumerateDevices() override {
    std::vector<hid::InputDeviceInfo> result;
    for (uint32_t i = 0; i < 4; ++i) {
      if (Connected(i)) {
        result.push_back({uint8_t(i), "android-player-" + std::to_string(i),
                          "Android controller", 1, 0});
      }
    }
    return result;
  }
  X_RESULT GetCapabilities(uint32_t index, uint32_t,
                           hid::X_INPUT_CAPABILITIES* caps) override {
    if (!Connected(index)) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    *caps = {};
    caps->type = hid::XINPUT_DEVTYPE_GAMEPAD;
    caps->sub_type = hid::XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->gamepad.buttons = 0xF3FF;
    caps->gamepad.left_trigger = caps->gamepad.right_trigger = 255;
    caps->gamepad.thumb_lx = caps->gamepad.thumb_ly = 32767;
    caps->gamepad.thumb_rx = caps->gamepad.thumb_ry = 32767;
    caps->vibration.left_motor_speed = caps->vibration.right_motor_speed =
        65535;
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetState(uint32_t index, hid::X_INPUT_STATE* out) override {
    if (!Connected(index)) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    std::lock_guard<std::mutex> lock(states_[index]->mutex);
    *out = states_[index]->packet;
    return X_ERROR_SUCCESS;
  }
  X_RESULT SetState(uint32_t index,
                    hid::X_INPUT_VIBRATION* vibration) override {
    if (!Connected(index)) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (!vibration) {
      return X_ERROR_BAD_ARGUMENTS;
    }
    states_[index]->vibration.store(
        uint32_t(vibration->left_motor_speed) |
        (uint32_t(vibration->right_motor_speed) << 16));
    return X_ERROR_SUCCESS;
  }
  X_RESULT GetKeystroke(uint32_t index, uint32_t,
                        hid::X_INPUT_KEYSTROKE* out) override {
    if (index == XUserIndexAny) {
      for (uint32_t slot = 0; slot < 4; ++slot) {
        if (!Connected(slot)) {
          continue;
        }
        auto result = GetKeystroke(slot, 0, out);
        if (result != X_ERROR_EMPTY) {
          return result;
        }
      }
      return X_ERROR_EMPTY;
    }
    if (!Connected(index)) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (!out) {
      return X_ERROR_BAD_ARGUMENTS;
    }
    auto& state_ = *states_[index];
    std::lock_guard<std::mutex> lock(state_.mutex);
    const uint32_t now = Clock::QueryGuestUptimeMillis();
    while (!state_.pending_keys.empty()) {
      const auto keys = state_.pending_keys.front();
      const auto result = state_.keystroke.Get(keys, now, index, out);
      if (state_.keystroke.buttons == keys) {
        state_.pending_keys.pop_front();
      }
      if (result != X_ERROR_EMPTY) {
        return result;
      }
    }
    return state_.keystroke.Get(state_.keyfield, now, index, out);
  }

 private:
  bool Connected(uint32_t index) const {
    return index < 4 && states_[index] && states_[index]->connected.load();
  }
  std::array<AndroidInputState*, 4> states_;
};
}  // namespace xe::app

#endif  // XENIA_APP_ANDROID_INPUT_H_
