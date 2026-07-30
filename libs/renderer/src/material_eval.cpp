#include "renderer/material_eval.h"

#include "threedi/threedi_ctrl_catalog.h"
#include "threedi/threedi_panm_runtime.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace renderer {
namespace {

static uint8_t localCtrlOrdinal(
        int idx, const std::vector<std::string>& ctrl_names) {
    if (idx < 0 || idx >= static_cast<int>(ctrl_names.size())) {
        return THREEDI_CTRL_LOD_FRAC;
    }
    return threedi_ctrl_register_loader_ordinal(
            ctrl_names[static_cast<size_t>(idx)].c_str());
}

static int32_t regValue(int idx,
                        const std::vector<std::string>& ctrl_names,
                        const ControlRegisterValues& ctrl_bus) {
    return ctrl_bus[localCtrlOrdinal(idx, ctrl_names)];
}

static int64_t roundNearest(double value) {
    return value >= 0.0 ? static_cast<int64_t>(value + 0.5)
                        : static_cast<int64_t>(value - 0.5);
}

static uint8_t quantizeByte(float value, float scale) {
    const int64_t raw = roundNearest(static_cast<double>(value) * scale);
    return static_cast<uint8_t>(std::clamp<int64_t>(raw, 0, 255));
}

static int16_t quantizeS16(float value, float scale) {
    const int64_t raw = roundNearest(static_cast<double>(value) * scale);
    // Preview the packed file value, including the writer's low-word wrap for
    // out-of-range editor inputs. Clamping here made the live preview disagree
    // with the exported .3di and with retail's signed-word consumer.
    // [orig: packed s16 generator fields; OpenNova writer
    //  buffer_append_scaled_s16 in threedi_3di3_write.cpp]
    const uint16_t bits = static_cast<uint16_t>(raw);
    return bits <= static_cast<uint16_t>(std::numeric_limits<int16_t>::max())
            ? static_cast<int16_t>(bits)
            : static_cast<int16_t>(static_cast<int32_t>(bits) - 0x10000);
}

// The loader rewrites the packed parameter byte for every style > 0x70,
// even when a later consumer interprets that style as a waveform and uses
// the byte as phase rather than reading the CTRL bus.
// [orig: sub_5B4640 @ 0x5B4640]
static uint8_t phaseOrRegisterByte(
        uint8_t style,
        float phase,
        int32_t reg,
        const std::vector<std::string>& ctrl_names) {
    return style <= 112 ? quantizeByte(phase, 256.0f)
                        : localCtrlOrdinal(reg, ctrl_names);
}

static UvAnimChannel rawUvChannel(
        const ThreediUvParams& params,
        const std::vector<std::string>& ctrl_names) {
    UvAnimChannel channel;
    channel.type = params.style;
    channel.phase = phaseOrRegisterByte(
            params.style, params.phase, params.reg, ctrl_names);
    channel.speed = quantizeS16(params.gen_rate, 256.0f);
    channel.base = quantizeS16(params.start, 256.0f);
    channel.range = quantizeS16(params.end, 256.0f);
    return channel;
}

static bool uvChannelUsesNoise(const UvAnimChannel& channel) {
    const uint8_t mode = channel.type & 0xF0;
    return channel.type <= 0x70 && (channel.type & 0x0F) == 6 &&
           mode != 0 && mode != 0x10 && mode != 0x20;
}

static uint16_t timeUnits16(uint32_t time_ms) {
    return static_cast<uint16_t>((time_ms << 8) / 1000u);
}

static uint16_t phase16(uint8_t phase_byte, int16_t rate_word, uint32_t time_ms) {
    const uint32_t sum =
            (static_cast<uint32_t>(phase_byte) << 8) +
            static_cast<uint32_t>(timeUnits16(time_ms)) *
                    static_cast<uint32_t>(static_cast<uint16_t>(rate_word));
    return static_cast<uint16_t>(sum);
}

// Retail keeps only IMUL's low 32 bits, then performs an arithmetic SAR 16.
// Spell out both the wrap and signed shift so the result is portable.
// [orig: AlphaGen_EvaluateValue @ 0x5B234C; RgbGen_EvaluateColor @ 0x5B24AC]
static int32_t mulShift16(int32_t delta, int32_t fraction) {
    const uint32_t low_product =
            static_cast<uint32_t>(delta) * static_cast<uint32_t>(fraction);
    const int64_t product =
            low_product <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
                    ? static_cast<int64_t>(low_product)
                    : static_cast<int64_t>(low_product) - 0x100000000ll;
    if (product >= 0) {
        return static_cast<int32_t>(product / 65536);
    }
    return -static_cast<int32_t>((-product + 65535) / 65536);
}

static int32_t waveformFraction(uint8_t style,
                                uint8_t phase_byte,
                                int16_t rate_word,
                                uint32_t time_ms) {
    const uint16_t random =
            (style & 0x0F) == 6 ? threedi_wave_rand15() : 0;
    return uv_anim_wave_lookup(style, phase16(phase_byte, rate_word, time_ms), random);
}

static void evalRgbGen(uint8_t style,
                       uint8_t phase_byte,
                       int16_t rate_word,
                       const std::array<uint8_t, 3>& start,
                       const std::array<uint8_t, 3>& end,
                       uint32_t time_ms,
                       int32_t ctrl_value,
                       float* out_r,
                       float* out_g,
                       float* out_b) {
    // [orig: RgbGen_EvaluateColor @ 0x5B23D0]
    constexpr float kByteToFloat = 1.0f / 255.0f;
    const int32_t fraction =
            style == 24
                    ? 0
                    : ((style == 113 || style == 114)
                               ? ctrl_value
                               : waveformFraction(style, phase_byte, rate_word, time_ms));
    *out_r = static_cast<float>(
                     start[0] + mulShift16(end[0] - start[0], fraction)) *
             kByteToFloat;
    *out_g = static_cast<float>(
                     start[1] + mulShift16(end[1] - start[1], fraction)) *
             kByteToFloat;
    *out_b = static_cast<float>(
                     start[2] + mulShift16(end[2] - start[2], fraction)) *
             kByteToFloat;
}

} // namespace

