#include "tdp/tdp.h"
#include "threedi/threedi_ir.h"
#include "threedi/threedi_panm.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <io/strutil.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void copy_str(char *dst, size_t dst_size, const char *src) {
    if (!src || !dst || dst_size == 0) return;
    size_t len = std::strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    std::memcpy(dst, src, len);
    dst[len] = '\0';
}

using opennova::strutil::iequals;

static int to_int(const char *s) {
    if (!s || !*s) return 0;
    return static_cast<int>(std::strtol(s, nullptr, 10));
}

static float to_float(const char *s) {
    if (!s || !*s) return 0.0f;
    return std::strtof(s, nullptr);
}

// ---------------------------------------------------------------------------
// Tokenizer (ported from old project_parser.cpp)
// ---------------------------------------------------------------------------

#define MAX_TOKENS 30

struct Tokens {
    char buf[1024];
    const char *toks[MAX_TOKENS];
    int count;
};

static void tokenize(Tokens *t, const char *line) {
    t->count = 0;
    if (!line || !*line) return;

    size_t n = std::strlen(line);
    if (n >= sizeof(t->buf)) n = sizeof(t->buf) - 1;
    for (size_t i = 0; i < n; ++i) {
        char c = line[i];
        if (c == '\r') c = '\0';
        t->buf[i] = c;
    }
    // Zero from n onwards so the token scanner doesn't find garbage.
    std::memset(t->buf + n, 0, sizeof(t->buf) - n);

    bool in_quote = false;
    for (size_t i = 0; t->buf[i]; ++i) {
        if (!in_quote && t->buf[i] == ';') { t->buf[i] = '\0'; break; }
        if (!in_quote && t->buf[i] == '/' && t->buf[i + 1] == '/') { t->buf[i] = '\0'; break; }
        if (t->buf[i] == '"') {
            in_quote = !in_quote;
            t->buf[i] = '\0';
        } else if (!in_quote && (t->buf[i] == ' ' || t->buf[i] == '\t' || t->buf[i] == ',')) {
            t->buf[i] = '\0';
        }
    }

    for (size_t i = 0; i < sizeof(t->buf) && t->count < MAX_TOKENS;) {
        while (i < sizeof(t->buf) && t->buf[i] == '\0') ++i;
        if (i >= sizeof(t->buf) || !t->buf[i]) break;
        t->toks[t->count++] = &t->buf[i];
        while (i < sizeof(t->buf) && t->buf[i]) ++i;
    }
}

static const char *tok_at(const Tokens *t, int idx) {
    if (idx < 0 || idx >= t->count) return "";
    return t->toks[idx];
}

// ---------------------------------------------------------------------------
// Init / Free
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
    for (int i = 0; i < TDP_MAX_LODS; ++i) {
        std::free(proj->lods[i].part_anims);
        proj->lods[i].part_anims = nullptr;
        proj->lods[i].part_anim_count = 0;
        std::free(proj->lods[i].lights);
        proj->lods[i].lights = nullptr;
        proj->lods[i].light_count = 0;
    }
}

// ---------------------------------------------------------------------------
// Allocators (for FFI callers)
// ---------------------------------------------------------------------------

