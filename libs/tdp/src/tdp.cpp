// 3DP/3DA project parser and writer.
//
// TdpProject stores TdpMaterial records directly. 3DP is the native JO
// project format; 3DA is retained for legacy project text round-trips.

#include "tdp/tdp.h"
#include "tdp_internal.h"
#include "tdp/tdp_material.h"
#include "threedi/threedi_material_class.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

// ---------------------------------------------------------------------------
// Helper aliases â€” keep the shorter local names that pre-existed in this TU,
// but route them to the shared inline helpers in tdp_internal.h.
// ---------------------------------------------------------------------------

static inline void copy_str(char *dst, size_t dst_size, const char *src) {
    tdp_copy_str(dst, dst_size, src);
}
static inline bool iequals(const char *a, const char *b) {
    return tdp_iequals(a, b);
}
static inline int to_int(const char *s) { return tdp_to_int(s); }
static inline float to_float(const char *s) { return tdp_to_float(s); }
static inline int clamp_255(float v) { return tdp_clamp_255(v); }
static inline float byte_to_unit(int v) { return tdp_byte_to_unit(v); }

static inline int surface_type_to_ptype(uint8_t st) {
    return tdp_surface_type_to_ptype(st);
}
static inline uint8_t ptype_to_surface_type(int ptype) {
    return tdp_ptype_to_surface_type(ptype);
}

// Texture-slot accessors â€” re-export the shared helpers under the
// pre-existing short names used elsewhere in this file.
static inline const TdpMaterialTexture *
tdp_find_static_tex(const TdpMaterial *m, uint8_t slot) {
    return tdp_material_find_static_tex(m, slot);
}
static inline TdpMaterialTexture *
tdp_find_or_alloc_tex(TdpMaterial *m, uint8_t slot, uint8_t frame,
                      bool animated) {
    return tdp_material_find_or_alloc_tex(m, slot, frame, animated);
}

static int tdp_tex_clamped(const TdpMaterialTexture *t) {
    return (t && (t->flags & 0x02u /*CLAMPED*/)) ? 1 : 0;
}

static void tdp_tex_set_clamped(TdpMaterialTexture *t, int clamped) {
    if (!t) return;
    if (clamped) t->flags |= 0x02u;
    else         t->flags &= ~0x02u;
}

// Tokenizer alias â€” local short name routes to TdpTokens.
typedef TdpTokens Tokens;
static inline void tokenize(Tokens *t, const char *line) { tdp_tokenize(t, line); }
static inline const char *tok_at(const Tokens *t, int idx) { return tdp_tok_at(t, idx); }
static inline void extract_quoted(const char *line, char *dst, size_t dst_size) {
    tdp_extract_quoted(line, dst, dst_size);
}

// ---------------------------------------------------------------------------
// Init / Free / Allocators
// ---------------------------------------------------------------------------

void tdp_init(TdpProject *proj) {
    std::memset(proj, 0, sizeof(*proj));
    proj->version = 1;
}

void tdp_free(TdpProject *proj) {
    if (!proj) return;
    std::free(proj->materials);
    proj->materials = nullptr;
    proj->material_count = 0;
    std::free(proj->ctrl_regs);
    proj->ctrl_regs = nullptr;
    proj->ctrl_reg_count = 0;
    for (int i = 0; i < TDP_MAX_LODS; ++i) {
        std::free(proj->lods[i].part_anims);
        proj->lods[i].part_anims = nullptr;
        proj->lods[i].part_anim_count = 0;
        std::free(proj->lods[i].lights);
        proj->lods[i].lights = nullptr;
        proj->lods[i].light_count = 0;
    }
}

void tdp_alloc_materials(TdpProject *proj, size_t count) {
    if (!proj) return;
    std::free(proj->materials);
    proj->materials = nullptr;
    proj->material_count = 0;
    if (count > 0) {
        proj->materials = static_cast<TdpMaterial *>(
            std::calloc(count, sizeof(TdpMaterial)));
        if (proj->materials) proj->material_count = count;
    }
}

void tdp_alloc_ctrl_regs(TdpProject *proj, size_t count) {
    if (!proj) return;
    std::free(proj->ctrl_regs);
    proj->ctrl_regs = nullptr;
    proj->ctrl_reg_count = 0;
    if (count > 0) {
        proj->ctrl_regs = static_cast<TdpControlRegister *>(
            std::calloc(count, sizeof(TdpControlRegister)));
        if (proj->ctrl_regs) proj->ctrl_reg_count = count;
    }
}

void tdp_alloc_part_anims(TdpLod *lod, size_t count) {
    if (!lod) return;
    std::free(lod->part_anims);
    lod->part_anims = nullptr;
    lod->part_anim_count = 0;
    if (count > 0) {
        lod->part_anims = static_cast<TdpPartAnim *>(std::calloc(count, sizeof(TdpPartAnim)));
        if (lod->part_anims) lod->part_anim_count = count;
    }
}

void tdp_alloc_lights(TdpLod *lod, size_t count) {
    if (!lod) return;
    std::free(lod->lights);
    lod->lights = nullptr;
    lod->light_count = 0;
    if (count > 0) {
        lod->lights = static_cast<TdpLight *>(std::calloc(count, sizeof(TdpLight)));
        if (lod->lights) lod->light_count = count;
    }
}

// ---------------------------------------------------------------------------
// Parser internals
// ---------------------------------------------------------------------------

enum ParseState {
    ST_ROOT = 0,
    ST_HEADER,
    ST_MATERIALS,
    ST_MATERIAL,
    ST_LOD,
    ST_PARTANIM,
    ST_PARTANIMSUB,
    ST_LIGHTS,
    ST_LIGHT,
    ST_SKIP
};

static inline TdpMaterial *ensure_material(TdpProject *proj, size_t idx) {
    return tdp_ensure_material(proj, idx);
}

