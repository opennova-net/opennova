// GP format (GPM/GPS/GPP) reader - pure C implementation

// Split out of threedi_gp.cpp (quality campaign W3-3). Motion only — every body is
// unchanged. (libs/threedi is built from the format records in docs/threedi/, not
// from decompiled functions, so there is no original-code citation to carry.)
//
// The read half: the bounds-checked byte reader, the file lifecycle (init/free own
// what the parser allocates), mesh-type detection, and one parse_* per section.

#include "threedi/threedi_gp.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io/log.h>

// ============================================================================
// Byte Reader
// ============================================================================

typedef struct GpReader {
    const uint8_t *data;
    size_t size;
    size_t pos;
    int ok;
} GpReader;

static void gp_reader_init(GpReader *r, const uint8_t *data, size_t size) {
    r->data = data;
    r->size = size;
    r->pos = 0;
    r->ok = 1;
}

static int gp_ensure(GpReader *r, size_t count) {
    if (r->pos + count > r->size) {
        r->ok = 0;
        return 0;
    }
    return 1;
}

static size_t gp_remaining(const GpReader *r) {
    return r->size - r->pos;
}

static uint8_t gp_u8(GpReader *r) {
    if (!gp_ensure(r, 1)) return 0;
    return r->data[r->pos++];
}

static uint16_t gp_u16(GpReader *r) {
    if (!gp_ensure(r, 2)) return 0;
    uint16_t v = (uint16_t)r->data[r->pos]
               | ((uint16_t)r->data[r->pos + 1] << 8);
    r->pos += 2;
    return v;
}

static int16_t gp_i16(GpReader *r) {
    return (int16_t)gp_u16(r);
}

static uint32_t gp_u32(GpReader *r) {
    if (!gp_ensure(r, 4)) return 0;
    uint32_t v = (uint32_t)r->data[r->pos]
               | ((uint32_t)r->data[r->pos + 1] << 8)
               | ((uint32_t)r->data[r->pos + 2] << 16)
               | ((uint32_t)r->data[r->pos + 3] << 24);
    r->pos += 4;
    return v;
}

static int32_t gp_i32(GpReader *r) {
    return (int32_t)gp_u32(r);
}

static float gp_f32(GpReader *r) {
    uint32_t raw = gp_u32(r);
    float v;
    memcpy(&v, &raw, sizeof(float));
    return v;
}

static void gp_skip(GpReader *r, size_t count) {
    if (!gp_ensure(r, count)) return;
    r->pos += count;
}

static void gp_bytes(GpReader *r, uint8_t *dst, size_t count) {
    if (!gp_ensure(r, count)) return;
    memcpy(dst, r->data + r->pos, count);
    r->pos += count;
}

static const uint8_t *gp_span(GpReader *r, size_t count) {
    if (!gp_ensure(r, count)) return NULL;
    const uint8_t *ptr = r->data + r->pos;
    r->pos += count;
    return ptr;
}

static void gp_read_string(GpReader *r, char *out, size_t max_len, size_t read_len) {
    const uint8_t *ptr = gp_span(r, read_len);
    if (!ptr) {
        out[0] = '\0';
        return;
    }
    size_t copy_len = read_len < max_len - 1 ? read_len : max_len - 1;
    size_t i;
    for (i = 0; i < copy_len && ptr[i] != 0; ++i) {
        uint8_t b = ptr[i];
        if (b >= 32 && b < 127) {
            out[i] = (char)b;
        } else if (b == '\t') {
            out[i] = ' ';
        } else if (b < 32) {
            // Skip control characters by reducing i
            continue;
        } else {
            out[i] = '_';
        }
    }
    out[i] = '\0';
}

// ============================================================================
// Init/Free
// ============================================================================

void threedi_gp_init(ThreediGpFile *gp) {
    if (!gp) return;
    memset(gp, 0, sizeof(ThreediGpFile));
}

static void free_rmodel(ThreediGpRModel *rm) {
    if (!rm) return;
    free(rm->subobjects);
    if (rm->polys) {
        for (size_t i = 0; i < rm->poly_count; ++i) {
            free(rm->polys[i].indices);
        }
        free(rm->polys);
    }
    free(rm->extra_polys);
    free(rm->materials);
    free(rm->part_animations);
    free(rm->strip_triplets);
    free(rm->raw_blob);
}

void threedi_gp_free(ThreediGpFile *gp) {
    if (!gp) return;

    free(gp->userpoints);
    free(gp->rverts);

    if (gp->rmodels) {
        for (size_t i = 0; i < gp->rmodel_count; ++i) {
            free_rmodel(&gp->rmodels[i]);
        }
        free(gp->rmodels);
    }

    if (gp->collision) {
        free(gp->collision->vertices);
        free(gp->collision->normals);
        free(gp->collision->faces);
        free(gp->collision->objects);
        free(gp->collision->translations);
        free(gp->collision->planes);
        free(gp->collision->volumes);
        free(gp->collision);
    }

    if (gp->occlusion) {
        free(gp->occlusion->objects);
        free(gp->occlusion->vertices);
        free(gp->occlusion->planes);
        free(gp->occlusion->faces);
        free(gp->occlusion);
    }

    free(gp->material_lookups);

    free(gp->lights);
    free(gp->control_registers);
    free(gp->matrices);

    if (gp->vstream) {
        free(gp->vstream->data);
        free(gp->vstream);
    }

    memset(gp, 0, sizeof(ThreediGpFile));
}

// ============================================================================
// Detection
// ============================================================================

ThreediGpMeshType threedi_gp_detect(const uint8_t *data, size_t data_len) {
    if (!data || data_len < 3) return THREEDI_GP_MESH_UNKNOWN;

    if (memcmp(data, "GPM", 3) == 0) return THREEDI_GP_MESH_BASIC;
    if (memcmp(data, "GPS", 3) == 0) return THREEDI_GP_MESH_STATIC;
    if (memcmp(data, "GPP", 3) == 0) return THREEDI_GP_MESH_SKINNED;

    return THREEDI_GP_MESH_UNKNOWN;
}

// ============================================================================
// Parsing
// ============================================================================

#define GP_ASSERT_ZERO(r, label) do { \
    uint32_t _v = gp_u32(r); \
    if (_v != 0) { \
        opennova::io::logf(opennova::io::LogLevel::kWarn, \
                "[threedi_gp] expected zero for %s, got 0x%08X at pos=%zu", \
                label, _v, (r)->pos - 4); \
        return -1; \
    } \
} while(0)