MaterialRuntime eval_material_runtime(const ThreediMaterial& mat,
                                      uint32_t time_ms,
                                      const std::vector<std::string>& ctrl_names,
                                      const ControlRegisterValues& ctrl_values) {
    MaterialRuntime rt;
    // Retail patches every model-local material parameter to one of the 96
    // global CTRL slots during load. Authored unknown/missing local references
    // inherit the loader's ordinal-zero result.
    // [orig: sub_5B4640 @ 0x5B4640; CtrlName_ToOrdinal @ 0x57B290]
    const ControlRegisterValues& ctrl_bus = ctrl_values;

    if (mat.alpha_gen.style == 0) {
        rt.alpha = 1.0f;
    } else {
        // [orig: AlphaGen_EvaluateValue @ 0x5B2320]
        const int32_t ctrl = regValue(mat.alpha_gen.reg, ctrl_names, ctrl_bus);
        const int32_t fraction =
                mat.alpha_gen.style == 24
                        ? 0
                        : (mat.alpha_gen.style == 113
                                   ? ctrl
                                   : waveformFraction(
                                             mat.alpha_gen.style,
                                             phaseOrRegisterByte(
                                                     mat.alpha_gen.style,
                                                     mat.alpha_gen.phase,
                                                     mat.alpha_gen.reg,
                                                     ctrl_names),
                                             quantizeS16(mat.alpha_gen.rate, 256.0f),
                                             time_ms));
        const int32_t value =
                mat.alpha_gen.start +
                mulShift16(mat.alpha_gen.end - mat.alpha_gen.start, fraction);
        rt.alpha = static_cast<float>(value) * (1.0f / 255.0f);
    }

    if (mat.rgb_gen.style == 0) {
        rt.rgb_r = rt.rgb_g = rt.rgb_b = 1.0f;
    } else {
        const int32_t ctrl = regValue(mat.rgb_gen.reg, ctrl_names, ctrl_bus);
        const std::array<uint8_t, 3> start = {
            quantizeByte(mat.rgb_gen.start_color[0], 255.0f),
            quantizeByte(mat.rgb_gen.start_color[1], 255.0f),
            quantizeByte(mat.rgb_gen.start_color[2], 255.0f),
        };
        const std::array<uint8_t, 3> end = {
            quantizeByte(mat.rgb_gen.end_color[0], 255.0f),
            quantizeByte(mat.rgb_gen.end_color[1], 255.0f),
            quantizeByte(mat.rgb_gen.end_color[2], 255.0f),
        };
        evalRgbGen(
                mat.rgb_gen.style,
                phaseOrRegisterByte(
                        mat.rgb_gen.style,
                        mat.rgb_gen.phase,
                        mat.rgb_gen.reg,
                        ctrl_names),
                quantizeS16(mat.rgb_gen.rate, 256.0f),
                start,
                end,
                time_ms,
                ctrl,
                &rt.rgb_r,
                &rt.rgb_g,
                &rt.rgb_b);
    }

    // Retail evaluates AlphaGen, RgbGen, then UV in this order. Preserve that
    // ordering because noise waveforms share the CRT random stream.
    // [orig: apply_shader_parameters @ 0x58DB80]
    const UvAnimChannel u_channel = rawUvChannel(mat.u_params, ctrl_names);
    const UvAnimChannel v_channel = rawUvChannel(mat.v_params, ctrl_names);
    const bool u_uses_noise = uvChannelUsesNoise(u_channel);
    const bool v_uses_noise = uvChannelUsesNoise(v_channel);
    // Function-argument evaluation order is not portable. Consume the shared
    // CRT stream explicitly in retail's U-then-V order before dispatch.
    const uint16_t u_random =
            u_uses_noise ? threedi_wave_rand15() : 0;
    const uint16_t v_random =
            v_uses_noise ? threedi_wave_rand15() : 0;
    // The decoder exposes author-friendly floats; retail evaluates the packed
    // 8-byte channel blocks. Reconstruct those raw fields before entering the
    // exact transform port. [orig: compute_uv_transform_matrix @ 0x5B1990]
    rt.uv = uv_anim_transform(
            u_channel,
            v_channel,
            timeUnits16(time_ms),
            regValue(mat.u_params.reg, ctrl_names, ctrl_bus),
            regValue(mat.v_params.reg, ctrl_names, ctrl_bus),
            u_random,
            v_random);

    return rt;
}

