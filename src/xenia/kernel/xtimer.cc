/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xtimer.h"

#include "xenia/base/logging.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

XTimer::XTimer(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XTimer::~XTimer() {
  if (timer_) {
    timer_->Cancel();
  }
  RemoveApc();
  memory()->SystemHeapFree(apc_ptr_);
}

void XTimer::Initialize(uint32_t timer_type) {
  assert_false(timer_);
  switch (timer_type) {
    case 0:  // NotificationTimer
      timer_ = xe::threading::Timer::CreateManualResetTimer();
      break;
    case 1:  // SynchronizationTimer
      timer_ = xe::threading::Timer::CreateSynchronizationTimer();
      break;
    default:
      assert_always();
      break;
  }
  assert_not_null(timer_);
}

X_STATUS XTimer::SetTimer(int64_t due_time, uint32_t period_ms,
                          uint32_t routine, uint32_t routine_arg, bool resume) {
  using xe::chrono::WinSystemClock;
  using xe::chrono::XSystemClock;

  std::lock_guard<std::mutex> lock(timer_lock_);

  period_ms = Clock::ScaleGuestDurationMillis(period_ms);
  WinSystemClock::time_point due_tp;
  if (due_time < 0) {
    // Any timer implementation uses absolute times eventually, convert as early
    // as possible for increased accuracy
    auto after = xe::chrono::hundrednanoseconds(-due_time);
    due_tp = date::clock_cast<WinSystemClock>(XSystemClock::now() + after);
  } else {
    due_tp = date::clock_cast<WinSystemClock>(
        XSystemClock::from_file_time(due_time));
  }

  // The previous expiry must not be able to queue the APC while it is reused.
  timer_->Cancel();
  RemoveApc();

  // This callback will only be issued when the timer is fired.
  // Capture values by value to avoid racing with a future SetTimer() call.
  std::function<void()> callback = nullptr;
  if (routine) {
    if (!apc_ptr_) {
      apc_ptr_ = memory()->SystemHeapAlloc(XAPC::kSize);
      if (!apc_ptr_) {
        return X_STATUS_NO_MEMORY;
      }
    }
    apc_thread_ = retain_object(XThread::GetCurrentThread());
    xboxkrnl::xeKeInitializeApc(memory()->TranslateVirtual<XAPC*>(apc_ptr_),
                                apc_thread_->guest_object(),
                                XAPC::kOwnedKernelRoutine, 0, routine, 1,
                                routine_arg);
    XThread* cb_thread = apc_thread_.get();
    uint32_t cb_apc = apc_ptr_;
    callback = [cb_thread, cb_apc, routine, routine_arg]() {
      // Queue APC to call back routine with (arg, low, high).
      // It'll be executed on the thread that requested the timer.
      uint64_t time = xe::Clock::QueryGuestSystemTime();
      uint32_t time_low = static_cast<uint32_t>(time);
      uint32_t time_high = static_cast<uint32_t>(time >> 32);
      XELOGD(
          "XTimer enqueuing timer callback to {:08X}({:08X}, {:08X}, {:08X})",
          routine, routine_arg, time_low, time_high);
      cb_thread->InsertOwnedApc(cb_apc, time_low, time_high);
    };
  }

  bool result;
  if (!period_ms) {
    result = timer_->SetOnceAt(due_tp, std::move(callback));
  } else {
    result = timer_->SetRepeatingAt(
        due_tp, std::chrono::milliseconds(period_ms), std::move(callback));
  }

  if (resume) {
    XThread::SetLastError(X_ERROR_NOT_SUPPORTED);
    return X_STATUS_TIMER_RESUME_IGNORED;
  }

  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

X_STATUS XTimer::Cancel() {
  std::lock_guard<std::mutex> lock(timer_lock_);
  bool result = timer_->Cancel();
  RemoveApc();
  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

void XTimer::RemoveApc() {
  if (apc_thread_) {
    apc_thread_->RemoveOwnedApc(apc_ptr_);
    apc_thread_.reset();
  }
}

}  // namespace kernel
}  // namespace xe