static int parse_header(GpReader *r, ThreediGpHeader *out) {
    const uint8_t *raw = gp_span(r, THREEDI_GP_HEADER_SIZE);
    if (!raw || !r->ok) return -1;

    // Store raw header for roundtrip
    memcpy(out->raw, raw, THREEDI_GP_HEADER_SIZE);

    // Check magic
    if (memcmp(raw, "GPM", 3) != 0 &&
        memcmp(raw, "GPS", 3) != 0 &&
        memcmp(raw, "GPP", 3) != 0) {
        return -1;
    }

    out->mesh_type = (raw[2] == 'M') ? THREEDI_GP_MESH_BASIC :
                     (raw[2] == 'S') ? THREEDI_GP_MESH_STATIC :
                     (raw[2] == 'P') ? THREEDI_GP_MESH_SKINNED :
                                       THREEDI_GP_MESH_UNKNOWN;

    out->format_ver = raw[0x03];
    if (out->format_ver != 0x02) {
        opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[threedi_gp] unsupported format_ver 0x%02X (expected 0x02)",
                out->format_ver);
        return -1;
    }
    out->revision = (uint32_t)raw[0x04] | ((uint32_t)raw[0x05] << 8) |
                    ((uint32_t)raw[0x06] << 16) | ((uint32_t)raw[0x07] << 24);

    out->flags = (uint32_t)raw[0x18] | ((uint32_t)raw[0x19] << 8) |
                 ((uint32_t)raw[0x1A] << 16) | ((uint32_t)raw[0x1B] << 24);
    out->num_lods = (int32_t)((uint32_t)raw[0x1C] | ((uint32_t)raw[0x1D] << 8) |
                    ((uint32_t)raw[0x1E] << 16) | ((uint32_t)raw[0x1F] << 24));

    // LOD thresholds (Q16.16 fixed-point) at 0x20, 0x24, 0x28
    for (int i = 0; i < 3; ++i) {
        int off = 0x20 + i * 4;
        out->lod_thresholds[i] = (uint32_t)raw[off] | ((uint32_t)raw[off+1] << 8) |
                                 ((uint32_t)raw[off+2] << 16) | ((uint32_t)raw[off+3] << 24);
    }

    // Model type tag at 0x40 (4 chars, repeated to 16 bytes; take first 4)
    memset(out->model_tag, 0, sizeof(out->model_tag));
    for (int i = 0; i < 4; ++i) {
        uint8_t b = raw[0x40 + i];
        out->model_tag[i] = (b >= 32 && b < 127) ? (char)b : '\0';
    }
    out->rverts_count = (int32_t)((uint32_t)raw[0x88] | ((uint32_t)raw[0x89] << 8) |
                        ((uint32_t)raw[0x8A] << 16) | ((uint32_t)raw[0x8B] << 24));
    out->userpoint_count = (int32_t)((uint32_t)raw[0xB0] | ((uint32_t)raw[0xB1] << 8) |
                           ((uint32_t)raw[0xB2] << 16) | ((uint32_t)raw[0xB3] << 24));
    out->ctrl_reg_count = (int32_t)((uint32_t)raw[0xBC] | ((uint32_t)raw[0xBD] << 8) |
                          ((uint32_t)raw[0xBE] << 16) | ((uint32_t)raw[0xBF] << 24));
    out->matrix_count = (int32_t)((uint32_t)raw[0xC4] | ((uint32_t)raw[0xC5] << 8) |
                        ((uint32_t)raw[0xC6] << 16) | ((uint32_t)raw[0xC7] << 24));
    out->occlusion_count = (int32_t)((uint32_t)raw[0xD0] | ((uint32_t)raw[0xD1] << 8) |
                           ((uint32_t)raw[0xD2] << 16) | ((uint32_t)raw[0xD3] << 24));

    // Name at offset 0x08, 16 bytes
    memset(out->name, 0, sizeof(out->name));
    for (int i = 0; i < 16 && raw[0x08 + i] != 0; ++i) {
        uint8_t b = raw[0x08 + i];
        if (b >= 32 && b < 127) {
            out->name[i] = (char)b;
        } else {
            out->name[i] = '_';
        }
    }

    return 0;
}

static int parse_userpoints(GpReader *r, int count, ThreediGpUserPoint **out, size_t *out_count) {
    if (count <= 0) {
        *out = NULL;
        *out_count = 0;
        return 0;
    }

    size_t needed = (size_t)count * 48;
    if (gp_remaining(r) < needed) return -1;

    *out = (ThreediGpUserPoint *)calloc((size_t)count, sizeof(ThreediGpUserPoint));
    if (!*out) return -1;
    *out_count = (size_t)count;

    for (int i = 0; i < count; ++i) {
        ThreediGpUserPoint *up = &(*out)[i];
        up->x = gp_i32(r);
        up->y = gp_i32(r);
        up->z = gp_i32(r);
        up->rot_x = gp_i32(r);
        up->rot_y = gp_i32(r);
        up->rot_z = gp_i32(r);
        up->parent_subobject = gp_i32(r);
        up->type_code = gp_i32(r);
        gp_read_string(r, up->name, sizeof(up->name), 16);
    }

    return r->ok ? 0 : -1;
}

static int parse_material_lookup(GpReader *r, ThreediGpMaterialLookup **out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    if (gp_remaining(r) < 4) return 0;
    uint32_t count = gp_u32(r);
    if (!r->ok) return -1;
    if (count == 0) return 0;

    size_t needed = (size_t)count * 60;
    if (gp_remaining(r) < needed) return -1;

    *out = (ThreediGpMaterialLookup *)calloc(count, sizeof(ThreediGpMaterialLookup));
    if (!*out) return -1;
    *out_count = count;

    for (uint32_t i = 0; i < count; ++i) {
        ThreediGpMaterialLookup *ml = &(*out)[i];
        gp_read_string(r, ml->texture_name, sizeof(ml->texture_name), 16);
        ml->unk_10 = gp_u32(r);
        ml->unk_14 = gp_u32(r);
        ml->unk_18 = gp_u32(r);
        ml->unk_1C = gp_u32(r);
        ml->unk_20 = gp_u32(r);
        ml->seq_index = gp_u8(r);
        ml->pad_25 = gp_u8(r);
        ml->flags_26 = gp_u8(r);
        ml->slot_type = gp_u8(r);
        ml->tex_width = gp_u16(r);
        ml->tex_height = gp_u16(r);
        ml->runtime_2C = gp_u32(r);
        ml->runtime_30 = gp_u32(r);
        ml->unk_34 = gp_u32(r);
        ml->unk_38 = gp_u32(r);
    }
    return r->ok ? 0 : -1;
}

