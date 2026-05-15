// 3DA project file reader (legacy ModSuperOED format).
//
// Mirrors df4oed.exe::sub_425730 — a token-driven `_stricmp` dispatch
// against `begin <section>`/`end <section>` block markers and per-section
// field tokens.  Populates a TdpProject's materials such that a
// subsequent `tdp_write_3da` produces byte-identical output (modulo the
// header timestamp line, which the writer regenerates from `time(0)`).
//
// Two-material multitex split (writer side: a single TDP material with a
// detail texture emits TWO 3DA `material` entries — one with
// `multitexture_flags 1`, one with `multitexture_flags 2`) is folded back
// here: when we see `multitexture_flags 2`, we treat the current 3DA
// material as a continuation of the previous TDP material's DETAIL slot
// rather than allocating a fresh material entry.

#include "tdp/tdp.h"
#include "tdp_internal.h"
#include "tdp/tdp_material.h"
#include "threedi/threedi_material_class.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// Per-block accumulator state
// ---------------------------------------------------------------------------

namespace {

struct Block3DA {
    enum class Kind {
        None,
        General,
        Material,
        PartAnimation,
        AmbientLight,
        DirectLight,
        Light,
        Unknown,  // unrecognized — silently skipped to `end <name>`
    };
    Kind kind = Kind::None;
    char name_token[64] = {};  // copy of the section name for matching `end <name>`
};

struct Mat3DAState {
    // Inputs collected from individual field tokens; classify at `end material`.
    uint32_t shader_type = 0;
    uint32_t render_attributes = 0;
    int      multitexture_flags = 0;
    uint32_t blending_mode = 0;
    uint32_t alpha_type = 0;
    uint8_t  use_alpha_pcx = 0;
    int      target_material_index = -1;  // TDP material slot we are populating
    bool     is_detail_continuation = false;  // multitexture_flags == 2
    char     green_texture[64] = {};  // captured separately to also act as
                                      // the DETAIL slot name on continuation.
    bool     have_blending_mode = false;
    bool     have_alpha_type = false;
};

}  // namespace

// ---------------------------------------------------------------------------
// Light helpers
// ---------------------------------------------------------------------------

static TdpLight *ensure_light_3da(TdpLod *lod, size_t idx) {
    if (idx >= lod->light_count) {
        size_t new_count = idx + 1;
        lod->lights = static_cast<TdpLight *>(
            std::realloc(lod->lights, new_count * sizeof(TdpLight)));
        for (size_t i = lod->light_count; i < new_count; ++i)
            std::memset(&lod->lights[i], 0, sizeof(TdpLight));
        lod->light_count = new_count;
    }
    return &lod->lights[idx];
}

static TdpPartAnim *ensure_part_anim_3da(TdpLod *lod, size_t idx) {
    if (idx >= lod->part_anim_count) {
        size_t new_count = idx + 1;
        lod->part_anims = static_cast<TdpPartAnim *>(
            std::realloc(lod->part_anims, new_count * sizeof(TdpPartAnim)));
        for (size_t i = lod->part_anim_count; i < new_count; ++i)
            std::memset(&lod->part_anims[i], 0, sizeof(TdpPartAnim));
        lod->part_anim_count = new_count;
    }
    return &lod->part_anims[idx];
}

// ---------------------------------------------------------------------------
// Material parsing helpers
// ---------------------------------------------------------------------------

