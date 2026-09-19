#include <runtime/renderer/material_eval.h>

#include <base/crt/crt_rng.h>
#include <formats/threedi/threedi_ctrl_catalog.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

using namespace opennova::crt;
using namespace opennova::threedi;

namespace opennova::renderer {
namespace {

uint8_t local_ctrl_ordinal(
        int idx, const std::vector<std::string>& ctrl_names) {
    if (idx < 0 || idx >= static_cast<int>(ctrl_names.size())) {
        return THREEDI_CTRL_LOD_FRAC;
    }
    return threedi_ctrl_register_loader_ordinal(
            ctrl_names[static_cast<size_t>(idx)].c_str());
}

int32_t reg_value(int idx,
                        const std::vector<std::string>& ctrl_names,
                        const ControlRegisterValues& ctrl_bus) {
    return ctrl_bus[local_ctrl_ordinal(idx, ctrl_names)];
}

bool ctrl_uses_discrete_frame_selector(uint8_t ordinal) {
    return ordinal >= THREEDI_CTRL_TEX_TEAM &&
           ordinal <= THREEDI_CTRL_TEX_CAMO3;
}

int64_t round_nearest(double value) {
    return value >= 0.0 ? static_cast<int64_t>(value + 0.5)
                        : static_cast<int64_t>(value - 0.5);
}

uint8_t quantize_byte(float value, float scale) {
    const int64_t raw = round_nearest(static_cast<double>(value) * scale);
    return static_cast<uint8_t>(std::clamp<int64_t>(raw, 0, 255));
}

int16_t quantize_s16(float value, float scale) {
    const int64_t raw = round_nearest(static_cast<double>(value) * scale);
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
// [orig: ThreediGp_LoadCtrlRegisters @ 0x5B4640]
uint8_t phase_or_register_byte(
        uint8_t style,
        float phase,
        int32_t reg,
        const std::vector<std::string>& ctrl_names) {
    return style <= 112 ? quantize_byte(phase, 256.0f)
                        : local_ctrl_ordinal(reg, ctrl_names);
}

UvAnimChannel raw_uv_channel(
        const ThreediUvParams& params,
        const std::vector<std::string>& ctrl_names) {
    UvAnimChannel channel;
    channel.type = params.style;
    channel.phase = phase_or_register_byte(
            params.style, params.phase, params.reg, ctrl_names);
    channel.speed = quantize_s16(params.gen_rate, 256.0f);
    channel.base = quantize_s16(params.start, 256.0f);
    channel.range = quantize_s16(params.end, 256.0f);
    return channel;
}

bool uv_channel_uses_noise(const UvAnimChannel& channel) {
    const uint8_t mode = channel.type & 0xF0;
    return channel.type <= 0x70 && (channel.type & 0x0F) == 6 &&
           mode != 0 && mode != 0x10 && mode != 0x20;
}

uint16_t time_units16(uint32_t time_ms) {
    return static_cast<uint16_t>((time_ms << 8) / 1000u);
}

uint16_t phase16(uint8_t phase_byte, int16_t rate_word, uint32_t time_ms) {
    const uint32_t sum =
            (static_cast<uint32_t>(phase_byte) << 8) +
            static_cast<uint32_t>(time_units16(time_ms)) *
                    static_cast<uint32_t>(static_cast<uint16_t>(rate_word));
    return static_cast<uint16_t>(sum);
}

// Retail keeps only IMUL's low 32 bits, then performs an arithmetic SAR 16.
// Spell out both the wrap and signed shift so the result is portable.
// [orig: AlphaGen_EvaluateValue @ 0x5B234C; RgbGen_EvaluateColor @ 0x5B24AC]
int32_t mul_shift16(int32_t delta, int32_t fraction) {
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

int32_t waveform_fraction(uint8_t style,
                                uint8_t phase_byte,
                                int16_t rate_word,
                                uint32_t time_ms) {
    const uint16_t random =
            (style & 0x0F) == 6 ? crt_rand15() : 0;
    return uv_anim_wave_lookup(style, phase16(phase_byte, rate_word, time_ms), random);
}

void eval_rgb_gen(uint8_t style,
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
                               : waveform_fraction(style, phase_byte, rate_word, time_ms));
    *out_r = static_cast<float>(
                     start[0] + mul_shift16(end[0] - start[0], fraction)) *
             kByteToFloat;
    *out_g = static_cast<float>(
                     start[1] + mul_shift16(end[1] - start[1], fraction)) *
             kByteToFloat;
    *out_b = static_cast<float>(
                     start[2] + mul_shift16(end[2] - start[2], fraction)) *
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
    // [orig: ThreediGp_LoadCtrlRegisters @ 0x5B4640; CtrlName_ToOrdinal @ 0x57B290]
    const ControlRegisterValues& ctrl_bus = ctrl_values;

    if (mat.alpha_gen.style == 0) {
        rt.alpha = 1.0f;
    } else {
        // [orig: AlphaGen_EvaluateValue @ 0x5B2320 — `if (style != 24 && style
        //  != 113) { waveform } return start`: style 113 ('q') is a CONSTANT
        //  start like 24, not the register-driven form RgbGen 113/114 use
        //  (@0x5B24AC); its sole caller @0x58DB80 uploads the value to constant
        //  223 with no 113 special case. The earlier port drove it from the
        //  CTRL register (jo-c cross-check 2026-09-10).]
        const int32_t ctrl = reg_value(mat.alpha_gen.reg, ctrl_names, ctrl_bus);
        (void)ctrl;
        const int32_t fraction =
                mat.alpha_gen.style == 24
                        ? 0
                        : (mat.alpha_gen.style == 113
                                   ? 0
                                   : waveform_fraction(
                                             mat.alpha_gen.style,
                                             phase_or_register_byte(
                                                     mat.alpha_gen.style,
                                                     mat.alpha_gen.phase,
                                                     mat.alpha_gen.reg,
                                                     ctrl_names),
                                             quantize_s16(mat.alpha_gen.rate, 256.0f),
                                             time_ms));
        const int32_t value =
                mat.alpha_gen.start +
                mul_shift16(mat.alpha_gen.end - mat.alpha_gen.start, fraction);
        rt.alpha = static_cast<float>(value) * (1.0f / 255.0f);
    }

    if (mat.rgb_gen.style == 0) {
        rt.rgb_r = rt.rgb_g = rt.rgb_b = 1.0f;
    } else {
        const int32_t ctrl = reg_value(mat.rgb_gen.reg, ctrl_names, ctrl_bus);
        const std::array<uint8_t, 3> start = {
            quantize_byte(mat.rgb_gen.start_color[0], 255.0f),
            quantize_byte(mat.rgb_gen.start_color[1], 255.0f),
            quantize_byte(mat.rgb_gen.start_color[2], 255.0f),
        };
        const std::array<uint8_t, 3> end = {
            quantize_byte(mat.rgb_gen.end_color[0], 255.0f),
            quantize_byte(mat.rgb_gen.end_color[1], 255.0f),
            quantize_byte(mat.rgb_gen.end_color[2], 255.0f),
        };
        eval_rgb_gen(
                mat.rgb_gen.style,
                phase_or_register_byte(
                        mat.rgb_gen.style,
                        mat.rgb_gen.phase,
                        mat.rgb_gen.reg,
                        ctrl_names),
                quantize_s16(mat.rgb_gen.rate, 256.0f),
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
    const UvAnimChannel u_channel = raw_uv_channel(mat.u_params, ctrl_names);
    const UvAnimChannel v_channel = raw_uv_channel(mat.v_params, ctrl_names);
    const bool u_uses_noise = uv_channel_uses_noise(u_channel);
    const bool v_uses_noise = uv_channel_uses_noise(v_channel);
    // Function-argument evaluation order is not portable. Consume the shared
    // CRT stream explicitly in retail's U-then-V order before dispatch.
    const uint16_t u_random =
            u_uses_noise ? crt_rand15() : 0;
    const uint16_t v_random =
            v_uses_noise ? crt_rand15() : 0;
    // The decoder exposes author-friendly floats; retail evaluates the packed
    // 8-byte channel blocks. Reconstruct those raw fields before entering the
    // exact transform port. [orig: compute_uv_transform_matrix @ 0x5B1990]
    rt.uv = uv_anim_transform(
            u_channel,
            v_channel,
            time_units16(time_ms),
            reg_value(mat.u_params.reg, ctrl_names, ctrl_bus),
            reg_value(mat.v_params.reg, ctrl_names, ctrl_bus),
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
    eval_rgb_gen(
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
    const uint8_t ctrl_ordinal = local_ctrl_ordinal(reg_index, ctrl_names);
    const int32_t ctrl = ctrl_bus[ctrl_ordinal];
    // TEX_TEAM and TEX_CAMO1/2/3 are discrete texture selectors; avatar CAMO
    // writers zero-extend their bytes while TEX_TEAM retains its signed input.
    // Their adjacent state dwords are statically 1, so retail takes the
    // modulo-frame branch instead of interpreting these values as signed 16.16.
    // [orig: Avatar_SetArmsCamoCtrl @ 0x57A3B0;
    //  dword_83FFCC/dword_83FFD4/dword_83FFDC/dword_83FFE4 = 1;
    //  apply_shader_parameters @ 0x58DC36..0x58DC42]
    if (ctrl_uses_discrete_frame_selector(ctrl_ordinal)) {
        return ctrl % frame_count;
    }
    // The odd dword in retail's 8-byte CTRL slot selects an alternate modulo
    // mode. Ordinary animation controls retain the signed low-dword IMUL/SAR
    // fractional-frame branch; the static texture-selector state is handled above.
    // [orig: apply_shader_parameters @ 0x58DB80]
    int frame = mul_shift16(frame_count, ctrl);
    if (frame >= frame_count) frame = frame_count - 1;
    return frame;
}

}  // namespace opennova::renderer