static int parse_collision(GpReader *r, ThreediGpCollision **out) {
    if (gp_remaining(r) < THREEDI_GP_COLLISION_HEADER_SIZE) return 0;

    ThreediGpCollision *col = (ThreediGpCollision *)calloc(1, sizeof(ThreediGpCollision));
    if (!col) return -1;

    // Store raw header for roundtrip
    memcpy(col->raw_header, r->data + r->pos, THREEDI_GP_COLLISION_HEADER_SIZE);

    col->is_skinned = gp_u32(r);
    col->data_size = gp_u32(r);
    gp_u32(r); // unk08

    // mid/min/max (9 floats)
    for (int i = 0; i < 3; ++i) col->mid[i] = gp_f32(r);
    for (int i = 0; i < 3; ++i) col->min[i] = gp_f32(r);
    for (int i = 0; i < 3; ++i) col->max[i] = gp_f32(r);

    col->vertex_count = gp_i32(r);
    GP_ASSERT_ZERO(r, "collision.vertex_ptr");
    col->normal_count = gp_i32(r);
    GP_ASSERT_ZERO(r, "collision.normal_ptr");
    col->face_count = gp_i32(r);
    GP_ASSERT_ZERO(r, "collision.face_ptr");
    col->object_count = gp_i32(r);
    GP_ASSERT_ZERO(r, "collision.object_ptr");
    col->translation_count = gp_i32(r);
    GP_ASSERT_ZERO(r, "collision.translation_ptr");
    col->plane_count = gp_i32(r);
    GP_ASSERT_ZERO(r, "collision.plane_ptr");
    col->volume_count = gp_i32(r);

    // 9 trailing runtime pointers
    for (int _i = 0; _i < 9; ++_i) {
        GP_ASSERT_ZERO(r, "collision.trailing_ptr");
    }

    if (!r->ok) {
        free(col);
        return -1;
    }

    // Parse the collision data blob
    if (col->data_size == 0 || gp_remaining(r) < col->data_size) {
        gp_skip(r, col->data_size);
        *out = col;
        return r->ok ? 0 : -1;
    }

    // Create sub-reader for the data blob
    const uint8_t *blob = gp_span(r, col->data_size);
    if (!blob) { free(col); return -1; }

    GpReader br;
    gp_reader_init(&br, blob, col->data_size);

    // 1. Vertices (8 bytes each)
    if (col->vertex_count > 0) {
        col->vertices = (ThreediGpCollisionVertex *)calloc((size_t)col->vertex_count, sizeof(ThreediGpCollisionVertex));
        if (!col->vertices) { free(col); return -1; }
        for (int32_t i = 0; i < col->vertex_count; ++i) {
            col->vertices[i].x = gp_i16(&br);
            col->vertices[i].y = gp_i16(&br);
            col->vertices[i].z = gp_i16(&br);
            col->vertices[i].material_index = gp_i16(&br);
        }
    }

    // 2. Normals (8 bytes each)
    if (col->normal_count > 0) {
        col->normals = (ThreediGpCollisionNormal *)calloc((size_t)col->normal_count, sizeof(ThreediGpCollisionNormal));
        if (!col->normals) { free(col->vertices); free(col); return -1; }
        for (int32_t i = 0; i < col->normal_count; ++i) {
            col->normals[i].nx = gp_i16(&br);
            col->normals[i].ny = gp_i16(&br);
            col->normals[i].nz = gp_i16(&br);
            col->normals[i].dominant_axis = gp_i16(&br);
        }
    }

    // 3. Faces (44 bytes each)
    if (col->face_count > 0) {
        col->faces = (ThreediGpCollisionFace *)calloc((size_t)col->face_count, sizeof(ThreediGpCollisionFace));
        if (!col->faces) { free(col->normals); free(col->vertices); free(col); return -1; }
        for (int32_t i = 0; i < col->face_count; ++i) {
            ThreediGpCollisionFace *f = &col->faces[i];
            f->vertex_indices[0] = gp_u16(&br);
            f->vertex_indices[1] = gp_u16(&br);
            f->vertex_indices[2] = gp_u16(&br);
            f->normal_index = gp_i16(&br);
            f->plane_d = gp_i32(&br);
            f->bbox_min_x = gp_i32(&br);
            f->bbox_max_x = gp_i32(&br);
            f->bbox_min_y = gp_i32(&br);
            f->bbox_max_y = gp_i32(&br);
            f->bbox_min_z = gp_i32(&br);
            f->bbox_max_z = gp_i32(&br);
            f->surface_flags = gp_i32(&br);
            f->surface_type = gp_u8(&br);
            f->pad = gp_u8(&br);
            f->reserved = gp_i16(&br);
        }
    }

    // 4. Objects (128 bytes each)
    if (col->object_count > 0) {
        col->objects = (ThreediGpCollisionObject *)calloc((size_t)col->object_count, sizeof(ThreediGpCollisionObject));
        if (!col->objects) { free(col->faces); free(col->normals); free(col->vertices); free(col); return -1; }
        for (int32_t i = 0; i < col->object_count; ++i) {
            ThreediGpCollisionObject *obj = &col->objects[i];
            obj->flags = gp_i32(&br);
            obj->vertex_count = gp_i32(&br);
            obj->vertex_ptr = gp_u32(&br);
            obj->face_count = gp_i32(&br);
            obj->face_ptr = gp_u32(&br);
            obj->normal_count = gp_i32(&br);
            obj->normal_ptr = gp_u32(&br);
            obj->volume_count = gp_i32(&br);
            obj->volume_ptr = gp_u32(&br);
            if (obj->vertex_ptr | obj->face_ptr | obj->normal_ptr | obj->volume_ptr) {
                opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[threedi_gp] warning: collision_obj[%d] has stale runtime ptrs "
                        "(v=0x%08X f=0x%08X n=0x%08X vol=0x%08X)",
                        i, obj->vertex_ptr, obj->face_ptr, obj->normal_ptr, obj->volume_ptr);
            }
            obj->parent_subobject = gp_i32(&br);
            for (int j = 0; j < 3; ++j) obj->reserved[j] = gp_i32(&br);
            for (int j = 0; j < 3; ++j) obj->translation[j] = gp_i32(&br);
            obj->bbox_min_x = gp_i32(&br);
            obj->bbox_max_x = gp_i32(&br);
            obj->bbox_min_y = gp_i32(&br);
            obj->bbox_max_y = gp_i32(&br);
            obj->bbox_min_z = gp_i32(&br);
            obj->bbox_max_z = gp_i32(&br);
            for (int j = 0; j < 3; ++j) obj->center[j] = gp_i32(&br);
            obj->bounding_sphere_radius = gp_i32(&br);
            obj->bounding_cylinder_radius = gp_i32(&br);
            obj->bbox_height = gp_i32(&br);
            for (int j = 0; j < 4; ++j) obj->reserved2[j] = gp_i32(&br);
        }
    }

    // 5. Translations (12 bytes each)
    if (col->translation_count > 0) {
        col->translations = (ThreediGpCollisionTranslation *)calloc((size_t)col->translation_count, sizeof(ThreediGpCollisionTranslation));
        if (!col->translations) { free(col->objects); free(col->faces); free(col->normals); free(col->vertices); free(col); return -1; }
        for (int32_t i = 0; i < col->translation_count; ++i) {
            col->translations[i].x = gp_i32(&br);
            col->translations[i].y = gp_i32(&br);
            col->translations[i].z = gp_i32(&br);
        }
    }

    // 6. Planes (16 bytes each)
    if (col->plane_count > 0) {
        col->planes = (ThreediGpCollisionPlane *)calloc((size_t)col->plane_count, sizeof(ThreediGpCollisionPlane));
        if (!col->planes) { free(col->translations); free(col->objects); free(col->faces); free(col->normals); free(col->vertices); free(col); return -1; }
        for (int32_t i = 0; i < col->plane_count; ++i) {
            col->planes[i].a = gp_i32(&br);
            col->planes[i].b = gp_i32(&br);
            col->planes[i].c = gp_i32(&br);
            col->planes[i].d = gp_i32(&br);
        }
    }

    // 7. Volumes (96 bytes each)
    if (col->volume_count > 0) {
        col->volumes = (ThreediGpCollisionVolume *)calloc((size_t)col->volume_count, sizeof(ThreediGpCollisionVolume));
        if (!col->volumes) { free(col->planes); free(col->translations); free(col->objects); free(col->faces); free(col->normals); free(col->vertices); free(col); return -1; }
        for (int32_t i = 0; i < col->volume_count; ++i) {
            ThreediGpCollisionVolume *vol = &col->volumes[i];
            vol->type = gp_i32(&br);
            vol->flags = gp_i32(&br);
            vol->min_x = gp_i32(&br);
            vol->max_x = gp_i32(&br);
            vol->min_y = gp_i32(&br);
            vol->max_y = gp_i32(&br);
            vol->min_z = gp_i32(&br);
            vol->max_z = gp_i32(&br);
            for (int j = 0; j < 3; ++j) vol->extent[j] = gp_i32(&br);
            vol->reserved1 = gp_i32(&br);
            vol->bbox_min_x = gp_i32(&br);
            vol->bbox_max_x = gp_i32(&br);
            vol->bbox_min_y = gp_i32(&br);
            vol->bbox_max_y = gp_i32(&br);
            vol->bbox_min_z = gp_i32(&br);
            vol->bbox_max_z = gp_i32(&br);
            vol->plane_count = gp_i32(&br);
            vol->plane_ptr = gp_u32(&br);
            if (vol->plane_ptr != 0) {
                opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[threedi_gp] expected zero for collision_vol.plane_ptr, got 0x%08X at pos=%zu",
                        vol->plane_ptr, br.pos - 4);
                free(col); return -1;
            }
            for (int j = 0; j < 4; ++j) vol->reserved2[j] = gp_i32(&br);
        }
    }

    if (!br.ok) {
        opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[threedi_gp] collision blob parse incomplete at pos=%zu/%zu", br.pos, br.size);
    }

    *out = col;
    return r->ok ? 0 : -1;
}

