/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_ENTRY_TABLE_H_
#define XENIA_CPU_ENTRY_TABLE_H_

#include <map>
#include <unordered_map>
#include <vector>

#include "xenia/base/mutex.h"
namespace xe {
namespace cpu {

class Function;

typedef struct Entry_t {
  typedef enum {
    STATUS_NEW = 0,
    STATUS_COMPILING,
    STATUS_READY,
    STATUS_FAILED,
  } Status;

  uint32_t address;
  uint32_t end_address;
  Status status;
  Function* function;
} Entry;

class EntryTable {
 public:
  EntryTable();
  ~EntryTable();

  Entry* Get(uint32_t address);
  Entry::Status GetOrCreate(uint32_t address, Entry** out_entry);
  // Publishes the result of compiling `entry` (obtained via GetOrCreate
  // returning STATUS_NEW) under the same lock GetOrCreate's spin-wait uses to
  // read entry->status. Callers must go through these instead of writing
  // entry->status/function/end_address directly -- unsynchronized writes here
  // raced against the lock-protected reads in GetOrCreate's spin-wait, so a
  // waiting thread on a weak memory model (e.g. Apple Silicon) could observe
  // STATUS_READY before entry->function was actually visible, returning a
  // stale/torn function pointer.
  void MarkReady(Entry* entry, Function* function, uint32_t end_address);
  void MarkFailed(Entry* entry);
  void Delete(uint32_t address);

  std::vector<Function*> FindWithAddress(uint32_t address);

 private:
  xe::global_critical_region global_critical_region_;
  std::unordered_map<uint32_t, Entry*> map_;
  // Ready entries by start address, for range scans.
  std::map<uint32_t, Entry*> ready_by_address_;
  uint32_t max_ready_span_ = 0;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_ENTRY_TABLE_H_
