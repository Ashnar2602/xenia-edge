// Display controls shared by desktop and Android frontends.
#include "xenia/ui/display_config.h"
#include "xenia/base/cvar.h"
#include "xenia/config.h"
#include "xenia/gpu/graphics_system.h"
DEFINE_string(
    postprocess_antialiasing, "",
    "Post-processing anti-aliasing effect to apply to the image output of the "
    "game.\n"
    "Using post-process anti-aliasing is heavily recommended when AMD "
    "FidelityFX Contrast Adaptive Sharpening or Super Resolution 1.0 is "
    "active.\n"
    "Use: [none, fxaa, fxaa_extreme]\n"
    " none (or any value not listed here):\n"
    "  Don't alter the original image.\n"
    " fxaa:\n"
    "  NVIDIA Fast Approximate Anti-Aliasing 3.11, normal quality preset (12)."
    "\n"
    " fxaa_extreme:\n"
    "  NVIDIA Fast Approximate Anti-Aliasing 3.11, extreme quality preset "
    "(39).",
    "Display");
DEFINE_string(
    postprocess_scaling_and_sharpening, "",
    "Post-processing effect to use for resampling and/or sharpening of the "
    "final display output.\n"
    "Use: [bilinear, cas, fsr]\n"
    " bilinear (or any value not listed here):\n"
    "  Original image at 1:1, simple bilinear stretching for resampling.\n"
    " cas:\n"
    "  Use AMD FidelityFX Contrast Adaptive Sharpening (CAS) for sharpening "
    "at scaling factors of up to 2x2, with additional bilinear stretching for "
    "larger factors.\n"
    " fsr:\n"
    "  Use AMD FidelityFX Super Resolution 1.0 (FSR) for highest-quality "
    "upscaling, or AMD FidelityFX Contrast Adaptive Sharpening for sharpening "
    "while not scaling or downsampling.\n"
    "  For scaling by factors of more than 2x2, multiple FSR passes are done.",
    "Display");
DEFINE_double(
    postprocess_ffx_cas_additional_sharpness,
    xe::ui::Presenter::GuestOutputPaintConfig::kCasAdditionalSharpnessDefault,
    "Additional sharpness for AMD FidelityFX Contrast Adaptive Sharpening "
    "(CAS), from 0 to 1.\n"
    "Higher is sharper.",
    "Display");
DEFINE_uint32(
    postprocess_ffx_fsr_max_upsampling_passes,
    xe::ui::Presenter::GuestOutputPaintConfig::kFsrMaxUpscalingPassesMax,
    "Maximum number of upsampling passes performed in AMD FidelityFX Super "
    "Resolution 1.0 (FSR) before falling back to bilinear stretching after the "
    "final pass.\n"
    "Each pass upscales only to up to 2x2 the previous size. If the game "
    "outputs a 1280x720 image, 1 pass will upscale it to up to 2560x1440 "
    "(below 4K), after 2 passes it will be upscaled to a maximum of 5120x2880 "
    "(including 3840x2160 for 4K), and so on.\n"
    "This variable has no effect if the display resolution isn't very high, "
    "but may be reduced on resolutions like 4K or 8K in case the performance "
    "impact of multiple FSR upsampling passes is too high, or if softer edges "
    "are desired.\n"
    "The default value is the maximum internally supported by Xenia.",
    "Display");
DEFINE_double(
    postprocess_ffx_fsr_sharpness_reduction,
    xe::ui::Presenter::GuestOutputPaintConfig::kFsrSharpnessReductionDefault,
    "Sharpness reduction for AMD FidelityFX Super Resolution 1.0 (FSR), in "
    "stops.\n"
    "Lower is sharper.",
    "Display");
// Dithering to 8bpc is enabled by default since the effect is minor, only
// effects what can't be shown normally by host displays, and nothing is changed
// by it for 8bpc source without resampling.
DEFINE_bool(
    postprocess_dither, true,
    "Dither the final image output from the internal precision to 8 bits per "
    "channel so gradients are smoother.\n"
    "On a 10bpc display, the lower 2 bits will still be kept, but noise will "
    "be added to them - disabling may be recommended for 10bpc, but it "
    "depends on the 10bpc displaying capabilities of the actual display used.",
    "Display");