static int parse_rverts(GpReader *r, ThreediGpMeshType mesh_type, int count,
                        ThreediGpRVert **out, size_t *out_count) {
    if (count <= 0) {
        *out = NULL;
        *out_count = 0;
        return 0;
    }

    int stride = (mesh_type == THREEDI_GP_MESH_BASIC) ? 44 :
                 (mesh_type == THREEDI_GP_MESH_STATIC) ? 48 : 60;
    size_t needed = (size_t)count * (size_t)stride;
    if (gp_remaining(r) < needed) return -1;

    *out = (ThreediGpRVert *)calloc((size_t)count, sizeof(ThreediGpRVert));
    if (!*out) return -1;
    *out_count = (size_t)count;

    for (int i = 0; i < count; ++i) {
        ThreediGpRVert *rv = &(*out)[i];

        rv->position[0] = gp_f32(r);
        rv->position[1] = gp_f32(r);
        rv->position[2] = gp_f32(r);

        if (mesh_type == THREEDI_GP_MESH_BASIC) {
            rv->normal[0] = gp_f32(r);
            rv->normal[1] = gp_f32(r);
            rv->normal[2] = gp_f32(r);
            rv->packed_color = gp_u32(r);
            rv->has_normal = 1;
        } else if (mesh_type == THREEDI_GP_MESH_STATIC) {
            // GPS: normal(3f) + extra(1f) + packed_color
            rv->normal[0] = gp_f32(r);
            rv->normal[1] = gp_f32(r);
            rv->normal[2] = gp_f32(r);
            gp_f32(r); // extra float (tangent sign / padding)
            rv->packed_color = gp_u32(r);
            rv->has_normal = 1;
        } else {
            // GPP: skinned mesh - weights, indices, then normal(3f)
            rv->bone_weights[0] = gp_f32(r);
            rv->bone_weights[1] = gp_f32(r);
            rv->bone_weights[2] = gp_f32(r);
            rv->bone_indices[0] = gp_u8(r);
            rv->bone_indices[1] = gp_u8(r);
            rv->bone_indices[2] = gp_u8(r);
            rv->bone_indices[3] = gp_u8(r);
            rv->normal[0] = gp_f32(r);
            rv->normal[1] = gp_f32(r);
            rv->normal[2] = gp_f32(r);
            rv->packed_color = gp_u32(r);
            rv->is_skinned = 1;
            rv->has_normal = 1;
        }

        rv->uv0[0] = gp_f32(r);
        rv->uv0[1] = gp_f32(r);
        rv->uv1[0] = gp_f32(r);
        rv->uv1[1] = gp_f32(r);
    }

    return r->ok ? 0 : -1;
}

