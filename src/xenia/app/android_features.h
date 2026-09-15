#ifndef XENIA_APP_ANDROID_FEATURES_H_
#define XENIA_APP_ANDROID_FEATURES_H_
#include <jni.h>
#include "xenia/emulator.h"
namespace xe::app {
// The owning activity keeps this context alive until its serial worker stops.
Emulator* AndroidEmulator(jlong context);
}  // namespace xe::app
#endif
