/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/vfs/device.h"

#include <algorithm>

#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

DEFINE_bool(storage_request_timing, true,
            "Make reads from the launched title's storage take as long as a "
            "console drive would. Disable to read at host speed.",
            "Storage");

namespace xe {
namespace vfs {

namespace {
// Positioning and rotational latency, unscaled since scaling reopens the races.
constexpr double kSeekMs = 6.0;
// A 360 hard disk, which is what installed content is read from.
constexpr double kBytesPerMs = 35000.0;
// Caps the backlog so a mis-modeled title runs slow rather than compounding.
// Must stay above the cost of one large request, around a second.
constexpr double kMaxQueuedAheadMs = 4000.0;
}  // namespace

void DriveTiming::Configure() { enabled_ = true; }

uint64_t DriveTiming::Reserve(const Entry* entry, uint64_t offset,
                              size_t length) {
  if (!enabled_ || !cvars::storage_request_timing) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(lock_);
  const bool seek = entry != head_entry_ || offset != head_offset_;
  head_entry_ = entry;
  head_offset_ = offset + length;
  const double ms = (seek ? kSeekMs : 0.0) + double(length) / kBytesPerMs;

  const double now = double(Clock::QueryHostUptimeMillis());
  // Past the cap this moves the queue backwards, deliberately dropping what an
  // in-flight request still had reserved.
  const double start =
      std::min(std::max(now, free_at_ms_), now + kMaxQueuedAheadMs);
  free_at_ms_ = start + ms;
  // Truncated, so a request done within this millisecond does not wait.
  return uint64_t(free_at_ms_);
}

Device::Device(const std::string_view mount_path) : mount_path_(mount_path) {}
Device::~Device() = default;

}  // namespace vfs
}  // namespace xe
