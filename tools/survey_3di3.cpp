// survey_3di3 - Survey 3DI3 files for unique 3DP-relevant settings.
// Outputs a CSV-style summary of features found in each file, to identify
// which settings need test coverage.
//
// Usage: survey_3di3 <dir_or_file> [...]

#include "threedi/threedi_3di3.h"
#include "threedi/threedi_panm.h"
#include "threedi/threedi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

static const char *basename_ptr(const char *path) {
    const char *p = strrchr(path, '/');
    if (!p) p = strrchr(path, '\\');
    return p ? p + 1 : path;
}

static int ends_with_3di(const char *name) {
    size_t len = strlen(name);
    if (len < 4) return 0;
    return (strcasecmp(name + len - 4, ".3di") == 0);
}

struct Stats {
    // PANM flags
    int has_panm;
    int panm_count;
    uint8_t rot_types_seen;   // bitmask of rotation_type values 0-3
    uint8_t scale_types_seen; // bitmask of scale_type values 0-3
    uint8_t trans_types_seen; // bitmask of trans_type values 0-3
    int has_rot_reversed;
    int has_scale_y_data;     // scale_y slot has non-zero data
    int has_scale_z_data;     // scale_z slot has non-zero data
    int has_bind_matrix;      // bind_matrix_index != -1

    // Material features
    int mat_count;
    int has_alpha_gen;        // alpha_gen style != 0
    int has_rgb_gen;          // rgb_gen style != 0
    int has_u_scroll;         // u_params style != 0
    int has_v_scroll;         // v_params style != 0
    int has_glass;
    int has_emissive;
    int has_alpha_test;
    int has_two_sided;
    int has_tex_anim;         // animation.num_frames > 0
    int has_detail_tex;       // detail texture slot
    int has_normal_map;       // normal map texture
    int has_clamped_tex;      // clamped texture
    int has_multi_tex;        // multi-texture material
    uint8_t alpha_gen_styles; // bitmask
    uint8_t rgb_gen_styles;   // bitmask
    uint8_t u_styles;         // bitmask
    uint8_t v_styles;         // bitmask
    uint8_t tex_anim_types;   // bitmask

    // Lights
    int light_count;
    uint8_t light_styles;     // bitmask of light styles seen
    int has_light_flags;      // any light with flags != 0

    // Other
    int has_collision;
    int has_occlusion;
    int has_matrices;
    int has_userpoints;
    int has_ctrl_regs;
    int lod_count;
    int is_skinned;
};

static int is_transform_nonzero(const ThreediTransform *t) {
    return t->control != 0 || t->control_param != 0 ||
           t->rate != 0 || t->start != 0 || t->end != 0;
}