static int parse_rmodel(GpReader *r, ThreediGpMeshType mesh_type, ThreediGpRModel *out) {
    // Read raw header for roundtrip
    if (gp_remaining(r) < THREEDI_GP_RMODEL_HEADER_SIZE) return -1;
    const uint8_t *hdr_raw = r->data + r->pos;
    memcpy(out->raw_header, hdr_raw, THREEDI_GP_RMODEL_HEADER_SIZE);

    uint32_t data_size = gp_u32(r);
    GP_ASSERT_ZERO(r, "rmodel.data_ptr");
    uint32_t material_count = gp_u32(r);
    GP_ASSERT_ZERO(r, "rmodel.material_ptr");
    uint32_t subobject_count = gp_u32(r);
    GP_ASSERT_ZERO(r, "rmodel.subobject_ptr");
    uint32_t strip_triplet_count = gp_u32(r);
    GP_ASSERT_ZERO(r, "rmodel.strip_triplet_ptr");

    // Section 0x20-0x4C (12 u32s): 3 have data in 15/1092 files, rest always zero
    out->unk_20 = gp_u32(r);   // 0x20: Q16.16 on skinned/flag models
    gp_u32(r);                  // 0x24: zero
    out->unk_28 = gp_u32(r);   // 0x28: small int (3) on some models
    gp_u32(r);                  // 0x2C: zero
    gp_u32(r);                  // 0x30: zero
    gp_u32(r);                  // 0x34: zero
    out->unk_38 = gp_u32(r);   // 0x38: Q16.16 on flag models
    gp_u32(r);                  // 0x3C: zero
    gp_u32(r);                  // 0x40: zero
    gp_u32(r);                  // 0x44: zero
    out->unk_48 = gp_u32(r);   // 0x48: zero in corpus
    out->unk_4C = gp_u32(r);   // 0x4C: zero in corpus

    out->flags = gp_u32(r); // flags at 0x50
    GP_ASSERT_ZERO(r, "rmodel.zero54");
    GP_ASSERT_ZERO(r, "rmodel.zero58");
    GP_ASSERT_ZERO(r, "rmodel.zero5C");
    out->extra_poly_count = gp_u32(r);
    GP_ASSERT_ZERO(r, "rmodel.zero64");
    out->extra_xform_count = gp_u32(r);

    // 0x6C-0x84: always zero in corpus (7 u32s)
    for (int i = 0; i < 7; ++i) gp_u32(r);

    if (!r->ok) return -1;
    if (gp_remaining(r) < data_size) return -1;

    // Create sub-reader for blob
    const uint8_t *blob = gp_span(r, data_size);
    if (!blob) return -1;

    // Store raw blob for roundtrip
    out->raw_blob = (uint8_t *)malloc(data_size);
    if (!out->raw_blob) return -1;
    memcpy(out->raw_blob, blob, data_size);
    out->raw_blob_len = data_size;

    GpReader rbr;
    gp_reader_init(&rbr, blob, data_size);

    // Parse strip triplets
    out->strip_triplet_count = strip_triplet_count;
    if (strip_triplet_count > 0) {
        out->strip_triplets = (ThreediGpStripTriplet *)calloc(strip_triplet_count, sizeof(ThreediGpStripTriplet));
        if (!out->strip_triplets) return -1;
        for (uint32_t i = 0; i < strip_triplet_count; ++i) {
            out->strip_triplets[i].a = gp_u32(&rbr);
            out->strip_triplets[i].b = gp_u32(&rbr);
            out->strip_triplets[i].c = gp_u32(&rbr);
        }
    }

    // Parse subobjects (72 bytes each)
    out->subobject_count = subobject_count;
    if (subobject_count > 0) {
        out->subobjects = (ThreediGpSubObject *)calloc(subobject_count, sizeof(ThreediGpSubObject));
        if (!out->subobjects) return -1;

        for (uint32_t i = 0; i < subobject_count; ++i) {
            ThreediGpSubObject *sub = &out->subobjects[i];
            sub->unk_00 = gp_u32(&rbr);
            if (sub->unk_00 != 0) {
                opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[threedi_gp] expected zero for subobject.unk_00, got 0x%08X at pos=%zu",
                        sub->unk_00, rbr.pos - 4);
                return -1;
            }
            sub->batch_count = gp_i32(&rbr);
            sub->unk_08 = gp_u32(&rbr);
            sub->parent = gp_i32(&rbr);
            sub->rel[0] = gp_f32(&rbr);
            sub->rel[1] = gp_f32(&rbr);
            sub->rel[2] = gp_f32(&rbr);
            sub->abs[0] = gp_f32(&rbr);
            sub->abs[1] = gp_f32(&rbr);
            sub->abs[2] = gp_f32(&rbr);
            sub->bounding_min[0] = gp_f32(&rbr);
            sub->bounding_min[1] = gp_f32(&rbr);
            sub->bounding_min[2] = gp_f32(&rbr);
            sub->bounding_radius = gp_u32(&rbr);
            sub->bounding_max[0] = gp_f32(&rbr);
            sub->bounding_max[1] = gp_f32(&rbr);
            sub->bounding_max[2] = gp_f32(&rbr);
            sub->visible = gp_u8(&rbr);
            sub->special_flag = gp_u8(&rbr);
            sub->pad[0] = gp_u8(&rbr);
            sub->pad[1] = gp_u8(&rbr);
        }
    }

    // Parse batches
    typedef struct BatchEntry {
        uint32_t opaque_count;
        uint32_t transparent_count;
    } BatchEntry;

    BatchEntry *batches = NULL;
    size_t batch_count = 0;
    int local_batch = (out->flags & 1u) == 0;

    if (local_batch) {
        // Count total batches
        for (size_t i = 0; i < out->subobject_count; ++i) {
            batch_count += (size_t)out->subobjects[i].batch_count;
        }
        if (batch_count > 0) {
            batches = (BatchEntry *)calloc(batch_count, sizeof(BatchEntry));
            if (!batches) return -1;
            size_t idx = 0;
            for (size_t i = 0; i < out->subobject_count; ++i) {
                for (int b = 0; b < out->subobjects[i].batch_count; ++b, ++idx) {
                    GP_ASSERT_ZERO(&rbr, "local_batch.opaque_ptr");
                    batches[idx].opaque_count = gp_u32(&rbr);
                    GP_ASSERT_ZERO(&rbr, "local_batch.transparent_ptr");
                    batches[idx].transparent_count = gp_u32(&rbr);
                    gp_u32(&rbr); // reserved10
                    gp_u32(&rbr); // reserved14
                    gp_u32(&rbr); // reserved18
                    gp_u32(&rbr); // reserved1C
                }
            }
        }
    } else {
        GP_ASSERT_ZERO(&rbr, "global_batch.shared_ptr");
        uint32_t total_batch = gp_u32(&rbr);
        gp_skip(&rbr, 20);
        batch_count = total_batch;
        if (batch_count > 0) {
            batches = (BatchEntry *)calloc(batch_count, sizeof(BatchEntry));
            if (!batches) return -1;
            for (size_t i = 0; i < batch_count; ++i) {
                GP_ASSERT_ZERO(&rbr, "global_batch.opaque_ptr");
                batches[i].opaque_count = gp_u32(&rbr);
                GP_ASSERT_ZERO(&rbr, "global_batch.transparent_ptr");
                batches[i].transparent_count = gp_u32(&rbr);
                gp_u32(&rbr); // reserved10
                gp_u32(&rbr); // reserved14
                gp_u32(&rbr); // reserved18
                gp_u32(&rbr); // reserved1C
            }
        }
    }

    // Count total polys
    size_t total_polys = 0;
    for (size_t i = 0; i < batch_count; ++i) {
        total_polys += batches[i].opaque_count + batches[i].transparent_count;
    }

    // Parse polys
    if (total_polys > 0) {
        out->polys = (ThreediGpVariablePoly *)calloc(total_polys, sizeof(ThreediGpVariablePoly));
        if (!out->polys) {
            free(batches);
            return -1;
        }
        out->poly_count = total_polys;

        size_t poly_idx = 0;
        size_t batch_cursor = 0;

        if (local_batch) {
            for (size_t sub_idx = 0; sub_idx < out->subobject_count; ++sub_idx) {
                int bc = out->subobjects[sub_idx].batch_count;
                for (int b = 0; b < bc && batch_cursor < batch_count; ++b, ++batch_cursor) {
                    BatchEntry *entry = &batches[batch_cursor];
                    for (uint32_t p = 0; p < entry->opaque_count + entry->transparent_count; ++p, ++poly_idx) {
                        ThreediGpVariablePoly *poly = &out->polys[poly_idx];
                        poly->subobject_index = (int32_t)sub_idx;
                        poly->material_index = gp_i32(&rbr);
                        uint32_t indices_tag = gp_u32(&rbr);
                        uint16_t index_count = gp_u16(&rbr);
                        uint16_t triangle_count = gp_u16(&rbr);
                        poly->topology = gp_i32(&rbr);
                        poly->first_vertex = gp_i32(&rbr);
                        poly->max_vertex_index = gp_i32(&rbr);

                        // Local batch: 16 bytes bone_table
                        for (int i = 0; i < 16; ++i) {
                            poly->bone_table[i] = gp_u8(&rbr);
                        }
                        poly->bone_table_length = 16;

                        if (indices_tag != 0x72646441 || triangle_count != 0 ||
                            (poly->topology != 0 && poly->topology != 1)) {
                            free(batches);
                            return -1;
                        }

                        // Parse indices
                        poly->index_count = index_count;
                        if (index_count > 0) {
                            poly->indices = (uint16_t *)malloc(index_count * sizeof(uint16_t));
                            if (poly->indices) {
                                for (uint16_t ii = 0; ii < index_count; ++ii) {
                                    poly->indices[ii] = gp_u16(&rbr);
                                }
                            }
                        }
                    }
                }
            }
        } else {
            for (size_t bi = 0; bi < batch_count; ++bi) {
                BatchEntry *entry = &batches[bi];
                for (uint32_t p = 0; p < entry->opaque_count + entry->transparent_count; ++p, ++poly_idx) {
                    ThreediGpVariablePoly *poly = &out->polys[poly_idx];
                    poly->subobject_index = 0;
                    poly->material_index = gp_i32(&rbr);
                    uint32_t indices_tag = gp_u32(&rbr);
                    uint16_t index_count = gp_u16(&rbr);
                    uint16_t triangle_count = gp_u16(&rbr);
                    poly->topology = gp_i32(&rbr);
                    poly->first_vertex = gp_i32(&rbr);
                    poly->max_vertex_index = gp_i32(&rbr);

                    // Global batch: 16 bytes bone_table + 7 padding + 1 length byte
                    for (int i = 0; i < 16; ++i) {
                        poly->bone_table[i] = gp_u8(&rbr);
                    }
                    gp_skip(&rbr, 7);
                    poly->bone_table_length = (int32_t)gp_u8(&rbr);

                    if (indices_tag != 0x72646441 || triangle_count != 0 ||
                        (poly->topology != 0 && poly->topology != 1)) {
                        free(batches);
                        return -1;
                    }

                    // Parse indices
                    poly->index_count = index_count;
                    if (index_count > 0) {
                        poly->indices = (uint16_t *)malloc(index_count * sizeof(uint16_t));
                        if (poly->indices) {
                            for (uint16_t ii = 0; ii < index_count; ++ii) {
                                poly->indices[ii] = gp_u16(&rbr);
                            }
                        }
                    }
                }
            }
        }
    }

    free(batches);

    // Parse materials (152 bytes each)
    out->material_count = material_count;
    if (material_count > 0) {
        out->materials = (ThreediGpMaterial *)calloc(material_count, sizeof(ThreediGpMaterial));
        if (!out->materials) return -1;

        for (uint32_t i = 0; i < material_count; ++i) {
            ThreediGpMaterial *mat = &out->materials[i];
            gp_read_string(&rbr, mat->texture_name, sizeof(mat->texture_name), 16);
            mat->render_attributes = gp_u32(&rbr);
            mat->physical_attributes = gp_u32(&rbr);
            mat->use_alpha_pcx = gp_u32(&rbr);
            mat->color_rgb[0] = gp_u32(&rbr);
            mat->color_rgb[1] = gp_u32(&rbr);
            mat->color_rgb[2] = gp_u32(&rbr);
            mat->render_lookup = gp_u32(&rbr);
            mat->luminosity = gp_u32(&rbr);
            mat->specular_intensity = gp_u32(&rbr);
            mat->specular_sharpness = gp_u32(&rbr);
            mat->shader_flags = gp_u32(&rbr);
            mat->tex_addressing_mode = gp_u32(&rbr);
            mat->reserved_40 = gp_u32(&rbr);
            mat->reserved_44 = gp_u32(&rbr);
            mat->u_tiling = gp_f32(&rbr);
            mat->v_tiling = gp_f32(&rbr);
            // mapfunc_u (8 bytes)
            mat->mapfunc_u.style = gp_u8(&rbr);
            mat->mapfunc_u.param = gp_u8(&rbr);
            mat->mapfunc_u.rate = gp_i16(&rbr);
            mat->mapfunc_u.start = gp_i16(&rbr);
            mat->mapfunc_u.end = gp_i16(&rbr);
            // mapfunc_v (8 bytes)
            mat->mapfunc_v.style = gp_u8(&rbr);
            mat->mapfunc_v.param = gp_u8(&rbr);
            mat->mapfunc_v.rate = gp_i16(&rbr);
            mat->mapfunc_v.start = gp_i16(&rbr);
            mat->mapfunc_v.end = gp_i16(&rbr);
            // rgbgen (8 bytes)
            mat->rgbgen.style = gp_u8(&rbr);
            mat->rgbgen.param = gp_u8(&rbr);
            mat->rgbgen.rate = gp_i16(&rbr);
            mat->rgbgen.start = gp_i16(&rbr);
            mat->rgbgen.end = gp_i16(&rbr);
            mat->emissive_color = gp_u32(&rbr);
            // alphagen (8 bytes)
            mat->alphagen.style = gp_u8(&rbr);
            mat->alphagen.param = gp_u8(&rbr);
            mat->alphagen.rate = gp_i16(&rbr);
            mat->alphagen.start = gp_i16(&rbr);
            mat->alphagen.end = gp_i16(&rbr);
            // tail (36 bytes): runtime_ptr, reflect, actionplane, projector
            const uint8_t *tail_data = gp_span(&rbr, 36);
            if (tail_data) {
                memcpy(&mat->runtime_ptr, tail_data, 4);
                memcpy(&mat->reflect_r, tail_data + 4, 4);
                memcpy(&mat->reflect_g, tail_data + 8, 4);
                memcpy(&mat->reflect_b, tail_data + 12, 4);
                memcpy(&mat->reflect_type, tail_data + 16, 4);
                memcpy(&mat->reflect_alpha, tail_data + 20, 4);
                memcpy(&mat->actionplane_type, tail_data + 24, 4);
                memcpy(&mat->projector_type, tail_data + 28, 4);
                memcpy(&mat->projector_no_receive, tail_data + 32, 4);
            }
        }
    }

    // Parse per-part animation data (92 bytes each) if flag 2 is set
    if ((out->flags & 2u) != 0) {
        out->part_animation_count = subobject_count;
        out->part_animations = (ThreediGpPartAnimation *)calloc(subobject_count, sizeof(ThreediGpPartAnimation));
        if (!out->part_animations) { free(batches); return -1; }
        for (uint32_t i = 0; i < subobject_count; ++i) {
            ThreediGpPartAnimation *pa = &out->part_animations[i];
            pa->flags = gp_u32(&rbr);
            pa->parent_subobject = gp_u8(&rbr);
            pa->subobject_index = gp_u8(&rbr);
            pa->matrix_index = gp_u8(&rbr);
            pa->matrix_offset = gp_u8(&rbr);
            pa->bind_matrix_index = gp_i32(&rbr);
            // Read 7 transforms (8 bytes each = 56 bytes) - GP order: scale first, then rotation
            #define READ_ANIM_XFORM(field) do { \
                (field).control = gp_u8(&rbr); \
                (field).param = gp_u8(&rbr); \
                (field).rate = gp_i16(&rbr); \
                (field).start = gp_i16(&rbr); \
                (field).end = gp_i16(&rbr); \
            } while(0)
            READ_ANIM_XFORM(pa->scale_x);
            READ_ANIM_XFORM(pa->scale_y);
            READ_ANIM_XFORM(pa->scale_z);
            READ_ANIM_XFORM(pa->rot_x);
            READ_ANIM_XFORM(pa->rot_y);
            READ_ANIM_XFORM(pa->rot_z);
            READ_ANIM_XFORM(pa->translate);
            #undef READ_ANIM_XFORM
            // legacy rotate_type, scale_type, transform_as, yaw_rate, pitch_rate, roll_rate
            pa->rotate_type = gp_u32(&rbr);
            pa->scale_type = gp_u32(&rbr);
            pa->transform_as = gp_u32(&rbr);
            pa->yaw_rate = gp_f32(&rbr);
            pa->pitch_rate = gp_f32(&rbr);
            pa->roll_rate = gp_f32(&rbr);
        }
    }

    // Parse extra polys (88 bytes each = VariablePoly1[40] + VariablePoly2[48])
    if (out->extra_poly_count > 0) {
        out->extra_polys = (ThreediGpExtraPoly *)calloc(out->extra_poly_count, sizeof(ThreediGpExtraPoly));
        if (!out->extra_polys) return -1;

        for (uint32_t i = 0; i < out->extra_poly_count; ++i) {
            ThreediGpExtraPoly *ep = &out->extra_polys[i];

            // VariablePoly1 (40 bytes): header(24) + reserved(16)
            ep->vp1.material_index = gp_i32(&rbr);
            ep->vp1.indices_tag = gp_u32(&rbr);
            ep->vp1.index_count = gp_u16(&rbr);
            ep->vp1.triangle_count = gp_u16(&rbr);
            ep->vp1.topology = gp_i32(&rbr);
            ep->vp1.first_vertex = gp_i32(&rbr);
            ep->vp1.vertex_count = gp_i32(&rbr);
            for (int j = 0; j < 4; ++j) ep->vp1_reserved[j] = gp_u32(&rbr);

            // VariablePoly2 (48 bytes): header(24) + bbox(24)
            ep->vp2.material_index = gp_i32(&rbr);
            ep->vp2.indices_tag = gp_u32(&rbr);
            ep->vp2.index_count = gp_u16(&rbr);
            ep->vp2.triangle_count = gp_u16(&rbr);
            ep->vp2.topology = gp_i32(&rbr);
            ep->vp2.first_vertex = gp_i32(&rbr);
            ep->vp2.vertex_count = gp_i32(&rbr);
            ep->bbox_min[0] = gp_f32(&rbr);
            ep->bbox_min[1] = gp_f32(&rbr);
            ep->bbox_min[2] = gp_f32(&rbr);
            ep->bbox_max[0] = gp_f32(&rbr);
            ep->bbox_max[1] = gp_f32(&rbr);
            ep->bbox_max[2] = gp_f32(&rbr);
        }
    }

    (void)mesh_type;

    return rbr.ok ? 0 : -1;
}