LightRuntime eval_light_runtime(uint8_t style,
                                uint8_t phase_byte,
                                uint16_t rate_word,
                                const std::array<uint8_t, 4>& color_start,
                                const std::array<uint8_t, 4>& color_end,
                                uint32_t time_ms,
                                int32_t ctrl_value) {
    LightRuntime rt;
    const float sb = static_cast<float>(color_start[0]);
    const float sg = static_cast<float>(color_start[1]);
    const float sr = static_cast<float>(color_start[2]);

    if (style == 0) {
        // An absent Light RgbGen leaves the light's static packed color.
        rt.r = sr / 255.0f;
        rt.g = sg / 255.0f;
        rt.b = sb / 255.0f;
        return rt;
    }

    const std::array<uint8_t, 3> start = {
        color_start[2], color_start[1], color_start[0],
    };
    const std::array<uint8_t, 3> end = {
        color_end[2], color_end[1], color_end[0],
    };
    evalRgbGen(
            style,
            phase_byte,
            static_cast<int16_t>(rate_word),
            start,
            end,
            time_ms,
            ctrl_value,
            &rt.r,
            &rt.g,
            &rt.b);
    return rt;
}

int compute_anim_frame(const ThreediMaterial& mat,
                       uint32_t max_anim_frames,
                       uint32_t time_ms,
                       const std::vector<std::string>& ctrl_names,
                       const ControlRegisterValues& ctrl_values) {
    const ControlRegisterValues& ctrl_bus = ctrl_values;
    const uint8_t nframes = mat.animation.num_frames;
    if (nframes <= 1) return 0;
    const uint32_t bounded_count = max_anim_frames == 0
            ? static_cast<uint32_t>(nframes)
            : std::min(static_cast<uint32_t>(nframes), max_anim_frames);
    if (bounded_count <= 1) return 0;
    const int frame_count = static_cast<int>(bounded_count);

    if (mat.animation.animation_type == 0) {
        int frame_ms = static_cast<int>(mat.animation.cycle_frame_time);
        if (frame_ms <= 0) frame_ms = 100;
        return (time_ms / static_cast<uint32_t>(frame_ms)) % frame_count;
    }

    const int reg_index = static_cast<int>(mat.animation.cycle_frame_time);
    const int32_t ctrl = regValue(reg_index, ctrl_names, ctrl_bus);
    // The odd dword in retail's 8-byte CTRL slot selects an alternate modulo
    // mode, but has no writer in Joint Operations. The live path is therefore
    // always the signed low-dword IMUL/SAR fractional-frame branch.
    // [orig: apply_shader_parameters @ 0x58DB80]
    int frame = mulShift16(frame_count, ctrl);
    if (frame >= frame_count) frame = frame_count - 1;
    return frame;
}

} // namespace renderer