static TdpPartAnim *ensure_part_anim(TdpLod *lod, size_t idx) {
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

static TdpLight *ensure_light(TdpLod *lod, size_t idx) {
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

static void load_axis(TdpAxisFunc *axis, const Tokens *t, int base) {
    axis->func_id = to_int(tok_at(t, base));
    axis->param2  = to_float(tok_at(t, base + 1)); // start
    axis->param3  = to_float(tok_at(t, base + 2)); // end
    axis->param0  = to_float(tok_at(t, base + 3)); // rate
    axis->param1  = to_float(tok_at(t, base + 4)); // phase
    copy_str(axis->ctrl_reg, sizeof(axis->ctrl_reg), tok_at(t, base + 5));
}

// Re-derive classification from rattrib + already-set shader tag at end of
// material parse.  Called after `}` closes a material block.
static void finalize_material_classification(TdpMaterial *m,
                                              uint32_t rattrib) {
    classify_from_jo_shader_tag(m->shader_name,
                                rattrib,
                                m->material_flags,
                                m->emissive_type,
                                m->is_glass ? 1 : 0,
                                &m->classification);
    // Classification's blend_mode tracks the suffix from shader_name; use it
    // as the 3DI3 model's blend_mode when not explicitly overridden.
    switch (m->classification.blend_mode) {
    case THREEDI_CLASS_BLEND_OPAQUE:   m->blend_mode = TDP_BLEND_OPAQUE; break;
    case THREEDI_CLASS_BLEND_ALPHA:    m->blend_mode = TDP_BLEND_ALPHA;  break;
    case THREEDI_CLASS_BLEND_ADDITIVE: m->blend_mode = TDP_BLEND_ADD;    break;
    }
    if (m->classification.alpha_test)
        m->flags |= TDP_MATERIAL_FLAG_ALPHA_TEST;
    if (m->classification.alpha_test_invert)
        m->flags |= TDP_MATERIAL_FLAG_ALPHA_INVERT;
    if (m->classification.two_sided)
        m->flags |= TDP_MATERIAL_FLAG_TWO_SIDED;
}

// ---------------------------------------------------------------------------
// 3DP Parser
// ---------------------------------------------------------------------------

int tdp_parse(const char *path, TdpProject *out) {
    tdp_init(out);

    FILE *fp = std::fopen(path, "r");
    if (!fp) return -1;

    ParseState state_stack[32];
    int stack_depth = 0;
    state_stack[stack_depth++] = ST_ROOT;

    int current_material = -1;
    int current_lod = -1;
    int current_part = -1;
    int current_light = -1;
    uint32_t current_rattrib = 0;  // Buffer rattrib per-material until material block closes

    char line[1024];
    while (std::fgets(line, sizeof(line), fp)) {
        size_t len = std::strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        Tokens t;
        tokenize(&t, line);
        if (t.count == 0) continue;

        const char *tag = t.toks[0];

        if (tag[0] == '}') {
            // If closing a material block, finalize classification
            if (stack_depth > 0 && state_stack[stack_depth - 1] == ST_MATERIAL) {
                if (current_material >= 0 &&
                    (size_t)current_material < out->material_count) {
                    finalize_material_classification(
                        &out->materials[current_material], current_rattrib);
                }
                current_rattrib = 0;
            }
            if (stack_depth > 1) --stack_depth;
            continue;
        }

        ParseState state = state_stack[stack_depth - 1];
        switch (state) {
        case ST_ROOT:
            if (iequals(tag, "header")) {
                state_stack[stack_depth++] = ST_HEADER;
            } else if (iequals(tag, "materials")) {
                state_stack[stack_depth++] = ST_MATERIALS;
            } else if (iequals(tag, "lod")) {
                current_lod = to_int(tok_at(&t, 1));
                if (current_lod >= 0 && current_lod < TDP_MAX_LODS)
                    state_stack[stack_depth++] = ST_LOD;
                else
                    state_stack[stack_depth++] = ST_SKIP;
            }
            break;

        case ST_HEADER:
            if (iequals(tag, "version"))
                out->version = to_int(tok_at(&t, 1));
            else if (iequals(tag, "poly_collision_lod"))
                out->poly_collision_lod = to_int(tok_at(&t, 1));
            break;

        case ST_MATERIALS:
            if (iequals(tag, "nummaterials")) {
                int count = to_int(tok_at(&t, 1));
                if (count > 0) {
                    out->materials = static_cast<TdpMaterial *>(
                        std::calloc(static_cast<size_t>(count), sizeof(TdpMaterial)));
                    out->material_count = static_cast<size_t>(count);
                }
            } else if (iequals(tag, "material")) {
                current_material = to_int(tok_at(&t, 1));
                if (current_material >= 0) {
                    ensure_material(out, static_cast<size_t>(current_material));
                    current_rattrib = 0;
                    state_stack[stack_depth++] = ST_MATERIAL;
                }
            }
            break;

        case ST_MATERIAL: {
            TdpMaterial *mat = &out->materials[current_material];
            mat->index = current_material;
            if (iequals(tag, "name")) {
                if (t.count > 1 && tok_at(&t, 1)[0])
                    copy_str(mat->name, sizeof(mat->name), tok_at(&t, 1));
                else
                    extract_quoted(line, mat->name, sizeof(mat->name));
            } else if (iequals(tag, "shadertag")) {
                if (t.count > 1 && tok_at(&t, 1)[0])
                    copy_str(mat->shader_name, sizeof(mat->shader_name), tok_at(&t, 1));
                else
                    extract_quoted(line, mat->shader_name, sizeof(mat->shader_name));
            } else if (iequals(tag, "rattrib")) {
                current_rattrib = static_cast<uint32_t>(to_int(tok_at(&t, 1)));
            } else if (iequals(tag, "pattrib")) {
                mat->pattrib = static_cast<uint32_t>(to_int(tok_at(&t, 1)));
            } else if (iequals(tag, "ptype")) {
                mat->surface_type = ptype_to_surface_type(to_int(tok_at(&t, 1)));
            } else if (iequals(tag, "geofx")) {
                mat->geofx = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "geofx_value")) {
                mat->geofx_value = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "alphatestvalue")) {
                int v = to_int(tok_at(&t, 1));
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                mat->alpha_test_value_byte = static_cast<uint8_t>(v);
                mat->alpha_threshold = static_cast<float>(v) / 255.0f;
            } else if (iequals(tag, "diffusetex[0]") || iequals(tag, "diffusetex[1]") ||
                       iequals(tag, "normaltex[0]") || iequals(tag, "normaltex[1]")) {
                bool is_normal = (tag[0] == 'n' || tag[0] == 'N');
                int slot = 0;
                const char *lb = std::strchr(tag, '[');
                const char *rb = std::strchr(tag, ']');
                if (lb && rb && rb > lb + 1) {
                    char tmp[4] = {};
                    size_t slen = static_cast<size_t>(rb - lb - 1);
                    if (slen < sizeof(tmp)) { std::memcpy(tmp, lb + 1, slen); }
                    slot = to_int(tmp);
                }
                const char *tex_path = tok_at(&t, 1);
                int flag_val = to_int(tok_at(&t, 2));
                char qpath[32] = {};
                if (t.count == 2) {
                    tex_path = "";
                    flag_val = to_int(tok_at(&t, 1));
                }
                if (!tex_path[0]) {
                    extract_quoted(line, qpath, sizeof(qpath));
                    tex_path = qpath;
                }
                if (slot >= 0 && slot < 2) {
                    uint8_t ir_slot;
                    if (is_normal)
                        ir_slot = (slot == 0) ? TDP_TEX_SLOT_NORMAL
                                              : TDP_TEX_SLOT_NORMAL_B;
                    else
                        ir_slot = (slot == 0) ? TDP_TEX_SLOT_DIFFUSE
                                              : TDP_TEX_SLOT_DETAIL;
                    // Treat "0" sentinel as empty path for non-normal slots
                    bool sentinel = (tex_path[0] == '0' && tex_path[1] == '\0');
                    if (!sentinel || is_normal) {
                        TdpMaterialTexture *tex = tdp_find_or_alloc_tex(
                            mat, ir_slot, /*frame=*/0, /*animated=*/false);
                        if (tex) {
                            if (sentinel) tex->name[0] = '\0';
                            else copy_str(tex->name, sizeof(tex->name), tex_path);
                            tdp_tex_set_clamped(tex, flag_val & 1);
                        }
                    }
                }
            } else if (iequals(tag, "anim_frames")) {
                int v = to_int(tok_at(&t, 1));
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                mat->animation.num_frames = static_cast<uint8_t>(v);
            } else if (iequals(tag, "anim_type")) {
                int v = to_int(tok_at(&t, 1));
                mat->animation.animation_type = static_cast<uint8_t>(v & 0xff);
            } else if (iequals(tag, "anim_frametime")) {
                int v = to_int(tok_at(&t, 1));
                mat->animation.cycle_frame_time = static_cast<int16_t>(v);
            } else if (iequals(tag, "anim_ctrlreg")) {
                if (t.count > 1 && tok_at(&t, 1)[0])
                    copy_str(mat->anim_ctrlreg, sizeof(mat->anim_ctrlreg), tok_at(&t, 1));
                else
                    extract_quoted(line, mat->anim_ctrlreg, sizeof(mat->anim_ctrlreg));
            } else if (std::strstr(tag, "anim_diffusetex") || std::strstr(tag, "anim_normaltex") ||
                       std::strstr(tag, "anim_DIFFUSETEX") || std::strstr(tag, "anim_NORMALTEX")) {
                bool is_normal = (std::strstr(tag, "normal") != nullptr ||
                                  std::strstr(tag, "NORMAL") != nullptr);
                int tex_slot = (std::strstr(tag, "[1]") != nullptr) ? 1 : 0;
                int frame = to_int(tok_at(&t, 1));
                if (frame >= 0 && frame < TDP_MAX_ANIM_FRAMES) {
                    TdpAnimTexture *dst = is_normal
                        ? &mat->anim_normal[tex_slot][frame]
                        : &mat->anim_diffuse[tex_slot][frame];
                    const char *fpath = tok_at(&t, 2);
                    if (!fpath[0]) extract_quoted(line, dst->path, sizeof(dst->path));
                    else copy_str(dst->path, sizeof(dst->path), fpath);
                    dst->enabled = to_int(tok_at(&t, 3)) & 1;
                }
            } else if (iequals(tag, "reflect_rgb")) {
                int r = to_int(tok_at(&t, 1));
                int g = to_int(tok_at(&t, 2));
                int b = to_int(tok_at(&t, 3));
                // 3DP stores plain RGB triplet; reflect_color is BGRA in TDP material data.
                mat->reflect_color[0] = byte_to_unit(b);  // B
                mat->reflect_color[1] = byte_to_unit(g);  // G
                mat->reflect_color[2] = byte_to_unit(r);  // R
                mat->reflect_color[3] = 0.0f;             // A
            } else if (iequals(tag, "rgbgen_style")) {
                int v = to_int(tok_at(&t, 1));
                mat->rgb_gen.style = static_cast<uint8_t>(v & 0xff);
            } else if (iequals(tag, "rgbgen_rate")) {
                mat->rgb_gen.rate = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "rgbgen_phase")) {
                mat->rgb_gen.phase = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "rgbgen_srgb")) {
                mat->rgb_gen.start_color[0] = byte_to_unit(to_int(tok_at(&t, 1)));
                mat->rgb_gen.start_color[1] = byte_to_unit(to_int(tok_at(&t, 2)));
                mat->rgb_gen.start_color[2] = byte_to_unit(to_int(tok_at(&t, 3)));
                mat->rgb_gen.start_color[3] = 0.0f;
            } else if (iequals(tag, "rgbgen_ergb")) {
                mat->rgb_gen.end_color[0] = byte_to_unit(to_int(tok_at(&t, 1)));
                mat->rgb_gen.end_color[1] = byte_to_unit(to_int(tok_at(&t, 2)));
                mat->rgb_gen.end_color[2] = byte_to_unit(to_int(tok_at(&t, 3)));
                mat->rgb_gen.end_color[3] = 0.0f;
            } else if (iequals(tag, "rgbgen_ctrlreg")) {
                char regname[32] = {};
                if (t.count > 1 && tok_at(&t, 1)[0])
                    copy_str(regname, sizeof(regname), tok_at(&t, 1));
                else
                    extract_quoted(line, regname, sizeof(regname));
                mat->rgb_gen.reg = tdp_ctrlreg_intern(out, regname);
            } else if (iequals(tag, "alphagen_style")) {
                int v = to_int(tok_at(&t, 1));
                mat->alpha_gen.style = static_cast<uint8_t>(v & 0xff);
            } else if (iequals(tag, "alphagen_rate")) {
                mat->alpha_gen.rate = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "alphagen_phase")) {
                mat->alpha_gen.phase = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "alphagen_start")) {
                mat->alpha_gen.start = static_cast<int16_t>(to_int(tok_at(&t, 1)));
            } else if (iequals(tag, "alphagen_end")) {
                mat->alpha_gen.end = static_cast<int16_t>(to_int(tok_at(&t, 1)));
            } else if (iequals(tag, "alphagen_ctrlreg")) {
                char regname[32] = {};
                if (t.count > 1 && tok_at(&t, 1)[0])
                    copy_str(regname, sizeof(regname), tok_at(&t, 1));
                else
                    extract_quoted(line, regname, sizeof(regname));
                mat->alpha_gen.reg = tdp_ctrlreg_intern(out, regname);
            } else if (iequals(tag, "mapfunc_u_style")) {
                int v = to_int(tok_at(&t, 1));
                mat->u_params.style = static_cast<uint8_t>(v & 0xff);
            } else if (iequals(tag, "mapfunc_u_rate")) {
                mat->u_params.gen_rate = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_u_phase")) {
                mat->u_params.phase = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_u_start")) {
                mat->u_params.start = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_u_end")) {
                mat->u_params.end = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_u_ctrlreg")) {
                char regname[32] = {};
                if (t.count > 1 && tok_at(&t, 1)[0])
                    copy_str(regname, sizeof(regname), tok_at(&t, 1));
                else
                    extract_quoted(line, regname, sizeof(regname));
                mat->u_params.reg = tdp_ctrlreg_intern(out, regname);
            } else if (iequals(tag, "mapfunc_v_style")) {
                int v = to_int(tok_at(&t, 1));
                mat->v_params.style = static_cast<uint8_t>(v & 0xff);
            } else if (iequals(tag, "mapfunc_v_rate")) {
                mat->v_params.gen_rate = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_v_phase")) {
                mat->v_params.phase = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_v_start")) {
                mat->v_params.start = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_v_end")) {
                mat->v_params.end = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_v_ctrlreg")) {
                char regname[32] = {};
                if (t.count > 1 && tok_at(&t, 1)[0])
                    copy_str(regname, sizeof(regname), tok_at(&t, 1));
                else
                    extract_quoted(line, regname, sizeof(regname));
                mat->v_params.reg = tdp_ctrlreg_intern(out, regname);
            }
            break;
        }

        case ST_LOD: {
            TdpLod *lod = &out->lods[current_lod];
            if (iequals(tag, "scenename")) {
                const char *sn = tok_at(&t, 1);
                if (!sn[0]) extract_quoted(line, lod->scene_file, sizeof(lod->scene_file));
                else copy_str(lod->scene_file, sizeof(lod->scene_file), sn);
            } else if (iequals(tag, "attributes")) {
                lod->attributes = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "render_function")) {
                copy_str(lod->render_function, sizeof(lod->render_function), tok_at(&t, 1));
            } else if (iequals(tag, "threshold")) {
                lod->threshold = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "partanimation")) {
                state_stack[stack_depth++] = ST_PARTANIM;
            } else if (iequals(tag, "lightsources")) {
                if (current_lod == 0)
                    state_stack[stack_depth++] = ST_LIGHTS;
                else
                    state_stack[stack_depth++] = ST_SKIP;
            }
            break;
        }

        case ST_PARTANIM: {
            TdpLod *lod = &out->lods[current_lod];
            if (iequals(tag, "enablepartanim")) {
                lod->part_anim_enabled = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "subobject")) {
                current_part = to_int(tok_at(&t, 1));
                if (current_part >= 0) {
                    ensure_part_anim(lod, static_cast<size_t>(current_part));
                    state_stack[stack_depth++] = ST_PARTANIMSUB;
                }
            }
            break;
        }

        case ST_PARTANIMSUB: {
            TdpLod *lod = &out->lods[current_lod];
            TdpPartAnim *pa = &lod->part_anims[current_part];
            if (iequals(tag, "rotate_type")) {
                pa->rotate_type = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "scale_type")) {
                pa->scale_type = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "trans_type")) {
                pa->trans_type = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "transform_as")) {
                pa->transform_as = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "yaw_rate")) {
                pa->yaw_rate = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "pitch_rate")) {
                pa->pitch_rate = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "roll_rate")) {
                pa->roll_rate = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "yaw_func")) {
                load_axis(&pa->yaw, &t, 1);
            } else if (iequals(tag, "pitch_func")) {
                load_axis(&pa->pitch, &t, 1);
            } else if (iequals(tag, "roll_func")) {
                load_axis(&pa->roll, &t, 1);
            } else if (iequals(tag, "reverserotate")) {
                pa->reverse_rotate = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "scale_func")) {
                load_axis(&pa->scale, &t, 1);
            } else if (iequals(tag, "scalex_func")) {
                load_axis(&pa->scale_x, &t, 1);
            } else if (iequals(tag, "scaley_func")) {
                load_axis(&pa->scale_y, &t, 1);
            } else if (iequals(tag, "scalez_func")) {
                load_axis(&pa->scale_z, &t, 1);
            } else if (iequals(tag, "transx_func")) {
                load_axis(&pa->trans_x, &t, 1);
            } else if (iequals(tag, "transy_func")) {
                load_axis(&pa->trans_y, &t, 1);
            } else if (iequals(tag, "transz_func")) {
                load_axis(&pa->trans_z, &t, 1);
            }
            break;
        }

        case ST_LIGHTS: {
            TdpLod *lod = &out->lods[current_lod];
            if (iequals(tag, "numlights")) {
                int count = to_int(tok_at(&t, 1));
                if (count > 0) {
                    lod->lights = static_cast<TdpLight *>(
                        std::calloc(static_cast<size_t>(count), sizeof(TdpLight)));
                    lod->light_count = static_cast<size_t>(count);
                }
            } else if (iequals(tag, "light")) {
                current_light = to_int(tok_at(&t, 1));
                if (current_light >= 0) {
                    ensure_light(lod, static_cast<size_t>(current_light));
                    state_stack[stack_depth++] = ST_LIGHT;
                }
            }
            break;
        }

        case ST_LIGHT: {
            TdpLod *lod = &out->lods[current_lod];
            TdpLight *light = &lod->lights[current_light];
            if (iequals(tag, "name")) {
                if (t.count > 1 && tok_at(&t, 1)[0])
                    copy_str(light->name, sizeof(light->name), tok_at(&t, 1));
                else
                    extract_quoted(line, light->name, sizeof(light->name));
            } else if (iequals(tag, "colorgen_style")) {
                light->colorgen_style = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "colorgen_rate")) {
                light->colorgen_rate = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "colorgen_phase")) {
                light->colorgen_phase = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "colorgen_start")) {
                light->colorgen_start[0] = to_int(tok_at(&t, 1));
                light->colorgen_start[1] = to_int(tok_at(&t, 2));
                light->colorgen_start[2] = to_int(tok_at(&t, 3));
            } else if (iequals(tag, "colorgen_end")) {
                light->colorgen_end[0] = to_int(tok_at(&t, 1));
                light->colorgen_end[1] = to_int(tok_at(&t, 2));
                light->colorgen_end[2] = to_int(tok_at(&t, 3));
            } else if (iequals(tag, "colorgen_ctrlreg")) {
                if (t.count > 1 && tok_at(&t, 1)[0])
                    copy_str(light->colorgen_ctrlreg, sizeof(light->colorgen_ctrlreg), tok_at(&t, 1));
                else
                    extract_quoted(line, light->colorgen_ctrlreg, sizeof(light->colorgen_ctrlreg));
            } else if (iequals(tag, "disable_corona")) {
                light->disable_corona = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "disable_lightterrain")) {
                light->disable_lightterrain = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "disable_lightobjects")) {
                light->disable_lightobjects = to_int(tok_at(&t, 1));
            }
            break;
        }

        case ST_SKIP:
            break;
        }
    }

    std::fclose(fp);
    return 0;
}

