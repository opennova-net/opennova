#include "renderer/material_eval.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool nearly_equal(float a, float b, float epsilon = 0.0001f) {
    return std::fabs(a - b) <= epsilon;
}

renderer::MaterialRuntime eval_runtime(
        const ThreediMaterial& material,
        uint32_t time_ms,
        const std::vector<std::string>& ctrl_names = {},
        const std::unordered_map<std::string, uint16_t>& ctrl_values = {}) {
    return renderer::eval_material_runtime(material, time_ms, ctrl_names, ctrl_values);
}

renderer::MaterialRuntime eval_rgb_runtime(const ThreediMaterial& material,
                                           uint32_t time_ms) {
    return eval_runtime(material, time_ms);
}

}  // namespace

int main() {
    ThreediMaterial material{};

    {
        material.rgb_gen.style = 0;
        const renderer::MaterialRuntime runtime = eval_rgb_runtime(material, 750);
        expect(nearly_equal(runtime.rgb_r, 1.0f) &&
                   nearly_equal(runtime.rgb_g, 1.0f) &&
                   nearly_equal(runtime.rgb_b, 1.0f),
               "Style 0 RGB gen should leave the fixed-function color source at 1");
    }

    {
        material = {};
        material.rgb_gen.style = 24;
        material.rgb_gen.start_color[0] = 0.35f;
        material.rgb_gen.start_color[1] = 0.10f;
        material.rgb_gen.start_color[2] = 0.60f;
        material.rgb_gen.end_color[0] = 1.0f;
        material.rgb_gen.end_color[1] = 1.0f;
        material.rgb_gen.end_color[2] = 1.0f;

        const renderer::MaterialRuntime a = eval_rgb_runtime(material, 0);
        const renderer::MaterialRuntime b = eval_rgb_runtime(material, 4000);
        expect(nearly_equal(a.rgb_r, 0.35f) &&
                   nearly_equal(a.rgb_g, 0.10f) &&
                   nearly_equal(a.rgb_b, 0.60f),
               "Style 24 RGB gen should stay at its start color");
        expect(nearly_equal(a.rgb_r, b.rgb_r) &&
                   nearly_equal(a.rgb_g, b.rgb_g) &&
                   nearly_equal(a.rgb_b, b.rgb_b),
               "Style 24 RGB gen should remain constant over time");
    }

    {
        material = {};
        material.rgb_gen.style = 50;
        material.rgb_gen.rate = 0.75f;
        material.rgb_gen.start_color[0] = 100.0f / 255.0f;
        material.rgb_gen.end_color[0] = 0.0f;

        const renderer::MaterialRuntime a = eval_rgb_runtime(material, 0);
        const renderer::MaterialRuntime b = eval_rgb_runtime(material, 700);
        expect(!nearly_equal(a.rgb_r, b.rgb_r),
               "Waveform RGB gen style 50 should vary over time");
    }

    {
        material = {};
        material.rgb_gen.style = 50;
        material.rgb_gen.rate = 0.75f;
        material.rgb_gen.start_color[0] = 100.0f / 255.0f;
        material.rgb_gen.end_color[0] = 0.0f;

        ThreediMaterial phased = material;
        phased.rgb_gen.phase = 0.375f;

        const renderer::MaterialRuntime a = eval_rgb_runtime(material, 900);
        const renderer::MaterialRuntime b = eval_rgb_runtime(phased, 900);
        expect(!nearly_equal(a.rgb_r, b.rgb_r),
               "Waveform RGB gen phase offsets should produce different samples");
    }

    {
        material = {};
        material.u_params.style = 16;
        material.u_params.phase = 0.25f;
        material.u_params.gen_rate = 1.0f;

        const renderer::MaterialRuntime runtime = eval_runtime(material, 500);
        expect(nearly_equal(runtime.uv.offset_u, 0.75f),
               "UV style 16 should advance positive U offset by phase plus time * rate");
    }

    {
        material = {};
        material.v_params.style = 17;
        material.v_params.phase = 0.25f;
        material.v_params.gen_rate = 1.0f;

        const renderer::MaterialRuntime runtime = eval_runtime(material, 500);
        expect(nearly_equal(runtime.uv.offset_v, -0.75f),
               "UV style 17 should advance negative V offset by phase plus time * rate");
    }

    {
        material = {};
        material.u_params.style = 116;
        material.u_params.start = 1.0f;
        material.u_params.end = 3.0f;
        material.u_params.reg = 0;

        const renderer::MaterialRuntime runtime =
                eval_runtime(material, 0, {"uv"}, {{"uv", 32768}});
        expect(nearly_equal(runtime.uv.scale_u, 2.0f, 0.0002f),
               "UV style 116 should apply controlled scale from the mapped register");
    }

    {
        material = {};
        material.u_params.style = 32;
        material.u_params.phase = 0.25f;
        material.u_params.gen_rate = 0.5f;

        const renderer::MaterialRuntime runtime = eval_runtime(material, 500);
        expect(nearly_equal(runtime.uv.rotation, -3.14159265f),
               "UV style 32 should produce clockwise rotation in radians");
    }

    {
        material = {};
        material.u_params.style = 33;
        material.u_params.phase = 0.25f;
        material.u_params.gen_rate = 0.5f;

        const renderer::MaterialRuntime runtime = eval_runtime(material, 500);
        expect(nearly_equal(runtime.uv.rotation, 3.14159265f),
               "UV style 33 should produce counter-clockwise rotation in radians");
    }

    return 0;
}
