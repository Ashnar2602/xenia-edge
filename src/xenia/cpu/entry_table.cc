/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/entry_table.h"

#include <algorithm>

#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"

namespace xe {
namespace cpu {

EntryTable::EntryTable() = default;

EntryTable::~EntryTable() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& [address, entry] : map_) {
    delete entry;
  }
}

Entry* EntryTable::Get(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = map_.find(address);
  if (it == map_.end()) {
    return nullptr;
  }
  Entry* entry = it->second;
  if (entry) {
    // TODO(benvanik): wait if needed?
    if (entry->status != Entry::STATUS_READY) {
      entry = nullptr;
    }
  }
  return entry;
}

Entry::Status EntryTable::GetOrCreate(uint32_t address, Entry** out_entry) {
  // TODO(benvanik): replace with a map with wait-free for find.
  // https://github.com/facebook/folly/blob/master/folly/AtomicHashMap.h

  auto global_lock = global_critical_region_.Acquire();

  auto it = map_.find(address);
  Entry* entry = it != map_.end() ? it->second : nullptr;
  Entry::Status status;
  if (entry) {
    // If we aren't ready yet spin and wait.
    if (entry->status == Entry::STATUS_COMPILING) {
      // Compiles run outside the lock, so another caller can land here.
      do {
        global_lock.unlock();
        // TODO(benvanik): sleep for less time?
        xe::threading::Sleep(std::chrono::microseconds(10));
        global_lock.lock();
      } while (entry->status == Entry::STATUS_COMPILING);
    }
    status = entry->status;
  } else {
    // Create and return for initialization.
    entry = new Entry();
    entry->address = address;
    entry->end_address = 0;
    entry->status = Entry::STATUS_COMPILING;
    entry->function = 0;
    map_.emplace(address, entry);
    status = Entry::STATUS_NEW;
  }
  global_lock.unlock();
  *out_entry = entry;
  return status;
}

void EntryTable::MarkReady(Entry* entry, Function* function,
                           uint32_t end_address) {
  auto global_lock = global_critical_region_.Acquire();
  entry->function = function;
  entry->end_address = end_address;
  entry->status = Entry::STATUS_READY;
  // A module unload can remove the entry while it compiles.
  auto it = map_.find(entry->address);
  if (it == map_.end() || it->second != entry) {
    return;
  }
  ready_by_address_[entry->address] = entry;
  if (end_address > entry->address) {
    max_ready_span_ = std::max(max_ready_span_, end_address - entry->address);
  }
}

void EntryTable::MarkFailed(Entry* entry) {
  auto global_lock = global_critical_region_.Acquire();
  entry->status = Entry::STATUS_FAILED;
}

void EntryTable::Delete(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  // doesnt this leak memory by not deleting the entry?
  map_.erase(address);
  ready_by_address_.erase(address);
}

std::vector<Function*> EntryTable::FindWithAddress(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  std::vector<Function*> fns;
  // No entry starting further below can reach the address.
  const uint32_t lowest_start =
      address > max_ready_span_ ? address - max_ready_span_ : 0;
  const auto end = ready_by_address_.upper_bound(address);
  for (auto it = ready_by_address_.lower_bound(lowest_start); it != end; ++it) {
    if (address <= it->second->end_address) {
      fns.push_back(it->second->function);
    }
  }
  return fns;
}
}  // namespace cpu
}  // namespace xe
