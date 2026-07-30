#include "threedi/threedi_panm_runtime.h"

#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/*
    Matrix convention used here (matches the original tool / your current code):

    - Stored as 16 floats in ROW-MAJOR order.
      Element (row r, col c) is m[r*4 + c].

    - Points/vectors are treated as ROW VECTORS multiplied on the left:
        p' = p * M

      Which means translation lives in the LAST ROW:
        tx = m[12], ty = m[13], tz = m[14]

    - This file only uses affine matrices (no perspective). We assume the last column is:
        [ 0 0 0 1 ]^T  (i.e., m[3]=m[7]=m[11]=0, m[15]=1)
*/

/* Raw bit reinterpret as float (used by "spinner" mode). */
static inline float bits_to_float(const void *p) {
    uint32_t u = 0;
    float f;
    memcpy(&u, p, sizeof(u));
    memcpy(&f, &u, sizeof(f));
    return f;
}

static inline void add_pivot_in_parent_space(ThreediMatrix4x4 *m, const ThreediVec3 *pivot, const ThreediMatrix4x4 *parent_or_null) {
    float base[3] = { pivot->x, pivot->y, pivot->z };

    if (parent_or_null) {
        float pt[3];
        threedi_mat4_apply_point(parent_or_null, base, pt);
        threedi_mat4_add_translation(m, pt[0], pt[1], pt[2]);
    } else {
        threedi_mat4_add_translation(m, base[0], base[1], base[2]);
    }
}

/* Translation track: move along the ORIGINAL basis of in_matrices[i] (matches existing behavior). */
static inline void apply_translation_track(ThreediMatrix4x4 *m,
                                           uint8_t trans_type,
                                           const ThreediTransform *track,
                                           const ThreediMatrix4x4 *basis_input,
                                           uint32_t time_ms,
                                           const int32_t *ctrl_values) {
    if (trans_type == THREEDI_TRANS_NONE) return;
    if (!track || !track->control) return;

    const float kScaleUnit = 1.0f / 65536.0f;
    float t = (float)threedi_panm_sample_track_raw(track, time_ms, ctrl_values) * kScaleUnit;

    switch (trans_type) {
        case THREEDI_TRANS_X:
            threedi_mat4_add_translation(m, t * basis_input->m[0], t * basis_input->m[1], t * basis_input->m[2]);
            break;
        case THREEDI_TRANS_Y:
            threedi_mat4_add_translation(m, t * basis_input->m[4], t * basis_input->m[5], t * basis_input->m[6]);
            break;
        case THREEDI_TRANS_Z:
            threedi_mat4_add_translation(m, t * basis_input->m[8], t * basis_input->m[9], t * basis_input->m[10]);
            break;
        default:
            break;
    }
}

static inline void sample_scale(float *sx, float *sy, float *sz,
                                const ThreediPartAnimation *n,
                                uint8_t scale_type,
                                uint32_t time_ms,
                                const int32_t *ctrl_values) {
    const float kScaleUnit = 1.0f / 65536.0f;

    float x = 1.0f, y = 1.0f, z = 1.0f;

    if (scale_type == 1) {
        if (n->scale_x.control) {
            float s = (float)threedi_panm_sample_track_raw(&n->scale_x, time_ms, ctrl_values) * kScaleUnit;
            x = y = z = s;
        }
    } else if (scale_type == 2) {
        if (n->scale_x.control) x = (float)threedi_panm_sample_track_raw(&n->scale_x, time_ms, ctrl_values) * kScaleUnit;
        if (n->scale_y.control) y = (float)threedi_panm_sample_track_raw(&n->scale_y, time_ms, ctrl_values) * kScaleUnit;
        if (n->scale_z.control) z = (float)threedi_panm_sample_track_raw(&n->scale_z, time_ms, ctrl_values) * kScaleUnit;
    }

    *sx = x; *sy = y; *sz = z;
}

/* --------- Build modes (each fills *out) --------- */

static void build_static_copy(ThreediMatrix4x4 *out,
                              const ThreediMatrix4x4 *in_sub,
                              const ThreediVec3 *pivot,
                              const ThreediMatrix4x4 *parent_or_null) {
    ThreediMatrix4x4 rot_only, t_to_pivot, tmp;

    rot_only = *in_sub;
    threedi_mat4_zero_translation(&rot_only);

    threedi_mat4_make_translation(&t_to_pivot, -pivot->x, -pivot->y, -pivot->z);
    threedi_mat4_mul_affine(&tmp, &t_to_pivot, &rot_only);

    add_pivot_in_parent_space(&tmp, pivot, parent_or_null);

    tmp.m[15] = in_sub->m[15];
    *out = tmp;
}

