// Land Warrior .3di format definitions and parser.
//
// LW predates the GP-era "GPM/GPS/GPP" and modern "3DI3" containers. Version
// 10 is used by Delta Force: Land Warrior; version 8 is detected but rejected.

#ifndef THREEDI_LW_H
#define THREEDI_LW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ThreediLwVersion {
    THREEDI_LW_VERSION_UNKNOWN = 0,
    THREEDI_LW_VERSION_8 = 8,
    THREEDI_LW_VERSION_10 = 10
} ThreediLwVersion;

ThreediLwVersion threedi_lw_detect(const uint8_t *data, size_t len);

typedef struct ThreediLwMaterial {
    char texture0[17];
    char texture1[17];
    uint32_t group_id;
    uint16_t selector_id;
    uint16_t flags;
    uint16_t tex_width;
    uint16_t tex_height;
} ThreediLwMaterial;

typedef struct ThreediLwVertex {
    int16_t x, y, z, w;
} ThreediLwVertex;

typedef struct ThreediLwFace {
    int32_t u[3];
    int32_t v[3];
    int16_t vertex[3];
    int16_t normal[3];
    int32_t surface_index;
} ThreediLwFace;

typedef struct ThreediLwSubObject {
    uint32_t vertex_count;
    uint32_t face_count;
    uint32_t normal_count;
    int32_t parent;
    int32_t pos[3];
} ThreediLwSubObject;

typedef struct ThreediLwSurface {
    char name[17];
    uint32_t flags;
    uint16_t material_index;
    uint8_t anim_frames;
    uint8_t material_selectors[4];
} ThreediLwSurface;

typedef struct ThreediLwLod {
    uint32_t blob_size;
    uint32_t flags;
    uint32_t vertex_count;
    uint32_t normal_count;
    uint32_t face_count;
    uint32_t array12_count;
    uint32_t subobject_count;
    uint32_t triindex_count;
    uint32_t surface_count;
    uint32_t array8_count;
    uint32_t array80_count;
    ThreediLwVertex *vertices;
    ThreediLwVertex *normals;
    ThreediLwFace *faces;
    ThreediLwSurface *surfaces;
    ThreediLwSubObject *subobjects;
} ThreediLwLod;

typedef struct ThreediLwFile {
    ThreediLwVersion version;
    char name[20];
    uint32_t lod_count;
    uint32_t lod_thresholds[3];
    char render_tags[4][5];
    uint32_t material_count;
    ThreediLwMaterial *materials;
    ThreediLwLod *lods;
} ThreediLwFile;

void threedi_lw_init(ThreediLwFile *out);
int threedi_lw_parse(const uint8_t *data, size_t len, ThreediLwFile *out);
int threedi_lw_read(const char *path, ThreediLwFile *out);
void threedi_lw_free(ThreediLwFile *out);

#ifdef __cplusplus
}
#endif

#endif
