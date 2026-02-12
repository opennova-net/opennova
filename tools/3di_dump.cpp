// 3di_dump - Dump GP and 3DI3 files in a diff-friendly text format.
// Used for cross-format field correlation during reverse engineering.
//
// Usage:
//   3di_dump <file.3di>           (auto-detect format)
//   3di_dump --gp <file.3di>      (force GP parse)
//   3di_dump --3di3 <file.3di>    (force 3DI3 parse)

#include "threedi/threedi_gp.h"
#include "threedi/threedi_3di3.h"
#include "threedi/threedi_ir.h"
#include "threedi/threedi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Helpers
// ============================================================================

static void print_hex_bytes(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        if (i > 0) printf(" ");
        printf("%02X", data[i]);
    }
}

static void print_material_transform(const char *name, const ThreediGpMaterialTransform *t) {
    printf("  %-20s = style=%u param=%u rate=%d start=%d end=%d\n",
           name, t->style, t->param, t->rate, t->start, t->end);
}

static void print_anim_transform(const char *name, const ThreediGpAnimTransform *t) {
    printf("%s=(ctrl=%u p=%u rate=%d s=%d e=%d)",
           name, t->control, t->param, t->rate, t->start, t->end);
}

// ============================================================================
// GP Dump
// ============================================================================

static void dump_gp_header(const ThreediGpFile *gp) {
    printf("=== HEADER ===\n");
    const char *type_str = gp->header.mesh_type == THREEDI_GP_MESH_BASIC ? "GPM" :
                           gp->header.mesh_type == THREEDI_GP_MESH_STATIC ? "GPS" :
                           gp->header.mesh_type == THREEDI_GP_MESH_SKINNED ? "GPP" : "???";
    printf("  format             = GP (%s)\n", type_str);
    printf("  name               = \"%s\"\n", gp->header.name);
    printf("  format_ver         = 0x%02X\n", gp->header.format_ver);
    printf("  revision           = 0x%04X\n", gp->header.revision);
    printf("  flags              = 0x%08X\n", gp->header.flags);
    printf("  num_lods           = %d\n", gp->header.num_lods);
    printf("  lod_thresholds     = [%u, %u, %u] (Q16.16: %.1f, %.1f, %.1f)\n",
           gp->header.lod_thresholds[0], gp->header.lod_thresholds[1], gp->header.lod_thresholds[2],
           (float)gp->header.lod_thresholds[0] / 65536.0f,
           (float)gp->header.lod_thresholds[1] / 65536.0f,
           (float)gp->header.lod_thresholds[2] / 65536.0f);
    printf("  model_tag          = \"%s\"\n", gp->header.model_tag);
    printf("  rverts_count       = %d\n", gp->header.rverts_count);
    printf("  userpoint_count    = %d\n", gp->header.userpoint_count);
    printf("  ctrl_reg_count     = %d\n", gp->header.ctrl_reg_count);
    printf("  matrix_count       = %d\n", gp->header.matrix_count);
    printf("  occlusion_count    = %d\n", gp->header.occlusion_count);
    printf("\n");
}

static void dump_gp_material_lookups(const ThreediGpFile *gp) {
    if (gp->material_lookup_count == 0) return;
    printf("=== MATERIAL LOOKUPS (%zu entries, 60 bytes each) ===\n", gp->material_lookup_count);
    for (size_t i = 0; i < gp->material_lookup_count; ++i) {
        const ThreediGpMaterialLookup *ml = &gp->material_lookups[i];
        const char *slot_name = ml->slot_type == 0x02 ? "diffuse" :
                                ml->slot_type == 0x04 ? "lightmap" : "unknown";
        printf("  [%3zu] \"%s\" slot=0x%02X(%s) seq=%u flags=0x%02X dim=%ux%u\n",
               i, ml->texture_name, ml->slot_type, slot_name,
               ml->seq_index, ml->flags_26, ml->tex_width, ml->tex_height);
        printf("         unk=[0x%08X 0x%08X 0x%08X 0x%08X 0x%08X] tail=[0x%08X 0x%08X]\n",
               ml->unk_10, ml->unk_14, ml->unk_18, ml->unk_1C, ml->unk_20,
               ml->unk_34, ml->unk_38);
    }
    printf("\n");
}

static void dump_gp_materials(const ThreediGpRModel *rm) {
    for (size_t i = 0; i < rm->material_count; ++i) {
        const ThreediGpMaterial *mat = &rm->materials[i];
        printf("=== MATERIAL[%zu] \"%s\" ===\n", i, mat->texture_name);
        printf("  render_attributes  = 0x%08X\n", mat->render_attributes);
        printf("  physical_attributes= 0x%08X\n", mat->physical_attributes);
        printf("  use_alpha_pcx      = 0x%08X\n", mat->use_alpha_pcx);
        printf("  color_rgb          = [0x%08X, 0x%08X, 0x%08X]\n",
               mat->color_rgb[0], mat->color_rgb[1], mat->color_rgb[2]);
        printf("  render_lookup      = 0x%08X (%u)\n", mat->render_lookup, mat->render_lookup);
        printf("  luminosity         = 0x%08X\n", mat->luminosity);
        printf("  specular_intensity = 0x%08X (%u)\n", mat->specular_intensity, mat->specular_intensity);
        printf("  specular_sharpness = 0x%08X\n", mat->specular_sharpness);
        printf("  shader_flags       = 0x%08X\n", mat->shader_flags);
        printf("  tex_addressing     = 0x%08X\n", mat->tex_addressing_mode);
        printf("  reserved_40        = 0x%08X\n", mat->reserved_40);
        printf("  reserved_44        = 0x%08X\n", mat->reserved_44);
        printf("  tiling             = (%.6f, %.6f)\n", mat->u_tiling, mat->v_tiling);
        printf("  emissive_color     = 0x%08X\n", mat->emissive_color);
        print_material_transform("mapfunc_u", &mat->mapfunc_u);
        print_material_transform("mapfunc_v", &mat->mapfunc_v);
        print_material_transform("rgbgen", &mat->rgbgen);
        print_material_transform("alphagen", &mat->alphagen);
        printf("  runtime_ptr        = 0x%08X\n", mat->runtime_ptr);
        printf("  reflect_rgb        = (%.4f, %.4f, %.4f) type=%u alpha=%.4f\n",
               mat->reflect_r, mat->reflect_g, mat->reflect_b,
               mat->reflect_type, mat->reflect_alpha);
        printf("  actionplane_type   = %u\n", mat->actionplane_type);
        printf("  projector          = type=%u no_receive=%u\n",
               mat->projector_type, mat->projector_no_receive);
        printf("\n");
    }
}

static void dump_gp_subobjects(const ThreediGpRModel *rm) {
    for (size_t i = 0; i < rm->subobject_count; ++i) {
        const ThreediGpSubObject *sub = &rm->subobjects[i];
        printf("=== SUBOBJECT[%zu] ===\n", i);
        printf("  unk_00             = 0x%08X\n", sub->unk_00);
        printf("  batch_count        = %d\n", sub->batch_count);
        printf("  unk_08             = 0x%08X\n", sub->unk_08);
        printf("  parent             = %d\n", sub->parent);
        printf("  rel                = (%.6f, %.6f, %.6f)\n", sub->rel[0], sub->rel[1], sub->rel[2]);
        printf("  abs                = (%.6f, %.6f, %.6f)\n", sub->abs[0], sub->abs[1], sub->abs[2]);
        printf("  bounding_min       = (%.6f, %.6f, %.6f)\n", sub->bounding_min[0], sub->bounding_min[1], sub->bounding_min[2]);
        printf("  bounding_radius    = 0x%08X (q16=%.6f)\n", sub->bounding_radius, sub->bounding_radius / 65536.0f);
        printf("  bounding_max       = (%.6f, %.6f, %.6f)\n", sub->bounding_max[0], sub->bounding_max[1], sub->bounding_max[2]);
        printf("  visible            = %u\n", sub->visible);
        printf("  special_flag       = %u\n", sub->special_flag);
        printf("\n");
    }
}