/* Main animated Euler rotation path (rot_type==2). */
static void build_euler(ThreediMatrix4x4 *out,
                        const ThreediMatrix4x4 *in_sub,
                        const ThreediVec3 *pivot,
                        const ThreediMatrix4x4 *parent_or_null,
                        float sx, float sy, float sz,
                        bool zyx_order,
                        const ThreediPartAnimation *n,
                        uint32_t time_ms,
                        const int32_t *ctrl_values) {
    const float kAngleScale = 0.0000014980282f;

    ThreediMatrix4x4 tmp, rot, bind_rot;

    // tmp = T(-pivot*scale) with scale on diagonal
    threedi_mat4_make_translation(&tmp, -pivot->x * sx, -pivot->y * sy, -pivot->z * sz);
    tmp.m[0]  = sx;
    tmp.m[5]  = sy;
    tmp.m[10] = sz;

    // NOTE: axis mapping here intentionally matches your original implementation:
    //   rotation_x track -> rotate around Y
    //   rotation_y track -> rotate around X
    //   rotation_z track -> rotate around Z
    if (zyx_order) {
        if (n->rotation_z.control) {
            float ang = (float)threedi_panm_sample_track_raw(&n->rotation_z, time_ms, ctrl_values) * kAngleScale;
            threedi_mat4_make_rot_z(&rot, ang);
            threedi_mat4_mul_affine(&tmp, &tmp, &rot);
        }
        if (n->rotation_y.control) {
            float ang = (float)threedi_panm_sample_track_raw(&n->rotation_y, time_ms, ctrl_values) * kAngleScale;
            threedi_mat4_make_rot_x(&rot, ang);
            threedi_mat4_mul_affine(&tmp, &tmp, &rot);
        }
        if (n->rotation_x.control) {
            float ang = (float)threedi_panm_sample_track_raw(&n->rotation_x, time_ms, ctrl_values) * kAngleScale;
            threedi_mat4_make_rot_y(&rot, ang);
            threedi_mat4_mul_affine(&tmp, &tmp, &rot);
        }
    } else {
        if (n->rotation_x.control) {
            float ang = (float)threedi_panm_sample_track_raw(&n->rotation_x, time_ms, ctrl_values) * kAngleScale;
            threedi_mat4_make_rot_y(&rot, ang);
            threedi_mat4_mul_affine(&tmp, &tmp, &rot);
        }
        if (n->rotation_y.control) {
            float ang = (float)threedi_panm_sample_track_raw(&n->rotation_y, time_ms, ctrl_values) * kAngleScale;
            threedi_mat4_make_rot_x(&rot, ang);
            threedi_mat4_mul_affine(&tmp, &tmp, &rot);
        }
        if (n->rotation_z.control) {
            float ang = (float)threedi_panm_sample_track_raw(&n->rotation_z, time_ms, ctrl_values) * kAngleScale;
            threedi_mat4_make_rot_z(&rot, ang);
            threedi_mat4_mul_affine(&tmp, &tmp, &rot);
        }
    }

    // Multiply by bind orientation only (translation stripped)
    bind_rot = *in_sub;
    threedi_mat4_zero_translation(&bind_rot);
    threedi_mat4_mul_affine(&tmp, &tmp, &bind_rot);

    // Add pivot back in world/parent space
    add_pivot_in_parent_space(&tmp, pivot, parent_or_null);

    *out = tmp;
}

