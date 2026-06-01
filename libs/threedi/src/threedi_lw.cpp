#include "threedi/threedi_lw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void copy_name(char *dst, size_t dst_size, const uint8_t *src, size_t field) {
    size_t n = field < dst_size - 1 ? field : dst_size - 1;
    size_t i = 0;
    for (; i < n && src[i] != '\0'; ++i) {
        dst[i] = (char)src[i];
    }
    dst[i] = '\0';
}

static int checked_mul_size(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > SIZE_MAX / a) return -1;
    *out = a * b;
    return 0;
}

static int checked_add_size(size_t a, size_t b, size_t *out) {
    if (b > SIZE_MAX - a) return -1;
    *out = a + b;
    return 0;
}

#define LW_V10_HEADER_SIZE     0xA0
#define LW_V10_MATERIAL_SIZE   0x50
#define LW_V10_PRE_RECORD      20
#define LW_V10_LOD_HEADER_SIZE 0xE8
#define LW_V10_FACE_SIZE       0x50
#define LW_V10_SURFACE_SIZE    0x80
#define LW_V10_SUBOBJECT_SIZE  0x78

ThreediLwVersion threedi_lw_detect(const uint8_t *data, size_t len) {
    if (!data || len < 4) return THREEDI_LW_VERSION_UNKNOWN;
    if (memcmp(data, "3DI", 3) != 0) return THREEDI_LW_VERSION_UNKNOWN;
    switch (data[3]) {
        case 8: return THREEDI_LW_VERSION_8;
        case 10: return THREEDI_LW_VERSION_10;
        default: return THREEDI_LW_VERSION_UNKNOWN;
    }
}

void threedi_lw_init(ThreediLwFile *out) {
    if (out) memset(out, 0, sizeof(*out));
}

void threedi_lw_free(ThreediLwFile *out) {
    if (!out) return;
    free(out->materials);
    if (out->lods) {
        for (uint32_t i = 0; i < out->lod_count; ++i) {
            free(out->lods[i].vertices);
            free(out->lods[i].normals);
            free(out->lods[i].faces);
            free(out->lods[i].surfaces);
            free(out->lods[i].subobjects);
        }
        free(out->lods);
    }
    memset(out, 0, sizeof(*out));
}

static ThreediLwVertex *read_v10_vec4(const uint8_t *p, uint32_t count) {
    if (count == 0) return NULL;
    ThreediLwVertex *out = (ThreediLwVertex *)calloc(count, sizeof(ThreediLwVertex));
    if (!out) return NULL;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *r = p + (size_t)i * 8;
        out[i].x = (int16_t)rd_u16(r + 0);
        out[i].y = (int16_t)rd_u16(r + 2);
        out[i].z = (int16_t)rd_u16(r + 4);
        out[i].w = (int16_t)rd_u16(r + 6);
    }
    return out;
}

static ThreediLwFace *read_v10_faces(const uint8_t *p, uint32_t count) {
    if (count == 0) return NULL;
    ThreediLwFace *out = (ThreediLwFace *)calloc(count, sizeof(ThreediLwFace));
    if (!out) return NULL;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *r = p + (size_t)i * LW_V10_FACE_SIZE;
        ThreediLwFace *f = &out[i];
        for (int k = 0; k < 3; ++k) {
            f->u[k] = (int32_t)rd_u32(r + 0x04 + 4 * k);
            f->v[k] = (int32_t)rd_u32(r + 0x10 + 4 * k);
            f->vertex[k] = (int16_t)rd_u16(r + 0x1C + 2 * k);
            f->normal[k] = (int16_t)rd_u16(r + 0x22 + 2 * k);
        }
        f->surface_index = (int32_t)rd_u32(r + 0x4C);
    }
    return out;
}

static ThreediLwSurface *read_v10_surfaces(const uint8_t *p, uint32_t count) {
    if (count == 0) return NULL;
    ThreediLwSurface *out = (ThreediLwSurface *)calloc(count, sizeof(ThreediLwSurface));
    if (!out) return NULL;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *r = p + (size_t)i * LW_V10_SURFACE_SIZE;
        ThreediLwSurface *s = &out[i];
        copy_name(s->name, sizeof(s->name), r + 0x00, 16);
        s->flags = rd_u32(r + 0x10);
        s->material_index = rd_u16(r + 0x18);
        s->anim_frames = r[0x1E];
        memcpy(s->material_selectors, r + 0x34, sizeof(s->material_selectors));
    }
    return out;
}