// Apply collected material classifier inputs at end-of-material.
static void finalize_material_3da(TdpMaterial *mat, const Mat3DAState &st) {
    classify_from_bhd_shader_type(st.shader_type,
                                  st.render_attributes,
                                  st.use_alpha_pcx,
                                  st.blending_mode,
                                  st.alpha_type,
                                  &mat->classification);

    // Mirror material convenience flags from classification (parallel to
    // finalize_material_classification in tdp.cpp's 3DP reader).
    switch (mat->classification.blend_mode) {
    case THREEDI_CLASS_BLEND_OPAQUE:
        mat->blend_mode = TDP_BLEND_OPAQUE;
        break;
    case THREEDI_CLASS_BLEND_ALPHA:
        mat->blend_mode = TDP_BLEND_ALPHA;
        break;
    case THREEDI_CLASS_BLEND_ADDITIVE:
        mat->blend_mode = TDP_BLEND_ADD;
        break;
    }
    if (mat->classification.alpha_test)
        mat->flags |= TDP_MATERIAL_FLAG_ALPHA_TEST;
    if (mat->classification.alpha_test_invert)
        mat->flags |= TDP_MATERIAL_FLAG_ALPHA_INVERT;
    if (mat->classification.two_sided)
        mat->flags |= TDP_MATERIAL_FLAG_TWO_SIDED;
}