static int parse_control_registers(GpReader *r, int count,
                                    ThreediGpControlRegister **out, size_t *out_count) {
    if (count <= 0) { *out = NULL; *out_count = 0; return 0; }
    *out = (ThreediGpControlRegister *)calloc((size_t)count, sizeof(ThreediGpControlRegister));
    if (!*out) return -1;
    *out_count = (size_t)count;
    for (int i = 0; i < count; ++i) {
        ThreediGpControlRegister *cr = &(*out)[i];
        gp_read_string(r, cr->name, sizeof(cr->name), 16);
        cr->name_index = gp_u32(r);
        for (int j = 0; j < 6; ++j) {
            cr->param[j] = gp_u32(r);
        }
    }
    return r->ok ? 0 : -1;
}

static int parse_matrices(GpReader *r, int count,
                           ThreediGpMatrix **out, size_t *out_count) {
    if (count <= 0) { *out = NULL; *out_count = 0; return 0; }
    *out = (ThreediGpMatrix *)calloc((size_t)count, sizeof(ThreediGpMatrix));
    if (!*out) return -1;
    *out_count = (size_t)count;
    for (int i = 0; i < count; ++i) {
        for (int j = 0; j < 16; ++j) {
            (*out)[i].m[j] = gp_f32(r);
        }
    }
    return r->ok ? 0 : -1;
}