static void survey_file(const char *path, Stats *s) {
    memset(s, 0, sizeof(*s));

    Threedi3di3 model;
    memset(&model, 0, sizeof(model));
    if (threedi_3di3_read(path, &model) != 0) {
        return;
    }

    s->lod_count = (int)model.lod_count;
    s->is_skinned = (model.header.mesh_type == THREEDI_MESH_SKINNED) ? 1 : 0;
    s->has_collision = model.collision ? 1 : 0;
    s->has_occlusion = (model.occlusion_object_count > 0) ? 1 : 0;
    s->has_matrices = (model.mtrx.count > 0) ? 1 : 0;
    s->has_userpoints = (model.user_point_count > 0) ? 1 : 0;
    s->has_ctrl_regs = (model.ctrl.count > 0) ? 1 : 0;

    // PANM
    s->panm_count = (int)model.part_animation_count;
    s->has_panm = s->panm_count > 0 ? 1 : 0;
    for (size_t i = 0; i < model.part_animation_count; ++i) {
        const ThreediPartAnimation *pa = &model.part_animations[i];
        uint8_t rt = threedi_panm_rotation_type(pa->flags);
        uint8_t st = threedi_panm_scale_type(pa->flags);
        uint8_t tt = threedi_panm_translate_type(pa->flags);
        uint8_t rr = threedi_panm_rotation_reversed(pa->flags);
        if (rt < 8) s->rot_types_seen |= (1u << rt);
        if (st < 8) s->scale_types_seen |= (1u << st);
        if (tt < 8) s->trans_types_seen |= (1u << tt);
        if (rr) s->has_rot_reversed = 1;
        if (is_transform_nonzero(&pa->scale_y)) s->has_scale_y_data = 1;
        if (is_transform_nonzero(&pa->scale_z)) s->has_scale_z_data = 1;
        if (pa->bind_matrix_index != -1) s->has_bind_matrix = 1;
    }

    // Materials
    s->mat_count = (int)model.material_count;
    for (uint32_t i = 0; i < model.material_count; ++i) {
        const ThreediMaterial *mat = &model.materials[i];
        if (mat->alpha_gen.style != 0) {
            s->has_alpha_gen = 1;
            if (mat->alpha_gen.style < 8) s->alpha_gen_styles |= (1u << mat->alpha_gen.style);
        }
        if (mat->rgb_gen.style != 0) {
            s->has_rgb_gen = 1;
            if (mat->rgb_gen.style < 8) s->rgb_gen_styles |= (1u << mat->rgb_gen.style);
        }
        if (mat->u_params.style != 0) {
            s->has_u_scroll = 1;
            if (mat->u_params.style < 8) s->u_styles |= (1u << mat->u_params.style);
        }
        if (mat->v_params.style != 0) {
            s->has_v_scroll = 1;
            if (mat->v_params.style < 8) s->v_styles |= (1u << mat->v_params.style);
        }
        if (mat->is_glass) s->has_glass = 1;
        if (mat->emissive_type != 0) s->has_emissive = 1;
        if (mat->material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) s->has_alpha_test = 1;
        if (mat->material_flags & THREEDI_MATERIAL_FLAG_TWO_SIDED) s->has_two_sided = 1;
        if (mat->animation.num_frames > 0) {
            s->has_tex_anim = 1;
            if (mat->animation.animation_type < 8) s->tex_anim_types |= (1u << mat->animation.animation_type);
        }

        int tex_slots_used = 0;
        for (uint32_t t = 0; t < mat->texture_count && t < 24; ++t) {
            const ThreediMaterialTexture *tx = &mat->textures[t];
            if (tx->name[0] == 0) continue;
            tex_slots_used++;
            if (tx->slot == THREEDI_TEX_SLOT_DETAIL) s->has_detail_tex = 1;
            if (tx->slot == THREEDI_TEX_SLOT_NORMAL) s->has_normal_map = 1;
            if (tx->flags & THREEDI_TEX_FLAG_CLAMPED) s->has_clamped_tex = 1;
        }
        if (tex_slots_used > 1) s->has_multi_tex = 1;
    }

    // Lights
    s->light_count = (int)model.light_count;
    for (size_t i = 0; i < model.light_count; ++i) {
        const ThreediLight *l = &model.lights[i];
        if (l->style < 8) s->light_styles |= (1u << l->style);
        if (l->flags != 0) s->has_light_flags = 1;
    }

    threedi_3di3_free(&model);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: survey_3di3 <file_or_dir> [...]\n");
        return 1;
    }

    // Print header
    printf("file,lods,skinned,mats,panm_count,"
           "rot_types,scale_types,trans_types,rot_reversed,"
           "scale_y_data,scale_z_data,bind_matrix,"
           "alpha_gen,rgb_gen,u_scroll,v_scroll,"
           "glass,emissive,alpha_test,two_sided,"
           "tex_anim,detail_tex,normal_map,clamped_tex,multi_tex,"
           "alpha_gen_styles,rgb_gen_styles,u_styles,v_styles,tex_anim_types,"
           "lights,light_styles,light_flags,"
           "collision,occlusion,matrices,userpoints,ctrl_regs\n");

    for (int a = 1; a < argc; ++a) {
        // Check if directory
        DIR *dir = opendir(argv[a]);
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (!ends_with_3di(ent->d_name)) continue;
                char fullpath[4096];
                snprintf(fullpath, sizeof(fullpath), "%s/%s", argv[a], ent->d_name);

                // Check magic
                FILE *fp = fopen(fullpath, "rb");
                if (!fp) continue;
                char magic[4];
                if (fread(magic, 1, 4, fp) != 4) { fclose(fp); continue; }
                fclose(fp);
                if (memcmp(magic, "3DI3", 4) != 0) continue;

                Stats s;
                survey_file(fullpath, &s);
                printf("%s,%d,%d,%d,%d,"
                       "0x%02X,0x%02X,0x%02X,%d,"
                       "%d,%d,%d,"
                       "%d,%d,%d,%d,"
                       "%d,%d,%d,%d,"
                       "%d,%d,%d,%d,%d,"
                       "0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,"
                       "%d,0x%02X,%d,"
                       "%d,%d,%d,%d,%d\n",
                       basename_ptr(fullpath), s.lod_count, s.is_skinned, s.mat_count, s.panm_count,
                       s.rot_types_seen, s.scale_types_seen, s.trans_types_seen, s.has_rot_reversed,
                       s.has_scale_y_data, s.has_scale_z_data, s.has_bind_matrix,
                       s.has_alpha_gen, s.has_rgb_gen, s.has_u_scroll, s.has_v_scroll,
                       s.has_glass, s.has_emissive, s.has_alpha_test, s.has_two_sided,
                       s.has_tex_anim, s.has_detail_tex, s.has_normal_map, s.has_clamped_tex, s.has_multi_tex,
                       s.alpha_gen_styles, s.rgb_gen_styles, s.u_styles, s.v_styles, s.tex_anim_types,
                       s.light_count, s.light_styles, s.has_light_flags,
                       s.has_collision, s.has_occlusion, s.has_matrices, s.has_userpoints, s.has_ctrl_regs);
            }
            closedir(dir);
        } else {
            // Single file
            Stats s;
            survey_file(argv[a], &s);
            printf("%s,%d,%d,%d,%d,"
                   "0x%02X,0x%02X,0x%02X,%d,"
                   "%d,%d,%d,"
                   "%d,%d,%d,%d,"
                   "%d,%d,%d,%d,"
                   "%d,%d,%d,%d,%d,"
                   "0x%02X,0x%02X,0x%02X,0x%02X,0x%02X,"
                   "%d,0x%02X,%d,"
                   "%d,%d,%d,%d,%d\n",
                   basename_ptr(argv[a]), s.lod_count, s.is_skinned, s.mat_count, s.panm_count,
                   s.rot_types_seen, s.scale_types_seen, s.trans_types_seen, s.has_rot_reversed,
                   s.has_scale_y_data, s.has_scale_z_data, s.has_bind_matrix,
                   s.has_alpha_gen, s.has_rgb_gen, s.has_u_scroll, s.has_v_scroll,
                   s.has_glass, s.has_emissive, s.has_alpha_test, s.has_two_sided,
                   s.has_tex_anim, s.has_detail_tex, s.has_normal_map, s.has_clamped_tex, s.has_multi_tex,
                   s.alpha_gen_styles, s.rgb_gen_styles, s.u_styles, s.v_styles, s.tex_anim_types,
                   s.light_count, s.light_styles, s.has_light_flags,
                   s.has_collision, s.has_occlusion, s.has_matrices, s.has_userpoints, s.has_ctrl_regs);
        }
    }

    return 0;
}