static void dump_gp_strip_triplets(const ThreediGpRModel *rm) {
    if (rm->strip_triplet_count == 0) return;
    printf("=== STRIP TRIPLETS (%zu) ===\n", rm->strip_triplet_count);
    for (size_t i = 0; i < rm->strip_triplet_count; ++i) {
        printf("  [%3zu] a=%u b=%u c=%u\n", i,
               rm->strip_triplets[i].a, rm->strip_triplets[i].b, rm->strip_triplets[i].c);
    }
    printf("\n");
}

static void dump_gp_polys(const ThreediGpRModel *rm) {
    printf("=== PRIMITIVES (%zu) ===\n", rm->poly_count);
    for (size_t i = 0; i < rm->poly_count; ++i) {
        const ThreediGpVariablePoly *p = &rm->polys[i];
        printf("  [%3zu] sub=%d mat=%d topo=%d first=%d max=%d indices=%zu\n",
               i, p->subobject_index, p->material_index, p->topology,
               p->first_vertex, p->max_vertex_index, p->index_count);
    }
    printf("\n");
}

static void dump_gp_lights(const ThreediGpFile *gp) {
    if (gp->has_ambient_light) {
        const ThreediGpAmbientLight *al = &gp->ambient_light;
        printf("=== AMBIENT LIGHT ===\n");
        printf("  colorgen_style     = %u\n", al->colorgen_style);
        printf("  colorgen_rate      = %.6f\n", al->colorgen_rate);
        printf("  colorgen_phase     = %.6f\n", al->colorgen_phase);
        printf("  color_start        = (%.4f, %.4f, %.4f)\n",
               al->color_start[0], al->color_start[1], al->color_start[2]);
        printf("  color_end          = (%.4f, %.4f, %.4f)\n",
               al->color_end[0], al->color_end[1], al->color_end[2]);
        printf("\n");
    }
    if (gp->light_count == 0) return;
    printf("=== LIGHTS (%zu) ===\n", gp->light_count);
    for (size_t i = 0; i < gp->light_count; ++i) {
        const ThreediGpLight *l = &gp->lights[i];
        printf("  [%zu] style=%u phase=%u rate=%u rgb_start=(%u,%u,%u) rgb_end=(%u,%u,%u)\n",
               i, l->style, l->phase, l->rate,
               l->color_start[0], l->color_start[1], l->color_start[2],
               l->color_end[0], l->color_end[1], l->color_end[2]);
        printf("       pos=(%.4f, %.4f, %.4f) atten=(%.4f, %.4f) part=%d\n",
               l->position[0], l->position[1], l->position[2],
               l->attenuation_start, l->attenuation_end, l->part_index);
        printf("       unk=[0x%08X, 0x%08X, 0x%08X]\n",
               l->unk_36, l->unk_40, l->unk_44);
    }
    printf("\n");
}

static void dump_gp_ctrl_regs(const ThreediGpFile *gp) {
    if (gp->control_register_count == 0) return;
    printf("=== CONTROL REGISTERS (%zu) ===\n", gp->control_register_count);
    for (size_t i = 0; i < gp->control_register_count; ++i) {
        const ThreediGpControlRegister *cr = &gp->control_registers[i];
        printf("  [%zu] \"%s\" index=%u params=[%u, %u, %u, %u, %u, %u]\n",
               i, cr->name, cr->name_index,
               cr->param[0], cr->param[1], cr->param[2],
               cr->param[3], cr->param[4], cr->param[5]);
    }
    printf("\n");
}

static void dump_gp_vstream(const ThreediGpFile *gp) {
    if (!gp->vstream) return;
    printf("=== VSTREAM ===\n");
    printf("  buffer_ptr         = 0x%08X\n", gp->vstream->buffer_ptr);
    printf("  data_size          = %u\n", gp->vstream->data_size);
    printf("  unk_08             = 0x%08X\n", gp->vstream->unk_08);
    printf("  unk_0C             = 0x%08X\n", gp->vstream->unk_0C);
    printf("  actual_data_len    = %zu\n", gp->vstream->data_len);
    printf("\n");
}

static void dump_gp_extra_polys(const ThreediGpRModel *rm) {
    if (rm->extra_poly_count == 0) return;
    printf("=== EXTRA POLYS (%u, 88 bytes each) ===\n", rm->extra_poly_count);
    for (uint32_t i = 0; i < rm->extra_poly_count; ++i) {
        const ThreediGpExtraPoly *ep = &rm->extra_polys[i];
        printf("  [%u] VP1: mat=%d tag=0x%08X idx=%u tri=%u topo=%d first=%d vtx=%d\n",
               i, ep->vp1.material_index, ep->vp1.indices_tag,
               ep->vp1.index_count, ep->vp1.triangle_count,
               ep->vp1.topology, ep->vp1.first_vertex, ep->vp1.vertex_count);
        printf("       VP1 reserved=[0x%08X, 0x%08X, 0x%08X, 0x%08X]\n",
               ep->vp1_reserved[0], ep->vp1_reserved[1],
               ep->vp1_reserved[2], ep->vp1_reserved[3]);
        printf("       VP2: mat=%d tag=0x%08X idx=%u tri=%u topo=%d first=%d vtx=%d\n",
               ep->vp2.material_index, ep->vp2.indices_tag,
               ep->vp2.index_count, ep->vp2.triangle_count,
               ep->vp2.topology, ep->vp2.first_vertex, ep->vp2.vertex_count);
        printf("       bbox=(%.4f,%.4f,%.4f)-(%.4f,%.4f,%.4f)\n",
               ep->bbox_min[0], ep->bbox_min[1], ep->bbox_min[2],
               ep->bbox_max[0], ep->bbox_max[1], ep->bbox_max[2]);
    }
    printf("\n");
}

static void dump_gp_part_animations(const ThreediGpRModel *rm) {
    if (rm->part_animation_count == 0) return;
    printf("=== PART ANIMATIONS (%zu) ===\n", rm->part_animation_count);
    for (size_t i = 0; i < rm->part_animation_count; ++i) {
        const ThreediGpPartAnimation *pa = &rm->part_animations[i];
        printf("  [%zu] flags=0x%08X parent=%u subobj=%u mat_idx=%u mat_off=%u bind=%d\n",
               i, pa->flags, pa->parent_subobject, pa->subobject_index,
               pa->matrix_index, pa->matrix_offset, pa->bind_matrix_index);
        printf("       ");
        print_anim_transform("scale_x", &pa->scale_x);
        printf(" ");
        print_anim_transform("scale_y", &pa->scale_y);
        printf(" ");
        print_anim_transform("scale_z", &pa->scale_z);
        printf("\n");
        printf("       ");
        print_anim_transform("rot_x", &pa->rot_x);
        printf(" ");
        print_anim_transform("rot_y", &pa->rot_y);
        printf(" ");
        print_anim_transform("rot_z", &pa->rot_z);
        printf("\n");
        printf("       ");
        print_anim_transform("translate", &pa->translate);
        printf("\n");
        printf("       rotate_type=%u scale_type=%u transform_as=%u\n",
               pa->rotate_type, pa->scale_type, pa->transform_as);
        printf("       yaw_rate=%.6f pitch_rate=%.6f roll_rate=%.6f\n",
               pa->yaw_rate, pa->pitch_rate, pa->roll_rate);
    }
    printf("\n");
}

