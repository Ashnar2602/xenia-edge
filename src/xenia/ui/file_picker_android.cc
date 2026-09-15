/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <memory>

#include "xenia/base/logging.h"
#include "xenia/ui/file_picker.h"
#include "xenia/ui/windowed_app_context_android.h"

namespace xe {
namespace ui {

// Android activities deliver selected local paths without blocking the UI
// thread.
class AndroidFilePicker : public FilePicker {
 public:
  bool Show(Window* parent_window) override {
    XELOGE("Android file selection requires ShowAsync");
    return false;
  }
  void ShowAsync(Window* window, Callback callback) override {
    if (!window) {
      callback({});
      return;
    }
    static_cast<AndroidWindowedAppContext&>(window->app_context())
        .PickFiles(int(mode_), int(type_), multi_selection_,
                   std::move(callback));
  }
};

std::unique_ptr<FilePicker> FilePicker::Create() {
  return std::make_unique<AndroidFilePicker>();
}

}  // namespace ui
}  // namespace xe
