#pragma once

#include "renderer/uv_anim.h"
#include "threedi/threedi_3di3.h"
#include "threedi/threedi_ctrl_catalog.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace renderer {

struct MaterialRuntime {
    UvAnimTransform uv;
    float rgb_r = 1.0f, rgb_g = 1.0f, rgb_b = 1.0f;
    float alpha = 1.0f;
};

struct LightRuntime {
    float r = 1.0f, g = 1.0f, b = 1.0f;
    float intensity = 1.0f;
};

using ControlRegisterValues =
        std::array<int32_t, THREEDI_CTRL_REGISTER_COUNT>;

// Evaluate per-frame material animation state (UV, RGB, alpha).
// ctrl_values is retail's canonical signed 96-slot bus. ctrl_names preserves
// the model-local table used by material parameter indices; those names are
// patched through the loader convention before indexing the global bus.
MaterialRuntime eval_material_runtime(const ThreediMaterial& mat,
                                      uint32_t time_ms,
                                      const std::vector<std::string>& ctrl_names,
                                      const ControlRegisterValues& ctrl_values);

// Evaluate per-frame light color animation.
LightRuntime eval_light_runtime(uint8_t style,
                                uint8_t phase_byte,
                                uint16_t rate_word,
                                const std::array<uint8_t, 4>& color_start,
                                const std::array<uint8_t, 4>& color_end,
                                uint32_t time_ms,
                                int32_t ctrl_value);

// Compute the animation frame index from raw .3di animation params + ctrl regs.
// max_anim_frames clamps cycle length when the engine pads slot tables; pass 0
// for no clamp. ctrl_names/ctrl_values use the same loader-patched global-bus
// resolution as eval_material_runtime and are only consumed for type 1.
int compute_anim_frame(const ThreediMaterial& mat,
                       uint32_t max_anim_frames,
                       uint32_t time_ms,
                       const std::vector<std::string>& ctrl_names,
                       const ControlRegisterValues& ctrl_values);

} // namespace renderer