static void dump_gp_userpoints(const ThreediGpFile *gp) {
    if (gp->userpoint_count == 0) return;
    printf("=== USERPOINTS (%zu) ===\n", gp->userpoint_count);
    for (size_t i = 0; i < gp->userpoint_count; ++i) {
        const ThreediGpUserPoint *up = &gp->userpoints[i];
        printf("  [%zu] \"%s\" xyz=(%d,%d,%d) rot=(%d,%d,%d) parent=%d type=%d\n",
               i, up->name, up->x, up->y, up->z,
               up->rot_x, up->rot_y, up->rot_z,
               up->parent_subobject, up->type_code);
    }
    printf("\n");
}

// ============================================================================
// GP Unknowns Dump (raw byte scan)
// ============================================================================

// Helper: read little-endian u32 from raw buffer
static uint32_t raw_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t raw_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// Print non-zero u32 field
static void unk_u32(const char *file, const char *section, const char *field,
                     const uint8_t *base, size_t offset) {
    uint32_t v = raw_u32(base + offset);
    if (v != 0) printf("%s:%s.%s = 0x%08X\n", file, section, field, v);
}

// Print non-zero u8 field
static void unk_u8(const char *file, const char *section, const char *field,
                    const uint8_t *base, size_t offset) {
    uint8_t v = base[offset];
    if (v != 0) printf("%s:%s.%s = 0x%02X\n", file, section, field, v);
}

// Print non-zero byte range
static void unk_bytes(const char *file, const char *section, const char *field,
                       const uint8_t *base, size_t offset, size_t len) {
    int any_nonzero = 0;
    for (size_t i = 0; i < len; ++i) {
        if (base[offset + i] != 0) { any_nonzero = 1; break; }
    }
    if (!any_nonzero) return;
    printf("%s:%s.%s = [", file, section, field);
    for (size_t i = 0; i < len; ++i) {
        if (i > 0) printf(" ");
        printf("%02X", base[offset + i]);
    }
    printf("]\n");
}

// Print non-zero string field (prints as quoted string if printable, hex otherwise)
static void unk_str(const char *file, const char *section, const char *field,
                     const uint8_t *base, size_t offset, size_t len) {
    int any_nonzero = 0;
    int all_printable = 1;
    for (size_t i = 0; i < len; ++i) {
        if (base[offset + i] != 0) {
            any_nonzero = 1;
            if (base[offset + i] < 32 || base[offset + i] >= 127)
                all_printable = 0;
        }
    }
    if (!any_nonzero) return;
    if (all_printable) {
        printf("%s:%s.%s = \"", file, section, field);
        for (size_t i = 0; i < len && base[offset + i] != 0; ++i)
            putchar(base[offset + i]);
        printf("\"\n");
    } else {
        unk_bytes(file, section, field, base, offset, len);
    }
}

