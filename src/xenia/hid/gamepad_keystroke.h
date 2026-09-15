/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_GAMEPAD_KEYSTROKE_H_
#define XENIA_HID_GAMEPAD_KEYSTROKE_H_

#include "xenia/hid/input.h"
#include "xenia/xbox.h"

namespace xe::hid {
// XInput key synthesis shared by SDL desktop controllers and Android input.
// The caller serializes access and supplies guest time (milliseconds).
class GamepadKeystroke {
 public:
  static uint64_t Keyfield(const X_INPUT_GAMEPAD& gamepad);
  X_RESULT Get(uint64_t curr_butts, uint32_t guest_now, uint8_t user_index,
               X_INPUT_KEYSTROKE* out_keystroke);
  uint64_t buttons = 0;

 private:
  enum class RepeatState { Idle, Waiting, Repeating };
  RepeatState repeat_state = RepeatState::Idle;
  uint8_t repeat_butt_idx = 0;
  uint32_t repeat_time = 0;
};
}  // namespace xe::hid

#endif  // XENIA_HID_GAMEPAD_KEYSTROKE_H_