// Per-line dispatch for tokens inside a `begin material N ... end material`
// block.  `mat` is the 3DI3 model material being populated; `state` carries
// classifier inputs and continuation tracking.
static void parse_material_field(TdpProject *out,
                                  TdpMaterial *mat,
                                  Mat3DAState *state,
                                  const TdpTokens &t,
                                  const char *line) {
    const char *tag = tdp_tok_at(&t, 0);

    // ----- Identification -----
    if (tdp_iequals(tag, "name")) {
        // 3DA `name` is the green-texture filename; capture and route to
        // the diffuse slot (or detail slot, on continuation).
        char buf[64] = {};
        if (t.count > 1 && tdp_tok_at(&t, 1)[0])
            tdp_copy_str(buf, sizeof(buf), tdp_tok_at(&t, 1));
        else
            tdp_extract_quoted(line, buf, sizeof(buf));
        tdp_copy_str(state->green_texture, sizeof(state->green_texture), buf);
        // Don't write to a slot yet — green_texture line is followed by a
        // separate `green_texture` token in OED's emission, so wait for
        // that.  But not all 3DA writers emit both; if green_texture is
        // missing, fall through and write here.  To stay symmetric with
        // our writer, defer slot writes to green_texture handler below.
    } else if (tdp_iequals(tag, "description")) {
        char buf[80] = {};
        if (t.count > 1 && tdp_tok_at(&t, 1)[0])
            tdp_copy_str(buf, sizeof(buf), tdp_tok_at(&t, 1));
        else
            tdp_extract_quoted(line, buf, sizeof(buf));
        if (!state->is_detail_continuation)
            tdp_copy_str(mat->name, sizeof(mat->name), buf);
        // On detail continuation, the description is a duplicate of the
        // primary's display name — don't overwrite.
    }

    // ----- Bitfields routed through classify_from_bhd_shader_type -----
    else if (tdp_iequals(tag, "render_attributes")) {
        state->render_attributes = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "multitexture_flags")) {
        // Already handled at `begin material` — but double-record for
        // late-arriving emissions.
        state->multitexture_flags = tdp_to_int(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "physical_attributes")) {
        mat->pattrib = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "color_type")) {
        mat->color_type = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "alpha_type")) {
        state->alpha_type = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
        state->have_alpha_type = true;
    } else if (tdp_iequals(tag, "blending_mode")) {
        state->blending_mode = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
        state->have_blending_mode = true;
    } else if (tdp_iequals(tag, "use_alpha_pcx")) {
        state->use_alpha_pcx = static_cast<uint8_t>(tdp_to_int(tdp_tok_at(&t, 1)) & 0xff);
    } else if (tdp_iequals(tag, "shader_type")) {
        state->shader_type = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    }

    // ----- Texture path fields -----
    else if (tdp_iequals(tag, "green_texture")) {
        char buf[64] = {};
        if (t.count > 1 && tdp_tok_at(&t, 1)[0])
            tdp_copy_str(buf, sizeof(buf), tdp_tok_at(&t, 1));
        else
            tdp_extract_quoted(line, buf, sizeof(buf));
        tdp_copy_str(state->green_texture, sizeof(state->green_texture), buf);

        // Route to the appropriate material slot.
        uint8_t slot = state->is_detail_continuation
            ? TDP_TEX_SLOT_DETAIL
            : TDP_TEX_SLOT_DIFFUSE;
        if (buf[0]) {
            TdpMaterialTexture *tex = tdp_material_find_or_alloc_tex(
                mat, slot, /*frame=*/0, /*animated=*/false);
            if (tex) tdp_copy_str(tex->name, sizeof(tex->name), buf);
        }
    } else if (tdp_iequals(tag, "alpha_texture")) {
        // Informational — alpha_test handling in classify covers behavior.
        // No TDP material field; intentionally drop.
    }

    // ----- Per-channel BHD FFP material values -----
    else if (tdp_iequals(tag, "color_green")) {
        mat->color_green[0] = static_cast<uint8_t>(tdp_to_int(tdp_tok_at(&t, 1)));
        mat->color_green[1] = static_cast<uint8_t>(tdp_to_int(tdp_tok_at(&t, 2)));
        mat->color_green[2] = static_cast<uint8_t>(tdp_to_int(tdp_tok_at(&t, 3)));
    } else if (tdp_iequals(tag, "color_alpha")) {
        mat->color_alpha[0] = static_cast<uint8_t>(tdp_to_int(tdp_tok_at(&t, 1)));
        mat->color_alpha[1] = static_cast<uint8_t>(tdp_to_int(tdp_tok_at(&t, 2)));
        mat->color_alpha[2] = static_cast<uint8_t>(tdp_to_int(tdp_tok_at(&t, 3)));
    } else if (tdp_iequals(tag, "luminosity")) {
        mat->luminosity = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "transparency")) {
        mat->transparency = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "specular_intensity")) {
        mat->specular_intensity = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "specular_sharpness")) {
        mat->specular_sharpness = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    }

    // ----- Animation -----
    else if (tdp_iequals(tag, "anim_frames")) {
        int v = tdp_to_int(tdp_tok_at(&t, 1));
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        mat->animation.num_frames = static_cast<uint8_t>(v);
    } else if (tdp_iequals(tag, "anim_time")) {
        int v = tdp_to_int(tdp_tok_at(&t, 1));
        mat->animation.cycle_frame_time = static_cast<int16_t>(v);
    } else if (tdp_iequals(tag, "anim_sequence")) {
        mat->anim_sequence = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    }

    // ----- Alpha test -----
    else if (tdp_iequals(tag, "alpha_test")) {
        int v = tdp_to_int(tdp_tok_at(&t, 1));
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        mat->alpha_test_value_byte = static_cast<uint8_t>(v);
        mat->alpha_threshold = static_cast<float>(v) / 255.0f;
    }

    // ----- UV transform -----
    else if (tdp_iequals(tag, "u_offset")) {
        mat->u_offset = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "v_offset")) {
        mat->v_offset = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "u_tiling")) {
        mat->u_tiling = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "v_tiling")) {
        mat->v_tiling = tdp_to_float(tdp_tok_at(&t, 1));
    }

    // ----- mapfunc_u_* -----
    else if (tdp_iequals(tag, "mapfunc_u_style")) {
        int v = tdp_to_int(tdp_tok_at(&t, 1));
        mat->u_params.style = static_cast<uint8_t>(v & 0xff);
    } else if (tdp_iequals(tag, "mapfunc_u_rate")) {
        mat->u_params.gen_rate = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "mapfunc_u_start")) {
        mat->u_params.start = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "mapfunc_u_end")) {
        mat->u_params.end = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "mapfunc_u_phase")) {
        mat->u_params.phase = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "mapfunc_u_ctrlreg")) {
        char regname[32] = {};
        if (t.count > 1 && tdp_tok_at(&t, 1)[0])
            tdp_copy_str(regname, sizeof(regname), tdp_tok_at(&t, 1));
        else
            tdp_extract_quoted(line, regname, sizeof(regname));
        mat->u_params.reg = tdp_ctrlreg_intern(out, regname);
    }

    // ----- mapfunc_v_* -----
    else if (tdp_iequals(tag, "mapfunc_v_style")) {
        int v = tdp_to_int(tdp_tok_at(&t, 1));
        mat->v_params.style = static_cast<uint8_t>(v & 0xff);
    } else if (tdp_iequals(tag, "mapfunc_v_rate")) {
        mat->v_params.gen_rate = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "mapfunc_v_start")) {
        mat->v_params.start = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "mapfunc_v_end")) {
        mat->v_params.end = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "mapfunc_v_phase")) {
        mat->v_params.phase = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "mapfunc_v_ctrlreg")) {
        char regname[32] = {};
        if (t.count > 1 && tdp_tok_at(&t, 1)[0])
            tdp_copy_str(regname, sizeof(regname), tdp_tok_at(&t, 1));
        else
            tdp_extract_quoted(line, regname, sizeof(regname));
        mat->v_params.reg = tdp_ctrlreg_intern(out, regname);
    }

    // ----- rgbgen -----
    else if (tdp_iequals(tag, "rgbgen_style")) {
        int v = tdp_to_int(tdp_tok_at(&t, 1));
        mat->rgb_gen.style = static_cast<uint8_t>(v & 0xff);
    } else if (tdp_iequals(tag, "rgbgen_rate")) {
        mat->rgb_gen.rate = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "rgbgen_phase")) {
        mat->rgb_gen.phase = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "rgbgen_sr")) {
        mat->rgb_gen.start_color[0] = tdp_byte_to_unit(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "rgbgen_sg")) {
        mat->rgb_gen.start_color[1] = tdp_byte_to_unit(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "rgbgen_sb")) {
        mat->rgb_gen.start_color[2] = tdp_byte_to_unit(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "rgbgen_er")) {
        mat->rgb_gen.end_color[0] = tdp_byte_to_unit(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "rgbgen_eg")) {
        mat->rgb_gen.end_color[1] = tdp_byte_to_unit(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "rgbgen_eb")) {
        mat->rgb_gen.end_color[2] = tdp_byte_to_unit(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "rgbgen_ctrlreg")) {
        char regname[32] = {};
        if (t.count > 1 && tdp_tok_at(&t, 1)[0])
            tdp_copy_str(regname, sizeof(regname), tdp_tok_at(&t, 1));
        else
            tdp_extract_quoted(line, regname, sizeof(regname));
        mat->rgb_gen.reg = tdp_ctrlreg_intern(out, regname);
    }

    // ----- alphagen -----
    else if (tdp_iequals(tag, "alphagen_style")) {
        int v = tdp_to_int(tdp_tok_at(&t, 1));
        mat->alpha_gen.style = static_cast<uint8_t>(v & 0xff);
    } else if (tdp_iequals(tag, "alphagen_rate")) {
        mat->alpha_gen.rate = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "alphagen_phase")) {
        mat->alpha_gen.phase = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "alphagen_start")) {
        mat->alpha_gen.start = static_cast<int16_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "alphagen_end")) {
        mat->alpha_gen.end = static_cast<int16_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "alphagen_ctrlreg")) {
        char regname[32] = {};
        if (t.count > 1 && tdp_tok_at(&t, 1)[0])
            tdp_copy_str(regname, sizeof(regname), tdp_tok_at(&t, 1));
        else
            tdp_extract_quoted(line, regname, sizeof(regname));
        mat->alpha_gen.reg = tdp_ctrlreg_intern(out, regname);
    }

    // ----- Reflection -----
    else if (tdp_iequals(tag, "reflect_r")) {
        // Writer emits `reflect_r` from clamp_255(reflect_color[2]) — index
        // 2 in BGRA is R.  Reverse: byte_to_unit → reflect_color[2].
        mat->reflect_color[2] = tdp_byte_to_unit(tdp_to_int(tdp_tok_at(&t, 1)));
        mat->reflect_color[3] = (mat->reflect_color[3] != 0.0f)
                                 ? mat->reflect_color[3] : 0.0f;
    } else if (tdp_iequals(tag, "reflect_g")) {
        mat->reflect_color[1] = tdp_byte_to_unit(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "reflect_b")) {
        mat->reflect_color[0] = tdp_byte_to_unit(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "reflect_type")) {
        mat->reflect_type = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "reflect_alpha")) {
        mat->reflect_alpha = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    }

    // ----- Action plane / projector -----
    else if (tdp_iequals(tag, "actionplane_type")) {
        mat->actionplane_type = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "projector_type")) {
        mat->projector_type = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "projector_no_receive")) {
        mat->projector_no_receive = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "projector_yaw")) {
        mat->projector_yaw = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    } else if (tdp_iequals(tag, "projector_pitch")) {
        mat->projector_pitch = static_cast<uint32_t>(tdp_to_int(tdp_tok_at(&t, 1)));
    }
}