static int dump_gp_unknowns(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open: %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size < 0xEC) {
        fprintf(stderr, "File too small for GP header: %s\n", path);
        fclose(f);
        return 1;
    }
    uint8_t *data = (uint8_t *)malloc((size_t)file_size);
    if (!data) { fclose(f); return 1; }
    size_t nread = fread(data, 1, (size_t)file_size, f);
    fclose(f);
    if (nread != (size_t)file_size) { free(data); return 1; }

    // Extract just the filename for output
    const char *fname = strrchr(path, '/');
    if (!fname) fname = strrchr(path, '\\');
    fname = fname ? fname + 1 : path;

    // === HEADER (0xEC bytes) ===
    const uint8_t *hdr = data;

    unk_u8(fname, "header", "format_ver", hdr, 0x03);
    unk_u32(fname, "header", "revision", hdr, 0x04);
    unk_u32(fname, "header", "lod_far_q16", hdr, 0x20);
    unk_u32(fname, "header", "lod_mid_q16", hdr, 0x24);
    unk_u32(fname, "header", "lod_near_q16", hdr, 0x28);
    unk_u32(fname, "header", "reserved1", hdr, 0x2C);
    unk_u32(fname, "header", "reserved2", hdr, 0x30);
    unk_bytes(fname, "header", "pad_34_3F", hdr, 0x34, 12);
    unk_str(fname, "header", "tag_fourcc", hdr, 0x40, 16);
    unk_bytes(fname, "header", "pad_50_5F", hdr, 0x50, 16);
    unk_u32(fname, "header", "offset_a", hdr, 0x60);
    unk_u32(fname, "header", "offset_b", hdr, 0x64);
    unk_u32(fname, "header", "offset_c", hdr, 0x68);
    unk_u32(fname, "header", "offset_d", hdr, 0x6C);
    unk_bytes(fname, "header", "unk_70_83", hdr, 0x70, 20);
    unk_u32(fname, "header", "cmodel_ptr", hdr, 0x84);
    unk_u32(fname, "header", "rverts_ptr", hdr, 0x8C);
    unk_u32(fname, "header", "rmodel_ptr0", hdr, 0x90);
    unk_u32(fname, "header", "rmodel_ptr1", hdr, 0x94);
    unk_u32(fname, "header", "rmodel_ptr2", hdr, 0x98);
    unk_u32(fname, "header", "rmodel_ptr3", hdr, 0x9C);
    unk_bytes(fname, "header", "unk_A0_AF", hdr, 0xA0, 16);
    unk_u32(fname, "header", "userpoint_ptr", hdr, 0xB4);
    unk_u32(fname, "header", "ctrl_lightinfo_ptr", hdr, 0xB8);
    unk_u32(fname, "header", "ctrl_array_ptr", hdr, 0xC0);
    unk_u32(fname, "header", "matrix_array_ptr", hdr, 0xC8);
    unk_u32(fname, "header", "vstream_ptr", hdr, 0xCC);
    unk_u32(fname, "header", "occ_ptr", hdr, 0xD4);
    unk_bytes(fname, "header", "unk_tail_pad", hdr, 0xD8, 20);

    // === Now parse with the normal parser to get structured access ===
    ThreediGpFile gp;
    threedi_gp_init(&gp);
    if (threedi_gp_parse(data, (size_t)file_size, &gp) != 0) {
        fprintf(stderr, "Parse failed (unknowns mode still dumped header): %s\n", path);
        threedi_gp_free(&gp);
        free(data);
        return 1;
    }

    // === RMODEL HEADERS ===
    // Walk raw bytes to find rmodel header fields the parser discards.
    // The rmodel headers start after: header(0xEC) + userpoints + material_lookup + collision + rverts.
    // Rather than recompute, we use the parsed struct values but need raw rmodel header access.
    // We need to re-walk the file to find rmodel header positions.
    {
        size_t pos = 0xEC; // after header

        // Skip userpoints: count * 48
        pos += (size_t)gp.header.userpoint_count * 48;

        // Skip material lookup: 4 bytes count + count * 60
        if (pos + 4 <= (size_t)file_size) {
            uint32_t ml_count = raw_u32(data + pos);
            pos += 4 + (size_t)ml_count * 60;
        }

        // Skip collision header + blob
        if (gp.collision && pos + 8 <= (size_t)file_size) {
            // collision header: is_skinned(4) + data_size(4) + unk08(4) + 9 floats(36) +
            //   6*(count+ptr) pairs(48) + volume_count(4) + 9 trailing ptrs(36) = 136 bytes
            uint32_t col_data_size = raw_u32(data + pos + 4);
            pos += 136 + col_data_size;
        }

        // Skip rverts
        int rvert_stride = (gp.header.mesh_type == THREEDI_GP_MESH_BASIC) ? 44 :
                           (gp.header.mesh_type == THREEDI_GP_MESH_STATIC) ? 48 : 60;
        pos += (size_t)gp.header.rverts_count * (size_t)rvert_stride;

        // Now at rmodel headers
        for (size_t lod = 0; lod < gp.rmodel_count; ++lod) {
            char section[32];
            snprintf(section, sizeof(section), "rmodel[%zu]", lod);

            if (pos + 136 > (size_t)file_size) break;
            const uint8_t *rm = data + pos;

            // Offsets within the 136-byte rmodel header
            unk_u32(fname, section, "unk_20", rm, 0x20);
            unk_u32(fname, section, "zero_24", rm, 0x24);
            unk_u32(fname, section, "unk_28", rm, 0x28);
            unk_u32(fname, section, "zero_2C", rm, 0x2C);
            unk_u32(fname, section, "zero_30", rm, 0x30);
            unk_u32(fname, section, "zero_34", rm, 0x34);
            unk_u32(fname, section, "unk_38", rm, 0x38);
            unk_u32(fname, section, "zero_3C", rm, 0x3C);
            unk_u32(fname, section, "zero_40", rm, 0x40);
            unk_u32(fname, section, "zero_44", rm, 0x44);
            unk_u32(fname, section, "unk_48", rm, 0x48);
            unk_u32(fname, section, "unk_4C", rm, 0x4C);
            // zero6C..zero84 (7 u32s at offsets 0x6C..0x84)
            unk_u32(fname, section, "zero_6C", rm, 0x6C);
            unk_u32(fname, section, "zero_70", rm, 0x70);
            unk_u32(fname, section, "zero_74", rm, 0x74);
            unk_u32(fname, section, "zero_78", rm, 0x78);
            unk_u32(fname, section, "zero_7C", rm, 0x7C);
            unk_u32(fname, section, "zero_80", rm, 0x80);
            unk_u32(fname, section, "zero_84", rm, 0x84);

            // Advance past the 136-byte header + the data blob
            uint32_t rm_data_size = raw_u32(rm);
            pos += 136 + rm_data_size;
        }
    }

    // === SUBOBJECTS (from parsed structs) ===
    for (size_t lod = 0; lod < gp.rmodel_count; ++lod) {
        const ThreediGpRModel *rm = &gp.rmodels[lod];
        for (size_t i = 0; i < rm->subobject_count; ++i) {
            char section[64];
            snprintf(section, sizeof(section), "lod%zu.subobj[%zu]", lod, i);
            const ThreediGpSubObject *sub = &rm->subobjects[i];

            if (sub->unk_00 != 0)
                printf("%s:%s.runtime_batch_ptr = 0x%08X\n", fname, section, sub->unk_00);
            if (sub->unk_08 != 0)
                printf("%s:%s.unk_08 = 0x%08X\n", fname, section, sub->unk_08);
            if (sub->pad[0] != 0)
                printf("%s:%s.pad_46 = 0x%02X\n", fname, section, sub->pad[0]);
            if (sub->pad[1] != 0)
                printf("%s:%s.pad_47 = 0x%02X\n", fname, section, sub->pad[1]);
        }
    }

    // === MATERIALS (from parsed structs) ===
    for (size_t lod = 0; lod < gp.rmodel_count; ++lod) {
        const ThreediGpRModel *rm = &gp.rmodels[lod];
        for (size_t i = 0; i < rm->material_count; ++i) {
            char section[64];
            snprintf(section, sizeof(section), "lod%zu.mat[%zu]", lod, i);
            const ThreediGpMaterial *mat = &rm->materials[i];

            if (mat->physical_attributes != 0)
                printf("%s:%s.physical_attributes = 0x%08X\n", fname, section, mat->physical_attributes);
            if (mat->use_alpha_pcx != 0)
                printf("%s:%s.use_alpha_pcx = 0x%08X\n", fname, section, mat->use_alpha_pcx);
            if (mat->color_rgb[0] != 0)
                printf("%s:%s.color_rgb_0 = 0x%08X\n", fname, section, mat->color_rgb[0]);
            if (mat->color_rgb[1] != 0)
                printf("%s:%s.color_rgb_1 = 0x%08X\n", fname, section, mat->color_rgb[1]);
            if (mat->color_rgb[2] != 0)
                printf("%s:%s.color_rgb_2 = 0x%08X\n", fname, section, mat->color_rgb[2]);
            if (mat->specular_sharpness != 0)
                printf("%s:%s.specular_sharpness = 0x%08X\n", fname, section, mat->specular_sharpness);
            if (mat->tex_addressing_mode != 0)
                printf("%s:%s.tex_addressing_mode = 0x%08X\n", fname, section, mat->tex_addressing_mode);
            if (mat->reserved_40 != 0)
                printf("%s:%s.reserved_40 = 0x%08X\n", fname, section, mat->reserved_40);
            if (mat->reserved_44 != 0)
                printf("%s:%s.reserved_44 = 0x%08X\n", fname, section, mat->reserved_44);
            if (mat->emissive_color != 0)
                printf("%s:%s.emissive_color = 0x%08X\n", fname, section, mat->emissive_color);

            // Individual tail fields (formerly tail[36])
            if (mat->runtime_ptr != 0)
                printf("%s:%s.runtime_ptr = 0x%08X\n", fname, section, mat->runtime_ptr);
            if (mat->reflect_r != 0.0f)
                printf("%s:%s.reflect_r = %.6f\n", fname, section, mat->reflect_r);
            if (mat->reflect_g != 0.0f)
                printf("%s:%s.reflect_g = %.6f\n", fname, section, mat->reflect_g);
            if (mat->reflect_b != 0.0f)
                printf("%s:%s.reflect_b = %.6f\n", fname, section, mat->reflect_b);
            if (mat->reflect_type != 0)
                printf("%s:%s.reflect_type = 0x%08X\n", fname, section, mat->reflect_type);
            if (mat->reflect_alpha != 0.0f)
                printf("%s:%s.reflect_alpha = %.6f\n", fname, section, mat->reflect_alpha);
            if (mat->actionplane_type != 0)
                printf("%s:%s.actionplane_type = 0x%08X\n", fname, section, mat->actionplane_type);
            if (mat->projector_type != 0)
                printf("%s:%s.projector_type = 0x%08X\n", fname, section, mat->projector_type);
            if (mat->projector_no_receive != 0)
                printf("%s:%s.projector_no_receive = 0x%08X\n", fname, section, mat->projector_no_receive);
        }
    }

    // === EXTRA POLYS ===
    for (size_t lod = 0; lod < gp.rmodel_count; ++lod) {
        const ThreediGpRModel *rm = &gp.rmodels[lod];
        for (uint32_t i = 0; i < rm->extra_poly_count; ++i) {
            char section[64];
            snprintf(section, sizeof(section), "lod%zu.extra_poly[%u]", lod, i);
            const ThreediGpExtraPoly *ep = &rm->extra_polys[i];

            printf("%s:%s.vp1_mat=%d vp1_tag=0x%08X vp1_idx=%u vp1_tri=%u vp1_topo=%d vp1_first=%d vp1_vtx=%d\n",
                   fname, section, ep->vp1.material_index, ep->vp1.indices_tag,
                   ep->vp1.index_count, ep->vp1.triangle_count,
                   ep->vp1.topology, ep->vp1.first_vertex, ep->vp1.vertex_count);
            for (int j = 0; j < 4; ++j) {
                if (ep->vp1_reserved[j] != 0) {
                    char field[32];
                    snprintf(field, sizeof(field), "vp1_reserved_%d", j);
                    printf("%s:%s.%s = 0x%08X\n", fname, section, field, ep->vp1_reserved[j]);
                }
            }
            printf("%s:%s.vp2_mat=%d vp2_tag=0x%08X vp2_idx=%u vp2_tri=%u vp2_topo=%d vp2_first=%d vp2_vtx=%d\n",
                   fname, section, ep->vp2.material_index, ep->vp2.indices_tag,
                   ep->vp2.index_count, ep->vp2.triangle_count,
                   ep->vp2.topology, ep->vp2.first_vertex, ep->vp2.vertex_count);
            printf("%s:%s.bbox = (%.4f,%.4f,%.4f)-(%.4f,%.4f,%.4f)\n",
                   fname, section,
                   ep->bbox_min[0], ep->bbox_min[1], ep->bbox_min[2],
                   ep->bbox_max[0], ep->bbox_max[1], ep->bbox_max[2]);
        }
    }

    // === VSTREAM HEADER ===
    if (gp.vstream) {
        if (gp.vstream->buffer_ptr != 0)
            printf("%s:vstream.buffer_ptr = 0x%08X\n", fname, gp.vstream->buffer_ptr);
        if (gp.vstream->unk_08 != 0)
            printf("%s:vstream.unk_08 = 0x%08X\n", fname, gp.vstream->unk_08);
        if (gp.vstream->unk_0C != 0)
            printf("%s:vstream.unk_0C = 0x%08X\n", fname, gp.vstream->unk_0C);
    }

    // === COLLISION OBJECTS ===
    if (gp.collision) {
        for (int32_t i = 0; i < gp.collision->object_count; ++i) {
            char section[48];
            snprintf(section, sizeof(section), "cobj[%d]", i);
            const ThreediGpCollisionObject *obj = &gp.collision->objects[i];

            if (obj->vertex_ptr != 0)
                printf("%s:%s.vertex_ptr = 0x%08X\n", fname, section, obj->vertex_ptr);
            if (obj->face_ptr != 0)
                printf("%s:%s.face_ptr = 0x%08X\n", fname, section, obj->face_ptr);
            if (obj->normal_ptr != 0)
                printf("%s:%s.normal_ptr = 0x%08X\n", fname, section, obj->normal_ptr);
            if (obj->volume_ptr != 0)
                printf("%s:%s.volume_ptr = 0x%08X\n", fname, section, obj->volume_ptr);

            // reserved[3] — IDA: unk28..unk2C + one more
            for (int j = 0; j < 3; ++j) {
                if (obj->reserved[j] != 0) {
                    char field[16];
                    snprintf(field, sizeof(field), "reserved_%d", j);
                    printf("%s:%s.%s = 0x%08X\n", fname, section, field, (uint32_t)obj->reserved[j]);
                }
            }

            // reserved2[4] — trailing 16 bytes
            for (int j = 0; j < 4; ++j) {
                if (obj->reserved2[j] != 0) {
                    char field[16];
                    snprintf(field, sizeof(field), "reserved2_%d", j);
                    printf("%s:%s.%s = 0x%08X\n", fname, section, field, (uint32_t)obj->reserved2[j]);
                }
            }
        }

        // === COLLISION VOLUMES ===
        for (int32_t i = 0; i < gp.collision->volume_count; ++i) {
            char section[48];
            snprintf(section, sizeof(section), "cvol[%d]", i);
            const ThreediGpCollisionVolume *vol = &gp.collision->volumes[i];

            if (vol->reserved1 != 0)
                printf("%s:%s.reserved1 = 0x%08X\n", fname, section, (uint32_t)vol->reserved1);
            if (vol->plane_ptr != 0)
                printf("%s:%s.plane_ptr = 0x%08X\n", fname, section, vol->plane_ptr);
            for (int j = 0; j < 4; ++j) {
                if (vol->reserved2[j] != 0) {
                    char field[16];
                    snprintf(field, sizeof(field), "reserved2_%d", j);
                    printf("%s:%s.%s = 0x%08X\n", fname, section, field, (uint32_t)vol->reserved2[j]);
                }
            }
        }

        // === COLLISION FACES — pad/reserved fields ===
        for (int32_t i = 0; i < gp.collision->face_count; ++i) {
            const ThreediGpCollisionFace *face = &gp.collision->faces[i];
            if (face->pad != 0) {
                char section[48];
                snprintf(section, sizeof(section), "cface[%d]", i);
                printf("%s:%s.pad = 0x%02X\n", fname, section, face->pad);
            }
            if (face->reserved != 0) {
                char section[48];
                snprintf(section, sizeof(section), "cface[%d]", i);
                printf("%s:%s.reserved = 0x%04X\n", fname, section, (uint16_t)face->reserved);
            }
        }
    }

    // === LIGHT UNKNOWN FIELDS ===
    for (size_t i = 0; i < gp.light_count; ++i) {
        const ThreediGpLight *l = &gp.lights[i];
        char section[32];
        snprintf(section, sizeof(section), "light[%zu]", i);
        if (l->unk_36 != 0)
            printf("%s:%s.unk_36 = 0x%08X\n", fname, section, l->unk_36);
        if (l->unk_40 != 0)
            printf("%s:%s.unk_40 = 0x%08X\n", fname, section, l->unk_40);
        if (l->unk_44 != 0)
            printf("%s:%s.unk_44 = 0x%08X\n", fname, section, l->unk_44);
    }

    // === CONTROL REGISTER PARAMS ===
    for (size_t i = 0; i < gp.control_register_count; ++i) {
        const ThreediGpControlRegister *cr = &gp.control_registers[i];
        char section[32];
        snprintf(section, sizeof(section), "ctrl_reg[%zu]", i);
        for (int j = 0; j < 6; ++j) {
            if (cr->param[j] != 0) {
                printf("%s:%s.param_%d = 0x%08X (%u)\n", fname, section, j + 1, cr->param[j], cr->param[j]);
            }
        }
    }

    threedi_gp_free(&gp);
    free(data);
    return 0;
}

