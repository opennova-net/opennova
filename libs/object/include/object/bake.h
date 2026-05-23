// OED 3DI export session API.
// Parse ASE scenes once, then export/re-export with project updates.

#ifndef OPENNOVA_OBJECT_BAKE_H
#define OPENNOVA_OBJECT_BAKE_H

#include <stdint.h>
#include "tdp/tdp.h"
#include "threedi/threedi_3di3.h"

#ifdef _WIN32
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define OBJECT_EXPORT __declspec(dllexport)
#  else
#    define OBJECT_EXPORT
#  endif
#else
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define OBJECT_EXPORT __attribute__((visibility("default")))
#  else
#    define OBJECT_EXPORT
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum BakeStatus {
    BAKE_STATUS_OK = 0,
    BAKE_STATUS_INVALID_ARGUMENT = -1,
    BAKE_STATUS_ALLOCATION_FAILED = -2,
    BAKE_STATUS_ASE_PARSE_FAILED = -3,
    BAKE_STATUS_CONVERT_FAILED = -4,
    BAKE_STATUS_EXPORT_FAILED = -5,
    BAKE_STATUS_PROJECT_PARSE_FAILED = -6,
    BAKE_STATUS_MISSING_ASE = -7,
} BakeStatus;

// Re-export mask bits (matches original ModSuperOED ReExport3DI bit layout).
enum {
    BAKE_UPDATE_MTRL = 1u << 0,  // Rebuild material chunk data.
    BAKE_UPDATE_LGHT = 1u << 1,  // Rebuild light chunk data.
    BAKE_UPDATE_PANM = 1u << 2,  // Rebuild per-LOD part animation chunk data.
    BAKE_UPDATE_ALL  = BAKE_UPDATE_MTRL | BAKE_UPDATE_LGHT | BAKE_UPDATE_PANM,
};

typedef struct BakeExportRequest {
    // Required.
    const TdpProject *project;
    const char *output_path;

    // Optional. 0 defaults to BAKE_UPDATE_ALL.
    uint8_t update_mask;

    // Optional override for GHDR model name. If null/empty, output filename stem is used.
    const char *model_name;
} BakeExportRequest;

// Opaque export session holding parsed ASE workspaces.
typedef struct BakeSession BakeSession;

// Create a session by parsing/converting ASE scene files (one per render LOD).
// The project is used to seed initial materials/lights/part animations.
OBJECT_EXPORT BakeStatus bake_session_create(const char **ase_paths,
                                        int ase_count,
                                        const TdpProject *project,
                                        BakeSession **out_session);

// Export/re-export using the existing session and a fresh TdpProject view.
OBJECT_EXPORT BakeStatus bake_session_export(BakeSession *session,
                                        const BakeExportRequest *request);

// Build an in-memory 3DI3 model from the existing session and a fresh project
// view. The caller owns the returned allocations and must call
// threedi_3di3_free() when done.
OBJECT_EXPORT BakeStatus bake_session_build_model(BakeSession *session,
                                             const TdpProject *project,
                                             uint8_t update_mask,
                                             const char *model_name,
                                             Threedi3di3 *out_model);

OBJECT_EXPORT void bake_session_destroy(BakeSession *session);

// Convenience entry point for authoring-source parity tests and Python FFI:
// parse a .3dp project, resolve its referenced LOD ASE files next to the
// project path, bake, export, and clean up all native allocations.
OBJECT_EXPORT BakeStatus bake_project_export(const char *project_path,
                                        const char *output_path,
                                        const char *model_name,
                                        uint8_t update_mask);

#ifdef __cplusplus
}
#endif

#endif // OPENNOVA_OBJECT_BAKE_H