// ---------------------------------------------------------------------------
// 3DP Writer
// ---------------------------------------------------------------------------

static void write_axis(FILE *fp, const char *label, const TdpAxisFunc *a) {
    std::fprintf(fp, "            %-16s%2i  %1.3f %1.3f %1.3f %1.3f %s\n",
                 label, a->func_id,
                 static_cast<double>(a->param2), static_cast<double>(a->param3),
                 static_cast<double>(a->param0), static_cast<double>(a->param1),
                 a->ctrl_reg);
}

// Compose the JO shader_tag the writer should emit.  The native shader_name
// field is authoritative when present; classification is a fallback for
// format-neutral callers that did not provide a concrete JO tag.
static void writer_compose_shader_tag(const TdpMaterial *m,
                                       char *out, size_t out_size) {
    out[0] = '\0';
    if (m->shader_name[0]) {
        copy_str(out, out_size, m->shader_name);
        return;
    }
    synthesize_jo_shader_tag(&m->classification, out, out_size);
}

// Compute the rattrib int the writer should emit, using the synthesizer.
static uint32_t writer_compose_rattrib(const TdpMaterial *m) {
    int has_anim = (m->animation.num_frames > 0) ? 1 : 0;
    return synthesize_jo_rattrib(&m->classification, has_anim);
}

