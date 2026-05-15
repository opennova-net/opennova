#ifndef OPENNOVA_THREEDI_SHADER_TAGS_H
#define OPENNOVA_THREEDI_SHADER_TAGS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Canonical shader-tag feature table for OpenNova materials.
 *
 * Mirrors gMaterialInfoTable (oed::kMaterialInfoTable in opennova-godot-new
 * src/libs/oed/types.h) which itself was dumped from the original ModSuperOed
 * binary. The 41 entries here describe how each shader tag uses its texture
 * stages and visual feature flags. Hosts (3ds Max, Blender, future Godot
 * scene builder) consult this table to know how to wire each material; the
 * Importers do not consult it to mutate material fields.
 *
 * The Python mirror lives at pyopennova/materials.py:_MATERIAL_INFO_FLAGS;
 * tests/test_shader_table_parity.py asserts the two stay in sync.
 */

#define THREEDI_MAT_FLAG_EMISSIVE   0x00000001u
#define THREEDI_MAT_FLAG_ALPHA      0x00000002u
#define THREEDI_MAT_FLAG_DIFFUSE    0x00000004u
#define THREEDI_MAT_FLAG_SECONDARY  0x00000008u
#define THREEDI_MAT_FLAG_NORMAL_A   0x00000010u
#define THREEDI_MAT_FLAG_NORMAL_B   0x00000020u
#define THREEDI_MAT_FLAG_SPECIAL    0x00001000u
#define THREEDI_MAT_FLAG_GLASS      0x00002000u
#define THREEDI_MAT_FLAG_FILTER     0x00004000u
#define THREEDI_MAT_FLAG_SMOOTH     0x00008000u
#define THREEDI_MAT_FLAG_UI_TOGGLE  0x00010000u
#define THREEDI_MAT_FLAG_LUMINANCE  0x10000000u

typedef struct ThreediShaderInfo {
    const char *name;
    uint32_t flags;
} ThreediShaderInfo;

extern const ThreediShaderInfo kThreediShaderTable[];
extern const size_t kThreediShaderTableCount;

/* Returns flags for an exact (case-insensitive) tag match, or 0 if unknown. */
uint32_t threedi_shader_flags(const char *tag);

/* Returns 1 when tag is present in the table (non-zero flags), 0 otherwise. */
int threedi_shader_known(const char *tag);

#ifdef __cplusplus
}
#endif

#endif /* OPENNOVA_THREEDI_SHADER_TAGS_H */
