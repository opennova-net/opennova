#include "renderer/material_eval.h"
#include "threedi/threedi_panm.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace renderer {
namespace {

static uint32_t g_rand_state = 1;

static float clamp01(float v) {
    return std::max(0.0f, std::min(1.0f, v));
}

static uint16_t msvcRand15() {
    g_rand_state = g_rand_state * 214013u + 2531011u;
    return static_cast<uint16_t>((g_rand_state >> 16) & 0x7FFFu);
}

static int32_t waveLookup(uint8_t func, int16_t a3) {
    const uint8_t* table = threedi_panm_wave_table();
    const uint8_t idx = static_cast<uint8_t>(a3 >> 8);
    const uint8_t frac = static_cast<uint8_t>(a3);
    switch (func & 0x0F) {
        case 1: return static_cast<int32_t>(table[idx]) << 8;
        case 2: return static_cast<int32_t>(table[256 + idx]) << 8;
        case 3: return static_cast<int32_t>(table[768 + idx]) << 8;
        case 4: return static_cast<int32_t>(table[1024 + idx]) << 8;
        case 5: return static_cast<int32_t>(table[1280 + idx]) << 8;
        case 6: return 16 * static_cast<int32_t>(msvcRand15() & 0x0FFF);
        case 7: {
            const int32_t v0 = table[1536 + idx];
            const int32_t v1 = table[1536 + static_cast<uint8_t>(idx + 1)];
            return (v1 - v0) * frac + (v0 << 8);
        }
        case 8: return static_cast<int32_t>(table[1792 + idx]) << 8;
        case 9: return static_cast<int32_t>(table[2048 + idx]) << 8;
        case 0xA: {
            const int32_t v0 = table[2304 + idx];
            const int32_t v1 = table[2304 + static_cast<uint8_t>(idx + 1)];
            return (v1 - v0) * frac + (v0 << 8);
        }
        case 0xF: return static_cast<int32_t>(table[2560 + idx]) << 8;
        default: return 0;
    }
}

static float styleSample01(int style, float phase, float rate, uint32_t time_ms) {
    // Original OED stores phase as an 8-bit byte and passes phase_byte << 8
    // into EvaluateWaveformLUT. ThreediMaterial keeps that byte normalized to
    // [0, 1), so convert it back to the original fixed-point form here.
    const int32_t phase_fp8 = static_cast<int32_t>(phase * 65536.0f);
    const int32_t rate_fp8 = static_cast<int32_t>(rate * 256.0f);
    // Original OED reference: EvaluateAnimParam @ 0x4769D0 passes
    // (phase << 8) + ((ticks << 8) / 1000) * rate_word directly to
    // EvaluateWaveformLUT without shifting the 8.8 rate product back down.
    const int32_t a3 = phase_fp8 + static_cast<int32_t>(((time_ms << 8) / 1000u) * rate_fp8);
    const int32_t wave_fp8 = waveLookup(static_cast<uint8_t>(style), static_cast<int16_t>(a3));
    return clamp01(static_cast<float>(wave_fp8) / 65535.0f);
}

static float ctrl01(uint16_t v) {
    return static_cast<float>(v) * (1.0f / 65535.0f);
}

static float ctrlSigned(uint16_t raw) {
    const int16_t v = static_cast<int16_t>(raw);
    if (v >= 0) return std::min(1.0f, static_cast<float>(v) / 32767.0f);
    return std::max(-1.0f, static_cast<float>(v) / 32768.0f);
}

static float evalStyleScalar(int style,
                             float rate,
                             float phase,
                             float start,
                             float end,
                             uint16_t ctrl,
                             uint32_t time_ms) {
    if (style == 0) return start;
    if (style == 24) return start;

    const float time_sec = static_cast<float>(time_ms) * 0.001f;
    if (style == 16) return start + (phase + time_sec * rate);
    if (style == 17) return start - (phase + time_sec * rate);

    float w = 0.0f;
    const bool ctrl_style = (style > 112);
    int op = 0;

    if (ctrl_style) {
        const int ctrl_op = style - 112;
        op = std::max(1, std::min(ctrl_op, 5));
        w = (op == 3 || op == 5) ? ctrlSigned(ctrl) : ctrl01(ctrl);
    } else {
        const int major = (style >> 4) & 0xF;
        if (major >= 3 && major <= 6) {
            op = major - 2;
            w = styleSample01(style, phase, rate, time_ms);
        }
    }

    const float delta = end - start;
    switch (op) {
        case 1: return start + delta * w;
        case 2: return start + delta * w;
        case 3: return start + delta * w;
        case 4: {
            const float base = (std::fabs(start) > 0.00001f) ? start : 1.0f;
            return base * (1.0f + delta * w);
        }
        case 5: return start + delta * w;
        default: return start;
    }
}

static void evalMapAxis(const ThreediUvParams& p,
                        uint16_t ctrl,
                        uint32_t time_ms,
                        float* out_offset,
                        float* out_scale,
                        float* out_rotation) {
    if (out_offset) *out_offset = 0.0f;
    if (out_scale) *out_scale = 1.0f;
    if (out_rotation) *out_rotation = 0.0f;

    const int style = p.style;
    if (style == 0) return;

    if (style == 32 || style == 33) {
        const float dir = (style == 32) ? -1.0f : 1.0f;
        const float turns = p.phase + static_cast<float>(time_ms) * 0.001f * p.gen_rate;
        if (out_rotation) *out_rotation = dir * turns * 6.28318530718f;
        return;
    }

    if (style == 117) {
        const float deg = evalStyleScalar(style, p.gen_rate, p.phase, p.start, p.end, ctrl, time_ms);
        if (out_rotation) *out_rotation = deg * 0.01745329251994f;
        return;
    }

    const float value = evalStyleScalar(style, p.gen_rate, p.phase, p.start, p.end, ctrl, time_ms);
    if (style == 16 || style == 17 || ((style >> 4) == 3) || ((style >> 4) == 4) ||
        ((style > 112) && style != 115 && style != 116 && style != 117)) {
        if (out_offset) *out_offset = value;
        return;
    }

    if (((style >> 4) == 5) || style == 115) {
        if (out_offset) *out_offset = value * 0.5f;
        return;
    }

    if (((style >> 4) == 6) || style == 116) {
        if (out_scale) *out_scale = (std::fabs(value) > 0.0001f) ? value : 1.0f;
        return;
    }

    if (out_offset) *out_offset = value;
}

static uint16_t regValue(int idx,
                         const std::vector<std::string>& ctrl_names,
                         const std::unordered_map<std::string, uint16_t>& ctrl_values) {
    if (idx < 0 || idx >= static_cast<int>(ctrl_names.size())) return 0;
    auto it = ctrl_values.find(ctrl_names[static_cast<size_t>(idx)]);
    return (it != ctrl_values.end()) ? it->second : 0;
}

} // namespace