void tdp_alloc_materials(TdpProject *proj, size_t count) {
    if (!proj) return;
    std::free(proj->materials);
    proj->materials = nullptr;
    proj->material_count = 0;
    if (count > 0) {
        proj->materials = static_cast<TdpMaterial *>(std::calloc(count, sizeof(TdpMaterial)));
        if (proj->materials) proj->material_count = count;
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
// Parser
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

static TdpMaterial *ensure_material(TdpProject *proj, size_t idx) {
    if (idx >= proj->material_count) {
        size_t new_count = idx + 1;
        proj->materials = static_cast<TdpMaterial *>(
            std::realloc(proj->materials, new_count * sizeof(TdpMaterial)));
        for (size_t i = proj->material_count; i < new_count; ++i)
            std::memset(&proj->materials[i], 0, sizeof(TdpMaterial));
        proj->material_count = new_count;
    }
    return &proj->materials[idx];
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

// Extract quoted string from raw line for name/path fields
static void extract_quoted(const char *line, char *dst, size_t dst_size) {
    const char *q0 = std::strchr(line, '"');
    if (!q0) { dst[0] = '\0'; return; }
    const char *q1 = std::strchr(q0 + 1, '"');
    if (!q1 || q1 <= q0 + 1) { dst[0] = '\0'; return; }
    size_t len = static_cast<size_t>(q1 - q0 - 1);
    if (len >= dst_size) len = dst_size - 1;
    std::memcpy(dst, q0 + 1, len);
    dst[len] = '\0';
}

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

    char line[1024];
    while (std::fgets(line, sizeof(line), fp)) {
        // Strip trailing newline
        size_t len = std::strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        Tokens t;
        tokenize(&t, line);
        if (t.count == 0) continue;

        const char *tag = t.toks[0];

        if (tag[0] == '}') {
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
                    state_stack[stack_depth++] = ST_MATERIAL;
                }
            }
            break;

        case ST_MATERIAL: {
            TdpMaterial *mat = &out->materials[current_material];
            if (iequals(tag, "name")) {
                if (t.count > 1 && tok_at(&t, 1)[0])
                    copy_str(mat->name, sizeof(mat->name), tok_at(&t, 1));
                else
                    extract_quoted(line, mat->name, sizeof(mat->name));
            } else if (iequals(tag, "shadertag")) {
                if (t.count > 1 && tok_at(&t, 1)[0])
                    copy_str(mat->shader_tag, sizeof(mat->shader_tag), tok_at(&t, 1));
                else
                    extract_quoted(line, mat->shader_tag, sizeof(mat->shader_tag));
            } else if (iequals(tag, "rattrib")) {
                mat->rattrib = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "pattrib")) {
                mat->pattrib = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "ptype")) {
                mat->ptype = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "geofx")) {
                mat->geofx = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "geofx_value")) {
                mat->geofx_value = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "alphatestvalue")) {
                mat->alphatestvalue = to_int(tok_at(&t, 1));
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
                    if (is_normal) {
                        copy_str(mat->normal_tex[slot], sizeof(mat->normal_tex[slot]), tex_path);
                        mat->normal_flags[slot] = (flag_val & 1) | (mat->normal_flags[slot] & ~1);
                    } else {
                        copy_str(mat->diffuse_tex[slot], sizeof(mat->diffuse_tex[slot]), tex_path);
                        mat->diffuse_flags[slot] = (flag_val & 1) | (mat->diffuse_flags[slot] & ~1);
                    }
                }
            } else if (iequals(tag, "anim_frames")) {
                mat->anim_frames = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "anim_type")) {
                mat->anim_type = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "anim_frametime")) {
                mat->anim_frametime = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "anim_ctrlreg")) {
                if (t.count > 1 && tok_at(&t, 1)[0])
                    copy_str(mat->anim_ctrlreg, sizeof(mat->anim_ctrlreg), tok_at(&t, 1));
                else
                    extract_quoted(line, mat->anim_ctrlreg, sizeof(mat->anim_ctrlreg));
            } else if (std::strstr(tag, "anim_diffusetex") || std::strstr(tag, "anim_normaltex") ||
                       std::strstr(tag, "anim_DIFFUSETEX") || std::strstr(tag, "anim_NORMALTEX")) {
                // anim_diffusetex[0] or anim_normaltex[1] etc.
                bool is_normal = (std::strstr(tag, "normal") != nullptr || std::strstr(tag, "NORMAL") != nullptr);
                int tex_slot = (std::strstr(tag, "[1]") != nullptr) ? 1 : 0;
                int frame = to_int(tok_at(&t, 1));
                if (frame >= 0 && frame < TDP_MAX_ANIM_FRAMES) {
                    TdpAnimFrame *dst = is_normal
                        ? &mat->anim_normal[tex_slot][frame]
                        : &mat->anim_diffuse[tex_slot][frame];
                    const char *fpath = tok_at(&t, 2);
                    if (!fpath[0]) extract_quoted(line, dst->path, sizeof(dst->path));
                    else copy_str(dst->path, sizeof(dst->path), fpath);
                    dst->enabled = to_int(tok_at(&t, 3)) & 1;
                }
            } else if (iequals(tag, "reflect_rgb")) {
                mat->reflect_rgb[0] = to_int(tok_at(&t, 1));
                mat->reflect_rgb[1] = to_int(tok_at(&t, 2));
                mat->reflect_rgb[2] = to_int(tok_at(&t, 3));
            } else if (iequals(tag, "rgbgen_style")) {
                mat->rgbgen_style = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "rgbgen_rate")) {
                mat->rgbgen_rate = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "rgbgen_phase")) {
                mat->rgbgen_phase = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "rgbgen_srgb")) {
                mat->rgbgen_srgb[0] = to_int(tok_at(&t, 1));
                mat->rgbgen_srgb[1] = to_int(tok_at(&t, 2));
                mat->rgbgen_srgb[2] = to_int(tok_at(&t, 3));
            } else if (iequals(tag, "rgbgen_ergb")) {
                mat->rgbgen_ergb[0] = to_int(tok_at(&t, 1));
                mat->rgbgen_ergb[1] = to_int(tok_at(&t, 2));
                mat->rgbgen_ergb[2] = to_int(tok_at(&t, 3));
            } else if (iequals(tag, "rgbgen_ctrlreg")) {
                if (t.count > 1 && tok_at(&t, 1)[0])
                    copy_str(mat->rgbgen_ctrlreg, sizeof(mat->rgbgen_ctrlreg), tok_at(&t, 1));
                else
                    extract_quoted(line, mat->rgbgen_ctrlreg, sizeof(mat->rgbgen_ctrlreg));
            } else if (iequals(tag, "alphagen_style")) {
                mat->alphagen_style = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "alphagen_rate")) {
                mat->alphagen_rate = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "alphagen_phase")) {
                mat->alphagen_phase = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "alphagen_start")) {
                mat->alphagen_start = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "alphagen_end")) {
                mat->alphagen_end = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "alphagen_ctrlreg")) {
                if (t.count > 1 && tok_at(&t, 1)[0])
                    copy_str(mat->alphagen_ctrlreg, sizeof(mat->alphagen_ctrlreg), tok_at(&t, 1));
                else
                    extract_quoted(line, mat->alphagen_ctrlreg, sizeof(mat->alphagen_ctrlreg));
            } else if (iequals(tag, "mapfunc_u_style")) {
                mat->mapfunc_u_style = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_u_rate")) {
                mat->mapfunc_u_rate = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_u_phase")) {
                mat->mapfunc_u_phase = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_u_start")) {
                mat->mapfunc_u_start = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_u_end")) {
                mat->mapfunc_u_end = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_u_ctrlreg")) {
                if (t.count > 1 && tok_at(&t, 1)[0])
                    copy_str(mat->mapfunc_u_ctrlreg, sizeof(mat->mapfunc_u_ctrlreg), tok_at(&t, 1));
                else
                    extract_quoted(line, mat->mapfunc_u_ctrlreg, sizeof(mat->mapfunc_u_ctrlreg));
            } else if (iequals(tag, "mapfunc_v_style")) {
                mat->mapfunc_v_style = to_int(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_v_rate")) {
                mat->mapfunc_v_rate = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_v_phase")) {
                mat->mapfunc_v_phase = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_v_start")) {
                mat->mapfunc_v_start = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_v_end")) {
                mat->mapfunc_v_end = to_float(tok_at(&t, 1));
            } else if (iequals(tag, "mapfunc_v_ctrlreg")) {
                if (t.count > 1 && tok_at(&t, 1)[0])
                    copy_str(mat->mapfunc_v_ctrlreg, sizeof(mat->mapfunc_v_ctrlreg), tok_at(&t, 1));
                else
                    extract_quoted(line, mat->mapfunc_v_ctrlreg, sizeof(mat->mapfunc_v_ctrlreg));
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
// Writer
// ---------------------------------------------------------------------------

static void write_axis(FILE *fp, const char *label, const TdpAxisFunc *a) {
    std::fprintf(fp, "            %-16s%2i  %1.3f %1.3f %1.3f %1.3f %s\n",
                 label, a->func_id,
                 static_cast<double>(a->param2), static_cast<double>(a->param3),
                 static_cast<double>(a->param0), static_cast<double>(a->param1),
                 a->ctrl_reg);
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
        std::fprintf(fp, "    material %zu\n    {\n", i);
        std::fprintf(fp, "        name             \"%s\"\n", m->name);
        std::fprintf(fp, "        shadertag        \"%s\"\n", m->shader_tag);
        std::fprintf(fp, "        rattrib          %d\n", m->rattrib);
        std::fprintf(fp, "        pattrib          %d\n", m->pattrib);
        std::fprintf(fp, "        ptype            %d\n", m->ptype);
        std::fprintf(fp, "        geofx            %d\n", m->geofx);
        std::fprintf(fp, "        geofx_value      %5.2f\n", static_cast<double>(m->geofx_value));
        std::fprintf(fp, "        alphatestvalue   %d\n", m->alphatestvalue);
        std::fprintf(fp, "        diffusetex[0]    \"%s\"  %d\n", m->diffuse_tex[0], m->diffuse_flags[0]);
        std::fprintf(fp, "        diffusetex[1]    \"%s\"  %d\n", m->diffuse_tex[1], m->diffuse_flags[1]);
        std::fprintf(fp, "        normaltex[0]     \"%s\"  %d\n", m->normal_tex[0], m->normal_flags[0]);
        std::fprintf(fp, "        normaltex[1]     \"%s\"  %d\n", m->normal_tex[1], m->normal_flags[1]);
        std::fprintf(fp, "        anim_frames      %d\n", m->anim_frames);
        std::fprintf(fp, "        anim_type        %d\n", m->anim_type);
        std::fprintf(fp, "        anim_frametime   %d\n", m->anim_frametime);
        std::fprintf(fp, "        anim_ctrlreg     \"%s\"\n", m->anim_ctrlreg);

        // Write anim frames — engine always writes all 4 slots × all frames
        // with specific format strings matching the engine binary output.
        for (int f = 0; f < m->anim_frames && f < TDP_MAX_ANIM_FRAMES; ++f)
            std::fprintf(fp, "        anim_diffusetex[0] %d \"%s\"   %d\n",
                         f, m->anim_diffuse[0][f].path, m->anim_diffuse[0][f].enabled);
        for (int f = 0; f < m->anim_frames && f < TDP_MAX_ANIM_FRAMES; ++f)
            std::fprintf(fp, "        anim_diffusetex[1] %d \"%s\"  %d\n",
                         f, m->anim_diffuse[1][f].path, m->anim_diffuse[1][f].enabled);
        for (int f = 0; f < m->anim_frames && f < TDP_MAX_ANIM_FRAMES; ++f)
            std::fprintf(fp, "        anim_normaltex[0]  %d \"%s\"  %d\n",
                         f, m->anim_normal[0][f].path, m->anim_normal[0][f].enabled);
        for (int f = 0; f < m->anim_frames && f < TDP_MAX_ANIM_FRAMES; ++f)
            std::fprintf(fp, "        anim_normaltex[1]  %d \"%s\"  %d\n",
                         f, m->anim_normal[1][f].path, m->anim_normal[1][f].enabled);

        std::fprintf(fp, "        reflect_rgb    %d %d %d\n",
                     m->reflect_rgb[0], m->reflect_rgb[1], m->reflect_rgb[2]);
        std::fprintf(fp, "        rgbgen_style    %d\n", m->rgbgen_style);
        std::fprintf(fp, "        rgbgen_rate     %f\n", static_cast<double>(m->rgbgen_rate));
        std::fprintf(fp, "        rgbgen_phase    %f\n", static_cast<double>(m->rgbgen_phase));
        std::fprintf(fp, "        rgbgen_srgb     %d %d %d\n",
                     m->rgbgen_srgb[0], m->rgbgen_srgb[1], m->rgbgen_srgb[2]);
        std::fprintf(fp, "        rgbgen_ergb     %d %d %d\n",
                     m->rgbgen_ergb[0], m->rgbgen_ergb[1], m->rgbgen_ergb[2]);
        std::fprintf(fp, "        rgbgen_ctrlreg  %s\n", m->rgbgen_ctrlreg);
        std::fprintf(fp, "        alphagen_style   %d\n", m->alphagen_style);
        std::fprintf(fp, "        alphagen_rate    %f\n", static_cast<double>(m->alphagen_rate));
        std::fprintf(fp, "        alphagen_phase   %f\n", static_cast<double>(m->alphagen_phase));
        std::fprintf(fp, "        alphagen_start   %i\n", static_cast<int>(m->alphagen_start));
        std::fprintf(fp, "        alphagen_end     %i\n", static_cast<int>(m->alphagen_end));
        std::fprintf(fp, "        alphagen_ctrlreg %s\n", m->alphagen_ctrlreg);
        std::fprintf(fp, "        mapfunc_u_style   %d\n", m->mapfunc_u_style);
        std::fprintf(fp, "        mapfunc_u_rate    %f\n", static_cast<double>(m->mapfunc_u_rate));
        std::fprintf(fp, "        mapfunc_u_phase   %f\n", static_cast<double>(m->mapfunc_u_phase));
        std::fprintf(fp, "        mapfunc_u_start   %f\n", static_cast<double>(m->mapfunc_u_start));
        std::fprintf(fp, "        mapfunc_u_end     %f\n", static_cast<double>(m->mapfunc_u_end));
        std::fprintf(fp, "        mapfunc_u_ctrlreg %s\n", m->mapfunc_u_ctrlreg);
        std::fprintf(fp, "        mapfunc_v_style   %d\n", m->mapfunc_v_style);
        std::fprintf(fp, "        mapfunc_v_rate    %f\n", static_cast<double>(m->mapfunc_v_rate));
        std::fprintf(fp, "        mapfunc_v_phase   %f\n", static_cast<double>(m->mapfunc_v_phase));
        std::fprintf(fp, "        mapfunc_v_start   %f\n", static_cast<double>(m->mapfunc_v_start));
        std::fprintf(fp, "        mapfunc_v_end     %f\n", static_cast<double>(m->mapfunc_v_end));
        std::fprintf(fp, "        mapfunc_v_ctrlreg %s\n", m->mapfunc_v_ctrlreg);
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

        // Part animation
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

        // Lights
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
// tdp_from_ir — populate TdpProject from ThreediModelIR
// ---------------------------------------------------------------------------

static void resolve_mat_ctrlreg(char *dst, size_t dst_size,
                                 uint8_t style, int32_t reg,
                                 const ThreediIRControlRegister *ctrl_regs,
                                 size_t ctrl_reg_count) {
    if (style > 0x70 && reg >= 0 && (size_t)reg < ctrl_reg_count && ctrl_regs) {
        copy_str(dst, dst_size, ctrl_regs[reg].name);
    }
}

static int surface_type_to_ptype(uint8_t st) {
    switch (st) {
        case 0x0E: return 0;  // Metal
        case 0x0D: return 1;  // Wood
        case 0x0C: return 2;  // Stone
        case 0x11: return 3;  // Foliage
        case 0x12: return 4;  // Hard Metal
        case 0x10: return 5;  // Cloth
        case 0x0F: return 6;  // Glass
        case 0x07: return 7;  // Water
        case 0x13: return 8;  // Flesh
        case 0x01: return 9;  // Dirt
        default:   return 9;
    }
}

static int clamp_255(float v) {
    int r = static_cast<int>(v * 255.0f);
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    return r;
}

static void ir_transform_to_axis(const ThreediIRTransform *xf, TdpAxisFunc *out,
                                  bool is_rotation,
                                  const ThreediIRControlRegister *ctrl_regs,
                                  size_t ctrl_reg_count) {
    std::memset(out, 0, sizeof(*out));
    out->func_id = xf->control;
    // Inverse of export_3di.cpp encoding:
    // out.rate = clamp_s16_from_float(f.param0 * 256.0f)  =>  param0 = rate / 256.0
    // out.control_param = clamp_u8_from_float(f.param1 * 256.0f)  =>  param1 = control_param / 256.0
    // out.start = clamp_s16_from_float(f.param2 * scale)  =>  param2 = start / scale
    // out.end = clamp_s16_from_float(f.param3 * scale)  =>  param3 = end / scale
    out->param0 = xf->rate / 256.0f;
    out->param1 = xf->control_param / 256.0f;
    float scale = is_rotation ? (16384.0f / 360.0f) : 256.0f;
    out->param2 = xf->start / scale;
    out->param3 = xf->end / scale;

    // Register-based control functions (codes 113-117, i.e. > 0x70):
    // control_param is an index into the control register table, not a phase.
    if (xf->control > 0x70 && ctrl_regs && xf->control_param < ctrl_reg_count) {
        copy_str(out->ctrl_reg, sizeof(out->ctrl_reg),
                 ctrl_regs[xf->control_param].name);
        out->param1 = 0.0f;
    }
}

// Populate a TdpLod's part_anims from an IR LOD's part_animations.
// Returns 0 on success, -1 on allocation failure.
static int populate_tdp_lod_panm(TdpLod *lod, const ThreediIRLod *ir_lod,
                                  const ThreediModelIR *ir) {
    // Part count from this LOD
    size_t part_count = 0;
    if (ir_lod->declared_part_count > 0)
        part_count = (size_t)ir_lod->declared_part_count;
    else
        part_count = ir_lod->part_count;

    // Check if any PANM entry has non-zero flags
    bool has_panm = false;
    for (size_t i = 0; i < ir_lod->part_animation_count; ++i) {
        if (ir_lod->part_animations[i].flags != 0) { has_panm = true; break; }
    }
    size_t panm_count = part_count;
    if (has_panm && ir_lod->part_animation_count > part_count)
        panm_count = ir_lod->part_animation_count;
    lod->part_anim_enabled = has_panm ? 1 : 0;

    if (panm_count == 0) return 0;

    lod->part_anims = static_cast<TdpPartAnim *>(
        std::calloc(panm_count, sizeof(TdpPartAnim)));
    if (!lod->part_anims) return -1;
    lod->part_anim_count = panm_count;

    if (has_panm) {
        for (size_t i = 0; i < ir_lod->part_animation_count; ++i) {
            const ThreediIRPartAnimation *src = &ir_lod->part_animations[i];
            TdpPartAnim *dst = &lod->part_anims[i];
            dst->transform_as = src->part_index;

            uint32_t flags = src->flags;
            dst->rotate_type = threedi_panm_rotation_type(flags);
            dst->scale_type = threedi_panm_scale_type(flags);
            dst->trans_type = threedi_panm_translate_type(flags);
            dst->reverse_rotate = threedi_panm_rotation_reversed(flags);

            const ThreediIRControlRegister *cregs = ir->control_registers;
            size_t creg_count = ir->control_register_count;

            if (dst->rotate_type == 2) {
                ir_transform_to_axis(&src->rotation_x, &dst->yaw, true, cregs, creg_count);
                ir_transform_to_axis(&src->rotation_y, &dst->pitch, true, cregs, creg_count);
                ir_transform_to_axis(&src->rotation_z, &dst->roll, true, cregs, creg_count);
            }

            if (dst->scale_type == 1) {
                ir_transform_to_axis(&src->scale_x, &dst->scale, false, cregs, creg_count);
            } else if (dst->scale_type == 2) {
                ir_transform_to_axis(&src->scale_x, &dst->scale_x, false, cregs, creg_count);
                ir_transform_to_axis(&src->scale_y, &dst->scale_y, false, cregs, creg_count);
                ir_transform_to_axis(&src->scale_z, &dst->scale_z, false, cregs, creg_count);
            }

            uint8_t tt = dst->trans_type;
            if (tt == 1)
                ir_transform_to_axis(&src->translation, &dst->trans_x, false, cregs, creg_count);
            else if (tt == 2)
                ir_transform_to_axis(&src->translation, &dst->trans_y, false, cregs, creg_count);
            else if (tt == 3)
                ir_transform_to_axis(&src->translation, &dst->trans_z, false, cregs, creg_count);
        }
        for (size_t i = ir_lod->part_animation_count; i < panm_count; ++i) {
            lod->part_anims[i].transform_as = static_cast<int32_t>(i);
        }
    } else {
        for (size_t i = 0; i < panm_count; ++i) {
            lod->part_anims[i].transform_as = static_cast<int32_t>(i);
        }
    }
    return 0;
}

int tdp_from_ir(const ThreediModelIR *ir, TdpProject *out) {
    if (!ir || !out) return -1;
    tdp_init(out);

    // Materials
    size_t num_mats = ir->material_count;
    if (num_mats > 0) {
        out->materials = static_cast<TdpMaterial *>(std::calloc(num_mats, sizeof(TdpMaterial)));
        if (!out->materials) return -1;
        out->material_count = num_mats;

        for (size_t i = 0; i < num_mats; ++i) {
            const ThreediIRMaterial *src = &ir->materials[i];
            TdpMaterial *dst = &out->materials[i];

            // Shader tag
            const char *shader = src->shader_name;
            if (!shader[0]) shader = "FF_ST_OP";
            copy_str(dst->shader_tag, sizeof(dst->shader_tag), shader);

            // Material name
            char name_buf[80];
            std::snprintf(name_buf, sizeof(name_buf), "Material_%zu_%s", i, dst->shader_tag);
            copy_str(dst->name, sizeof(dst->name), name_buf);

            // Surface type → ptype mapping
            dst->ptype = surface_type_to_ptype(src->surface_type);

            // Collision polygon attributes
            dst->pattrib = src->pattrib;

            // Default normal texture sentinel (OED stores "0" for empty slots)
            copy_str(dst->normal_tex[0], sizeof(dst->normal_tex[0]), "0");
            copy_str(dst->normal_tex[1], sizeof(dst->normal_tex[1]), "0");

            // Textures by slot
            for (uint32_t t = 0; t < src->texture_count && t < 8; ++t) {
                const ThreediIRMaterialTexture *tex = &src->textures[t];
                if (!tex->name[0]) continue;
                int clamped = (tex->flags >> 1) & 1;

                // Determine TDP slot index (0 or 1) and whether diffuse or normal
                int slot_idx = -1;
                bool is_normal = false;
                switch (tex->slot) {
                case THREEDI_IR_TEX_SLOT_DIFFUSE:  slot_idx = 0; break;
                case THREEDI_IR_TEX_SLOT_DETAIL:   slot_idx = 1; break;
                case THREEDI_IR_TEX_SLOT_NORMAL:   slot_idx = 0; is_normal = true; break;
                case THREEDI_IR_TEX_SLOT_NORMAL_B: slot_idx = 1; is_normal = true; break;
                default: continue;
                }

                if (tex->flags & THREEDI_TEX_FLAG_ANIMATED) {
                    // Animated frame
                    if (tex->frame < TDP_MAX_ANIM_FRAMES) {
                        TdpAnimFrame *af = is_normal
                            ? &dst->anim_normal[slot_idx][tex->frame]
                            : &dst->anim_diffuse[slot_idx][tex->frame];
                        copy_str(af->path, sizeof(af->path), tex->name);
                        af->enabled = clamped;
                        dst->anim_frames = std::max(dst->anim_frames, static_cast<int32_t>(tex->frame + 1));
                    }
                } else {
                    // Static texture (frame 0)
                    if (is_normal) {
                        copy_str(dst->normal_tex[slot_idx], sizeof(dst->normal_tex[slot_idx]), tex->name);
                        dst->normal_flags[slot_idx] = clamped;
                    } else {
                        copy_str(dst->diffuse_tex[slot_idx], sizeof(dst->diffuse_tex[slot_idx]), tex->name);
                        dst->diffuse_flags[slot_idx] = clamped;
                    }
                }
            }

            // For animated materials, set diffuse_tex from anim frame 0
            // (engine stores base diffuse separately from anim frames)
            for (int s = 0; s < 2; ++s) {
                if (!dst->diffuse_tex[s][0] && dst->anim_diffuse[s][0].path[0])
                    copy_str(dst->diffuse_tex[s], sizeof(dst->diffuse_tex[s]),
                             dst->anim_diffuse[s][0].path);
            }

            // Reflect color (float 0-1 -> int 0-255)
            dst->reflect_rgb[0] = clamp_255(src->reflect_color[0]);
            dst->reflect_rgb[1] = clamp_255(src->reflect_color[1]);
            dst->reflect_rgb[2] = clamp_255(src->reflect_color[2]);

            // Alpha test
            dst->alphatestvalue = clamp_255(src->alpha_threshold);

            // Animation params
            dst->anim_frames = std::max(dst->anim_frames, static_cast<int32_t>(src->animation.num_frames));
            dst->anim_type = src->animation.animation_type;
            // For ctrl-reg-driven animation (type 1), cycle_frame_time is the
            // register index, not a frame time — write 0 as the engine does.
            dst->anim_frametime = (src->animation.animation_type == 1) ? 0
                                : src->animation.cycle_frame_time;

            // RGB gen
            dst->rgbgen_style = src->rgb_gen.style;
            dst->rgbgen_rate = src->rgb_gen.rate;
            dst->rgbgen_phase = src->rgb_gen.phase;
            dst->rgbgen_srgb[0] = clamp_255(src->rgb_gen.start_color[0]);
            dst->rgbgen_srgb[1] = clamp_255(src->rgb_gen.start_color[1]);
            dst->rgbgen_srgb[2] = clamp_255(src->rgb_gen.start_color[2]);
            dst->rgbgen_ergb[0] = clamp_255(src->rgb_gen.end_color[0]);
            dst->rgbgen_ergb[1] = clamp_255(src->rgb_gen.end_color[1]);
            dst->rgbgen_ergb[2] = clamp_255(src->rgb_gen.end_color[2]);

            // Alpha gen
            dst->alphagen_style = src->alpha_gen.style;
            dst->alphagen_rate = src->alpha_gen.rate;
            dst->alphagen_phase = src->alpha_gen.phase;
            dst->alphagen_start = static_cast<float>(src->alpha_gen.start);
            dst->alphagen_end = static_cast<float>(src->alpha_gen.end);

            // Map func U
            dst->mapfunc_u_style = src->u_params.style;
            dst->mapfunc_u_rate = src->u_params.gen_rate;
            dst->mapfunc_u_phase = src->u_params.phase;
            dst->mapfunc_u_start = src->u_params.start;
            dst->mapfunc_u_end = src->u_params.end;

            // Map func V
            dst->mapfunc_v_style = src->v_params.style;
            dst->mapfunc_v_rate = src->v_params.gen_rate;
            dst->mapfunc_v_phase = src->v_params.phase;
            dst->mapfunc_v_start = src->v_params.start;
            dst->mapfunc_v_end = src->v_params.end;

            // Resolve control register names from IR register indices
            resolve_mat_ctrlreg(dst->rgbgen_ctrlreg, sizeof(dst->rgbgen_ctrlreg),
                                src->rgb_gen.style, src->rgb_gen.reg,
                                ir->control_registers, ir->control_register_count);
            resolve_mat_ctrlreg(dst->alphagen_ctrlreg, sizeof(dst->alphagen_ctrlreg),
                                src->alpha_gen.style, src->alpha_gen.reg,
                                ir->control_registers, ir->control_register_count);
            resolve_mat_ctrlreg(dst->mapfunc_u_ctrlreg, sizeof(dst->mapfunc_u_ctrlreg),
                                src->u_params.style, src->u_params.reg,
                                ir->control_registers, ir->control_register_count);
            resolve_mat_ctrlreg(dst->mapfunc_v_ctrlreg, sizeof(dst->mapfunc_v_ctrlreg),
                                src->v_params.style, src->v_params.reg,
                                ir->control_registers, ir->control_register_count);

            // Animation control register (type 1 = ctrl reg driven, cycle_frame_time is reg index)
            if (src->animation.animation_type == 1 &&
                src->animation.cycle_frame_time >= 0 &&
                (size_t)src->animation.cycle_frame_time < ir->control_register_count &&
                ir->control_registers) {
                copy_str(dst->anim_ctrlreg, sizeof(dst->anim_ctrlreg),
                         ir->control_registers[src->animation.cycle_frame_time].name);
            }

            // Fill empty anim texture slots with "0" sentinel (engine always
            // writes all 4 slots × all frames)
            if (dst->anim_frames > 0) {
                for (int s = 0; s < 2; ++s) {
                    for (int f = 0; f < dst->anim_frames && f < TDP_MAX_ANIM_FRAMES; ++f) {
                        if (!dst->anim_diffuse[s][f].path[0])
                            copy_str(dst->anim_diffuse[s][f].path, sizeof(dst->anim_diffuse[s][f].path), "0");
                        if (!dst->anim_normal[s][f].path[0])
                            copy_str(dst->anim_normal[s][f].path, sizeof(dst->anim_normal[s][f].path), "0");
                    }
                }
            }

            // rattrib: bit 0 = two-sided, bit 8 = animated, bit 10 = alpha test, bit 12 = alpha invert
            if (src->flags & THREEDI_IR_MATERIAL_FLAG_TWO_SIDED)
                dst->rattrib |= 0x1;
            if (src->flags & THREEDI_IR_MATERIAL_FLAG_ALPHA_TEST)
                dst->rattrib |= 0x400;
            if (src->flags & THREEDI_IR_MATERIAL_FLAG_ALPHA_INVERT)
                dst->rattrib |= 0x1000;
            if (dst->anim_frames > 0)
                dst->rattrib |= 0x100;

        }
    }

    // LOD 0
    TdpLod *lod = &out->lods[0];
    // Derive scene_file from IR model name (e.g. "dm1a1" → "dm1a1.ase")
    if (ir->name[0]) {
        char scene_name[64];
        std::snprintf(scene_name, sizeof(scene_name), "%s.ase", ir->name);
        copy_str(lod->scene_file, sizeof(lod->scene_file), scene_name);
    } else {
        copy_str(lod->scene_file, sizeof(lod->scene_file), "Untitled.ase");
    }
    // attributes maps to mesh type: 1=basic (GPM), 5=static/skinned (GPS/GPP)
    lod->attributes = (ir->mesh_type == THREEDI_IR_MESH_BASIC) ? 1 : 5;
    // render_function from IR (RMDL model_type), fallback to "gnrc"
    if (ir->render_function[0]) {
        copy_str(lod->render_function, sizeof(lod->render_function), ir->render_function);
    } else {
        copy_str(lod->render_function, sizeof(lod->render_function), "gnrc");
    }
    // LOD 0 threshold from IR
    if (ir->lod_count > 0) {
        lod->threshold = static_cast<float>(ir->lods[0].threshold);
    }

    // Part animations for LOD 0
    if (ir->lod_count > 0) {
        if (populate_tdp_lod_panm(lod, &ir->lods[0], ir) != 0) {
            tdp_free(out); return -1;
        }
    }

    // Lights from IR
    if (ir->light_count > 0) {
        lod->lights = static_cast<TdpLight *>(
            std::calloc(ir->light_count, sizeof(TdpLight)));
        if (!lod->lights) { tdp_free(out); return -1; }
        lod->light_count = ir->light_count;

        for (size_t i = 0; i < ir->light_count; ++i) {
            const ThreediIRLight *src = &ir->lights[i];
            TdpLight *dst = &lod->lights[i];
            std::snprintf(dst->name, sizeof(dst->name), "LP%02d", src->part_index + 1);
            dst->colorgen_style = src->style;
            dst->colorgen_rate = static_cast<float>(src->rate) / 256.0f;
            dst->colorgen_phase = static_cast<float>(src->phase) / 256.0f;

            // For styles > 0x70, phase encodes a control register index
            if (src->style > 0x70 && src->phase < ir->control_register_count && ir->control_registers) {
                copy_str(dst->colorgen_ctrlreg, sizeof(dst->colorgen_ctrlreg),
                         ir->control_registers[src->phase].name);
            }
            dst->colorgen_start[0] = clamp_255(src->color_start[0]);
            dst->colorgen_start[1] = clamp_255(src->color_start[1]);
            dst->colorgen_start[2] = clamp_255(src->color_start[2]);
            dst->colorgen_end[0] = clamp_255(src->color_end[0]);
            dst->colorgen_end[1] = clamp_255(src->color_end[1]);
            dst->colorgen_end[2] = clamp_255(src->color_end[2]);
            dst->disable_corona       = (src->flags & THREEDI_IR_LIGHT_FLAG_DISABLE_CORONA) ? 1 : 0;
            dst->disable_lightterrain = (src->flags & THREEDI_IR_LIGHT_FLAG_DISABLE_TERRAIN) ? 1 : 0;
            dst->disable_lightobjects = (src->flags & THREEDI_IR_LIGHT_FLAG_DISABLE_OBJECTS) ? 1 : 0;
        }
    }

    // LODs 1+ — populate scene_file, attributes, render_function, threshold, PANM
    for (size_t li = 1; li < ir->lod_count && li < TDP_MAX_LODS; ++li) {
        TdpLod *extra = &out->lods[li];
        if (ir->name[0]) {
            char scene_buf[64];
            std::snprintf(scene_buf, sizeof(scene_buf), "%s_lod%zu.ase",
                          ir->name, li);
            copy_str(extra->scene_file, sizeof(extra->scene_file), scene_buf);
        }
        extra->attributes = lod->attributes;
        copy_str(extra->render_function, sizeof(extra->render_function),
                 lod->render_function);
        extra->threshold = static_cast<float>(ir->lods[li].threshold);

        if (populate_tdp_lod_panm(extra, &ir->lods[li], ir) != 0) {
            tdp_free(out); return -1;
        }
    }

    return 0;
}