// ---------------------------------------------------------------------------
// Axis func helper for part_animation blocks
// ---------------------------------------------------------------------------

static void load_axis_3da(TdpAxisFunc *axis, const TdpTokens *t, int base) {
    axis->func_id = tdp_to_int(tdp_tok_at(t, base));
    axis->param2  = tdp_to_float(tdp_tok_at(t, base + 1));  // start
    axis->param3  = tdp_to_float(tdp_tok_at(t, base + 2));  // end
    axis->param0  = tdp_to_float(tdp_tok_at(t, base + 3));  // rate
    axis->param1  = tdp_to_float(tdp_tok_at(t, base + 4));  // phase
    tdp_copy_str(axis->ctrl_reg, sizeof(axis->ctrl_reg), tdp_tok_at(t, base + 5));
}

// Per-line dispatch for part_animation N block.
static void parse_part_anim_field(TdpPartAnim *pa,
                                   const TdpTokens &t) {
    const char *tag = tdp_tok_at(&t, 0);
    if (tdp_iequals(tag, "rotate_type")) {
        pa->rotate_type = tdp_to_int(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "scale_type")) {
        pa->scale_type = tdp_to_int(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "transform_as")) {
        pa->transform_as = tdp_to_int(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "yaw_rate")) {
        pa->yaw_rate = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "pitch_rate")) {
        pa->pitch_rate = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "roll_rate")) {
        pa->roll_rate = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "yaw_func")) {
        load_axis_3da(&pa->yaw, &t, 1);
    } else if (tdp_iequals(tag, "pitch_func")) {
        load_axis_3da(&pa->pitch, &t, 1);
    } else if (tdp_iequals(tag, "roll_func")) {
        load_axis_3da(&pa->roll, &t, 1);
    } else if (tdp_iequals(tag, "reverserotate")) {
        pa->reverse_rotate = tdp_to_int(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "scale_func")) {
        load_axis_3da(&pa->scale, &t, 1);
    } else if (tdp_iequals(tag, "scalex_func")) {
        load_axis_3da(&pa->scale_x, &t, 1);
    } else if (tdp_iequals(tag, "scaley_func")) {
        load_axis_3da(&pa->scale_y, &t, 1);
    } else if (tdp_iequals(tag, "scalez_func")) {
        load_axis_3da(&pa->scale_z, &t, 1);
    }
}

