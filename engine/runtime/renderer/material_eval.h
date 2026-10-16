#pragma once

#include <runtime/renderer/uv_anim.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::renderer {

// The effect parameters Material_ApplyShaderParameters writes per draw. Each field
// starts at the effect's own default and is written only where retail
// resolves a handle for it: an effect that never references MatTexCoord1,
// SelfLumColor or ReflectColor keeps the default.
// [orig: Material_ApplyShaderParameters @ 0x58DB80; _BaseInc.fx parameter defaults]
struct MaterialRuntime {
    UvAnimTransform uv;
    // SelfLumColor (_FFP.fx SELFLUM emissive), default {1, 1, 1}.
    float rgb_r = 1.0f, rgb_g = 1.0f, rgb_b = 1.0f;
    // ReflectColor (Glass/SkGlass/BumpMirrT/BmTxMirrT/EnvPhongT), default
    // {0.75, 0.75, 0.75, 0.75}.
    std::array<float, 4> reflect = {0.75f, 0.75f, 0.75f, 0.75f};
    // AlphaGenValue (_FFP.fx MaterialDiffuse.a).
    float alpha = 1.0f;
};

// The two colour routes a material channel can address. The loader keeps the
// authored routing byte only for 1 (ReflectColor) and 2 (SelfLumColor); any
// other byte routes nowhere.
// [orig: Material_ConvertDefinition @ 0x5B0563..0x5B057B (static colour
//  by is_glass) and @ 0x5B059E..0x5B05B5 (RgbGen by emissive_type)]
enum class MaterialColorTarget : uint8_t { None, ReflectColor, SelfLumColor };
MaterialColorTarget material_color_target(uint8_t routing_byte);

struct LightRuntime {
    float r = 1.0f, g = 1.0f, b = 1.0f;
    float intensity = 1.0f;
};

using ControlRegisterValues =
        std::array<int32_t, opennova::threedi::THREEDI_CTRL_REGISTER_COUNT>;

// Evaluate per-frame material animation state (UV, RGB, alpha).
// ctrl_values is retail's canonical signed 96-slot bus. ctrl_names preserves
// the model-local table used by material parameter indices; those names are
// patched through the loader convention before indexing the global bus.
// The material's shader tag selects the effect (an unknown tag resolves to
// the first registry entry, FF_ST_OP); only the parameters that effect
// references are evaluated, so a non-#UV effect never runs the UV transform
// and never draws its noise samples.
MaterialRuntime eval_material_runtime(const opennova::threedi::ThreediMaterial& mat,
                                      uint32_t time_ms,
                                      const std::vector<std::string>& ctrl_names,
                                      const ControlRegisterValues& ctrl_values);

// Whether any parameter the material's effect reads can change between
// draws: an active AlphaGen other than the constant style 24 (it runs for
// every effect), an active routed RgbGen other than 24, or an active UV
// channel on a #UV effect. Inactive generators (no high nibble) are constant.
bool material_runtime_is_dynamic(const opennova::threedi::ThreediMaterial& mat);

// The draw-invariant parameters: the routed static colours plus every
// generator that can not change. Dynamic generators keep their effect
// defaults; this reads no clock, no CTRL slot and draws no noise sample.
MaterialRuntime material_static_runtime(const opennova::threedi::ThreediMaterial& mat);

// Evaluate per-frame light color animation.
LightRuntime eval_light_runtime(uint8_t style,
                                uint8_t phase_byte,
                                uint16_t rate_word,
                                const std::array<uint8_t, 4>& color_start,
                                const std::array<uint8_t, 4>& color_end,
                                uint32_t time_ms,
                                int32_t ctrl_value);

// One model light as the object pass evaluates it: its LGHT record's index in
// the model and its colour/intensity this frame.
struct ModelLightEval {
    size_t index = 0;
    LightRuntime runtime;
};

// The model's LGHT records the object lights draw, each evaluated: a record
// with DISABLE_OBJECTS is skipped; a style that names a CTRL register has its
// phase byte rewritten from the model-local index to the global ordinal (the
// loader's fixup, as PANM's), and the styles that read the bus take that
// global slot's value; every record then runs eval_light_runtime at the PANM
// clock. [orig: model light fixups @ 0x5B5F4F..0x5B5F62; Light_GetPointLightParams
// @ 0x5A9180; RgbGen_EvaluateColor @ 0x5B23D0]
std::vector<ModelLightEval> eval_model_lights(const opennova::threedi::Threedi3di3& model,
                                              int64_t time_ms,
                                              const ControlRegisterValues& ctrl_values);

// Compute the animation frame index from raw .3di animation params + ctrl regs.
// max_anim_frames clamps cycle length when the engine pads slot tables; pass 0
// for no clamp. ctrl_names/ctrl_values use the same loader-patched global-bus
// resolution as eval_material_runtime and are only consumed for type 1.
int compute_anim_frame(const opennova::threedi::ThreediMaterial& mat,
                       uint32_t max_anim_frames,
                       uint32_t time_ms,
                       const std::vector<std::string>& ctrl_names,
                       const ControlRegisterValues& ctrl_values);

}  // namespace opennova::renderer