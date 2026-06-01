// LW .3di v10 parse test — header + materials against real fixtures.
// Fixtures live in fixtures/threedi/lw/ (copied from Land Warrior game data).
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "threedi/threedi_lw.h"
#include "common/test_paths.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
} while (0)

static void check_face_ranges(const ThreediLwFile *m, const char *tag);

static uint8_t *read_fixture_bytes(const char *root, const char *file, size_t *out_len) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/threedi/lw/%s", root, file);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        free(buf);
        return NULL;
    }
    *out_len = (size_t)len;
    return buf;
}

static void test_arblu(const char *root) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/threedi/lw/ARBLU.3DI", root);

    ThreediLwFile m;
    threedi_lw_init(&m);
    CHECK(threedi_lw_read(path, &m) == 0, "ARBLU: read failed");
    CHECK(m.version == THREEDI_LW_VERSION_10, "ARBLU: version != 10");
    CHECK(strcmp(m.name, "ARBLU") == 0, "ARBLU: name");
    CHECK(m.lod_count == 1, "ARBLU: lod_count != 1");
    CHECK(m.material_count == 1, "ARBLU: material_count != 1");
    if (m.material_count >= 1 && m.materials) {
        CHECK(strcmp(m.materials[0].tex_name_0, "AMIDAL.PCX") == 0, "ARBLU: material 0 name");
    }
    CHECK(m.lods != NULL, "ARBLU: lods null");
    if (m.lods) {
        CHECK(m.lods[0].vertex_count == 3, "ARBLU: LOD0 vertex_count != 3");
        CHECK(m.lods[0].normal_count == 1, "ARBLU: LOD0 normal_count != 1");
        CHECK(m.lods[0].subobject_count == 1, "ARBLU: LOD0 subobject_count != 1");
        CHECK(m.lods[0].surface_count == 1, "ARBLU: LOD0 surface_count != 1");
        CHECK(m.lods[0].vertices != NULL, "ARBLU: LOD0 vertices null");
        CHECK(m.lods[0].faceref_count == 1, "ARBLU: LOD0 faceref_count != 1");
        CHECK(m.lods[0].faces != NULL, "ARBLU: LOD0 faces null");
        if (m.lods[0].faces) {
            ThreediLwFace *f = &m.lods[0].faces[0];
            CHECK(f->vertex[0] == 1 && f->vertex[1] == 2 && f->vertex[2] == 0, "ARBLU: face0 vertices");
            CHECK(f->surface_index == 0, "ARBLU: face0 surface_index");
        }
        CHECK(m.lods[0].surfaces != NULL, "ARBLU: surfaces null");
        if (m.lods[0].surfaces) {
            CHECK(m.lods[0].surfaces[0].material_index == 0, "ARBLU: surface0 material");
        }
        if (m.lods[0].subobjects) {
            CHECK(m.lods[0].subobjects[0].normal_count == 1, "ARBLU: subobject[0].normal_count");
        }
        check_face_ranges(&m, "ARBLU: face index out of range");
    }
    threedi_lw_free(&m);
}

// Every face index must be in range for its LOD (defensive layout check).
static void check_face_ranges(const ThreediLwFile *m, const char *tag) {
    for (uint32_t l = 0; l < m->lod_count; ++l) {
        const ThreediLwLod *lod = &m->lods[l];
        if (!lod->faces) continue;
        for (uint32_t fi = 0; fi < lod->faceref_count; ++fi) {
            const ThreediLwFace *f = &lod->faces[fi];
            for (int k = 0; k < 3; ++k) {
                CHECK(f->vertex[k] >= 0 && (uint32_t)f->vertex[k] < lod->vertex_count, tag);
                if (lod->normal_count) {
                    CHECK(f->normal[k] >= 0 && (uint32_t)f->normal[k] < lod->normal_count, tag);
                }
            }
            CHECK(f->surface_index >= 0 && (uint32_t)f->surface_index < lod->surface_count, tag);
        }
    }
}