static int dump_gp(const char *path) {
    ThreediGpFile gp;
    threedi_gp_init(&gp);

    if (threedi_gp_read(path, &gp) != 0) {
        fprintf(stderr, "Failed to parse GP file: %s\n", path);
        threedi_gp_free(&gp);
        return 1;
    }

    dump_gp_header(&gp);
    dump_gp_userpoints(&gp);
    dump_gp_material_lookups(&gp);

    for (size_t lod = 0; lod < gp.rmodel_count; ++lod) {
        printf("### LOD %zu (flags=0x%08X) ###\n\n", lod, gp.rmodels[lod].flags);
        dump_gp_strip_triplets(&gp.rmodels[lod]);
        dump_gp_subobjects(&gp.rmodels[lod]);
        dump_gp_polys(&gp.rmodels[lod]);
        dump_gp_materials(&gp.rmodels[lod]);
        dump_gp_extra_polys(&gp.rmodels[lod]);
        dump_gp_part_animations(&gp.rmodels[lod]);
    }

    dump_gp_ctrl_regs(&gp);
    dump_gp_lights(&gp);
    dump_gp_vstream(&gp);

    printf("=== RVERTS: %zu ===\n", gp.rvert_count);
    // Just summary for vertices - too many to dump individually
    if (gp.rvert_count > 0) {
        printf("  first: pos=(%.4f, %.4f, %.4f) color=0x%08X\n",
               gp.rverts[0].position[0], gp.rverts[0].position[1], gp.rverts[0].position[2],
               gp.rverts[0].packed_color);
        if (gp.rvert_count > 1) {
            size_t last = gp.rvert_count - 1;
            printf("  last:  pos=(%.4f, %.4f, %.4f) color=0x%08X\n",
                   gp.rverts[last].position[0], gp.rverts[last].position[1], gp.rverts[last].position[2],
                   gp.rverts[last].packed_color);
        }
    }
    printf("\n");

    if (gp.collision) {
        printf("=== COLLISION ===\n");
        printf("  vertices=%d normals=%d faces=%d objects=%d volumes=%d planes=%d\n",
               gp.collision->vertex_count, gp.collision->normal_count,
               gp.collision->face_count, gp.collision->object_count,
               gp.collision->volume_count, gp.collision->plane_count);
        printf("\n");
    }

    if (gp.occlusion) {
        printf("=== OCCLUSION (%zu objects, %zu verts, %zu planes, %zu faces) ===\n",
               gp.occlusion->object_count, gp.occlusion->vertex_count,
               gp.occlusion->plane_count, gp.occlusion->face_count);
        for (size_t i = 0; i < gp.occlusion->object_count; ++i) {
            const ThreediGpOcclusionObject *obj = &gp.occlusion->objects[i];
            printf("  [%zu] pos=(%.3f, %.3f, %.3f) radius=%.3f\n",
                   i, obj->center[0], obj->center[1], obj->center[2],
                   obj->radius);
            printf("       verts=%d planes=%d faces=%d\n",
                   obj->num_vertices, obj->num_planes, obj->num_faces);
            printf("       type=%u parent=%u connecting=%u\n",
                   obj->type, obj->parent_subobject_index, obj->connecting_subobject);
        }
        printf("\n");
    }

    if (gp.matrix_count > 0) {
        printf("=== MATRICES (%zu) ===\n", gp.matrix_count);
        printf("\n");
    }

    threedi_gp_free(&gp);
    return 0;
}