/* Spinner / continuous rotation path (rot_type==1). Returns true if caller must break (preserve original behavior). */
static bool build_spinner(ThreediMatrix4x4 *out,
                          const ThreediMatrix4x4 *in_sub,
                          const ThreediVec3 *pivot,
                          const ThreediMatrix4x4 *parent_or_null,
                          float sx, float sy, float sz,
                          const ThreediPartAnimation *n,
                          float time_radians) {
    ThreediMatrix4x4 tmp, rot, bind_rot;

    bind_rot = *in_sub;
    threedi_mat4_zero_translation(&bind_rot);

    // tmp = T(-pivot*scale) with scale on diagonal
    threedi_mat4_make_translation(&tmp, -pivot->x * sx, -pivot->y * sy, -pivot->z * sz);
    tmp.m[0]  = sx;
    tmp.m[5]  = sy;
    tmp.m[10] = sz;

    // Raw float reinterpretation (matches your original code)
    float rot_y_f = bits_to_float(&n->rotation_y.control);
    if (rot_y_f != 0.0f) {
        threedi_mat4_make_rot_z(&rot, time_radians * rot_y_f);
        threedi_mat4_mul_affine(&tmp, &tmp, &rot);
    }

    float rot_x_start_f = bits_to_float(&n->rotation_x.start);
    if (rot_x_start_f != 0.0f) {
        threedi_mat4_make_rot_y(&rot, time_radians * rot_x_start_f);
        threedi_mat4_mul_affine(&tmp, &tmp, &rot);
    }

    float rot_x_f = bits_to_float(&n->rotation_x.control);
    if (rot_x_f != 0.0f) {
        threedi_mat4_make_rot_x(&rot, time_radians * rot_x_f);
        threedi_mat4_mul_affine(&tmp, &tmp, &rot);
    }

    threedi_mat4_mul_affine(&tmp, &tmp, &bind_rot);

    add_pivot_in_parent_space(&tmp, pivot, parent_or_null);

    *out = tmp;
    return true; // preserve: original implementation breaks after first spinner node
}

/* rot_type==3 view-aligned mode (0x100). Uses pivots[i] (not subobject pivot). */
static void build_viewaligned(ThreediMatrix4x4 *out,
                              const ThreediMatrix4x4 *view_inv,
                              const ThreediMatrix4x4 *node_input,
                              const ThreediVec3 *pivot_i,
                              float sx, float sy, float sz) {
    float base[3] = { pivot_i->x, pivot_i->y, pivot_i->z };

    float pt[3];
    threedi_mat4_apply_point(node_input, base, pt);

    float scaled[3] = { base[0] * sx, base[1] * sy, base[2] * sz };
    float proj[3];
    threedi_mat4_apply_vec3(view_inv, scaled, proj);

    ThreediMatrix4x4 tmp = *view_inv;
    threedi_mat4_set_translation(&tmp, pt[0] - proj[0], pt[1] - proj[1], pt[2] - proj[2]);

    ThreediMatrix4x4 scale_mat;
    threedi_mat4_identity(&scale_mat);
    scale_mat.m[0]  = sx;
    scale_mat.m[5]  = sy;
    scale_mat.m[10] = sz;

    threedi_mat4_mul_affine(&tmp, &scale_mat, &tmp);
    *out = tmp;
}

/* rot_type==4 upright billboard mode (0x200). Uses pivots[i] (not subobject pivot). */
static void build_upright_billboard(ThreediMatrix4x4 *out,
                                    const ThreediMatrix4x4 *view_inv,
                                    const ThreediMatrix4x4 *node_input,
                                    const ThreediVec3 *pivot_i) {
    float base[3] = { pivot_i->x, pivot_i->y, pivot_i->z };

    float pt[3];
    threedi_mat4_apply_point(node_input, base, pt);

    float proj[3];
    threedi_mat4_apply_vec3(view_inv, base, proj);

    ThreediMatrix4x4 tmp = *view_inv;
    threedi_mat4_set_translation(&tmp, pt[0] - proj[0], pt[1] - proj[1], pt[2] - proj[2]);

    // Force an orthonormal-ish basis using row1=(0,1,0) and row2=cross(row0,row1).
    tmp.m[4] = 0.0f;
    tmp.m[5] = 1.0f;
    tmp.m[6] = 0.0f;

    tmp.m[8]  = tmp.m[1] * tmp.m[6] - tmp.m[2] * tmp.m[5];
    tmp.m[9]  = tmp.m[2] * tmp.m[4] - tmp.m[0] * tmp.m[6];
    tmp.m[10] = tmp.m[0] * tmp.m[5] - tmp.m[1] * tmp.m[4];

    *out = tmp;
}

/* Fallback (no Euler/spinner/view modes): scale + bind_rot + pivot anchor. */
static void build_scaled_bind(ThreediMatrix4x4 *out,
                              const ThreediMatrix4x4 *in_sub,
                              const ThreediVec3 *pivot,
                              const ThreediMatrix4x4 *parent_or_null,
                              float sx, float sy, float sz) {
    ThreediMatrix4x4 tmp, bind_rot;

    bind_rot = *in_sub;
    threedi_mat4_zero_translation(&bind_rot);

    threedi_mat4_make_translation(&tmp, -pivot->x * sx, -pivot->y * sy, -pivot->z * sz);
    tmp.m[0]  = sx;
    tmp.m[5]  = sy;
    tmp.m[10] = sz;

    threedi_mat4_mul_affine(&tmp, &tmp, &bind_rot);
    add_pivot_in_parent_space(&tmp, pivot, parent_or_null);

    *out = tmp;
}