// Per-line dispatch for `light` block (also used for ambient/direct light).
static void parse_light_field(TdpLight *lt, const TdpTokens &t, const char *line) {
    const char *tag = tdp_tok_at(&t, 0);
    if (tdp_iequals(tag, "name")) {
        if (t.count > 1 && tdp_tok_at(&t, 1)[0])
            tdp_copy_str(lt->name, sizeof(lt->name), tdp_tok_at(&t, 1));
        else
            tdp_extract_quoted(line, lt->name, sizeof(lt->name));
    } else if (tdp_iequals(tag, "colorgen_style") || tdp_iequals(tag, "style")) {
        lt->colorgen_style = tdp_to_int(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "colorgen_rate") || tdp_iequals(tag, "rate")) {
        lt->colorgen_rate = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "colorgen_phase") || tdp_iequals(tag, "phase")) {
        lt->colorgen_phase = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "colorgen_start") || tdp_iequals(tag, "start")) {
        lt->colorgen_start[0] = tdp_to_int(tdp_tok_at(&t, 1));
        lt->colorgen_start[1] = tdp_to_int(tdp_tok_at(&t, 2));
        lt->colorgen_start[2] = tdp_to_int(tdp_tok_at(&t, 3));
    } else if (tdp_iequals(tag, "colorgen_end") || tdp_iequals(tag, "end")) {
        lt->colorgen_end[0] = tdp_to_int(tdp_tok_at(&t, 1));
        lt->colorgen_end[1] = tdp_to_int(tdp_tok_at(&t, 2));
        lt->colorgen_end[2] = tdp_to_int(tdp_tok_at(&t, 3));
    } else if (tdp_iequals(tag, "colorgen_ctrlreg")) {
        if (t.count > 1 && tdp_tok_at(&t, 1)[0])
            tdp_copy_str(lt->colorgen_ctrlreg, sizeof(lt->colorgen_ctrlreg),
                     tdp_tok_at(&t, 1));
        else
            tdp_extract_quoted(line, lt->colorgen_ctrlreg,
                           sizeof(lt->colorgen_ctrlreg));
    }
}