// ============================================================================
// 3DI3 Dump
// ============================================================================

static void dump_3di3_header(const Threedi3di3 *model) {
    printf("=== HEADER ===\n");
    printf("  format             = 3DI3\n");
    printf("  name               = \"%s\"\n", model->header.name);
    const char *mt = model->header.mesh_type == THREEDI_MESH_BASIC ? "BASIC" :
                     model->header.mesh_type == THREEDI_MESH_SKINNED ? "SKINNED" : "UNKNOWN";
    printf("  mesh_type          = %s (%d)\n", mt, model->header.mesh_type);
    printf("  lod_count_decl     = %d\n", model->header.lod_count_decl);
    printf("  lod_count_actual   = %zu\n", model->lod_count);
    printf("  material_count     = %u\n", model->material_count);
    printf("  light_count        = %zu\n", model->light_count);
    printf("  userpoint_count    = %zu\n", model->user_point_count);
    printf("  ctrl_reg_count     = %u\n", model->ctrl.count);
    printf("  matrix_count       = %u\n", model->mtrx.count);
    printf("  panm_count         = %zu\n", model->part_animation_count);
    printf("\n");
}

static void dump_3di3_materials(const Threedi3di3 *model) {
    for (uint32_t i = 0; i < model->material_count; ++i) {
        const ThreediMaterial *mat = &model->materials[i];
        // Find the primary texture name for keying
        const char *tex_name = "";
        for (uint32_t t = 0; t < mat->texture_count; ++t) {
            if (mat->textures[t].slot == THREEDI_TEX_SLOT_DIFFUSE && mat->textures[t].name[0]) {
                tex_name = mat->textures[t].name;
                break;
            }
        }
        printf("=== MATERIAL[%u] \"%s\" ===\n", i, tex_name);
        printf("  shader_name        = \"%s\"\n", mat->shader_name);
        printf("  material_flags     = 0x%02X\n", mat->material_flags);
        printf("  texture_count      = %u\n", mat->texture_count);
        for (uint32_t t = 0; t < mat->texture_count && t < 24; ++t) {
            const ThreediMaterialTexture *tx = &mat->textures[t];
            if (tx->name[0] == 0) continue;
            printf("  tex[%u]             = \"%s\" slot=%u type=%u flags=0x%02X frame=%u\n",
                   t, tx->name, tx->slot, tx->type, tx->flags, tx->frame);
        }
        printf("  emissive_type      = %u\n", mat->emissive_type);
        printf("  alpha_test_byte    = %u (%.4f)\n", mat->alpha_test_value_byte,
               mat->alpha_test_value_byte / 255.0f);
        printf("  is_glass           = %d\n", mat->is_glass);

        // Alpha gen
        printf("  alpha_gen          = style=%u phase=%.4f reg=%d rate=%.4f start=%d end=%d\n",
               mat->alpha_gen.style, mat->alpha_gen.phase, mat->alpha_gen.reg,
               mat->alpha_gen.rate, mat->alpha_gen.start, mat->alpha_gen.end);

        // RGB gen
        printf("  rgb_gen            = style=%u phase=%.4f reg=%d rate=%.4f\n",
               mat->rgb_gen.style, mat->rgb_gen.phase, mat->rgb_gen.reg, mat->rgb_gen.rate);
        printf("  rgb_gen.start      = (%.4f, %.4f, %.4f, %.4f)\n",
               mat->rgb_gen.start_color[0], mat->rgb_gen.start_color[1],
               mat->rgb_gen.start_color[2], mat->rgb_gen.start_color[3]);
        printf("  rgb_gen.end        = (%.4f, %.4f, %.4f, %.4f)\n",
               mat->rgb_gen.end_color[0], mat->rgb_gen.end_color[1],
               mat->rgb_gen.end_color[2], mat->rgb_gen.end_color[3]);

        // UV params
        printf("  u_params           = style=%u phase=%.4f reg=%d rate=%.4f start=%.4f end=%.4f\n",
               mat->u_params.style, mat->u_params.phase, mat->u_params.reg,
               mat->u_params.gen_rate, mat->u_params.start, mat->u_params.end);
        printf("  v_params           = style=%u phase=%.4f reg=%d rate=%.4f start=%.4f end=%.4f\n",
               mat->v_params.style, mat->v_params.phase, mat->v_params.reg,
               mat->v_params.gen_rate, mat->v_params.start, mat->v_params.end);

        // Reflect color
        printf("  reflect_color      = (%.4f, %.4f, %.4f, %.4f)\n",
               mat->reflect_color[0], mat->reflect_color[1],
               mat->reflect_color[2], mat->reflect_color[3]);

        // Animation
        printf("  animation          = frames=%u type=%u cycle_time=%d\n",
               mat->animation.num_frames, mat->animation.animation_type,
               mat->animation.cycle_frame_time);
        printf("\n");
    }
}

static void dump_3di3_subobjects(const Threedi3di3 *model) {
    for (size_t lod = 0; lod < model->lod_count; ++lod) {
        const ThreediLod *l = &model->lods[lod];
        printf("### LOD %zu (type=\"%s\" threshold=%d) ###\n\n", lod, l->model_type, l->lod_threshold);
        printf("  render_objects=%zu strips=%zu vertices=%u indices=%u\n\n",
               l->render_object_count, l->strip_count,
               l->vertices.count, l->indices.count);

        for (size_t i = 0; i < l->render_object_count; ++i) {
            const ThreediRenderObject *ro = &l->render_objects[i];
            printf("=== SUBOBJECT[%zu] ===\n", i);
            printf("  parent             = %d\n", ro->parent_index);
            printf("  num_strips         = %d\n", ro->num_strips);
            printf("  num_alpha_strips   = %d\n", ro->num_alpha_strips);
            printf("  rel                = (%.6f, %.6f, %.6f)\n", ro->rel[0], ro->rel[1], ro->rel[2]);
            printf("  abs                = (%.6f, %.6f, %.6f)\n", ro->abs[0], ro->abs[1], ro->abs[2]);
            printf("  bounding_center    = (%.6f, %.6f, %.6f)\n", ro->bounding_center[0], ro->bounding_center[1], ro->bounding_center[2]);
            printf("  bounding_radius    = %.6f\n", ro->bounding_radius);
            printf("\n");
        }

        // Dump strips summary
        printf("=== PRIMITIVES (%zu) ===\n", l->strip_count);
        for (size_t i = 0; i < l->strip_count; ++i) {
            const ThreediTriangleStrip *s = &l->strips[i];
            printf("  [%3zu] mat=%d topo=%d start_v=%d num_v=%d idx_off=%d num_idx=%u\n",
                   i, s->material_index, s->is_strip, s->start_vertex,
                   s->num_vertices, s->index_offset, s->num_indices);
        }
        printf("\n");
    }
}