MaterialRuntime eval_material_runtime(const ThreediMaterial& mat,
                                      uint32_t time_ms,
                                      const std::vector<std::string>& ctrl_names,
                                      const std::unordered_map<std::string, uint16_t>& ctrl_values) {
    MaterialRuntime rt;

    float u_off = 0.0f, u_scale = 1.0f, u_rot = 0.0f;
    float v_off = 0.0f, v_scale = 1.0f, v_rot = 0.0f;
    evalMapAxis(mat.u_params, regValue(mat.u_params.reg, ctrl_names, ctrl_values), time_ms, &u_off, &u_scale, &u_rot);
    evalMapAxis(mat.v_params, regValue(mat.v_params.reg, ctrl_names, ctrl_values), time_ms, &v_off, &v_scale, &v_rot);
    rt.uv.offset_u = u_off;
    rt.uv.offset_v = v_off;
    rt.uv.scale_u = u_scale;
    rt.uv.scale_v = v_scale;
    rt.uv.rotation = u_rot + v_rot;

    if (mat.rgb_gen.style == 0) {
        rt.rgb_r = rt.rgb_g = rt.rgb_b = 1.0f;
    } else {
        const uint16_t ctrl = regValue(mat.rgb_gen.reg, ctrl_names, ctrl_values);
        rt.rgb_r = std::max(0.0f, evalStyleScalar(mat.rgb_gen.style, mat.rgb_gen.rate, mat.rgb_gen.phase,
                                                  mat.rgb_gen.start_color[0], mat.rgb_gen.end_color[0],
                                                  ctrl, time_ms));
        rt.rgb_g = std::max(0.0f, evalStyleScalar(mat.rgb_gen.style, mat.rgb_gen.rate, mat.rgb_gen.phase,
                                                  mat.rgb_gen.start_color[1], mat.rgb_gen.end_color[1],
                                                  ctrl, time_ms));
        rt.rgb_b = std::max(0.0f, evalStyleScalar(mat.rgb_gen.style, mat.rgb_gen.rate, mat.rgb_gen.phase,
                                                  mat.rgb_gen.start_color[2], mat.rgb_gen.end_color[2],
                                                  ctrl, time_ms));
    }

    if (mat.alpha_gen.style == 0) {
        rt.alpha = 1.0f;
    } else {
        const uint16_t ctrl = regValue(mat.alpha_gen.reg, ctrl_names, ctrl_values);
        float a = evalStyleScalar(mat.alpha_gen.style, mat.alpha_gen.rate, mat.alpha_gen.phase,
                                  static_cast<float>(mat.alpha_gen.start),
                                  static_cast<float>(mat.alpha_gen.end),
                                  ctrl, time_ms);
        if (std::fabs(mat.alpha_gen.start) > 1.5f || std::fabs(mat.alpha_gen.end) > 1.5f) {
            a *= (1.0f / 255.0f);
        }
        rt.alpha = clamp01(a);
    }

    return rt;
}