// ---------------------------------------------------------------------------
// general_information section dispatch
// ---------------------------------------------------------------------------

static void parse_general_info_field(TdpProject *out, const TdpTokens &t,
                                      const char *line) {
    const char *tag = tdp_tok_at(&t, 0);
    TdpLod *lod0 = &out->lods[0];
    if (tdp_iequals(tag, "3da_version")) {
        out->version = tdp_to_int(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "attributes:") || tdp_iequals(tag, "attributes")) {
        // Writer emits "attributes: %d"; the colon is captured as part of
        // the token in our tokenizer — match both spellings to be safe.
        lod0->attributes = tdp_to_int(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "render_function")) {
        tdp_copy_str(lod0->render_function, sizeof(lod0->render_function),
                 tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "threshold")) {
        lod0->threshold = tdp_to_float(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "part_anim_enable")) {
        lod0->part_anim_enabled = tdp_to_int(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "polycollision")) {
        out->poly_collision_lod = tdp_to_int(tdp_tok_at(&t, 1));
    } else if (tdp_iequals(tag, "username")) {
        tdp_copy_str(out->username, sizeof(out->username), tdp_tok_at(&t, 1));
    }
    // Other fields (3di_version, num_materials, scale_factor, diffuse_set,
    // pm_enable, pm_polythresh, fakeskin_z) are informational — no TDP material field
    // home, deliberately ignored.  num_materials is verified at emission
    // time by the writer rather than enforced on read.
    (void)line;
}

// ---------------------------------------------------------------------------
// Top-level reader
// ---------------------------------------------------------------------------