int tdp_write(const char *path, const TdpProject *proj) {
    FILE *fp = std::fopen(path, "w");
    if (!fp) return -1;

    std::fprintf(fp, "// 3DP Project File\n\n");
    std::fprintf(fp, "header\n{\n");
    std::fprintf(fp, "    version %d\n", proj->version);
    std::fprintf(fp, "    poly_collision_lod %d\n", proj->poly_collision_lod);
    std::fprintf(fp, "}\n\n");

    // Materials
    std::fprintf(fp, "materials\n{\n");
    std::fprintf(fp, "    nummaterials %zu\n", proj->material_count);
    for (size_t i = 0; i < proj->material_count; ++i) {
        const TdpMaterial *m = &proj->materials[i];

        // Compose shader_tag + rattrib via synthesizers
        char shader_tag[33];
        writer_compose_shader_tag(m, shader_tag, sizeof(shader_tag));
        uint32_t rattrib = writer_compose_rattrib(m);

        // Texture lookups
        const TdpMaterialTexture *d0 = tdp_find_static_tex(m, TDP_TEX_SLOT_DIFFUSE);
        const TdpMaterialTexture *d1 = tdp_find_static_tex(m, TDP_TEX_SLOT_DETAIL);
        const TdpMaterialTexture *n0 = tdp_find_static_tex(m, TDP_TEX_SLOT_NORMAL);
        const TdpMaterialTexture *n1 = tdp_find_static_tex(m, TDP_TEX_SLOT_NORMAL_B);

        const char *d0_path = d0 ? d0->name : "";
        const char *d1_path = d1 ? d1->name : "";
        // Normal slot defaults to "0" sentinel when missing (engine convention)
        const char *n0_path = (n0 && n0->name[0]) ? n0->name : "0";
        const char *n1_path = (n1 && n1->name[0]) ? n1->name : "0";

        int d0_flag = tdp_tex_clamped(d0);
        int d1_flag = tdp_tex_clamped(d1);
        int n0_flag = tdp_tex_clamped(n0);
        int n1_flag = tdp_tex_clamped(n1);

        // Build anim_diffuse / anim_normal frame arrays from the material texture
        // table.  The 3DI3 reader stores per-frame paths in textures[] with
        // the ANIMATED flag bit set; the legacy 3DP encoding uses parallel
        // anim_diffuse[][] / anim_normal[][] arrays.  Either source may be
        // populated â€” readers from JO 3DI3 set textures[], writers from 3DP
        // (round-trip) set anim_diffuse directly.  Merge both into local
        // staging arrays for emission.
        TdpAnimTexture anim_diff[2][TDP_MAX_ANIM_FRAMES];
        TdpAnimTexture anim_norm[2][TDP_MAX_ANIM_FRAMES];
        std::memcpy(anim_diff, m->anim_diffuse, sizeof(anim_diff));
        std::memcpy(anim_norm, m->anim_normal,  sizeof(anim_norm));
        for (uint32_t ti = 0;
             ti < m->texture_count && ti < TDP_MAX_MATERIAL_TEXTURES;
             ++ti) {
            const TdpMaterialTexture *t = &m->textures[ti];
            if (!(t->flags & 0x01u /*ANIMATED*/)) continue;
            if (t->frame >= TDP_MAX_ANIM_FRAMES) continue;
            int slot_idx = -1;
            bool is_norm = false;
            switch (t->slot) {
            case TDP_TEX_SLOT_DIFFUSE:  slot_idx = 0; break;
            case TDP_TEX_SLOT_DETAIL:   slot_idx = 1; break;
            case TDP_TEX_SLOT_NORMAL:   slot_idx = 0; is_norm = true; break;
            case TDP_TEX_SLOT_NORMAL_B: slot_idx = 1; is_norm = true; break;
            default: continue;
            }
            TdpAnimTexture *dst = is_norm
                ? &anim_norm[slot_idx][t->frame]
                : &anim_diff[slot_idx][t->frame];
            if (!dst->path[0]) {
                copy_str(dst->path, sizeof(dst->path), t->name);
                dst->enabled = (t->flags & 0x02u) ? 1 : 0;
            }
        }

        // For animated materials with no static diffuse, the engine stores
        // the first anim frame as the base diffuse path.  Mirror that here
        // so 3DP fixtures with animated diffuse round-trip cleanly.
        if (!d0_path[0] && anim_diff[0][0].path[0])
            d0_path = anim_diff[0][0].path;
        if (!d1_path[0] && anim_diff[1][0].path[0])
            d1_path = anim_diff[1][0].path;

        // Fill empty anim slots with the matching STATIC tex path (so
        // OED's AppendTextureSlot @ 0x453300 collapses identical-name
        // frames to one MTRL entry with that name) when there ARE
        // animation frames AND the material has a non-zero static name
        // for that slot. Otherwise fall back to the "0" sentinel.
        //
        // Without this fallback, materials with animated diffuse + static
        // normal (e.g. Beret mat[1]) emit "0" anim_normaltex frames; OED
        // then collapses to one MTRL entry literally named "0" instead of
        // the static normaltex[0] value (e.g. "A_Beret.mdt"). Witnessed
        // against ImportWorkspace_Parse3daToken @ 0x40927d (parses
        // normaltex[0] into &mat->anim_textures) and WriteMTRL @ 0x4537e5
        // (reads &slots[i].anim_textures back when not animated, or via
        // AppendTextureSlot when animated).
        auto static_path_or_zero = [](const char *p) -> const char * {
            return (p && p[0] && std::strcmp(p, "0") != 0) ? p : "0";
        };
        if (m->animation.num_frames > 0) {
            int n_anim_fill = m->animation.num_frames;
            if (n_anim_fill > TDP_MAX_ANIM_FRAMES)
                n_anim_fill = TDP_MAX_ANIM_FRAMES;
            const char *diff_fallback[2] = {
                static_path_or_zero(d0_path),
                static_path_or_zero(d1_path),
            };
            const char *norm_fallback[2] = {
                static_path_or_zero(n0_path),
                static_path_or_zero(n1_path),
            };
            for (int s = 0; s < 2; ++s) {
                for (int f = 0; f < n_anim_fill; ++f) {
                    if (!anim_diff[s][f].path[0])
                        copy_str(anim_diff[s][f].path,
                                 sizeof(anim_diff[s][f].path),
                                 diff_fallback[s]);
                    if (!anim_norm[s][f].path[0])
                        copy_str(anim_norm[s][f].path,
                                 sizeof(anim_norm[s][f].path),
                                 norm_fallback[s]);
                }
            }
        }

        // Reflect_color is BGRA float in TDP material data; emit as plain RGB ints (R G B)
        int rr = clamp_255(m->reflect_color[2]);
        int rg = clamp_255(m->reflect_color[1]);
        int rb = clamp_255(m->reflect_color[0]);

        // RGB gen colors: float 0..1 â†’ int 0..255
        int sr = clamp_255(m->rgb_gen.start_color[0]);
        int sg = clamp_255(m->rgb_gen.start_color[1]);
        int sb = clamp_255(m->rgb_gen.start_color[2]);
        int er = clamp_255(m->rgb_gen.end_color[0]);
        int eg = clamp_255(m->rgb_gen.end_color[1]);
        int eb = clamp_255(m->rgb_gen.end_color[2]);

        // ctrl-reg names
        const char *rgb_reg = tdp_ctrlreg_name(proj, m->rgb_gen.reg);
        const char *alpha_reg = tdp_ctrlreg_name(proj, m->alpha_gen.reg);
        const char *u_reg = tdp_ctrlreg_name(proj, m->u_params.reg);
        const char *v_reg = tdp_ctrlreg_name(proj, m->v_params.reg);

        std::fprintf(fp, "    material %zu\n    {\n", i);
        std::fprintf(fp, "        name             \"%s\"\n", m->name);
        std::fprintf(fp, "        shadertag        \"%s\"\n", shader_tag);
        std::fprintf(fp, "        rattrib          %u\n", rattrib);
        std::fprintf(fp, "        pattrib          %u\n", m->pattrib);
        std::fprintf(fp, "        ptype            %d\n", surface_type_to_ptype(m->surface_type));
        std::fprintf(fp, "        geofx            %d\n", m->geofx);
        std::fprintf(fp, "        geofx_value      %5.2f\n", static_cast<double>(m->geofx_value));
        std::fprintf(fp, "        alphatestvalue   %d\n",
                     static_cast<int>(m->alpha_test_value_byte));
        std::fprintf(fp, "        diffusetex[0]    \"%s\"  %d\n", d0_path, d0_flag);
        std::fprintf(fp, "        diffusetex[1]    \"%s\"  %d\n", d1_path, d1_flag);
        std::fprintf(fp, "        normaltex[0]     \"%s\"  %d\n", n0_path, n0_flag);
        std::fprintf(fp, "        normaltex[1]     \"%s\"  %d\n", n1_path, n1_flag);
        std::fprintf(fp, "        anim_frames      %d\n",
                     static_cast<int>(m->animation.num_frames));
        std::fprintf(fp, "        anim_type        %d\n",
                     static_cast<int>(m->animation.animation_type));
        // For ctrl-reg-driven animation (type 1), cycle_frame_time is the
        // register index, not a frame time â€” emit 0 as the engine does.
        int frametime = (m->animation.animation_type == 1) ? 0
                        : m->animation.cycle_frame_time;
        std::fprintf(fp, "        anim_frametime   %d\n", frametime);
        std::fprintf(fp, "        anim_ctrlreg     \"%s\"\n", m->anim_ctrlreg);

        int n_anim = m->animation.num_frames;
        if (n_anim > TDP_MAX_ANIM_FRAMES) n_anim = TDP_MAX_ANIM_FRAMES;
        for (int f = 0; f < n_anim; ++f)
            std::fprintf(fp, "        anim_diffusetex[0] %d \"%s\"   %d\n",
                         f, anim_diff[0][f].path, anim_diff[0][f].enabled);
        for (int f = 0; f < n_anim; ++f)
            std::fprintf(fp, "        anim_diffusetex[1] %d \"%s\"  %d\n",
                         f, anim_diff[1][f].path, anim_diff[1][f].enabled);
        for (int f = 0; f < n_anim; ++f)
            std::fprintf(fp, "        anim_normaltex[0]  %d \"%s\"  %d\n",
                         f, anim_norm[0][f].path, anim_norm[0][f].enabled);
        for (int f = 0; f < n_anim; ++f)
            std::fprintf(fp, "        anim_normaltex[1]  %d \"%s\"  %d\n",
                         f, anim_norm[1][f].path, anim_norm[1][f].enabled);

        std::fprintf(fp, "        reflect_rgb    %d %d %d\n", rr, rg, rb);
        std::fprintf(fp, "        rgbgen_style    %d\n", static_cast<int>(m->rgb_gen.style));
        std::fprintf(fp, "        rgbgen_rate     %f\n", static_cast<double>(m->rgb_gen.rate));
        std::fprintf(fp, "        rgbgen_phase    %f\n", static_cast<double>(m->rgb_gen.phase));
        std::fprintf(fp, "        rgbgen_srgb     %d %d %d\n", sr, sg, sb);
        std::fprintf(fp, "        rgbgen_ergb     %d %d %d\n", er, eg, eb);
        std::fprintf(fp, "        rgbgen_ctrlreg  %s\n", rgb_reg);
        std::fprintf(fp, "        alphagen_style   %d\n", static_cast<int>(m->alpha_gen.style));
        std::fprintf(fp, "        alphagen_rate    %f\n", static_cast<double>(m->alpha_gen.rate));
        std::fprintf(fp, "        alphagen_phase   %f\n", static_cast<double>(m->alpha_gen.phase));
        std::fprintf(fp, "        alphagen_start   %i\n", static_cast<int>(m->alpha_gen.start));
        std::fprintf(fp, "        alphagen_end     %i\n", static_cast<int>(m->alpha_gen.end));
        std::fprintf(fp, "        alphagen_ctrlreg %s\n", alpha_reg);
        std::fprintf(fp, "        mapfunc_u_style   %d\n", static_cast<int>(m->u_params.style));
        std::fprintf(fp, "        mapfunc_u_rate    %f\n", static_cast<double>(m->u_params.gen_rate));
        std::fprintf(fp, "        mapfunc_u_phase   %f\n", static_cast<double>(m->u_params.phase));
        std::fprintf(fp, "        mapfunc_u_start   %f\n", static_cast<double>(m->u_params.start));
        std::fprintf(fp, "        mapfunc_u_end     %f\n", static_cast<double>(m->u_params.end));
        std::fprintf(fp, "        mapfunc_u_ctrlreg %s\n", u_reg);
        std::fprintf(fp, "        mapfunc_v_style   %d\n", static_cast<int>(m->v_params.style));
        std::fprintf(fp, "        mapfunc_v_rate    %f\n", static_cast<double>(m->v_params.gen_rate));
        std::fprintf(fp, "        mapfunc_v_phase   %f\n", static_cast<double>(m->v_params.phase));
        std::fprintf(fp, "        mapfunc_v_start   %f\n", static_cast<double>(m->v_params.start));
        std::fprintf(fp, "        mapfunc_v_end     %f\n", static_cast<double>(m->v_params.end));
        std::fprintf(fp, "        mapfunc_v_ctrlreg %s\n", v_reg);
        std::fprintf(fp, "    }\n");
    }
    std::fprintf(fp, "}\n\n");

    // LODs
    for (int li = 0; li < TDP_MAX_LODS; ++li) {
        const TdpLod *lod = &proj->lods[li];
        if (!lod->scene_file[0] && lod->part_anim_count == 0 && lod->light_count == 0)
            continue;

        std::fprintf(fp, "lod %d\n{\n", li);
        std::fprintf(fp, "    scenename        %s\n", lod->scene_file[0] ? lod->scene_file : "Untitled.ase");
        std::fprintf(fp, "    attributes       %d\n", lod->attributes);
        std::fprintf(fp, "    render_function  %s\n", lod->render_function[0] ? lod->render_function : "gnrc");
        std::fprintf(fp, "    threshold        %.3f\n", static_cast<double>(lod->threshold));

        std::fprintf(fp, "    partanimation\n    {\n");
        std::fprintf(fp, "        enablepartanim   %d\n", lod->part_anim_enabled);
        std::fprintf(fp, "        numpartanim      %zu\n", lod->part_anim_count);
        for (size_t p = 0; p < lod->part_anim_count; ++p) {
            const TdpPartAnim *pa = &lod->part_anims[p];
            std::fprintf(fp, "        subobject %zu\n        {\n", p);
            std::fprintf(fp, "            rotate_type     %2i\n", pa->rotate_type);
            std::fprintf(fp, "            scale_type      %2i\n", pa->scale_type);
            std::fprintf(fp, "            trans_type      %2i\n", pa->trans_type);
            std::fprintf(fp, "            transform_as    %2i\n", pa->transform_as);
            std::fprintf(fp, "            yaw_rate         %1.3f\n", static_cast<double>(pa->yaw_rate));
            std::fprintf(fp, "            pitch_rate       %1.3f\n", static_cast<double>(pa->pitch_rate));
            std::fprintf(fp, "            roll_rate        %1.3f\n", static_cast<double>(pa->roll_rate));
            write_axis(fp, "yaw_func", &pa->yaw);
            write_axis(fp, "pitch_func", &pa->pitch);
            write_axis(fp, "roll_func", &pa->roll);
            std::fprintf(fp, "            reverserotate   %2i\n", pa->reverse_rotate);
            write_axis(fp, "scale_func", &pa->scale);
            write_axis(fp, "scalex_func", &pa->scale_x);
            write_axis(fp, "scaley_func", &pa->scale_y);
            write_axis(fp, "scalez_func", &pa->scale_z);
            write_axis(fp, "transx_func", &pa->trans_x);
            write_axis(fp, "transy_func", &pa->trans_y);
            write_axis(fp, "transz_func", &pa->trans_z);
            std::fprintf(fp, "        }\n");
        }
        std::fprintf(fp, "    }\n");

        std::fprintf(fp, "    lightsources\n    {\n");
        std::fprintf(fp, "        numlights %zu\n", lod->light_count);
        for (size_t j = 0; j < lod->light_count; ++j) {
            const TdpLight *lt = &lod->lights[j];
            std::fprintf(fp, "        light  %zu\n        {\n", j);
            std::fprintf(fp, "            name                  \"%s\"\n", lt->name);
            std::fprintf(fp, "            colorgen_style        %i\n", lt->colorgen_style);
            std::fprintf(fp, "            colorgen_rate         %f\n", static_cast<double>(lt->colorgen_rate));
            std::fprintf(fp, "            colorgen_phase        %f\n", static_cast<double>(lt->colorgen_phase));
            std::fprintf(fp, "            colorgen_start        %i %i %i\n",
                         lt->colorgen_start[0], lt->colorgen_start[1], lt->colorgen_start[2]);
            std::fprintf(fp, "            colorgen_end          %i %i %i\n",
                         lt->colorgen_end[0], lt->colorgen_end[1], lt->colorgen_end[2]);
            std::fprintf(fp, "            colorgen_ctrlreg      %s\n", lt->colorgen_ctrlreg);
            std::fprintf(fp, "            disable_corona        %ld\n", static_cast<long>(lt->disable_corona));
            std::fprintf(fp, "            disable_lightterrain  %ld\n", static_cast<long>(lt->disable_lightterrain));
            std::fprintf(fp, "            disable_lightobjects  %ld\n", static_cast<long>(lt->disable_lightobjects));
            std::fprintf(fp, "        }\n");
        }
        std::fprintf(fp, "    }\n");
        std::fprintf(fp, "}\n\n");
    }

    std::fclose(fp);
    return 0;
}

