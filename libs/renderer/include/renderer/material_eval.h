#pragma once

#include "threedi/threedi_3di3.h"
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace renderer {

struct UvRuntime {
    float offset_u = 0.0f, offset_v = 0.0f;
    float scale_u = 1.0f, scale_v = 1.0f;
    float rotation = 0.0f;
};

struct MaterialRuntime {
    UvRuntime uv;
    float rgb_r = 1.0f, rgb_g = 1.0f, rgb_b = 1.0f;
    float alpha = 1.0f;
};

struct LightRuntime {
    float r = 1.0f, g = 1.0f, b = 1.0f;
    float intensity = 1.0f;
};

// Evaluate per-frame material animation state (UV, RGB, alpha).
MaterialRuntime eval_material_runtime(const ThreediMaterial& mat,
                                      uint32_t time_ms,
                                      const std::vector<std::string>& ctrl_names,
                                      const std::unordered_map<std::string, uint16_t>& ctrl_values);

// Evaluate per-frame light color animation.
LightRuntime eval_light_runtime(uint8_t style,
                                uint8_t phase_byte,
                                uint16_t rate_word,
                                const std::array<uint8_t, 4>& color_start,
                                const std::array<uint8_t, 4>& color_end,
                                uint32_t time_ms,
                                uint16_t ctrl_value);

// Compute the animation frame index from raw .3di animation params + ctrl regs.
// max_anim_frames clamps cycle length when the engine pads slot tables; pass 0
// for no clamp.  reg_value_lookup is only used when animation_type == 1.
int compute_anim_frame(const ThreediMaterial& mat,
                       uint32_t max_anim_frames,
                       uint32_t time_ms,
                       const std::vector<std::string>& ctrl_names,
                       const std::unordered_map<std::string, uint16_t>& ctrl_values);

} // namespace renderer