int tdp_read_3da(const char *path, TdpProject *out) {
    if (!path || !out) return -1;
    tdp_init(out);

    FILE *fp = std::fopen(path, "r");
    if (!fp) return -1;

    // Initialize a default LOD 0 scene_file so writers can re-emit a name
    // even when 3DA didn't carry one (3DA has no scene_file field).
    TdpLod *lod0 = &out->lods[0];
    if (!lod0->scene_file[0])
        tdp_copy_str(lod0->scene_file, sizeof(lod0->scene_file), "Untitled.ase");
    if (!lod0->render_function[0])
        tdp_copy_str(lod0->render_function, sizeof(lod0->render_function), "gnrc");

    Block3DA block;
    Mat3DAState mat_state;
    int  current_part_anim_idx = -1;
    int  current_light_idx = -1;
    int  next_3da_material_idx = 0;          // monotonic 3DA material counter
    int  last_primary_material_index = -1;         // last allocated TDP material
                                             // (for multitex=2 fold-in)

    char line[1024];
    while (std::fgets(line, sizeof(line), fp)) {
        size_t len = std::strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        TdpTokens t;
        tdp_tokenize(&t, line);
        if (t.count == 0) continue;

        const char *tag0 = tdp_tok_at(&t, 0);

        // ---------------- begin <section> ----------------
        if (tdp_iequals(tag0, "begin") && t.count >= 2) {
            const char *section = tdp_tok_at(&t, 1);
            if (tdp_iequals(section, "general_information")) {
                block.kind = Block3DA::Kind::General;
                tdp_copy_str(block.name_token, sizeof(block.name_token), section);
            } else if (tdp_iequals(section, "material")) {
                // Peek at all field lines via a two-pass... actually OED
                // emits multitexture_flags on a separate line AFTER `name`/
                // `description` but BEFORE green_texture, so we can decide
                // continuation lazily on the multitexture_flags handler.
                // For now allocate a fresh primary TDP material; on a
                // multitexture_flags=2 line we'll roll it back.
                std::memset(&mat_state, 0, sizeof(mat_state));
                mat_state.target_material_index = -1;
                mat_state.is_detail_continuation = false;

                // Default multitexture_flags=1 unless told otherwise.
                // We can't know yet whether this is a continuation, so
                // pre-scan by buffering lines until `end material`?  That
                // forces a two-pass.  Simpler: allocate now, fold later if
                // multitexture_flags 2 arrives.  Folding requires
                // truncating the TDP material array by 1 + copying the
                // green_texture into the previous primary's DETAIL slot.
                {
                    int material_idx = static_cast<int>(out->material_count);
                    TdpMaterial *mat = tdp_ensure_material(out, material_idx);
                    mat->index = material_idx;
                    mat_state.target_material_index = material_idx;
                }
                block.kind = Block3DA::Kind::Material;
                tdp_copy_str(block.name_token, sizeof(block.name_token), section);
            } else if (tdp_iequals(section, "part_animation")) {
                int idx = (t.count > 2) ? tdp_to_int(tdp_tok_at(&t, 2)) : 0;
                if (idx < 0) idx = 0;
                ensure_part_anim_3da(lod0, static_cast<size_t>(idx));
                current_part_anim_idx = idx;
                block.kind = Block3DA::Kind::PartAnimation;
                tdp_copy_str(block.name_token, sizeof(block.name_token), section);
            } else if (tdp_iequals(section, "ambientlight")) {
                block.kind = Block3DA::Kind::AmbientLight;
                tdp_copy_str(block.name_token, sizeof(block.name_token), section);
                // Ambient/direct light fields are informational — TdpLight
                // doesn't carry them as separate slots; ignore body.
            } else if (tdp_iequals(section, "directlight")) {
                block.kind = Block3DA::Kind::DirectLight;
                tdp_copy_str(block.name_token, sizeof(block.name_token), section);
            } else if (tdp_iequals(section, "light")) {
                int idx = (t.count > 2) ? tdp_to_int(tdp_tok_at(&t, 2)) : 0;
                if (idx < 0) idx = 0;
                ensure_light_3da(lod0, static_cast<size_t>(idx));
                current_light_idx = idx;
                block.kind = Block3DA::Kind::Light;
                tdp_copy_str(block.name_token, sizeof(block.name_token), section);
            } else {
                block.kind = Block3DA::Kind::Unknown;
                tdp_copy_str(block.name_token, sizeof(block.name_token), section);
            }
            continue;
        }

        // ---------------- end / end <section> ----------------
        if (tdp_iequals(tag0, "end")) {
            // OED emits both `end <section>` and bare `end` (lights use bare
            // `end\n` per the writer).  Either closes the current block.
            if (block.kind == Block3DA::Kind::Material &&
                mat_state.target_material_index >= 0 &&
                static_cast<size_t>(mat_state.target_material_index) < out->material_count) {
                TdpMaterial *mat =
                    &out->materials[mat_state.target_material_index];
                finalize_material_3da(mat, mat_state);
                // Track the just-closed primary material slot so a following
                // multitex=2 detail block can fold its `green_texture` into
                // it.  Skip if this block was itself a detail continuation
                // (target points at the previously-tracked primary).
                if (!mat_state.is_detail_continuation)
                    last_primary_material_index = mat_state.target_material_index;
            }
            block.kind = Block3DA::Kind::None;
            block.name_token[0] = '\0';
            current_part_anim_idx = -1;
            current_light_idx = -1;
            continue;
        }

        // ---------------- in-block field dispatch ----------------
        switch (block.kind) {
        case Block3DA::Kind::General:
            parse_general_info_field(out, t, line);
            break;

        case Block3DA::Kind::Material: {
            // multitexture_flags handler needs to handle the
            // continuation-fold side-effect before parse_material_field
            // touches the 3DI3 model slot.
            if (tdp_iequals(tag0, "multitexture_flags")) {
                int flags = tdp_to_int(tdp_tok_at(&t, 1));
                if (flags == 2 && last_primary_material_index >= 0 &&
                    !mat_state.is_detail_continuation &&
                    mat_state.target_material_index >= 0) {
                    // Roll back the freshly-allocated TDP material; redirect
                    // writes to the previous primary's slot instead.  The
                    // primary's classification has ALREADY been finalized
                    // (we hit `end material` for it on the previous block),
                    // so we should re-finalize after this block too — but
                    // since the detail and primary share most fields and
                    // OED emits identical values for both, the second
                    // finalize will overwrite to the same result.
                    int rollback_idx = mat_state.target_material_index;
                    if (rollback_idx == static_cast<int>(out->material_count) - 1) {
                        // Zero out the trailing slot we allocated and
                        // shrink material_count back.
                        std::memset(&out->materials[rollback_idx], 0,
                                    sizeof(TdpMaterial));
                        --out->material_count;
                    }
                    mat_state.target_material_index = last_primary_material_index;
                    mat_state.is_detail_continuation = true;
                }
                mat_state.multitexture_flags = flags;
                break;
            }
            if (mat_state.target_material_index < 0) break;
            TdpMaterial *mat =
                &out->materials[mat_state.target_material_index];
            parse_material_field(out, mat, &mat_state, t, line);
            break;
        }

        case Block3DA::Kind::PartAnimation: {
            if (current_part_anim_idx < 0) break;
            TdpPartAnim *pa =
                &lod0->part_anims[current_part_anim_idx];
            parse_part_anim_field(pa, t);
            break;
        }

        case Block3DA::Kind::Light: {
            if (current_light_idx < 0) break;
            TdpLight *lt = &lod0->lights[current_light_idx];
            parse_light_field(lt, t, line);
            break;
        }

        case Block3DA::Kind::AmbientLight:
        case Block3DA::Kind::DirectLight:
        case Block3DA::Kind::Unknown:
        case Block3DA::Kind::None:
            break;
        }

        // We don't consume `next_3da_material_idx` ourselves — it is the
        // writer's responsibility to emit the right number of 3DA material
        // headers.  Track it here as a sanity hook for future debugging.
        (void)next_3da_material_idx;
    }

    std::fclose(fp);
    return 0;
}
