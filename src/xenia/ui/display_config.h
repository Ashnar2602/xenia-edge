// Display controls shared by desktop and Android frontends.
#ifndef XENIA_UI_DISPLAY_CONFIG_H_
#define XENIA_UI_DISPLAY_CONFIG_H_
#include "xenia/emulator.h"
#include "xenia/gpu/command_processor.h"
#include "xenia/ui/presenter.h"
namespace xe::ui {
const char* GetCvarValueForSwapPostEffect(
    gpu::CommandProcessor::SwapPostEffect effect);
gpu::CommandProcessor::SwapPostEffect GetSwapPostEffectForCvarValue(
    const std::string& cvar_value);
const char* GetCvarValueForGuestOutputPaintEffect(
    ui::Presenter::GuestOutputPaintConfig::Effect effect);
ui::Presenter::GuestOutputPaintConfig::Effect
GetGuestOutputPaintEffectForCvarValue(const std::string& cvar_value);
ui::Presenter::GuestOutputPaintConfig GetGuestOutputPaintConfigForCvars();
void ApplyDisplayConfigForCvars(Emulator* emulator);
void UpdateAntiAliasingCvar(gpu::CommandProcessor::SwapPostEffect effect);
void UpdateScalingAndSharpeningCvar(
    ui::Presenter::GuestOutputPaintConfig::Effect effect);
void UpdateFsrSharpnessCvar(float value);
void UpdateFsrMaxUpsamplingPassesCvar(uint32_t value);
void UpdateCasSharpnessCvar(float value);
void UpdateDitherCvar(bool value);
}  // namespace xe::ui
#endif
