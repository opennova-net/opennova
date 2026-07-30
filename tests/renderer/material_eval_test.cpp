#include "renderer/material_eval.h"
#include "threedi/threedi_panm_runtime.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr float kInv255 = 1.0f / 255.0f;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool nearly_equal(float a, float b, float epsilon = 0.0001f) {
    return std::fabs(a - b) <= epsilon;
}

renderer::ControlRegisterValues ctrl_bus(
        const std::unordered_map<std::string, int32_t>& values = {}) {
    renderer::ControlRegisterValues bus{};
    for (const auto& [name, value] : values) {
        const int ordinal = threedi_ctrl_register_ordinal(name.c_str());
        if (ordinal != THREEDI_CTRL_REGISTER_NOT_FOUND) {
            bus[static_cast<size_t>(ordinal)] = value;
        }
    }
    return bus;
}

renderer::MaterialRuntime eval_runtime(
        const ThreediMaterial& material,
        uint32_t time_ms,
        const std::vector<std::string>& ctrl_names = {},
        const std::unordered_map<std::string, int32_t>& ctrl_values = {}) {
    return renderer::eval_material_runtime(
            material, time_ms, ctrl_names, ctrl_bus(ctrl_values));
}

void expect_uv(const renderer::UvAnimTransform& actual,
               float m00,
               float m01,
               float m10,
               float m11,
               float m20,
               float m21,
               const char* message) {
    expect(nearly_equal(actual.m00, m00) &&
                   nearly_equal(actual.m01, m01) &&
                   nearly_equal(actual.m10, m10) &&
                   nearly_equal(actual.m11, m11) &&
                   nearly_equal(actual.m20, m20) &&
                   nearly_equal(actual.m21, m21),
           message);
}

}  // namespace