static void dump_3di3_lights(const Threedi3di3 *model) {
    if (model->light_count == 0) return;
    printf("=== LIGHTS (%zu) ===\n", model->light_count);
    for (size_t i = 0; i < model->light_count; ++i) {
        const ThreediLight *l = &model->lights[i];
        printf("  [%zu] style=%u phase=%u rate=%u\n", i, l->style, l->phase, l->rate);
        printf("       color_start=(%u,%u,%u,%u) color_end=(%u,%u,%u,%u)\n",
               l->color_start[0], l->color_start[1], l->color_start[2], l->color_start[3],
               l->color_end[0], l->color_end[1], l->color_end[2], l->color_end[3]);
        printf("       pos=(%.4f, %.4f, %.4f) atten=(%.4f, %.4f) subobj=%u flags=%u\n",
               l->offset[0], l->offset[1], l->offset[2],
               l->atten_start, l->atten_end, l->subobj_index, l->flags);
    }
    printf("\n");
}

static void dump_3di3_userpoints(const Threedi3di3 *model) {
    if (model->user_point_count == 0) return;
    printf("=== USERPOINTS (%zu) ===\n", model->user_point_count);
    for (size_t i = 0; i < model->user_point_count; ++i) {
        const ThreediUserPoint *up = &model->user_points[i];
        printf("  [%zu] \"%s\" xyz=(%d,%d,%d) rot=(%d,%d,%d) parent=%d type=%d\n",
               i, up->name, up->x, up->y, up->z,
               up->rot_x, up->rot_y, up->rot_z,
               up->subobject_index, up->userpoint_type);
    }
    printf("\n");
}

static void dump_3di3_part_animations(const Threedi3di3 *model) {
    if (model->part_animation_count == 0) return;
    printf("=== PART ANIMATIONS (%zu) ===\n", model->part_animation_count);
    for (size_t i = 0; i < model->part_animation_count; ++i) {
        const ThreediPartAnimation *pa = &model->part_animations[i];
        printf("  [%zu] flags=0x%08X parent=%u subobj=%u mat_idx=%u mat_off=%u bind=%d\n",
               i, pa->flags, pa->parent_subobject, pa->subobject_index,
               pa->matrix_index, pa->matrix_offset, pa->bind_matrix_index);
        printf("       rot_x=(ctrl=%u p=%u rate=%d s=%d e=%d)\n",
               pa->rotation_x.control, pa->rotation_x.control_param,
               pa->rotation_x.rate, pa->rotation_x.start, pa->rotation_x.end);
        printf("       rot_y=(ctrl=%u p=%u rate=%d s=%d e=%d)\n",
               pa->rotation_y.control, pa->rotation_y.control_param,
               pa->rotation_y.rate, pa->rotation_y.start, pa->rotation_y.end);
        printf("       rot_z=(ctrl=%u p=%u rate=%d s=%d e=%d)\n",
               pa->rotation_z.control, pa->rotation_z.control_param,
               pa->rotation_z.rate, pa->rotation_z.start, pa->rotation_z.end);
        printf("       scale_x=(ctrl=%u p=%u rate=%d s=%d e=%d)\n",
               pa->scale_x.control, pa->scale_x.control_param,
               pa->scale_x.rate, pa->scale_x.start, pa->scale_x.end);
        printf("       translate=(ctrl=%u p=%u rate=%d s=%d e=%d)\n",
               pa->translation.control, pa->translation.control_param,
               pa->translation.rate, pa->translation.start, pa->translation.end);
    }
    printf("\n");
}

static void dump_3di3_ctrl_regs(const Threedi3di3 *model) {
    if (model->ctrl.count == 0) return;
    printf("=== CONTROL REGISTERS (%u) ===\n", model->ctrl.count);
    for (uint32_t i = 0; i < model->ctrl.count; ++i) {
        printf("  [%u] \"%s\"\n", i, model->ctrl.registers[i].name);
    }
    printf("\n");
}

static int dump_3di3(const char *path) {
    Threedi3di3 model;
    memset(&model, 0, sizeof(model));

    if (threedi_3di3_read(path, &model) != 0) {
        fprintf(stderr, "Failed to parse 3DI3 file: %s\n", path);
        threedi_3di3_free(&model);
        return 1;
    }

    dump_3di3_header(&model);
    dump_3di3_userpoints(&model);
    dump_3di3_subobjects(&model);
    dump_3di3_materials(&model);
    dump_3di3_lights(&model);
    dump_3di3_part_animations(&model);
    dump_3di3_ctrl_regs(&model);

    if (model.collision) {
        printf("=== COLLISION ===\n");
        printf("  vertices=%d normals=%d faces=%d objects=%zu volumes=%zu planes=%zu\n",
               model.collision->model_data.num_vertices,
               model.collision->model_data.num_normals,
               model.collision->model_data.num_faces,
               model.collision->object_count,
               model.collision->volume_count,
               model.collision->plane_count);
        printf("\n");
    }

    if (model.occlusion_object_count > 0) {
        printf("=== OCCLUSION (%zu objects, %zu verts, %zu planes, %zu faces) ===\n",
               model.occlusion_object_count, model.occlusion_vertex_count,
               model.occlusion_plane_count, model.occlusion_face_count);
        for (size_t i = 0; i < model.occlusion_object_count; ++i) {
            const ThreediOcclusionObject *obj = &model.occlusion_objects[i];
            printf("  [%zu] pos=(%.3f, %.3f, %.3f) radius=%.3f unk1=%d\n",
                   i, obj->position[0], obj->position[1], obj->position[2],
                   obj->radius, obj->unk1);
            printf("       verts=%d planes=%d faces=%d\n",
                   obj->num_vertices, obj->num_planes, obj->face_count);
            printf("       type=%u parent=%u connecting=%u\n",
                   obj->type, obj->parent_subobject_index, obj->connecting_subobject);
        }
        printf("\n");
    }

    if (model.mtrx.count > 0) {
        printf("=== MATRICES (%u) ===\n", model.mtrx.count);
        printf("\n");
    }

    threedi_3di3_free(&model);
    return 0;
}

// ============================================================================
// IR Dump (unified representation for cross-format comparison)
// ============================================================================

