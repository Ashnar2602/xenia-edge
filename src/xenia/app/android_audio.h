#ifndef XENIA_APP_ANDROID_AUDIO_H_
#define XENIA_APP_ANDROID_AUDIO_H_

#include "xenia/apu/audio_driver.h"
#include "xenia/apu/sdl/sdl_audio_system.h"

namespace xe::app {
// Keep the desktop SDL mixer/conversion. Android's cooperative pause leaves
// guest audio callbacks running, so pause their output without fencing workers.
class AndroidAudioSystem final : public apu::sdl::SDLAudioSystem {
 public:
  using SDLAudioSystem::CreateDriver;
  using SDLAudioSystem::SDLAudioSystem;

  void SetOutputPaused(bool paused) {
    auto lock = global_critical_region_.Acquire();
    output_paused_ = paused;
    for (auto& client : clients_) {
      if (client.in_use) {
        if (paused) {
          client.driver->Pause();
        } else {
          client.driver->Resume();
        }
      }
    }
  }

  X_RESULT CreateDriver(size_t index, threading::Semaphore* semaphore,
                        apu::AudioDriver** out_driver) override {
    auto lock = global_critical_region_.Acquire();
    auto result = SDLAudioSystem::CreateDriver(index, semaphore, out_driver);
    if (result == X_ERROR_SUCCESS && output_paused_) {
      (*out_driver)->Pause();
    }
    return result;
  }

 private:
  bool output_paused_ = false;
};
}  // namespace xe::app

#endif  // XENIA_APP_ANDROID_AUDIO_H_