static ThreediLwSubObject *read_v10_subobjects(const uint8_t *p, uint32_t count) {
    if (count == 0) return NULL;
    ThreediLwSubObject *out = (ThreediLwSubObject *)calloc(count, sizeof(ThreediLwSubObject));
    if (!out) return NULL;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *r = p + (size_t)i * LW_V10_SUBOBJECT_SIZE;
        ThreediLwSubObject *s = &out[i];
        s->vertex_count = rd_u32(r + 0x04);
        s->face_count = rd_u32(r + 0x0C);
        s->normal_count = rd_u32(r + 0x1C);
        s->parent = (int32_t)rd_u32(r + 0x2C);
        s->pos[0] = (int32_t)rd_u32(r + 0x3C);
        s->pos[1] = (int32_t)rd_u32(r + 0x40);
        s->pos[2] = (int32_t)rd_u32(r + 0x44);
    }
    return out;
}

static int parse_v10_lod(const uint8_t *data, size_t len, size_t *pos, ThreediLwLod *lod) {
    size_t p = *pos;
    if (p > len || LW_V10_LOD_HEADER_SIZE > len - p) return -1;
    const uint8_t *h = data + p;

    lod->flags = rd_u32(h + 0x10);
    lod->blob_size = rd_u32(h + 0x14);
    lod->vertex_count = rd_u32(h + 4 * 32);
    lod->normal_count = rd_u32(h + 4 * 34);
    lod->face_count = rd_u32(h + 4 * 36);
    lod->array12_count = rd_u32(h + 4 * 38);
    lod->subobject_count = rd_u32(h + 4 * 40);
    lod->triindex_count = rd_u32(h + 4 * 42);
    lod->surface_count = rd_u32(h + 4 * 44);
    lod->array8_count = rd_u32(h + 4 * 46);
    lod->array80_count = rd_u32(h + 4 * 48);

    size_t blob = 0;
    if (checked_add_size(p, LW_V10_LOD_HEADER_SIZE, &blob) != 0) return -1;
    if ((size_t)lod->blob_size > len - blob) return -1;

    size_t verts_bytes = 0, norms_bytes = 0, faces_bytes = 0, array12_bytes = 0;
    size_t subobj_bytes = 0, triindex_bytes = 0, surface_bytes = 0, array8_bytes = 0, array80_bytes = 0;
    if (checked_mul_size((size_t)lod->vertex_count, 8, &verts_bytes) != 0) return -1;
    if (checked_mul_size((size_t)lod->normal_count, 8, &norms_bytes) != 0) return -1;
    if (checked_mul_size((size_t)lod->face_count, LW_V10_FACE_SIZE, &faces_bytes) != 0) return -1;
    if (checked_mul_size((size_t)lod->array12_count, 12, &array12_bytes) != 0) return -1;
    if (checked_mul_size((size_t)lod->subobject_count, LW_V10_SUBOBJECT_SIZE, &subobj_bytes) != 0) return -1;
    if (checked_mul_size((size_t)lod->triindex_count, 12, &triindex_bytes) != 0) return -1;
    if (checked_mul_size((size_t)lod->surface_count, LW_V10_SURFACE_SIZE, &surface_bytes) != 0) return -1;
    if (checked_mul_size((size_t)lod->array8_count, 8, &array8_bytes) != 0) return -1;
    if (checked_mul_size((size_t)lod->array80_count, 80, &array80_bytes) != 0) return -1;

    size_t normals_off = verts_bytes;
    size_t faces_off = 0, array12_off = 0, subobj_off = 0, triindex_off = 0;
    size_t array8_off = 0, array80_off = 0, surface_off = 0, expected_blob = 0;
    if (checked_add_size(normals_off, norms_bytes, &faces_off) != 0) return -1;
    if (checked_add_size(faces_off, faces_bytes, &array12_off) != 0) return -1;
    if (checked_add_size(array12_off, array12_bytes, &subobj_off) != 0) return -1;
    if (checked_add_size(subobj_off, subobj_bytes, &triindex_off) != 0) return -1;
    if (checked_add_size(triindex_off, triindex_bytes, &array8_off) != 0) return -1;
    if (checked_add_size(array8_off, array8_bytes, &array80_off) != 0) return -1;
    if (checked_add_size(array80_off, array80_bytes, &surface_off) != 0) return -1;
    if (checked_add_size(surface_off, surface_bytes, &expected_blob) != 0) return -1;
    if (expected_blob != (size_t)lod->blob_size) return -1;

    lod->vertices = read_v10_vec4(data + blob, lod->vertex_count);
    lod->normals = read_v10_vec4(data + blob + normals_off, lod->normal_count);
    lod->faces = read_v10_faces(data + blob + faces_off, lod->face_count);
    lod->subobjects = read_v10_subobjects(data + blob + subobj_off, lod->subobject_count);
    lod->surfaces = read_v10_surfaces(data + blob + surface_off, lod->surface_count);
    if ((lod->vertex_count && !lod->vertices) ||
        (lod->normal_count && !lod->normals) ||
        (lod->face_count && !lod->faces) ||
        (lod->subobject_count && !lod->subobjects) ||
        (lod->surface_count && !lod->surfaces)) {
        return -1;
    }

    *pos = blob + lod->blob_size;
    return 0;
}