static int parse_light_info(GpReader *r, ThreediGpFile *gp) {
    /* Outer header: u32 version, u32 payload_size */
    gp->light_version = gp_u32(r);
    uint32_t payload_size = gp_u32(r);
    if (!r->ok) return -1;

    /* Inner header is 60 bytes; light_count is at offset 36 within it */
    if (payload_size < 60) {
        gp_skip(r, payload_size);
        gp->lights = NULL;
        gp->light_count = 0;
        gp->has_ambient_light = 0;
        return r->ok ? 0 : -1;
    }

    size_t inner_start = r->pos;

    /* Ambient light data: 36 bytes (style, rate, phase, start_rgb, end_rgb) */
    gp->ambient_light.colorgen_style = gp_u32(r);
    gp->ambient_light.colorgen_rate = gp_f32(r);
    gp->ambient_light.colorgen_phase = gp_f32(r);
    gp->ambient_light.color_start[0] = gp_f32(r);
    gp->ambient_light.color_start[1] = gp_f32(r);
    gp->ambient_light.color_start[2] = gp_f32(r);
    gp->ambient_light.color_end[0] = gp_f32(r);
    gp->ambient_light.color_end[1] = gp_f32(r);
    gp->ambient_light.color_end[2] = gp_f32(r);
    gp->has_ambient_light = 1;

    uint32_t light_count = gp_u32(r);
    if (!r->ok) return -1;

    /* Read inner header tail: runtime pointer + 4 reserved u32s (all zero on disk) */
    gp->light_entries_ptr = gp_u32(r);
    gp->light_reserved_2C = gp_u32(r);
    gp->light_reserved_30 = gp_u32(r);
    gp->light_reserved_34 = gp_u32(r);
    gp->light_reserved_38 = gp_u32(r);
    if (!r->ok) return -1;

    /* Assert these are always zero on disk (from IDA analysis) */
    assert(gp->light_entries_ptr == 0 && "light_entries_ptr should be zero on disk");
    assert(gp->light_reserved_2C == 0 && "light_reserved_2C should be zero");
    assert(gp->light_reserved_30 == 0 && "light_reserved_30 should be zero");
    assert(gp->light_reserved_34 == 0 && "light_reserved_34 should be zero");
    assert(gp->light_reserved_38 == 0 && "light_reserved_38 should be zero");

    /* Validate: payload should be 60 + 48 * light_count */
    size_t expected = 60 + (size_t)light_count * 48;
    if (expected > payload_size || light_count > 10000) {
        /* Unexpected size, skip remaining payload */
        size_t consumed = r->pos - inner_start;
        if (consumed < payload_size) gp_skip(r, payload_size - consumed);
        gp->lights = NULL;
        gp->light_count = 0;
        return r->ok ? 0 : -1;
    }

    if (light_count == 0) {
        size_t consumed = r->pos - inner_start;
        if (consumed < payload_size) gp_skip(r, payload_size - consumed);
        gp->lights = NULL;
        gp->light_count = 0;
        return 0;
    }

    ThreediGpLight *lights = (ThreediGpLight *)calloc(light_count, sizeof(ThreediGpLight));
    if (!lights) return -1;

    for (uint32_t i = 0; i < light_count; ++i) {
        ThreediGpLight *l = &lights[i];

        /* Bytes 0-3: style, phase, rate(u16 LE) */
        l->style = gp_u8(r);
        l->phase = gp_u8(r);
        l->rate = (uint16_t)(gp_u8(r) | (gp_u8(r) << 8));

        /* Bytes 4-7: color_start RGB + pad */
        l->color_start[0] = gp_u8(r);
        l->color_start[1] = gp_u8(r);
        l->color_start[2] = gp_u8(r);
        gp_u8(r); /* pad */

        /* Bytes 8-11: color_end RGB + pad */
        l->color_end[0] = gp_u8(r);
        l->color_end[1] = gp_u8(r);
        l->color_end[2] = gp_u8(r);
        gp_u8(r); /* pad */

        /* Bytes 12-23: position float[3] */
        l->position[0] = gp_f32(r);
        l->position[1] = gp_f32(r);
        l->position[2] = gp_f32(r);

        /* Bytes 24-31: attenuation */
        l->attenuation_start = gp_f32(r);
        l->attenuation_end = gp_f32(r);

        /* Bytes 32-35: part_index */
        l->part_index = gp_i32(r);

        /* Bytes 36-47: unknown fields */
        l->unk_36 = gp_u32(r);
        l->unk_40 = gp_u32(r);
        l->unk_44 = gp_u32(r);
    }

    if (!r->ok) {
        free(lights);
        return -1;
    }

    /* Skip any remaining payload bytes */
    size_t consumed = r->pos - inner_start;
    if (consumed < payload_size) gp_skip(r, payload_size - consumed);

    gp->lights = lights;
    gp->light_count = (size_t)light_count;
    return r->ok ? 0 : -1;
}

static int parse_vstream(GpReader *r, int rverts_count, ThreediGpVStream **out) {
    ThreediGpVStream *vs = (ThreediGpVStream *)calloc(1, sizeof(ThreediGpVStream));
    if (!vs) return -1;

    vs->buffer_ptr = gp_u32(r);
    if (vs->buffer_ptr != 0) {
        opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[threedi_gp] expected zero for vstream.buffer_ptr, got 0x%08X at pos=%zu",
                vs->buffer_ptr, r->pos - 4);
        free(vs); return -1;
    }
    vs->data_size = gp_u32(r);
    vs->unk_08 = gp_u32(r);
    vs->unk_0C = gp_u32(r);
    if (!r->ok) { free(vs); return -1; }

    uint32_t actual_size = (vs->data_size == 0) ? (uint32_t)(rverts_count * 24) : vs->data_size;
    if (gp_remaining(r) < actual_size) { free(vs); return -1; }

    vs->data = (uint8_t *)malloc(actual_size);
    if (!vs->data) { free(vs); return -1; }
    vs->data_len = actual_size;

    const uint8_t *blob = gp_span(r, actual_size);
    if (!blob) { free(vs->data); free(vs); return -1; }
    memcpy(vs->data, blob, actual_size);

    *out = vs;
    return r->ok ? 0 : -1;
}