static void dump_ir_materials(const ThreediModelIR *ir) {
    printf("=== IR MATERIALS (%zu) ===\n\n", ir->material_count);
    for (size_t i = 0; i < ir->material_count; ++i) {
        const ThreediIRMaterial *m = &ir->materials[i];

        // Find diffuse texture name for heading
        const char *diffuse = "";
        for (size_t t = 0; t < m->texture_count; ++t) {
            if (m->textures[t].slot == THREEDI_IR_TEX_SLOT_DIFFUSE && m->textures[t].name[0]) {
                diffuse = (const char *)m->textures[t].name;
                break;
            }
        }

        printf("  [%2d] \"%s\"\n", m->index, diffuse);
        printf("       shader           = \"%s\"\n", m->shader_name);
        printf("       textures         = %u\n", m->texture_count);
        for (size_t t = 0; t < m->texture_count; ++t) {
            const ThreediIRMaterialTexture *tx = &m->textures[t];
            if (tx->name[0] == 0) continue;
            const char *slot_name = tx->slot == THREEDI_IR_TEX_SLOT_DIFFUSE ? "diffuse" :
                                    tx->slot == THREEDI_IR_TEX_SLOT_DETAIL ? "detail" :
                                    tx->slot == THREEDI_IR_TEX_SLOT_NORMAL ? "normal" : "other";
            printf("         [%zu] \"%s\" slot=%s type=%u flags=0x%02X\n",
                   t, tx->name, slot_name, tx->type, tx->flags);
        }
        const char *blend_str = m->blend_mode == THREEDI_IR_BLEND_OPAQUE ? "opaque" :
                                m->blend_mode == THREEDI_IR_BLEND_ALPHA ? "alpha" :
                                m->blend_mode == THREEDI_IR_BLEND_ADD ? "additive" : "?";
        printf("       blend            = %s\n", blend_str);
        printf("       flags            = 0x%02X", m->flags);
        if (m->flags) {
            printf(" (");
            int first = 1;
            if (m->flags & THREEDI_IR_MATERIAL_FLAG_ALPHA_TEST) { printf("ALPHA_TEST"); first = 0; }
            if (m->flags & THREEDI_IR_MATERIAL_FLAG_ALPHA_INVERT) { if (!first) printf("|"); printf("ALPHA_INVERT"); first = 0; }
            if (m->flags & THREEDI_IR_MATERIAL_FLAG_TWO_SIDED) { if (!first) printf("|"); printf("TWO_SIDED"); first = 0; }
            if (m->flags & THREEDI_IR_MATERIAL_FLAG_EMISSIVE) { if (!first) printf("|"); printf("EMISSIVE"); first = 0; }
            printf(")");
        }
        printf("\n");
        printf("       is_glass         = %d\n", m->is_glass);
        if (m->specular_intensity > 0)
            printf("       specular         = %u\n", m->specular_intensity);
        if (m->luminosity > 0)
            printf("       luminosity       = %u\n", m->luminosity);
        if (m->emissive_type > 0)
            printf("       emissive_type    = %u\n", m->emissive_type);
        if (m->emissive_color != 0)
            printf("       emissive_color   = 0x%08X\n", m->emissive_color);
        if (m->u_tiling != 0.0f || m->v_tiling != 0.0f)
            printf("       tiling           = (%.4f, %.4f)\n", m->u_tiling, m->v_tiling);
        if (m->alpha_threshold > 0.0f)
            printf("       alpha_threshold  = %.4f\n", m->alpha_threshold);
        if (m->reflect_color[0] != 0 || m->reflect_color[1] != 0 || m->reflect_color[2] != 0)
            printf("       reflect          = (%.4f, %.4f, %.4f, %.4f)\n",
                   m->reflect_color[0], m->reflect_color[1], m->reflect_color[2], m->reflect_color[3]);
        if (m->rgb_gen.style != 0)
            printf("       rgb_gen          = style=%u phase=%.4f reg=%d rate=%.4f\n",
                   m->rgb_gen.style, m->rgb_gen.phase, m->rgb_gen.reg, m->rgb_gen.rate);
        if (m->alpha_gen.style != 0)
            printf("       alpha_gen        = style=%u phase=%.4f reg=%d rate=%.4f\n",
                   m->alpha_gen.style, m->alpha_gen.phase, m->alpha_gen.reg, m->alpha_gen.rate);
        if (m->u_params.style != 0 || m->v_params.style != 0)
            printf("       uv_anim         = u(style=%u) v(style=%u)\n",
                   m->u_params.style, m->v_params.style);
        printf("\n");
    }
}

static int dump_ir(const char *path, int force_gp, int force_3di3) {
    // Auto-detect format
    if (!force_gp && !force_3di3) {
        FILE *f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "Cannot open: %s\n", path); return 1; }
        uint8_t magic[4];
        size_t n = fread(magic, 1, 4, f);
        fclose(f);
        if (n < 3) { fprintf(stderr, "File too small: %s\n", path); return 1; }

        if (memcmp(magic, "3DI3", 4) == 0) force_3di3 = 1;
        else if (memcmp(magic, "GPM", 3) == 0 || memcmp(magic, "GPS", 3) == 0 || memcmp(magic, "GPP", 3) == 0) force_gp = 1;
        else { fprintf(stderr, "Unknown format\n"); return 1; }
    }

    ThreediModelIR ir;
    threedi_ir_init(&ir);

    if (force_gp) {
        ThreediGpFile gp;
        threedi_gp_init(&gp);
        if (threedi_gp_read(path, &gp) != 0) {
            fprintf(stderr, "Failed to parse GP: %s\n", path);
            threedi_gp_free(&gp);
            return 1;
        }
        if (threedi_ir_from_gp(&gp, &ir) != 0) {
            fprintf(stderr, "Failed GP->IR conversion\n");
            threedi_gp_free(&gp);
            return 1;
        }
        threedi_gp_free(&gp);
    } else {
        Threedi3di3 model;
        memset(&model, 0, sizeof(model));
        if (threedi_3di3_read(path, &model) != 0) {
            fprintf(stderr, "Failed to parse 3DI3: %s\n", path);
            threedi_3di3_free(&model);
            return 1;
        }
        if (threedi_ir_from_3di3(&model, &ir) != 0) {
            fprintf(stderr, "Failed 3DI3->IR conversion\n");
            threedi_3di3_free(&model);
            return 1;
        }
        threedi_3di3_free(&model);
    }

    const char *src_name = ir.source_format == THREEDI_IR_SOURCE_3DI3 ? "3DI3" :
                           ir.source_format == THREEDI_IR_SOURCE_GPM ? "GPM" :
                           ir.source_format == THREEDI_IR_SOURCE_GPS ? "GPS" :
                           ir.source_format == THREEDI_IR_SOURCE_GPP ? "GPP" : "???";
    printf("=== IR HEADER ===\n");
    printf("  name               = \"%s\"\n", ir.name);
    printf("  source_format      = %s\n", src_name);
    printf("  lod_count          = %zu\n", ir.lod_count);
    printf("  material_count     = %zu\n", ir.material_count);
    printf("  light_count        = %zu\n", ir.light_count);
    printf("  userpoint_count    = %zu\n", ir.userpoint_count);
    printf("\n");

    dump_ir_materials(&ir);

    threedi_ir_free(&ir);
    return 0;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: 3di_dump [--gp|--3di3|--ir|--unknowns] <file.3di>\n");
        return 1;
    }

    const char *path = NULL;
    int force_gp = 0, force_3di3 = 0, unknowns_mode = 0, ir_mode = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--gp") == 0) {
            force_gp = 1;
        } else if (strcmp(argv[i], "--3di3") == 0) {
            force_3di3 = 1;
        } else if (strcmp(argv[i], "--unknowns") == 0) {
            unknowns_mode = 1;
        } else if (strcmp(argv[i], "--ir") == 0) {
            ir_mode = 1;
        } else {
            path = argv[i];
        }
    }

    if (!path) {
        fprintf(stderr, "No input file specified.\n");
        return 1;
    }

    // --unknowns implies GP format
    if (unknowns_mode) {
        return dump_gp_unknowns(path);
    }

    // --ir: dump unified IR representation
    if (ir_mode) {
        return dump_ir(path, force_gp, force_3di3);
    }

    // Auto-detect if no flag
    if (!force_gp && !force_3di3) {
        FILE *f = fopen(path, "rb");
        if (!f) {
            fprintf(stderr, "Cannot open: %s\n", path);
            return 1;
        }
        uint8_t magic[4];
        size_t n = fread(magic, 1, 4, f);
        fclose(f);

        if (n < 3) {
            fprintf(stderr, "File too small: %s\n", path);
            return 1;
        }

        if (memcmp(magic, "3DI3", 4) == 0) {
            force_3di3 = 1;
        } else if (memcmp(magic, "GPM", 3) == 0 ||
                   memcmp(magic, "GPS", 3) == 0 ||
                   memcmp(magic, "GPP", 3) == 0) {
            force_gp = 1;
        } else {
            fprintf(stderr, "Unknown format (magic: %02X %02X %02X %02X)\n",
                    magic[0], magic[1], magic[2], magic[3]);
            return 1;
        }
    }

    if (force_gp) {
        return dump_gp(path);
    } else {
        return dump_3di3(path);
    }
}