// ---------------------------------------------------------------------------
// 3DA Writer (legacy ModSuperOED format)
// ---------------------------------------------------------------------------

// Per-label literal padding matching df4oed.exe::sub_421120 @ 0x4218f7â€¦0x421a9b.
// OED's format strings hardcode the padding per field name; a quirk in OED is
// that scalex/y/z_func have one extra space vs scale_func â€” preserved here.
static void write_axis_3da(FILE *fp, const char *label, const TdpAxisFunc *a) {
    const char *fmt = nullptr;
    if (std::strcmp(label, "yaw_func") == 0)
        fmt = "  yaw_func     %d %1.3f %1.3f %1.3f %1.3f %s\n";
    else if (std::strcmp(label, "pitch_func") == 0)
        fmt = "  pitch_func   %d %1.3f %1.3f %1.3f %1.3f %s\n";
    else if (std::strcmp(label, "roll_func") == 0)
        fmt = "  roll_func    %d %1.3f %1.3f %1.3f %1.3f %s\n";
    else if (std::strcmp(label, "scale_func") == 0)
        fmt = "  scale_func   %d %1.3f %1.3f %1.3f %1.3f %s\n";
    else if (std::strcmp(label, "scalex_func") == 0)
        fmt = "  scalex_func   %d %1.3f %1.3f %1.3f %1.3f %s\n";
    else if (std::strcmp(label, "scaley_func") == 0)
        fmt = "  scaley_func   %d %1.3f %1.3f %1.3f %1.3f %s\n";
    else if (std::strcmp(label, "scalez_func") == 0)
        fmt = "  scalez_func   %d %1.3f %1.3f %1.3f %1.3f %s\n";
    else
        fmt = "  %-13s%d %1.3f %1.3f %1.3f %1.3f %s\n";  // generic fallback
    std::fprintf(fp, fmt,
                 a->func_id,
                 static_cast<double>(a->param2), static_cast<double>(a->param3),
                 static_cast<double>(a->param0), static_cast<double>(a->param1),
                 a->ctrl_reg);
}

