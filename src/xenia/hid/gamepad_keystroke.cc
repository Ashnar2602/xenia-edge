/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/gamepad_keystroke.h"

#include <array>
#include <tuple>
#include "xenia/ui/virtual_key.h"

namespace xe::hid {
X_RESULT GamepadKeystroke::Get(uint64_t curr_butts, uint32_t guest_now,
                               uint8_t user_index,
                               X_INPUT_KEYSTROKE* out_keystroke) {
  constexpr uint32_t kRepeatDelay = 400, kRepeatRate = 100;
  // The order of this list is also the order in which events are send if
  // multiple buttons change at once.
  static_assert(sizeof(X_INPUT_GAMEPAD::buttons) == 2);
  static constexpr std::array<ui::VirtualKey, 35> kVkLookup = {
      // 00 - True buttons from xinput button field
      ui::VirtualKey::kXInputPadDpadUp,
      ui::VirtualKey::kXInputPadDpadDown,
      ui::VirtualKey::kXInputPadDpadLeft,
      ui::VirtualKey::kXInputPadDpadRight,
      ui::VirtualKey::kXInputPadStart,
      ui::VirtualKey::kXInputPadBack,
      ui::VirtualKey::kXInputPadLThumbPress,
      ui::VirtualKey::kXInputPadRThumbPress,
      ui::VirtualKey::kXInputPadLShoulder,
      ui::VirtualKey::kXInputPadRShoulder,
      // Guide has no VK (kNone), however using kXInputPadGuide.
      ui::VirtualKey::kXInputPadGuide,
      ui::VirtualKey::kNone, /* Unknown */
      ui::VirtualKey::kXInputPadA,
      ui::VirtualKey::kXInputPadB,
      ui::VirtualKey::kXInputPadX,
      ui::VirtualKey::kXInputPadY,
      // 16 - Fake buttons generated from analog inputs
      ui::VirtualKey::kXInputPadLTrigger,
      ui::VirtualKey::kXInputPadRTrigger,
      // 18
      ui::VirtualKey::kXInputPadLThumbUp,
      ui::VirtualKey::kXInputPadLThumbDown,
      ui::VirtualKey::kXInputPadLThumbRight,
      ui::VirtualKey::kXInputPadLThumbLeft,
      ui::VirtualKey::kXInputPadLThumbUpLeft,
      ui::VirtualKey::kXInputPadLThumbUpRight,
      ui::VirtualKey::kXInputPadLThumbDownRight,
      ui::VirtualKey::kXInputPadLThumbDownLeft,
      // 26
      ui::VirtualKey::kXInputPadRThumbUp,
      ui::VirtualKey::kXInputPadRThumbDown,
      ui::VirtualKey::kXInputPadRThumbRight,
      ui::VirtualKey::kXInputPadRThumbLeft,
      ui::VirtualKey::kXInputPadRThumbUpLeft,
      ui::VirtualKey::kXInputPadRThumbUpRight,
      ui::VirtualKey::kXInputPadRThumbDownRight,
      ui::VirtualKey::kXInputPadRThumbDownLeft,
  };

  // Handle repeating
  auto& last = *this;

  auto butts_changed = curr_butts ^ last.buttons;
  if (!butts_changed) {
    static_assert(kRepeatDelay >= kRepeatRate);
    if (last.repeat_state == RepeatState::Waiting &&
        (last.repeat_time + kRepeatDelay < guest_now)) {
      last.repeat_state = RepeatState::Repeating;
    }
    if (last.repeat_state == RepeatState::Repeating &&
        (last.repeat_time + kRepeatRate < guest_now)) {
      last.repeat_time = guest_now;
      ui::VirtualKey vk = kVkLookup.at(last.repeat_butt_idx);
      assert_true(vk != ui::VirtualKey::kNone);
      out_keystroke->virtual_key = uint16_t(vk);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = user_index;
      out_keystroke->hid_code = 0;
      out_keystroke->flags =
          X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT;
      return X_ERROR_SUCCESS;
    }

    return X_ERROR_EMPTY;
  }

  // First try to clear buttons with up events. This is to match xinput
  // behaviour when transitioning thumb sticks, e.g. so that THUMB_UPLEFT is
  // up before THUMB_LEFT is down.
  for (auto [clear_pass, i] = std::tuple{true, 0}; i < 2;
       clear_pass = false, i++) {
    for (uint8_t i = 0; i < uint8_t(std::size(kVkLookup)); i++) {
      auto fbutton = uint64_t(1) << i;
      if (!(butts_changed & fbutton)) {
        continue;
      }
      ui::VirtualKey vk = kVkLookup.at(i);
      if (vk == ui::VirtualKey::kNone) {
        continue;
      }

      out_keystroke->virtual_key = uint16_t(vk);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = user_index;
      out_keystroke->hid_code = 0;

      bool is_pressed = curr_butts & fbutton;
      if (clear_pass && !is_pressed) {
        // up
        out_keystroke->flags = X_INPUT_KEYSTROKE_KEYUP;
        last.buttons &= ~fbutton;
        last.repeat_state = RepeatState::Idle;
        return X_ERROR_SUCCESS;
      }
      if (!clear_pass && is_pressed) {
        // down
        out_keystroke->flags = X_INPUT_KEYSTROKE_KEYDOWN;
        last.buttons |= fbutton;
        last.repeat_state = RepeatState::Waiting;
        last.repeat_butt_idx = i;
        last.repeat_time = guest_now;
        return X_ERROR_SUCCESS;
      }
    }
  }
  return X_ERROR_EMPTY;
}

uint64_t GamepadKeystroke::Keyfield(const X_INPUT_GAMEPAD& gamepad) {
  uint64_t f = 0;

  f |= static_cast<uint64_t>(gamepad.left_trigger > 0x1F) << 16;
  f |= static_cast<uint64_t>(gamepad.right_trigger > 0x1F) << 17;

  auto thumb_x = gamepad.thumb_lx;
  auto thumb_y = gamepad.thumb_ly;
  for (size_t i = 0; i <= 8; i = i + 8) {
    uint64_t u = thumb_y > 0x4E00;
    uint64_t d = thumb_y < ~0x4E00;
    uint64_t r = thumb_x > 0x4E00;
    uint64_t l = thumb_x < ~0x4E00;
    if (u && l) {
      u = l = 0;
      f |= uint64_t(1) << (22 + i);
    }
    if (u && r) {
      u = r = 0;
      f |= uint64_t(1) << (23 + i);
    }
    if (d && r) {
      d = r = 0;
      f |= uint64_t(1) << (24 + i);
    }
    if (d && l) {
      d = l = 0;
      f |= uint64_t(1) << (25 + i);
    }
    f |= u << (18 + i);
    f |= d << (19 + i);
    f |= r << (20 + i);
    f |= l << (21 + i);

    thumb_x = gamepad.thumb_rx;
    thumb_y = gamepad.thumb_ry;
  }
  return f | (uint16_t(gamepad.buttons) & 0xF7FF);
}

}  // namespace xe::hid