int main() {
    ThreediMaterial material{};

    {
        const renderer::MaterialRuntime runtime = eval_runtime(material, 750);
        expect_uv(runtime.uv, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                  "Inactive UV channels should produce the identity matrix");
        expect(nearly_equal(runtime.rgb_r, 1.0f) &&
                       nearly_equal(runtime.rgb_g, 1.0f) &&
                       nearly_equal(runtime.rgb_b, 1.0f),
               "Style 0 RGB gen should leave the fixed-function color source at 1");
        expect(nearly_equal(runtime.alpha, 1.0f),
               "Style 0 alpha gen should leave the fixed-function alpha at 1");
    }

    {
        // Each noise wave_lookup consumes the next sample from the shared
        // ported MSVC-formula stream. U and V cannot share one sample. This
        // pins local consumer order, not retail's whole-process CRT call order.
        // srand(1) yields 41, 18467, 6334...
        // [orig: wave_lookup @ 0x5DE6B0 low-nibble-6 branch;
        //  apply_shader_parameters @ 0x58DB80 evaluates U then V]
        material = {};
        material.u_params.style = 0x36;
        material.u_params.end = 1.0f;
        material.v_params.style = 0x36;
        material.v_params.end = 1.0f;
        threedi_wave_srand(1);
        const renderer::MaterialRuntime dual_noise =
                eval_runtime(material, 0);
        expect(nearly_equal(dual_noise.uv.m20,
                            16.0f * 41.0f / 65535.0f) &&
                       nearly_equal(dual_noise.uv.m21,
                            16.0f * 2083.0f / 65535.0f),
               "Dual UV noise channels should consume distinct consecutive seeded samples");

        // PANM and material animation call the same ported waveform stream. A
        // PANM sample first consumes 41; the following material U/V
        // lookups must therefore see 18467 and 6334 (low 12 bits 2083/2238).
        ThreediTransform panm_noise{};
        panm_noise.control = 0x36;
        panm_noise.end = 256;
        threedi_wave_srand(1);
        expect(threedi_panm_sample_track_raw(&panm_noise, 0, nullptr) == 656,
               "PANM should consume the first seeded waveform-noise sample");
        const renderer::MaterialRuntime after_panm =
                eval_runtime(material, 0);
        expect(nearly_equal(after_panm.uv.m20,
                            16.0f * 2083.0f / 65535.0f) &&
                       nearly_equal(after_panm.uv.m21,
                            16.0f * 2238.0f / 65535.0f),
               "PANM and material noise should share one ported waveform sequence");
    }

    {
        material = {};
        material.rgb_gen.style = 24;
        material.rgb_gen.start_color[0] = 89.0f * kInv255;
        material.rgb_gen.start_color[1] = 26.0f * kInv255;
        material.rgb_gen.start_color[2] = 153.0f * kInv255;
        material.rgb_gen.end_color[0] = 1.0f;
        material.rgb_gen.end_color[1] = 1.0f;
        material.rgb_gen.end_color[2] = 1.0f;

        const renderer::MaterialRuntime a = eval_runtime(material, 0);
        const renderer::MaterialRuntime b = eval_runtime(material, 4000);
        expect(nearly_equal(a.rgb_r, 89.0f * kInv255) &&
                       nearly_equal(a.rgb_g, 26.0f * kInv255) &&
                       nearly_equal(a.rgb_b, 153.0f * kInv255),
               "Style 24 RGB gen should use the packed start color");
        expect(nearly_equal(a.rgb_r, b.rgb_r) &&
                       nearly_equal(a.rgb_g, b.rgb_g) &&
                       nearly_equal(a.rgb_b, b.rgb_b),
               "Style 24 RGB gen should remain constant over time");
    }

    {
        material = {};
        material.rgb_gen.style = 50;
        material.rgb_gen.rate = 0.75f;
        material.rgb_gen.start_color[0] = 100.0f * kInv255;
        material.rgb_gen.end_color[0] = 0.0f;

        const renderer::MaterialRuntime a = eval_runtime(material, 0);
        const renderer::MaterialRuntime b = eval_runtime(material, 700);
        expect(!nearly_equal(a.rgb_r, b.rgb_r),
               "Waveform RGB style 50 should vary over time");

        ThreediMaterial phased = material;
        phased.rgb_gen.phase = 0.375f;
        const renderer::MaterialRuntime c = eval_runtime(material, 900);
        const renderer::MaterialRuntime d = eval_runtime(phased, 900);
        expect(!nearly_equal(c.rgb_r, d.rgb_r),
               "Waveform RGB phase should be reconstructed as its packed byte");
    }

    {
        material = {};
        material.u_params.style = 16;
        material.u_params.phase = 0.25f;
        material.u_params.gen_rate = 1.0f;
        const renderer::MaterialRuntime runtime = eval_runtime(material, 500);
        expect_uv(runtime.uv, 1.0f, 0.0f, 0.0f, 1.0f, 0.75f, 0.0f,
                  "UV style 16 should use the retail wrapping phase accumulator");

        material = {};
        material.u_params.style = 114;
        material.u_params.start = 128.0f; // 32768 packed as signed s16 -> -32768.
        expect_uv(eval_runtime(material, 0, {"LOD_FRAC"},
                              {{"LOD_FRAC", 0}}).uv,
                  1.0f, 0.0f, 0.0f, 1.0f, -128.0f, 0.0f,
                  "Runtime preview should use the exported signed-word wrap");

        material = {};
        material.v_params.style = 17;
        material.v_params.phase = 0.25f;
        material.v_params.gen_rate = 1.0f;
        const renderer::MaterialRuntime reverse = eval_runtime(material, 500);
        expect_uv(reverse.uv, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, -0.75f,
                  "UV style 17 should apply negative V scroll");
    }

    {
        material = {};
        material.u_params.style = 32;
        material.u_params.phase = 0.25f;
        const renderer::MaterialRuntime clockwise = eval_runtime(material, 0);
        expect_uv(clockwise.uv, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f,
                  "UV style 32 should rotate the U row about texture center");

        material.u_params.style = 33;
        const renderer::MaterialRuntime counterclockwise = eval_runtime(material, 0);
        expect_uv(counterclockwise.uv, 0.0f, 0.0f, -1.0f, 1.0f, 1.0f, 0.0f,
                  "UV style 33 should rotate the U row in the opposite direction");
    }

    {
        const std::vector<std::string> names = {"LOD_FADE_IN", "FLICKER"};
        const std::unordered_map<std::string, int32_t> values = {{"FLICKER", 32768}};

        material = {};
        material.u_params.style = 113;
        material.u_params.reg = 1;
        material.u_params.start = 0.0f;
        material.u_params.end = 2.0f;
        expect_uv(eval_runtime(material, 9999, names, values).uv,
                  0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f,
                  "Controlled UV 113 should SET U and leave its diagonal zero");

        material.u_params.style = 114;
        expect_uv(eval_runtime(material, 9999, names, values).uv,
                  1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f,
                  "Controlled UV 114 should scroll U");

        material.u_params.style = 115;
        expect_uv(eval_runtime(material, 9999, names, values).uv,
                  1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f,
                  "Controlled UV 115 should shear U from V");

        material.u_params.style = 116;
        material.u_params.start = 1.0f;
        material.u_params.end = 3.0f;
        expect_uv(eval_runtime(material, 9999, names, values).uv,
                  2.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                  "Controlled UV 116 should scale U");

        material.u_params.style = 117;
        material.u_params.start = 0.0f;
        material.u_params.end = 0.5f;
        expect_uv(eval_runtime(material, 9999, names, values).uv,
                  0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f,
                  "Controlled UV 117 should interpret its value as turns");

        material = {};
        material.v_params.style = 115;
        material.v_params.reg = 1;
        material.v_params.start = 0.0f;
        material.v_params.end = 2.0f;
        expect_uv(eval_runtime(material, 9999, names, values).uv,
                  1.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                  "The material adapter should preserve the V-channel shear term");

        material = {};
        material.u_params.style = 115;
        material.u_params.reg = 1;
        material.u_params.start = 0.0f;
        material.u_params.end = 2.0f;
        expect_uv(eval_runtime(material, 9999, names, {{"FLICKER", -32768}}).uv,
                  1.0f, 0.0f, -1.0f, 1.0f, 0.0f, 0.0f,
                  "Controlled UV should preserve negative signed dword values");
    }

    {
        material = {};
        material.u_params.style = 114;
        material.u_params.reg = 0;
        material.u_params.end = 2.0f;
        material.v_params.style = 114;
        material.v_params.reg = 1;
        material.v_params.end = 2.0f;
        const std::vector<std::string> duplicate_names = {
            "FLICKER", "fLiCkEr",
        };
        const std::unordered_map<std::string, int32_t> case_aliases = {
            {"FLICKER", 32768},
            {"flicker", 32768},
        };
        expect_uv(eval_runtime(material, 0, duplicate_names, case_aliases).uv,
                  1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f,
                  "Duplicate local names and case variants should alias one global CTRL slot");

        material = {};
        material.rgb_gen.style = 113;
        material.rgb_gen.reg = 0;
        material.rgb_gen.end_color[0] = 1.0f;
        const renderer::MaterialRuntime ignored =
                eval_runtime(material, 0, {"FLICKER"},
                             {{"NOT_A_RETAIL_REGISTER", 65536}});
        expect(nearly_equal(ignored.rgb_r, 0.0f),
               "Unknown supplied CTRL keys should not alias the global bus");

        const renderer::MaterialRuntime loader_alias =
                eval_runtime(material, 0, {"MODEL_PRIVATE_CTRL"},
                             {{"lod_frac", 65536}});
        expect(nearly_equal(loader_alias.rgb_r, 1.0f),
               "Unknown authored CTRL names should alias lowercase LOD_FRAC");
    }

    {
        const std::vector<std::string> names = {"FLICKER"};
        for (uint8_t style : {uint8_t{113}, uint8_t{114}}) {
            material = {};
            material.rgb_gen.style = style;
            material.rgb_gen.reg = 0;
            material.rgb_gen.start_color[0] = 200.0f * kInv255;
            material.rgb_gen.start_color[1] = 10.0f * kInv255;
            material.rgb_gen.start_color[2] = 30.0f * kInv255;
            material.rgb_gen.end_color[0] = 10.0f * kInv255;
            material.rgb_gen.end_color[1] = 201.0f * kInv255;
            material.rgb_gen.end_color[2] = 130.0f * kInv255;

            const renderer::MaterialRuntime runtime =
                    eval_runtime(material, 12345, names, {{"FLICKER", 32768}});
            expect(nearly_equal(runtime.rgb_r, 105.0f * kInv255) &&
                           nearly_equal(runtime.rgb_g, 105.0f * kInv255) &&
                           nearly_equal(runtime.rgb_b, 80.0f * kInv255),
                   "RGB styles 113/114 should use integer CTRL interpolation");
        }

        material = {};
        material.rgb_gen.style = 113;
        material.rgb_gen.reg = 0;
        material.rgb_gen.end_color[0] = 1.0f;
        const renderer::MaterialRuntime almost_full =
                eval_runtime(material, 0, names, {{"FLICKER", 65535}});
        expect(nearly_equal(almost_full.rgb_r, 254.0f * kInv255),
               "RGB CTRL interpolation should retain the 65535 pre-endpoint");
        const renderer::MaterialRuntime full =
                eval_runtime(material, 0, names, {{"FLICKER", 65536}});
        expect(nearly_equal(full.rgb_r, 1.0f),
               "RGB CTRL 0x10000 should reach the exact endpoint");
        const renderer::MaterialRuntime wrapped =
                eval_runtime(material, 0, names,
                             {{"FLICKER", std::numeric_limits<int32_t>::max()}});
        expect(nearly_equal(wrapped.rgb_r, 32767.0f * kInv255),
               "RGB CTRL multiplication should retain retail's wrapped low dword");

        material.rgb_gen.start_color[0] = 100.0f * kInv255;
        material.rgb_gen.end_color[0] = 200.0f * kInv255;
        const renderer::MaterialRuntime negative =
                eval_runtime(material, 0, names, {{"FLICKER", -32768}});
        expect(nearly_equal(negative.rgb_r, 50.0f * kInv255),
               "RGB CTRL interpolation should preserve signed negative extrapolation");
    }

    {
        material = {};
        material.rgb_gen.style = 115;
        material.rgb_gen.reg = 37;  // The packed parameter byte becomes waveform phase.
        material.rgb_gen.rate = 0.75f;
        material.rgb_gen.end_color[0] = 1.0f;
        std::vector<std::string> names(38);
        names[37] = "FLICKER";

        const renderer::MaterialRuntime low =
                eval_runtime(material, 611, names, {{"FLICKER", 0}});
        const renderer::MaterialRuntime high =
                eval_runtime(material, 611, names, {{"FLICKER", 65535}});
        expect(nearly_equal(low.rgb_r, high.rgb_r),
               "RGB style 115 should be a waveform and must not read CTRL");

        ThreediMaterial global_ordinal = material;
        global_ordinal.rgb_gen.reg = 3;  // FLICKER's retail global ordinal.
        std::vector<std::string> ordinal_names(4);
        ordinal_names[3] = "FLICKER";
        const renderer::MaterialRuntime ordinal_phase =
                eval_runtime(global_ordinal, 611, ordinal_names, {});
        expect(nearly_equal(low.rgb_r, ordinal_phase.rgb_r),
               "RGB style 115 phase should use the loader-patched global ordinal, not local index 37");

        const renderer::MaterialRuntime later =
                eval_runtime(material, 1611, names, {{"FLICKER", 65535}});
        expect(!nearly_equal(high.rgb_r, later.rgb_r),
               "RGB style 115 should retain its waveform time dependence");
    }

    {
        material = {};
        material.alpha_gen.style = 24;
        material.alpha_gen.start = 64;
        material.alpha_gen.end = 255;
        expect(nearly_equal(eval_runtime(material, 999).alpha, 64.0f * kInv255),
               "Alpha style 24 should return packed start / 255");

        material.alpha_gen.style = 113;
        material.alpha_gen.reg = 0;
        material.alpha_gen.start = 10;
        material.alpha_gen.end = 210;
        const renderer::MaterialRuntime controlled =
                eval_runtime(material, 999, {"FLICKER"}, {{"FLICKER", 32768}});
        expect(nearly_equal(controlled.alpha, 110.0f * kInv255),
               "Alpha style 113 should use the retail CTRL-table interpolation");
        const renderer::MaterialRuntime endpoint =
                eval_runtime(material, 999, {"FLICKER"}, {{"FLICKER", 65536}});
        expect(nearly_equal(endpoint.alpha, 210.0f * kInv255),
               "Alpha CTRL 0x10000 should reach the exact endpoint");

        material.alpha_gen.style = 114;
        material.alpha_gen.reg = 37;
        material.alpha_gen.rate = 0.75f;
        std::vector<std::string> names(38);
        names[37] = "FLICKER";
        const float low =
                eval_runtime(material, 733, names, {{"FLICKER", 0}}).alpha;
        const float high =
                eval_runtime(material, 733, names, {{"FLICKER", 65535}}).alpha;
        expect(nearly_equal(low, high),
               "Alpha style 114 should remain a waveform and must not read CTRL");

        ThreediMaterial alpha_global_ordinal = material;
        alpha_global_ordinal.alpha_gen.reg = 3;
        std::vector<std::string> ordinal_names(4);
        ordinal_names[3] = "FLICKER";
        const float ordinal_phase =
                eval_runtime(alpha_global_ordinal, 733, ordinal_names, {}).alpha;
        expect(nearly_equal(low, ordinal_phase),
               "Alpha waveform phase should use the loader-patched global ordinal");
    }

    {
        const std::array<uint8_t, 4> start = {10, 20, 30, 0};
        const std::array<uint8_t, 4> end = {110, 120, 130, 0};
        for (uint8_t style : {uint8_t{113}, uint8_t{114}}) {
            const renderer::LightRuntime light = renderer::eval_light_runtime(
                    style, 0, 0, start, end, 0, 32768);
            expect(nearly_equal(light.r, 80.0f * kInv255) &&
                           nearly_equal(light.g, 70.0f * kInv255) &&
                           nearly_equal(light.b, 60.0f * kInv255),
                   "Light RGB styles 113/114 should share material RGB semantics");
        }
        const renderer::LightRuntime endpoint = renderer::eval_light_runtime(
                113, 0, 0, start, end, 0, 65536);
        expect(nearly_equal(endpoint.r, 130.0f * kInv255) &&
                       nearly_equal(endpoint.g, 120.0f * kInv255) &&
                       nearly_equal(endpoint.b, 110.0f * kInv255),
               "Light CTRL 0x10000 should reach its packed endpoint");
        const renderer::LightRuntime negative = renderer::eval_light_runtime(
                113, 0, 0, start, end, 0, -32768);
        expect(nearly_equal(negative.r, -20.0f * kInv255) &&
                       nearly_equal(negative.g, -30.0f * kInv255) &&
                       nearly_equal(negative.b, -40.0f * kInv255),
               "Light CTRL should preserve signed negative extrapolation");

        const renderer::LightRuntime low = renderer::eval_light_runtime(
                115, 37, 192, start, end, 733, 0);
        const renderer::LightRuntime high = renderer::eval_light_runtime(
                115, 37, 192, start, end, 733, 65535);
        expect(nearly_equal(low.r, high.r) &&
                       nearly_equal(low.g, high.g) &&
                       nearly_equal(low.b, high.b),
               "Light RGB style 115 should remain a waveform");
    }

    {
        material = {};
        material.animation.num_frames = 4;
        material.animation.animation_type = 1;
        material.animation.cycle_frame_time = 1;
        const std::vector<std::string> names = {"LOD_FADE_IN", "FLICKER"};
        expect(renderer::compute_anim_frame(
                       material, 0, 0, names, ctrl_bus({{"FLICKER", 0}})) == 0,
               "CTRL texture animation should start at frame zero");
        expect(renderer::compute_anim_frame(
                       material, 0, 0, names, ctrl_bus({{"FLICKER", 32768}})) == 2,
               "CTRL texture animation should use fractional frame selection");
        expect(renderer::compute_anim_frame(
                       material, 0, 0, names, ctrl_bus({{"FLICKER", 65535}})) == 3,
               "CTRL texture animation should retain the pre-endpoint frame");
        expect(renderer::compute_anim_frame(
                       material, 0, 0, names, ctrl_bus({{"FLICKER", 65536}})) == 3,
               "CTRL texture animation should clamp the exact endpoint to the last frame");
        expect(renderer::compute_anim_frame(
                       material, 2, 0, names, ctrl_bus({{"FLICKER", 32768}})) == 1,
               "The optional frame-table bound should cap controlled animation");
        expect(renderer::compute_anim_frame(
                       material, 0, 0, names, ctrl_bus({{"FLICKER", -32768}})) == -2,
               "CTRL texture animation should preserve retail signed SAR math");
        expect(renderer::compute_anim_frame(
                       material, 0, 0, {"LOD_FADE_IN", "MODEL_PRIVATE_FRAME"},
                       ctrl_bus({{"lod_frac", 32768}})) == 2,
               "Unknown authored texture CTRL names should alias lowercase LOD_FRAC");

        material.animation.cycle_frame_time = 255;
        expect(renderer::compute_anim_frame(
                       material, 0, 0, {}, ctrl_bus({{"LOD_FRAC", 32768}})) == 2,
               "An out-of-range local texture CTRL index aliases LOD_FRAC");

        material.animation.animation_type = 0;
        material.animation.cycle_frame_time = 100;
        expect(renderer::compute_anim_frame(
                       material, 2, 250, {}, ctrl_bus({})) == 0,
               "The optional frame-table bound should cap timed animation too");
    }

    return 0;
}