static bool has_detail_tex(const TdpMaterial *m) {
    const TdpMaterialTexture *d1 = tdp_find_static_tex(m, TDP_TEX_SLOT_DETAIL);
    return d1 && d1->name[0] && std::strcmp(d1->name, "0") != 0;
}

// Byte-faithful with df4oed.exe::sub_421120 @ 0x421120 (BHD's GP OED 3DA writer).
// Format strings, column padding, field order, and conditional-emission rules
// mirror that function exactly so BHD's engine reads our output the same way
// it reads OED's.
static void write_3da_material(FILE *fp, const TdpProject *proj,
                                const TdpMaterial *m, int idx,
                                int multitex_flags) {
    // Pick texture slot based on multitex_flags: 1=primary, 2=detail
    const TdpMaterialTexture *tex_entry = nullptr;
    if (multitex_flags == 2)
        tex_entry = tdp_find_static_tex(m, TDP_TEX_SLOT_DETAIL);
    else
        tex_entry = tdp_find_static_tex(m, TDP_TEX_SLOT_DIFFUSE);
    const char *tex = (tex_entry && tex_entry->name[0]) ? tex_entry->name : "";

    // Build green_texture path (append .pcx if no extension)
    char green_tex[64] = {};
    if (tex[0]) {
        copy_str(green_tex, sizeof(green_tex), tex);
        if (!std::strrchr(green_tex, '.')) {
            size_t len = std::strlen(green_tex);
            if (len + 4 < sizeof(green_tex))
                std::strcat(green_tex, ".pcx");
        }
    }

    // Classification-derived attributes
    bool is_alpha_blend = (m->classification.blend_mode == THREEDI_CLASS_BLEND_ALPHA);
    bool is_non_opaque  = (m->classification.blend_mode != THREEDI_CLASS_BLEND_OPAQUE);
    int alpha_type = is_alpha_blend ? 2 : 0;
    const char *alpha_texture = is_alpha_blend ? green_tex : "";
    int blending_mode = is_non_opaque ? 1 : 0;

    // use_alpha_pcx: 1 if classification.alpha_test set, else if alpha_test_value_byte > 0
    int use_alpha_pcx = (m->classification.alpha_test || m->alpha_test_value_byte > 0) ? 1 : 0;

    // render_attributes via synthesizer
    int has_anim = (m->animation.num_frames > 0) ? 1 : 0;
    uint32_t rattrib = synthesize_render_attributes(&m->classification, has_anim);

    // shader_type via BHD synthesizer
    uint32_t shader_type = synthesize_bhd_shader_type(&m->classification);

    // color_type from TDP material data (default 2 if zero)
    uint32_t color_type = m->color_type ? m->color_type : 2;

    // Reflect color: TDP material data is BGRA float; OED writes plain RGB ints
    int rr = clamp_255(m->reflect_color[2]);
    int rg = clamp_255(m->reflect_color[1]);
    int rb = clamp_255(m->reflect_color[0]);

    // RGB gen colors
    int sr = clamp_255(m->rgb_gen.start_color[0]);
    int sg = clamp_255(m->rgb_gen.start_color[1]);
    int sb = clamp_255(m->rgb_gen.start_color[2]);
    int er = clamp_255(m->rgb_gen.end_color[0]);
    int eg = clamp_255(m->rgb_gen.end_color[1]);
    int eb = clamp_255(m->rgb_gen.end_color[2]);

    // ctrl-reg names
    const char *rgb_reg = tdp_ctrlreg_name(proj, m->rgb_gen.reg);
    const char *alpha_reg = tdp_ctrlreg_name(proj, m->alpha_gen.reg);
    const char *u_reg = tdp_ctrlreg_name(proj, m->u_params.reg);
    const char *v_reg = tdp_ctrlreg_name(proj, m->v_params.reg);

    std::fprintf(fp, "begin material %d\n", idx);
    std::fprintf(fp, "  name \"%s\"\n", green_tex);
    std::fprintf(fp, "  description \"%s\"\n", m->name);

    // Conditional fields
    if (rattrib != 0)
        std::fprintf(fp, "  render_attributes %u\n", rattrib);
    if (multitex_flags != 0)
        std::fprintf(fp, "  multitexture_flags %d\n", multitex_flags);
    if (m->pattrib != 0)
        std::fprintf(fp, "  physical_attributes %u\n", m->pattrib);

    // Always emitted
    std::fprintf(fp, "  color_type %u\n", color_type);

    if (alpha_type != 0)
        std::fprintf(fp, "  alpha_type %d\n", alpha_type);
    if (blending_mode != 0)
        std::fprintf(fp, "  blending_mode %d\n", blending_mode);

    std::fprintf(fp, "  green_texture \"%s\"\n", green_tex);
    std::fprintf(fp, "  alpha_texture \"%s\"\n", alpha_texture);

    // Anim conditional â€” skip emission when zero
    if (m->animation.num_frames != 0)
        std::fprintf(fp, "  anim_frames %d\n", static_cast<int>(m->animation.num_frames));
    if (m->animation.cycle_frame_time != 0 && m->animation.animation_type != 1)
        std::fprintf(fp, "  anim_time %d\n",
                     static_cast<int>(m->animation.cycle_frame_time));
    if (m->anim_sequence != 0)
        std::fprintf(fp, "  anim_sequence %u\n", m->anim_sequence);

    // alpha_test
    if (m->alpha_test_value_byte != 0)
        std::fprintf(fp, "  alpha_test %d\n",
                     static_cast<int>(m->alpha_test_value_byte));

    // FFP material parameters (BHD-only, often zero from JO source)
    if (m->color_green[0] || m->color_green[1] || m->color_green[2])
        std::fprintf(fp, "  color_green %d %d %d\n",
                     m->color_green[0], m->color_green[1], m->color_green[2]);
    if (m->color_alpha[0] || m->color_alpha[1] || m->color_alpha[2])
        std::fprintf(fp, "  color_alpha %d %d %d\n",
                     m->color_alpha[0], m->color_alpha[1], m->color_alpha[2]);
    if (m->luminosity != 0)
        std::fprintf(fp, "  luminosity %u\n", m->luminosity);
    if (m->transparency != 0)
        std::fprintf(fp, "  transparency %u\n", m->transparency);
    if (m->specular_intensity != 0)
        std::fprintf(fp, "  specular_intensity %u\n", m->specular_intensity);
    if (m->specular_sharpness != 0)
        std::fprintf(fp, "  specular_sharpness %u\n", m->specular_sharpness);

    // UV transform â€” use TDP material fields, fall back to identity
    float u_off = m->u_offset;
    float v_off = m->v_offset;
    float u_til = (m->u_tiling != 0.0f) ? m->u_tiling : 1.0f;
    float v_til = (m->v_tiling != 0.0f) ? m->v_tiling : 1.0f;
    std::fprintf(fp, "  u_offset %f\n", static_cast<double>(u_off));
    std::fprintf(fp, "  v_offset %f\n", static_cast<double>(v_off));
    std::fprintf(fp, "  u_tiling %f\n", static_cast<double>(u_til));
    std::fprintf(fp, "  v_tiling %f\n", static_cast<double>(v_til));
    std::fprintf(fp, "  use_alpha_pcx %d\n", use_alpha_pcx);

    // mapfunc_u_*: OED order is style â†’ rate â†’ start â†’ end â†’ phase â†’ ctrlreg
    std::fprintf(fp, "  mapfunc_u_style  %d\n", static_cast<int>(m->u_params.style));
    std::fprintf(fp, "  mapfunc_u_rate   %f\n", static_cast<double>(m->u_params.gen_rate));
    std::fprintf(fp, "  mapfunc_u_start  %f\n", static_cast<double>(m->u_params.start));
    std::fprintf(fp, "  mapfunc_u_end    %f\n", static_cast<double>(m->u_params.end));
    std::fprintf(fp, "  mapfunc_u_phase  %f\n", static_cast<double>(m->u_params.phase));
    std::fprintf(fp, "  mapfunc_u_ctrlreg %s\n", u_reg);

    std::fprintf(fp, "  mapfunc_v_style  %d\n", static_cast<int>(m->v_params.style));
    std::fprintf(fp, "  mapfunc_v_rate   %f\n", static_cast<double>(m->v_params.gen_rate));
    std::fprintf(fp, "  mapfunc_v_start  %f\n", static_cast<double>(m->v_params.start));
    std::fprintf(fp, "  mapfunc_v_end    %f\n", static_cast<double>(m->v_params.end));
    std::fprintf(fp, "  mapfunc_v_phase  %f\n", static_cast<double>(m->v_params.phase));
    std::fprintf(fp, "  mapfunc_v_ctrlreg %s\n", v_reg);

    // rgbgen
    std::fprintf(fp, "  rgbgen_style  %d\n", static_cast<int>(m->rgb_gen.style));
    std::fprintf(fp, "  rgbgen_rate   %f\n", static_cast<double>(m->rgb_gen.rate));
    std::fprintf(fp, "  rgbgen_phase  %f\n", static_cast<double>(m->rgb_gen.phase));
    std::fprintf(fp, "  rgbgen_sr     %d\n", sr);
    std::fprintf(fp, "  rgbgen_sg     %d\n", sg);
    std::fprintf(fp, "  rgbgen_sb     %d\n", sb);
    std::fprintf(fp, "  rgbgen_er     %d\n", er);
    std::fprintf(fp, "  rgbgen_eg     %d\n", eg);
    std::fprintf(fp, "  rgbgen_eb     %d\n", eb);
    std::fprintf(fp, "  rgbgen_ctrlreg %s\n", rgb_reg);

    // alphagen: style â†’ rate â†’ start (i) â†’ end (i) â†’ phase â†’ ctrlreg
    std::fprintf(fp, "  alphagen_style  %d\n", static_cast<int>(m->alpha_gen.style));
    std::fprintf(fp, "  alphagen_rate   %f\n", static_cast<double>(m->alpha_gen.rate));
    std::fprintf(fp, "  alphagen_start  %d\n", static_cast<int>(m->alpha_gen.start));
    std::fprintf(fp, "  alphagen_end    %d\n", static_cast<int>(m->alpha_gen.end));
    std::fprintf(fp, "  alphagen_phase  %f\n", static_cast<double>(m->alpha_gen.phase));
    std::fprintf(fp, "  alphagen_ctrlreg %s\n", alpha_reg);

    // Reflect / projector / shader
    std::fprintf(fp, "  reflect_r    %d\n", rr);
    std::fprintf(fp, "  reflect_g    %d\n", rg);
    std::fprintf(fp, "  reflect_b    %d\n", rb);
    std::fprintf(fp, "  reflect_type %u\n", m->reflect_type);
    std::fprintf(fp, "  reflect_alpha %u\n", m->reflect_alpha);
    std::fprintf(fp, "  actionplane_type     %u\n", m->actionplane_type);
    std::fprintf(fp, "  projector_type       %u\n", m->projector_type);
    std::fprintf(fp, "  projector_no_receive %u\n", m->projector_no_receive);
    std::fprintf(fp, "  projector_yaw       %u\n", m->projector_yaw);
    std::fprintf(fp, "  projector_pitch     %u\n", m->projector_pitch);
    std::fprintf(fp, "  shader_type         %u\n", shader_type);
    std::fprintf(fp, "end material\n\n");
}