int threedi_panm_build_node_matrices(const ThreediPartAnimation *nodes,
                                     size_t node_count,
                                     const ThreediVec3 *pivots,
                                     const ThreediMatrix4x4 *view_inverse,
                                     const ThreediMatrix4x4 *in_matrices,
                                     const ThreediMatrix4x4 *mul_override,
                                     uint32_t time_ms,
                                     const int32_t *ctrl_values,
                                     ThreediMatrix4x4 *out_matrices) {
    if (!nodes || !pivots || !in_matrices || !out_matrices) {
        return -1;
    }

    const float time_radians = (float)time_ms * 0.0062831854f; // 2*pi/1000 * tick

    ThreediMatrix4x4 view_inv_base;
    if (view_inverse) {
        memcpy(view_inv_base.m, view_inverse->m, sizeof(view_inv_base.m));
    } else {
        threedi_mat4_identity(&view_inv_base);
    }
    const ThreediMatrix4x4 *view_inv = &view_inv_base;

    for (size_t i = 0; i < node_count; ++i) {
        const ThreediPartAnimation *n = &nodes[i];

        ThreediMatrix4x4 *dst = &out_matrices[i];

        const ThreediMatrix4x4 *in_sub = &in_matrices[n->subobject_index];
        const ThreediVec3 *pivot_sub = &pivots[n->subobject_index];

        const ThreediMatrix4x4 *basis_input = &in_matrices[i]; // basis for translation axis

        const ThreediMatrix4x4 *parent = NULL;
        if (n->parent_subobject < node_count) {
            parent = &out_matrices[n->parent_subobject];
        }

        const uint8_t scale_type = threedi_panm_scale_type(n->flags);
        const uint8_t rot_type   = threedi_panm_rotation_type(n->flags);
        const uint8_t trans_type = threedi_panm_translate_type(n->flags);
        const bool rot_rev       = (threedi_panm_rotation_reversed(n->flags) != 0);

        // "Animated?" gate: ensure special rot types don't accidentally fall into static copy.
        const bool animated =
            (rot_type != 0) ||
            (scale_type != 0) ||
            (trans_type != THREEDI_TRANS_NONE) ||
            rot_rev;

        if (!animated) {
            ThreediMatrix4x4 built;
            build_static_copy(&built, in_sub, pivot_sub, parent);
            built.m[15] = in_sub->m[15];
            *dst = built;
            continue;
        }

        float sx, sy, sz;
        sample_scale(&sx, &sy, &sz, n, scale_type, time_ms, ctrl_values);

        ThreediMatrix4x4 built;
        bool spinner_break = false;

        if (rot_type == 1) {
            spinner_break = build_spinner(&built, in_sub, pivot_sub, parent, sx, sy, sz, n, time_radians);
        } else if (rot_type == 2) {
            build_euler(&built, in_sub, pivot_sub, parent, sx, sy, sz, rot_rev, n, time_ms, ctrl_values);
        } else if (rot_type == 3) {
            const ThreediVec3 *pivot_i = &pivots[i];
            build_viewaligned(&built, view_inv, basis_input, pivot_i, sx, sy, sz);
        } else if (rot_type == 4) {
            const ThreediVec3 *pivot_i = &pivots[i];
            build_upright_billboard(&built, view_inv, basis_input, pivot_i);
        } else {
            build_scaled_bind(&built, in_sub, pivot_sub, parent, sx, sy, sz);
        }

        apply_translation_track(&built, trans_type, &n->translation, basis_input, time_ms, ctrl_values);

        built.m[15] = in_sub->m[15];
        *dst = built;

        // Preserve original: break after spinner branch
        if (spinner_break) {
            break;
        }
    }

    // Optional global multiply for outputs with non-zero w
    if (mul_override) {
        ThreediMatrix4x4 post;
        memcpy(post.m, mul_override->m, sizeof(post.m));

        for (size_t i = 0; i < node_count; ++i) {
            ThreediMatrix4x4 *d = &out_matrices[i];

            if (in_matrices[i].m[15] == 0.0f) {
                d->m[15] = 0.0f;
                continue;
            }

            threedi_mat4_mul_affine(d, d, &post);
        }
    }

    return 0;
}