namespace xe::ui {
const char* GetCvarValueForSwapPostEffect(
    gpu::CommandProcessor::SwapPostEffect effect) {
  switch (effect) {
    case gpu::CommandProcessor::SwapPostEffect::kFxaa:
      return "fxaa";
    case gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme:
      return "fxaa_extreme";
    default:
      return "";
  }
}

gpu::CommandProcessor::SwapPostEffect GetSwapPostEffectForCvarValue(
    const std::string& cvar_value) {
  if (cvar_value == GetCvarValueForSwapPostEffect(
                        gpu::CommandProcessor::SwapPostEffect::kFxaa)) {
    return gpu::CommandProcessor::SwapPostEffect::kFxaa;
  }
  if (cvar_value == GetCvarValueForSwapPostEffect(
                        gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme)) {
    return gpu::CommandProcessor::SwapPostEffect::kFxaaExtreme;
  }
  return gpu::CommandProcessor::SwapPostEffect::kNone;
}

const char* GetCvarValueForGuestOutputPaintEffect(
    ui::Presenter::GuestOutputPaintConfig::Effect effect) {
  switch (effect) {
    case ui::Presenter::GuestOutputPaintConfig::Effect::kCas:
      return "cas";
    case ui::Presenter::GuestOutputPaintConfig::Effect::kFsr:
      return "fsr";
    default:
      return "";
  }
}

ui::Presenter::GuestOutputPaintConfig::Effect
GetGuestOutputPaintEffectForCvarValue(const std::string& cvar_value) {
  if (cvar_value == GetCvarValueForGuestOutputPaintEffect(
                        ui::Presenter::GuestOutputPaintConfig::Effect::kCas)) {
    return ui::Presenter::GuestOutputPaintConfig::Effect::kCas;
  }
  if (cvar_value == GetCvarValueForGuestOutputPaintEffect(
                        ui::Presenter::GuestOutputPaintConfig::Effect::kFsr)) {
    return ui::Presenter::GuestOutputPaintConfig::Effect::kFsr;
  }
  return ui::Presenter::GuestOutputPaintConfig::Effect::kBilinear;
}

ui::Presenter::GuestOutputPaintConfig GetGuestOutputPaintConfigForCvars() {
  ui::Presenter::GuestOutputPaintConfig paint_config;
  paint_config.SetAllowOverscanCutoff(true);
  paint_config.SetEffect(GetGuestOutputPaintEffectForCvarValue(
      cvars::postprocess_scaling_and_sharpening));
  paint_config.SetCasAdditionalSharpness(
      float(cvars::postprocess_ffx_cas_additional_sharpness));
  paint_config.SetFsrMaxUpsamplingPasses(
      cvars::postprocess_ffx_fsr_max_upsampling_passes);
  paint_config.SetFsrSharpnessReduction(
      float(cvars::postprocess_ffx_fsr_sharpness_reduction));
  paint_config.SetDither(cvars::postprocess_dither);
  return paint_config;
}

void ApplyDisplayConfigForCvars(Emulator* emulator) {
  gpu::GraphicsSystem* graphics_system = emulator->graphics_system();
  if (!graphics_system) {
    return;
  }

  gpu::CommandProcessor* command_processor =
      graphics_system->command_processor();
  if (command_processor) {
    command_processor->SetDesiredSwapPostEffect(
        GetSwapPostEffectForCvarValue(cvars::postprocess_antialiasing));
  }

  ui::Presenter* presenter = graphics_system->presenter();
  if (presenter) {
    presenter->SetGuestOutputPaintConfigFromUIThread(
        GetGuestOutputPaintConfigForCvars());
  }
}

void UpdateAntiAliasingCvar(gpu::CommandProcessor::SwapPostEffect effect) {
  if (GetSwapPostEffectForCvarValue(cvars::postprocess_antialiasing) !=
      effect) {
    OVERRIDE_PERSIST_string(postprocess_antialiasing,
                            GetCvarValueForSwapPostEffect(effect));
  }
}

void UpdateScalingAndSharpeningCvar(
    ui::Presenter::GuestOutputPaintConfig::Effect effect) {
  auto cvars_config = GetGuestOutputPaintConfigForCvars();
  if (cvars_config.GetEffect() != effect) {
    OVERRIDE_PERSIST_string(postprocess_scaling_and_sharpening,
                            GetCvarValueForGuestOutputPaintEffect(effect));
  }
}

void UpdateFsrSharpnessCvar(float value) {
  auto cvars_config = GetGuestOutputPaintConfigForCvars();
  if (cvars_config.GetFsrSharpnessReduction() != value) {
    OVERRIDE_PERSIST_double(postprocess_ffx_fsr_sharpness_reduction, value);
  }
}

void UpdateFsrMaxUpsamplingPassesCvar(uint32_t value) {
  auto cvars_config = GetGuestOutputPaintConfigForCvars();
  if (cvars_config.GetFsrMaxUpsamplingPasses() != value) {
    OVERRIDE_PERSIST_uint32(postprocess_ffx_fsr_max_upsampling_passes, value);
  }
}

void UpdateCasSharpnessCvar(float value) {
  auto cvars_config = GetGuestOutputPaintConfigForCvars();
  if (cvars_config.GetCasAdditionalSharpness() != value) {
    OVERRIDE_PERSIST_double(postprocess_ffx_cas_additional_sharpness, value);
  }
}

void UpdateDitherCvar(bool value) {
  auto cvars_config = GetGuestOutputPaintConfigForCvars();
  if (cvars_config.GetDither() != value) {
    OVERRIDE_PERSIST_bool(postprocess_dither, value);
  }
}
}  // namespace xe::ui