int tdp_write_3da(const char *path, const TdpProject *proj) {
    FILE *fp = std::fopen(path, "w");
    if (!fp) return -1;

    const TdpLod *lod0 = &proj->lods[0];

    // Compute 3DA material count (with multi-texture splitting)
    int num_3da_materials = 0;
    for (size_t i = 0; i < proj->material_count; ++i) {
        ++num_3da_materials;
        if (has_detail_tex(&proj->materials[i]))
            ++num_3da_materials;
    }

    // Header timestamp
    {
        std::time_t now = std::time(nullptr);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        // df4oed emits ONE \n after the comment plus a blank line, i.e.
        // two newlines after the comment line.  This produces an empty
        // line at line 2 and an empty line at line 3 before the section.
        std::fprintf(fp, "/ 3DI metafile, saved on %d/%d/%d, %d:%02d\n\n\n",
                     local.tm_mon + 1, local.tm_mday, local.tm_year + 1900,
                     local.tm_hour, local.tm_min);
    }

    // general_information section
    std::fprintf(fp, "begin general_information\n");
    std::fprintf(fp, "  3da_version %d\n", 3);
    std::fprintf(fp, "  3di_version %d\n", 2);
    std::fprintf(fp, "  username    %s\n",
                 proj->username[0] ? proj->username : "opennova");
    std::fprintf(fp, "  attributes: %d\n", lod0->attributes ? lod0->attributes : 1);
    std::fprintf(fp, "  render_function %s\n",
                 lod0->render_function[0] ? lod0->render_function : "gnrc");
    std::fprintf(fp, "  threshold %f\n", static_cast<double>(lod0->threshold));
    std::fprintf(fp, "  num_materials %d\n", num_3da_materials);
    std::fprintf(fp, "  scale_factor %f\n", 1.0);
    std::fprintf(fp, "  diffuse_set  TRUE\n");
    std::fprintf(fp, "  pm_enable     %d\n", 0);
    std::fprintf(fp, "  pm_polythresh %d\n", 0);
    std::fprintf(fp, "  part_anim_enable %d\n", lod0->part_anim_enabled);
    std::fprintf(fp, "  fakeskin_z %d\n", 0);
    std::fprintf(fp, "  polycollision %d\n", proj->poly_collision_lod);
    std::fprintf(fp, "end general_information\n\n");

    // Materials â€” split by texture slot
    int tda_mat_idx = 0;
    for (size_t i = 0; i < proj->material_count; ++i) {
        const TdpMaterial *m = &proj->materials[i];
        write_3da_material(fp, proj, m, tda_mat_idx, 1);
        ++tda_mat_idx;
        if (has_detail_tex(m)) {
            write_3da_material(fp, proj, m, tda_mat_idx, 2);
            ++tda_mat_idx;
        }
    }

    // Part animations
    for (size_t p = 0; p < lod0->part_anim_count; ++p) {
        const TdpPartAnim *pa = &lod0->part_anims[p];
        std::fprintf(fp, "begin part_animation %zu\n", p);
        std::fprintf(fp, "  rotate_type  %d\n", pa->rotate_type);
        std::fprintf(fp, "  scale_type   %d\n", pa->scale_type);
        std::fprintf(fp, "  transform_as %d\n", pa->transform_as);
        std::fprintf(fp, "  yaw_rate     %f\n", static_cast<double>(pa->yaw_rate));
        std::fprintf(fp, "  pitch_rate   %f\n", static_cast<double>(pa->pitch_rate));
        std::fprintf(fp, "  roll_rate    %f\n", static_cast<double>(pa->roll_rate));
        write_axis_3da(fp, "yaw_func", &pa->yaw);
        write_axis_3da(fp, "pitch_func", &pa->pitch);
        write_axis_3da(fp, "roll_func", &pa->roll);
        std::fprintf(fp, "  reverserotate    %d\n", pa->reverse_rotate);
        write_axis_3da(fp, "scale_func", &pa->scale);
        write_axis_3da(fp, "scalex_func", &pa->scale_x);
        write_axis_3da(fp, "scaley_func", &pa->scale_y);
        write_axis_3da(fp, "scalez_func", &pa->scale_z);
        std::fprintf(fp, "end part_animation\n\n");
    }

    // Lights
    for (size_t j = 0; j < lod0->light_count; ++j) {
        const TdpLight *lt = &lod0->lights[j];
        std::fprintf(fp, "begin light %zu\n", j);
        std::fprintf(fp, "  name \"%s\"\n", lt->name);
        std::fprintf(fp, "  colorgen_style %i\n", lt->colorgen_style);
        std::fprintf(fp, "  colorgen_rate %f\n", static_cast<double>(lt->colorgen_rate));
        std::fprintf(fp, "  colorgen_phase %f\n", static_cast<double>(lt->colorgen_phase));
        std::fprintf(fp, "  colorgen_start %i %i %i\n",
                     lt->colorgen_start[0], lt->colorgen_start[1], lt->colorgen_start[2]);
        std::fprintf(fp, "  colorgen_end %i %i %i\n",
                     lt->colorgen_end[0], lt->colorgen_end[1], lt->colorgen_end[2]);
        std::fprintf(fp, "  colorgen_ctrlreg %s\n", lt->colorgen_ctrlreg);
        std::fprintf(fp, "end\n\n");
    }

    std::fclose(fp);
    return 0;
}
