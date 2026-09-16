/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xfile.h"
#include "xenia/vfs/virtual_file_system.h"

#include "xenia/base/byte_stream.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel_state.h"

DEFINE_int32(file_read_delay_ms, 0,
             "Diagnostic: sleep this many milliseconds before each file read, "
             "to approximate disc seek and read time.",
             "Kernel");

namespace xe {
namespace kernel {

namespace {
// Called inside the blocking host call, so only the thread waiting on the read
// stalls.
void DelayFileRead() {
  if (cvars::file_read_delay_ms > 0) {
    threading::Sleep(std::chrono::milliseconds(cvars::file_read_delay_ms));
  }
}

// What the I/O manager waits with for a title's synchronous request.
constexpr uint32_t kWaitReasonExecutive = 0;
constexpr uint32_t kUserMode = 1;
}  // namespace

XFile::XFile(KernelState* kernel_state, vfs::File* file, bool synchronous,
             bool alertable)
    : XObject(kernel_state, kObjectType),
      file_(file),
      is_synchronous_(synchronous),
      is_alertable_(alertable) {
  async_event_ = threading::Event::CreateAutoResetEvent(false);
  assert_not_null(async_event_);
}

XFile::XFile() : XObject(kObjectType), completion_port_lock_() {
  async_event_ = threading::Event::CreateAutoResetEvent(false);
  assert_not_null(async_event_);
}

XFile::~XFile() {
  // TODO(benvanik): signal that the file is closing?
  async_event_->Set();
  file_->Destroy();
  // A worker still signaling one holds a reference; a table reset may have
  // taken the handles already.
  for (auto& event : idle_io_events_) {
    if (!event->handles().empty()) {
      event->ReleaseHandle();
    }
  }
}

GuestScheduler::BlockingCallClass XFile::io_call_class() const {
  return device()->supports_concurrent_io()
             ? GuestScheduler::BlockingCallClass::kConcurrent
             : GuestScheduler::BlockingCallClass::kSerial;
}

void XFile::RunSynchronousIo(const std::function<void()>& fn) {
  auto* scheduler = kernel_state()->guest_scheduler();
  if (!GuestScheduler::CurrentThreadOffloadsBlockingCalls()) {
    fn();
    return;
  }
  auto event = AcquireIoEvent();
  // The worker writes into this frame, so only |done| may end the wait, and a
  // terminate must not end it either.
  std::atomic<bool> done{false};
  scheduler->PostHostCall(
      [&fn, &done, signal = retain_object(event.get())]() {
        fn();
        done.store(true, std::memory_order_release);
        signal->Set(kIoDiskIncrement, false);
      },
      io_call_class());
  uint32_t alertable = is_alertable_ ? 1 : 0;
  while (!done.load(std::memory_order_acquire)) {
    X_STATUS status =
        event->Wait(kWaitReasonExecutive, kUserMode, alertable, nullptr, false);
    if (status == X_STATUS_USER_APC) {
      // An alert cannot cancel the host request. The APCs run at the next
      // alertable wait, after the caller writes its status block.
      alertable = 0;
    } else if (status != X_STATUS_SUCCESS) {
      // A failed poll does not wait, so give up the CPU instead of spinning.
      XELOGW("XFile: I/O wait on {} returned {:08X}", name(), status);
      scheduler->YieldCurrentThread(false);
    }
  }
  ReleaseIoEvent(std::move(event));
}

object_ref<XEvent> XFile::AcquireIoEvent() {
  {
    std::lock_guard<std::mutex> lock(io_event_lock_);
    if (!idle_io_events_.empty()) {
      auto event = std::move(idle_io_events_.back());
      idle_io_events_.pop_back();
      return event;
    }
  }
  auto event = object_ref<XEvent>(new XEvent(kernel_state(), true));
  event->Initialize(false, false);
  // One signal per request would crowd the guest signals out of the ring.
  event->set_signal_ring_quiet(true);
  return event;
}

void XFile::ReleaseIoEvent(object_ref<XEvent> event) {
  // The completion can land after |done|, leaving it armed for the next user.
  event->Reset();
  std::lock_guard<std::mutex> lock(io_event_lock_);
  idle_io_events_.push_back(std::move(event));
}

uint64_t XFile::position() const { return position_.load(); }

void XFile::set_position(uint64_t value) { position_.store(value); }

X_STATUS XFile::QueryDirectory(X_FILE_DIRECTORY_INFORMATION* out_info,
                               size_t length, const std::string_view file_name,
                               bool restart) {
  // An I/O worker may already hold file_lock_ for a slow read.
  X_STATUS result = X_STATUS_SUCCESS;
  RunSynchronousIo([&]() {
    result = QueryDirectoryInternal(out_info, length, file_name, restart);
  });
  return result;
}

X_STATUS XFile::QueryDirectoryInternal(X_FILE_DIRECTORY_INFORMATION* out_info,
                                       size_t length,
                                       const std::string_view file_name,
                                       bool restart) {
  std::lock_guard<std::mutex> lock(file_lock_);
  assert_not_null(out_info);

  vfs::Entry* entry = nullptr;

  if (!file_name.empty()) {
    // Only queries in the current directory are supported for now.
    assert_true(utf8::find_any_of(file_name, "\\") == std::string_view::npos);

    find_engine_.SetRule(file_name);

    // Always restart the search?
    find_index_ = 0;
    entry = file_->entry()->IterateChildren(find_engine_, &find_index_);
    if (!entry) {
      return X_STATUS_NO_SUCH_FILE;
    }
  } else {
    if (restart) {
      find_index_ = 0;
    }

    entry = file_->entry()->IterateChildren(find_engine_, &find_index_);
    if (!entry) {
      return X_STATUS_NO_MORE_FILES;
    }
  }

  auto end = reinterpret_cast<uint8_t*>(out_info) + length;
  const auto& entry_name = entry->name();
  if (reinterpret_cast<uint8_t*>(&out_info->file_name[0]) + entry_name.size() >
      end) {
    assert_always("Buffer overflow?");
    return X_STATUS_NO_SUCH_FILE;
  }

  out_info->next_entry_offset = 0;
  out_info->file_index = static_cast<uint32_t>(find_index_);
  out_info->creation_time = entry->create_timestamp();
  out_info->last_access_time = entry->access_timestamp();
  out_info->last_write_time = entry->write_timestamp();
  out_info->change_time = entry->write_timestamp();
  out_info->end_of_file = entry->size();
  out_info->allocation_size = entry->allocation_size();
  out_info->attributes = entry->attributes();
  out_info->file_name_length = static_cast<uint32_t>(entry_name.size());
  std::memcpy(out_info->file_name, entry_name.data(), entry_name.size());

  return X_STATUS_SUCCESS;
}

X_STATUS XFile::Read(uint32_t buffer_guest_address, uint32_t buffer_length,
                     uint64_t byte_offset, uint32_t* out_bytes_read,
                     uint32_t apc_context, bool notify_completion) {
  // file_lock_ is taken inside the closure, on an I/O worker, so it is never
  // held while the calling fiber waits.
  X_STATUS result = X_STATUS_SUCCESS;
  RunSynchronousIo([&]() {
    DelayFileRead();
    std::lock_guard<std::mutex> lock(file_lock_);
    result = ReadInternal(buffer_guest_address, buffer_length, byte_offset,
                          out_bytes_read, apc_context, notify_completion);
  });
  return result;
}

X_STATUS XFile::ReadInternal(uint32_t buffer_guest_address,
                             uint32_t buffer_length, uint64_t byte_offset,
                             uint32_t* out_bytes_read, uint32_t apc_context,
                             bool notify_completion) {
  if (byte_offset == uint64_t(-1)) {
    // Read from current position.
    byte_offset = position_.load();
  }

  size_t bytes_read = 0;
  X_STATUS result = X_STATUS_SUCCESS;
  // Zero length means success for a valid file object according to Windows
  // tests.
  if (buffer_length) {
    if (UINT32_MAX - buffer_guest_address < buffer_length) {
      result = X_STATUS_ACCESS_VIOLATION;
    } else {
      // Games often read directly to texture/vertex buffer memory - in this
      // case, invalidation notifications must be sent. However, having any
      // memory callbacks in the range will result in STATUS_ACCESS_VIOLATION at
      // least on Windows, without anything being read or any callbacks being
      // triggered. So for physical memory, host protection must be bypassed,
      // and invalidation callbacks must be triggered manually (it's also wrong
      // to trigger invalidation callbacks before reading in this case, because
      // during the read, the guest may still access the data around the buffer
      // that is located in the same host pages as the buffer's start and end,
      // on the GPU - and that must not trigger a race condition).
      uint32_t buffer_guest_high_address =
          buffer_guest_address + buffer_length - 1;
      xe::BaseHeap* buffer_start_heap =
          memory()->LookupHeap(buffer_guest_address);
      const xe::BaseHeap* buffer_end_heap =
          memory()->LookupHeap(buffer_guest_high_address);
      if (!buffer_start_heap || !buffer_end_heap ||
          (buffer_start_heap->heap_type() == HeapType::kGuestPhysical) !=
              (buffer_end_heap->heap_type() == HeapType::kGuestPhysical) ||
          (buffer_start_heap->heap_type() == HeapType::kGuestPhysical &&
           buffer_start_heap != buffer_end_heap)) {
        result = X_STATUS_ACCESS_VIOLATION;
      } else {
        xe::PhysicalHeap* buffer_physical_heap =
            buffer_start_heap->heap_type() == HeapType::kGuestPhysical
                ? static_cast<xe::PhysicalHeap*>(buffer_start_heap)
                : nullptr;
        if (buffer_physical_heap &&
            buffer_physical_heap->QueryRangeAccess(buffer_guest_address,
                                                   buffer_guest_high_address) !=
                memory::PageAccess::kReadWrite) {
          result = X_STATUS_ACCESS_VIOLATION;
        } else {
          result = file_->ReadSync(
              std::span<uint8_t>(
                  buffer_physical_heap
                      ? memory()->TranslatePhysical(
                            buffer_physical_heap->GetPhysicalAddress(
                                buffer_guest_address))
                      : memory()->TranslateVirtual(buffer_guest_address),
                  buffer_length),
              size_t(byte_offset), &bytes_read);
          if (XSUCCEEDED(result)) {
            if (buffer_physical_heap) {
              buffer_physical_heap->TriggerCallbacks(
                  xe::global_critical_region::AcquireDirect(),
                  buffer_guest_address, buffer_length, true, true);
            }

            if (byte_offset) {
              position_.store(byte_offset);
            }
            position_.fetch_add(bytes_read);
          }
        }
      }
    }
  }

  if (out_bytes_read) {
    *out_bytes_read = uint32_t(bytes_read);
  }

  if (notify_completion) {
    NotifyCompletion(result, uint32_t(bytes_read), apc_context);
  }

  return result;
}

void XFile::PostIo(std::function<void()> fn) {
  kernel_state()->guest_scheduler()->PostHostCall(std::move(fn),
                                                  io_call_class());
}

X_STATUS XFile::ReadScatter(uint32_t segments_guest_address, uint32_t length,
                            uint64_t byte_offset, uint32_t* out_bytes_read,
                            uint32_t apc_context, bool notify_completion) {
  // The whole loop as one request, so the fiber waits once.
  X_STATUS result = X_STATUS_SUCCESS;
  RunSynchronousIo([&]() {
    DelayFileRead();
    result =
        ReadScatterInternal(segments_guest_address, length, byte_offset,
                            out_bytes_read, apc_context, notify_completion);
  });
  return result;
}

X_STATUS XFile::ReadScatterInternal(uint32_t segments_guest_address,
                                    uint32_t length, uint64_t byte_offset,
                                    uint32_t* out_bytes_read,
                                    uint32_t apc_context,
                                    bool notify_completion) {
  std::lock_guard<std::mutex> lock(file_lock_);
  X_STATUS result = X_STATUS_SUCCESS;

  // segments points to an array of buffer pointers of type
  // "FILE_SEGMENT_ELEMENT", but they can just be treated as normal pointers
  xe::be<uint32_t>* segments = reinterpret_cast<xe::be<uint32_t>*>(
      memory()->TranslateVirtual(segments_guest_address));

  // TODO: not sure if this is meant to change depending on buffer address?
  // (only game seen using this always seems to use 4096-byte buffers)
  uint32_t page_size = 4096;

  uint32_t read_total = 0;
  uint32_t read_remain = length;
  while (read_remain) {
    uint32_t read_length = read_remain;
    uint32_t read_buffer = *segments;
    if (read_length > page_size) {
      read_length = page_size;
      segments++;
    }

    uint32_t bytes_read = 0;
    result =
        ReadInternal(read_buffer, read_length,
                     byte_offset ? ((byte_offset != -1 && byte_offset != -2)
                                        ? byte_offset + read_total
                                        : byte_offset)
                                 : -1,
                     &bytes_read, apc_context, false);

    if (result != X_STATUS_SUCCESS) {
      break;
    }

    read_total += bytes_read;
    read_remain -= read_length;
  }

  if (out_bytes_read) {
    *out_bytes_read = uint32_t(read_total);
  }

  if (notify_completion) {
    NotifyCompletion(result, read_total, apc_context);
  }

  return result;
}

X_STATUS XFile::Write(uint32_t buffer_guest_address, uint32_t buffer_length,
                      uint64_t byte_offset, uint32_t* out_bytes_written,
                      uint32_t apc_context) {
  X_STATUS result = X_STATUS_SUCCESS;
  RunSynchronousIo([&]() {
    result = WriteInternal(buffer_guest_address, buffer_length, byte_offset,
                           out_bytes_written, apc_context);
  });
  return result;
}

X_STATUS XFile::WriteInternal(uint32_t buffer_guest_address,
                              uint32_t buffer_length, uint64_t byte_offset,
                              uint32_t* out_bytes_written,
                              uint32_t apc_context) {
  std::lock_guard<std::mutex> lock(file_lock_);
  if (byte_offset == uint64_t(-1)) {
    // Write from current position.
    byte_offset = position_.load();
  }

  size_t bytes_written = 0;
  X_STATUS result = file_->WriteSync(
      std::span<uint8_t>(memory()->TranslateVirtual(buffer_guest_address),
                         buffer_length),
      size_t(byte_offset), &bytes_written);
  if (XSUCCEEDED(result)) {
    position_.fetch_add(bytes_written);
  }

  if (out_bytes_written) {
    *out_bytes_written = uint32_t(bytes_written);
  }

  NotifyCompletion(result, uint32_t(bytes_written), apc_context);
  return result;
}

X_STATUS XFile::SetLength(size_t length) {
  X_STATUS result = X_STATUS_SUCCESS;
  RunSynchronousIo([&]() {
    std::lock_guard<std::mutex> lock(file_lock_);
    result = file_->SetLength(length);
  });
  return result;
}
X_STATUS XFile::Rename(const std::filesystem::path file_path) {
  entry()->Rename(file_path);
  return X_STATUS_SUCCESS;
}

void XFile::RegisterIOCompletionPort(uint32_t key,
                                     object_ref<XIOCompletion> port) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  completion_ports_.push_back({key, port});
}

void XFile::RemoveIOCompletionPort(uint32_t key) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  for (auto it = completion_ports_.begin(); it != completion_ports_.end();
       it++) {
    if (it->first == key) {
      completion_ports_.erase(it);
      break;
    }
  }
}