static void test_50cal(const char *root) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/threedi/lw/50CAL.3DI", root);

    ThreediLwFile m;
    threedi_lw_init(&m);
    CHECK(threedi_lw_read(path, &m) == 0, "50CAL: read failed");
    CHECK(m.version == THREEDI_LW_VERSION_10, "50CAL: version != 10");
    CHECK(m.lod_count == 4, "50CAL: lod_count != 4");
    CHECK(m.material_count == 7, "50CAL: material_count != 7");
    // Q16.16 thresholds 120 / 55 / 15
    CHECK(m.lod_thresholds[0] == (120u << 16), "50CAL: threshold[0]");
    CHECK(m.lod_thresholds[1] == (55u << 16), "50CAL: threshold[1]");
    CHECK(m.lod_thresholds[2] == (15u << 16), "50CAL: threshold[2]");
    if (m.material_count >= 1 && m.materials) {
        CHECK(strcmp(m.materials[0].tex_name_0, "Q50_SIDE.PCX") == 0, "50CAL: material 0 name");
    }
    CHECK(m.lods != NULL, "50CAL: lods null");
    if (m.lods) {
        CHECK(m.lods[0].vertex_count == 192, "50CAL: LOD0 vertex_count != 192");
        CHECK(m.lods[0].normal_count == 187, "50CAL: LOD0 normal_count != 187");
        CHECK(m.lods[0].faceref_count == 165, "50CAL: LOD0 faceref_count != 165");
        CHECK(m.lods[0].subobject_count == 3, "50CAL: LOD0 subobject_count != 3");
        CHECK(m.lods[0].surface_count == 9, "50CAL: LOD0 surface_count != 9");
        CHECK(m.lods[3].vertex_count == 34, "50CAL: LOD3 vertex_count != 34");
        if (m.lods[0].faces) {
            CHECK(m.lods[0].faces[0].vertex[0] == 0 && m.lods[0].faces[0].vertex[1] == 2 &&
                  m.lods[0].faces[0].vertex[2] == 5, "50CAL: face0 vertices");
            CHECK(m.lods[0].faces[1].vertex[0] == 5 && m.lods[0].faces[1].vertex[1] == 3 &&
                  m.lods[0].faces[1].vertex[2] == 0, "50CAL: face1 vertices");
        }
        check_face_ranges(&m, "50CAL: face index out of range");
        // Sub-objects: 3 parts partitioning verts/faces in order.
        CHECK(m.lods[0].subobjects != NULL, "50CAL: subobjects null");
        if (m.lods[0].subobjects) {
            uint32_t sv = 0, sn = 0, sf = 0;
            for (uint32_t s = 0; s < m.lods[0].subobject_count; ++s) {
                sv += m.lods[0].subobjects[s].vertex_count;
                sn += m.lods[0].subobjects[s].normal_count;
                sf += m.lods[0].subobjects[s].face_count;
            }
            CHECK(sv == m.lods[0].vertex_count, "50CAL: subobject verts sum != vertex_count");
            CHECK(sn == m.lods[0].normal_count, "50CAL: subobject normals sum != normal_count");
            CHECK(sf == m.lods[0].faceref_count, "50CAL: subobject faces sum != faceref_count");
            CHECK(m.lods[0].subobjects[0].normal_count == 88, "50CAL: subobject[0].normal_count");
            CHECK(m.lods[0].subobjects[1].normal_count == 81, "50CAL: subobject[1].normal_count");
            CHECK(m.lods[0].subobjects[2].normal_count == 18, "50CAL: subobject[2].normal_count");
            CHECK(m.lods[0].subobjects[2].parent == 1, "50CAL: subobject[2].parent != 1");
            uint32_t vertex_base = 0;
            for (uint32_t s = 0; s < m.lods[0].subobject_count; ++s) {
                for (uint32_t v = 0; v < m.lods[0].subobjects[s].vertex_count; ++v) {
                    CHECK(m.lods[0].vertices[vertex_base + v].w == (int16_t)s,
                          "50CAL: vertex w does not match owning subobject");
                }
                vertex_base += m.lods[0].subobjects[s].vertex_count;
            }
        }
    }
    threedi_lw_free(&m);
}

static void test_badguy_selectors_and_mixed_vertex_bones(const char *root) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/threedi/lw/BADGUY.3DI", root);

    ThreediLwFile m;
    threedi_lw_init(&m);
    CHECK(threedi_lw_read(path, &m) == 0, "BADGUY: read failed");
    CHECK(m.version == THREEDI_LW_VERSION_10, "BADGUY: version != 10");
    CHECK(m.lod_count == 4, "BADGUY: lod_count != 4");
    CHECK(m.material_count == 7, "BADGUY: material_count != 7");
    if (m.materials && m.material_count >= 7) {
        CHECK(m.materials[3].selector_id == 3, "BADGUY: material selector id");
        CHECK(strcmp(m.materials[3].tex_name_0, "AFacArab.pcx") == 0, "BADGUY: material 3 name");
    }
    if (m.lods && m.lods[0].surfaces && m.lods[0].surface_count >= 8) {
        CHECK(m.lods[0].surfaces[4].material_selectors[0] == 3, "BADGUY: face surface selector");
        CHECK(m.lods[0].surfaces[7].material_selectors[0] == 6, "BADGUY: feet surface selector");
    }
    if (m.lods && m.lods[0].vertices && m.lods[0].subobjects) {
        const ThreediLwLod *lod = &m.lods[0];
        uint32_t vertex_base = 0;
        uint32_t mixed_owner_vertices = 0;
        for (uint32_t s = 0; s < lod->subobject_count; ++s) {
            for (uint32_t v = 0; v < lod->subobjects[s].vertex_count; ++v) {
                if (lod->vertices[vertex_base + v].w != (int16_t)s) {
                    ++mixed_owner_vertices;
                }
            }
            vertex_base += lod->subobjects[s].vertex_count;
        }
        CHECK(mixed_owner_vertices > 0, "BADGUY: fixture should contain mixed vertex bone tags");
    }
    check_face_ranges(&m, "BADGUY: face index out of range");
    threedi_lw_free(&m);
}

static void test_v8_is_unsupported(const char *root) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/threedi/lw/JPAN8.3DI", root);

    ThreediLwFile m;
    threedi_lw_init(&m);
    CHECK(threedi_lw_read(path, &m) != 0, "JPAN8: v8 must be rejected as unsupported");
    threedi_lw_free(&m);
}

static void test_rejects_trailing_bytes(const char *root) {
    size_t len = 0;
    uint8_t *buf = read_fixture_bytes(root, "ARBLU.3DI", &len);
    CHECK(buf != NULL, "ARBLU trailing: read bytes");
    if (!buf) return;
    buf[len] = 0x7f;

    ThreediLwFile m;
    threedi_lw_init(&m);
    CHECK(threedi_lw_parse(buf, len + 1, &m) != 0, "ARBLU trailing: parser accepted extra bytes");
    threedi_lw_free(&m);
    free(buf);
}

int main(void) {
    const char *root = test_paths_repo_root(__FILE__);
    test_arblu(root);
    test_50cal(root);
    test_badguy_selectors_and_mixed_vertex_bones(root);
    test_v8_is_unsupported(root);
    test_rejects_trailing_bytes(root);

    if (failures) {
        printf("lw_parse test: %d failures\n", failures);
        return 1;
    }
    printf("lw_parse test: all checks passed\n");
    return 0;
}