static int parse_v10(const uint8_t *data, size_t len, ThreediLwFile *out) {
    if (len < LW_V10_HEADER_SIZE) return -1;
    out->version = THREEDI_LW_VERSION_10;
    copy_name(out->name, sizeof(out->name), data + 0x04, 16);
    out->lod_count = rd_u32(data + 0x14);
    if (out->lod_count > 4) return -1;
    out->lod_thresholds[0] = rd_u32(data + 0x18);
    out->lod_thresholds[1] = rd_u32(data + 0x1C);
    out->lod_thresholds[2] = rd_u32(data + 0x20);
    for (uint32_t i = 0; i < out->lod_count; ++i) {
        copy_name(out->render_tags[i], sizeof(out->render_tags[i]), data + 0x28 + 4 * i, 4);
    }

    uint32_t pre_count = rd_u32(data + 0x78);
    size_t pre_bytes = 0;
    if (checked_mul_size((size_t)pre_count, LW_V10_PRE_RECORD, &pre_bytes) != 0) return -1;
    size_t pos = 0;
    if (checked_add_size(LW_V10_HEADER_SIZE, pre_bytes, &pos) != 0) return -1;
    if (pos > len || 4 > len - pos) return -1;

    out->material_count = rd_u32(data + pos);
    pos += 4;
    size_t material_bytes = 0;
    if (checked_mul_size((size_t)out->material_count, LW_V10_MATERIAL_SIZE, &material_bytes) != 0) return -1;
    if (material_bytes > len - pos) return -1;

    if (out->material_count) {
        out->materials = (ThreediLwMaterial *)calloc(out->material_count, sizeof(ThreediLwMaterial));
        if (!out->materials) return -1;
        for (uint32_t i = 0; i < out->material_count; ++i) {
            const uint8_t *rec = data + pos + (size_t)i * LW_V10_MATERIAL_SIZE;
            ThreediLwMaterial *mat = &out->materials[i];
            copy_name(mat->texture0, sizeof(mat->texture0), rec + 0x00, 16);
            copy_name(mat->texture1, sizeof(mat->texture1), rec + 0x10, 16);
            mat->group_id = rd_u32(rec + 0x24);
            mat->selector_id = rd_u16(rec + 0x28);
            mat->flags = rd_u16(rec + 0x2A);
            mat->tex_width = rd_u16(rec + 0x2C);
            mat->tex_height = rd_u16(rec + 0x2E);
        }
        pos += material_bytes;
    }

    if (out->lod_count) {
        out->lods = (ThreediLwLod *)calloc(out->lod_count, sizeof(ThreediLwLod));
        if (!out->lods) return -1;
        for (uint32_t i = 0; i < out->lod_count; ++i) {
            if (parse_v10_lod(data, len, &pos, &out->lods[i]) != 0) return -1;
        }
    }

    if (pos != len) return -1;
    return 0;
}

int threedi_lw_parse(const uint8_t *data, size_t len, ThreediLwFile *out) {
    if (!data || !out) return -1;
    ThreediLwVersion ver = threedi_lw_detect(data, len);
    threedi_lw_init(out);
    int rc = -1;
    if (ver == THREEDI_LW_VERSION_10) {
        rc = parse_v10(data, len, out);
    }
    if (rc != 0) threedi_lw_free(out);
    return rc;
}

int threedi_lw_read(const char *path, ThreediLwFile *out) {
    if (!path || !out) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return -1;
    }
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return -1;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    int rc = (got == (size_t)size) ? threedi_lw_parse(buf, got, out) : -1;
    free(buf);
    return rc;
}