LightRuntime eval_light_runtime(uint8_t style,
                                uint8_t phase_byte,
                                uint16_t rate_word,
                                const std::array<uint8_t, 4>& color_start,
                                const std::array<uint8_t, 4>& color_end,
                                uint32_t time_ms,
                                uint16_t ctrl_value) {
    LightRuntime rt;
    const float sb = static_cast<float>(color_start[0]);
    const float sg = static_cast<float>(color_start[1]);
    const float sr = static_cast<float>(color_start[2]);
    const float eb = static_cast<float>(color_end[0]);
    const float eg = static_cast<float>(color_end[1]);
    const float er = static_cast<float>(color_end[2]);

    if (style == 0) {
        rt.r = sr / 255.0f;
        rt.g = sg / 255.0f;
        rt.b = sb / 255.0f;
        return rt;
    }

    float phase = static_cast<float>(phase_byte) * (1.0f / 256.0f);
    float rate = static_cast<float>(rate_word) * (1.0f / 256.0f);
    const float b = evalStyleScalar(style, rate, phase, sb, eb, ctrl_value, time_ms);
    const float g = evalStyleScalar(style, rate, phase, sg, eg, ctrl_value, time_ms);
    const float r = evalStyleScalar(style, rate, phase, sr, er, ctrl_value, time_ms);
    rt.r = clamp01(r / 255.0f);
    rt.g = clamp01(g / 255.0f);
    rt.b = clamp01(b / 255.0f);
    return rt;
}

int compute_anim_frame(const ThreediMaterial& mat,
                       uint32_t max_anim_frames,
                       uint32_t time_ms,
                       const std::vector<std::string>& ctrl_names,
                       const std::unordered_map<std::string, uint16_t>& ctrl_values) {
    if (max_anim_frames > 0 && max_anim_frames <= 1) return 0;
    const uint8_t nframes = mat.animation.num_frames;
    if (nframes <= 1) return 0;
    const int frame_count = std::max(1, static_cast<int>(nframes));

    if (mat.animation.animation_type == 0) {
        int frame_ms = static_cast<int>(mat.animation.cycle_frame_time);
        if (frame_ms <= 0) frame_ms = 100;
        return (time_ms / static_cast<uint32_t>(frame_ms)) % frame_count;
    }

    const int reg_index = static_cast<int>(mat.animation.cycle_frame_time);
    const uint16_t ctrl = regValue(reg_index, ctrl_names, ctrl_values);
    int frame = static_cast<int>((static_cast<uint32_t>(ctrl) * static_cast<uint32_t>(frame_count)) / 65536u);
    if (frame < 0) frame = 0;
    if (frame >= frame_count) frame = frame_count - 1;
    return frame;
}

} // namespace renderer