bool XFile::Save(ByteStream* stream) {
  // XELOGD("XFile {:08X} ({})", handle(),
  //        file_->entry()->absolute_path().c_str());

  if (!SaveObject(stream)) {
    return false;
  }

  stream->Write(file_->entry()->absolute_path());
  stream->Write<uint64_t>(position_);
  stream->Write(file_access());
  stream->Write<bool>(
      (file_->entry()->attributes() & vfs::kFileAttributeDirectory) != 0);
  stream->Write<bool>(is_synchronous_);

  return true;
}

object_ref<XFile> XFile::Restore(KernelState* kernel_state,
                                 ByteStream* stream) {
  auto file = new XFile();
  file->kernel_state_ = kernel_state;
  if (!file->RestoreObject(stream)) {
    delete file;
    return nullptr;
  }

  auto abs_path = stream->Read<std::string>();
  uint64_t position = stream->Read<uint64_t>();
  auto access = stream->Read<uint32_t>();
  auto is_directory = stream->Read<bool>();
  auto is_synchronous = stream->Read<bool>();

  // XELOGD("XFile {:08X} ({})", file->handle(), abs_path);

  vfs::File* vfs_file = nullptr;
  vfs::FileAction action;
  auto res = kernel_state->file_system()->OpenFile(
      nullptr, abs_path, vfs::FileDisposition::kOpen, access, is_directory,
      false, &vfs_file, &action);
  if (XFAILED(res)) {
    // XELOGE("Failed to open XFile: error {:08X}", res);
    return object_ref<XFile>(file);
  }

  file->file_ = vfs_file;
  file->position_ = position;
  file->is_synchronous_ = is_synchronous;

  return object_ref<XFile>(file);
}

void XFile::NotifyCompletion(X_STATUS status, uint32_t num_bytes,
                             uint32_t apc_context) {
  XIOCompletion::IONotification notify;
  notify.apc_context = apc_context;
  notify.num_bytes = num_bytes;
  notify.status = status;
  NotifyIOCompletionPorts(notify);
  async_event_->Set();
}

void XFile::NotifyIOCompletionPorts(
    XIOCompletion::IONotification& notification) {
  std::lock_guard<std::mutex> lock(completion_port_lock_);

  for (auto port : completion_ports_) {
    notification.key_context = port.first;
    port.second->QueueNotification(notification);
  }
}

}  // namespace kernel
}  // namespace xe