static int parse_occlusion(GpReader *r, int count, ThreediGpOcclusion **out) {
    if (count <= 0) {
        *out = NULL;
        return 0;
    }

    ThreediGpOcclusion *occ = (ThreediGpOcclusion *)calloc(1, sizeof(ThreediGpOcclusion));
    if (!occ) return -1;

    occ->object_count = (size_t)count;
    occ->objects = (ThreediGpOcclusionObject *)calloc((size_t)count, sizeof(ThreediGpOcclusionObject));
    if (!occ->objects) { free(occ); return -1; }

    // Parse all object headers (60 bytes each)
    // Layout verified from BHD revision (0x0103) files:
    //   0x00: type, parent_subobj, connecting_subobj, pad (4 bytes)
    //   0x04: center[3] (float) - position/centroid
    //   0x10: radius (float)
    //   0x14: num_vertices (int32) + vertex_ptr (uint32)
    //   0x1C: num_planes (int32) + plane_ptr (uint32)
    //   0x24: num_faces (int32) + face_ptr (uint32)
    //   0x2C: reserved[4] (16 bytes)
    size_t total_verts = 0, total_planes = 0, total_faces = 0;
    for (int i = 0; i < count; ++i) {
        ThreediGpOcclusionObject *obj = &occ->objects[i];

        // 0x00-0x03: type, parent, connecting, pad
        obj->type = gp_u8(r);
        obj->parent_subobject_index = gp_u8(r);
        obj->connecting_subobject = gp_u8(r);
        obj->pad = gp_u8(r);

        // 0x04-0x0F: center (position/centroid)
        obj->center[0] = gp_f32(r);
        obj->center[1] = gp_f32(r);
        obj->center[2] = gp_f32(r);

        // 0x10-0x13: bounding sphere radius
        obj->radius = gp_f32(r);

        // 0x14-0x1B: vertex count + runtime pointer
        obj->num_vertices = (int32_t)gp_u32(r);
        obj->vertex_ptr = gp_u32(r);

        // 0x1C-0x23: plane count + runtime pointer
        obj->num_planes = (int32_t)gp_u32(r);
        obj->plane_ptr = gp_u32(r);

        // 0x24-0x2B: face count + runtime pointer
        obj->num_faces = (int32_t)gp_u32(r);
        obj->face_ptr = gp_u32(r);

        // 0x2C-0x3B: reserved (16 bytes = 4 int32s)
        for (int j = 0; j < 4; ++j) {
            obj->reserved[j] = gp_i32(r);
        }

        total_verts += (size_t)obj->num_vertices;
        total_planes += (size_t)obj->num_planes;
        total_faces += (size_t)obj->num_faces;
    }

    if (!r->ok) {
        free(occ->objects);
        free(occ);
        return -1;
    }

    // Allocate flat arrays
    if (total_verts > 0) {
        occ->vertices = (ThreediGpOcclusionVertex *)calloc(total_verts, sizeof(ThreediGpOcclusionVertex));
        if (!occ->vertices) { free(occ->objects); free(occ); return -1; }
    }
    if (total_planes > 0) {
        occ->planes = (ThreediGpOcclusionPlane *)calloc(total_planes, sizeof(ThreediGpOcclusionPlane));
        if (!occ->planes) { free(occ->vertices); free(occ->objects); free(occ); return -1; }
    }
    if (total_faces > 0) {
        occ->faces = (ThreediGpOcclusionFace *)calloc(total_faces, sizeof(ThreediGpOcclusionFace));
        if (!occ->faces) { free(occ->planes); free(occ->vertices); free(occ->objects); free(occ); return -1; }
    }

    occ->vertex_count = total_verts;
    occ->plane_count = total_planes;
    occ->face_count = total_faces;

    // Parse per-object payload (verts, planes, faces for each object sequentially)
    size_t vert_cursor = 0, plane_cursor = 0, face_cursor = 0;
    for (int i = 0; i < count; ++i) {
        ThreediGpOcclusionObject *obj = &occ->objects[i];

        // Vertices: float[3] x 12 bytes each
        for (int v = 0; v < obj->num_vertices; ++v) {
            occ->vertices[vert_cursor + v].position[0] = gp_f32(r);
            occ->vertices[vert_cursor + v].position[1] = gp_f32(r);
            occ->vertices[vert_cursor + v].position[2] = gp_f32(r);
        }
        vert_cursor += (size_t)obj->num_vertices;

        // Planes: float[4] x 16 bytes each (normal[3] + radius)
        for (int p = 0; p < obj->num_planes; ++p) {
            occ->planes[plane_cursor + p].normal[0] = gp_f32(r);
            occ->planes[plane_cursor + p].normal[1] = gp_f32(r);
            occ->planes[plane_cursor + p].normal[2] = gp_f32(r);
            occ->planes[plane_cursor + p].radius = gp_f32(r);
        }
        plane_cursor += (size_t)obj->num_planes;

        // Faces: uint32[3] x 12 bytes each
        for (int f = 0; f < obj->num_faces; ++f) {
            occ->faces[face_cursor + f].raw_indices = gp_u32(r);
            occ->faces[face_cursor + f].edge_data = gp_u32(r);
            occ->faces[face_cursor + f].other_edge_data = gp_u32(r);
        }
        face_cursor += (size_t)obj->num_faces;
    }

    if (!r->ok) {
        free(occ->faces);
        free(occ->planes);
        free(occ->vertices);
        free(occ->objects);
        free(occ);
        return -1;
    }

    *out = occ;
    return 0;
}

int threedi_gp_parse(const uint8_t *data, size_t data_len, ThreediGpFile *out) {
    if (!data || !out) return -1;

    threedi_gp_init(out);

    GpReader r;
    gp_reader_init(&r, data, data_len);

    // Parse header
    if (parse_header(&r, &out->header) != 0) {
        opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[threedi_gp] parse_header failed at pos=%zu", r.pos);
        threedi_gp_free(out);
        return -1;
    }

    // Parse userpoints
    if (parse_userpoints(&r, out->header.userpoint_count,
                         &out->userpoints, &out->userpoint_count) != 0) {
        opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[threedi_gp] parse_userpoints failed at pos=%zu", r.pos);
        threedi_gp_free(out);
        return -1;
    }

    // Parse material lookup
    if (parse_material_lookup(&r, &out->material_lookups, &out->material_lookup_count) != 0) {
        opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[threedi_gp] parse_material_lookup failed at pos=%zu", r.pos);
        threedi_gp_free(out);
        return -1;
    }

    // Parse collision
    if (parse_collision(&r, &out->collision) != 0) {
        opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[threedi_gp] parse_collision failed at pos=%zu", r.pos);
        threedi_gp_free(out);
        return -1;
    }

    // Parse rverts
    if (parse_rverts(&r, out->header.mesh_type, out->header.rverts_count,
                     &out->rverts, &out->rvert_count) != 0) {
        opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[threedi_gp] parse_rverts failed at pos=%zu", r.pos);
        threedi_gp_free(out);
        return -1;
    }

    // Parse rmodels (LODs)
    if (out->header.num_lods > 0) {
        out->rmodels = (ThreediGpRModel *)calloc((size_t)out->header.num_lods, sizeof(ThreediGpRModel));
        if (!out->rmodels) {
            threedi_gp_free(out);
            return -1;
        }
        out->rmodel_count = (size_t)out->header.num_lods;

        for (int i = 0; i < out->header.num_lods; ++i) {
            if (parse_rmodel(&r, out->header.mesh_type, &out->rmodels[i]) != 0) {
                opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[threedi_gp] parse_rmodel %d failed at pos=%zu", i, r.pos);
                threedi_gp_free(out);
                return -1;
            }
        }
    }

    // Parse control registers
    if (parse_control_registers(&r, out->header.ctrl_reg_count,
                                &out->control_registers, &out->control_register_count) != 0) {
        opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[threedi_gp] parse_control_registers failed at pos=%zu", r.pos);
        threedi_gp_free(out);
        return -1;
    }

    // Parse matrices
    if (parse_matrices(&r, out->header.matrix_count,
                       &out->matrices, &out->matrix_count) != 0) {
        opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[threedi_gp] parse_matrices failed at pos=%zu", r.pos);
        threedi_gp_free(out);
        return -1;
    }

    if ((out->header.flags & 1) != 0) {
        if (parse_light_info(&r, out) != 0) {
            opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[threedi_gp] parse_light_info failed at pos=%zu", r.pos);
            threedi_gp_free(out);
            return -1;
        }
    }

    if ((out->header.flags & 2) != 0) {
        if (parse_vstream(&r, out->header.rverts_count, &out->vstream) != 0) {
            opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[threedi_gp] parse_vstream failed at pos=%zu", r.pos);
            threedi_gp_free(out);
            return -1;
        }
    }

    if ((out->header.flags & 4) != 0) {
        if (parse_occlusion(&r, out->header.occlusion_count, &out->occlusion) != 0) {
            opennova::io::logf(opennova::io::LogLevel::kWarn,
		"[threedi_gp] parse_occlusion failed at pos=%zu", r.pos);
            threedi_gp_free(out);
            return -1;
        }
    }

    return 0;
}

int threedi_gp_read(const char *path, ThreediGpFile *out) {
    if (!path || !out) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) {
        fclose(f);
        return -1;
    }

    uint8_t *data = (uint8_t *)malloc((size_t)size);
    if (!data) {
        fclose(f);
        return -1;
    }

    size_t read = fread(data, 1, (size_t)size, f);
    fclose(f);

    if (read != (size_t)size) {
        free(data);
        return -1;
    }

    int result = threedi_gp_parse(data, (size_t)size, out);
    free(data);

    return result;
}
