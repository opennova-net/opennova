// OED 3DI export session API.
// Parse ASE scenes once, then export/re-export with project updates.

#ifndef OED_H
#define OED_H

#include <stdint.h>
#include "tdp/tdp.h"
#include "threedi/threedi_3di3.h"

#include <io/export.h>
#define OED_EXPORT OPENNOVA_API

#ifdef __cplusplus
extern "C" {
#endif

typedef enum OedStatus {
    OED_STATUS_OK = 0,
    OED_STATUS_INVALID_ARGUMENT = -1,
    OED_STATUS_ALLOCATION_FAILED = -2,
    OED_STATUS_ASE_PARSE_FAILED = -3,
    OED_STATUS_CONVERT_FAILED = -4,
    OED_STATUS_EXPORT_FAILED = -5,
} OedStatus;

// Re-export mask bits (matches original ModSuperOED ReExport3DI bit layout).
enum {
    OED_UPDATE_MTRL = 1u << 0,  // Rebuild material chunk data.
    OED_UPDATE_LGHT = 1u << 1,  // Rebuild light chunk data.
    OED_UPDATE_PANM = 1u << 2,  // Rebuild per-LOD part animation chunk data.
    OED_UPDATE_ALL  = OED_UPDATE_MTRL | OED_UPDATE_LGHT | OED_UPDATE_PANM,
};

typedef struct OedExportRequest {
    // Required.
    const TdpProject *project;
    const char *output_path;

    // Optional. 0 defaults to OED_UPDATE_ALL.
    uint8_t update_mask;

    // Optional override for GHDR model name. If null/empty, output filename stem is used.
    const char *model_name;
} OedExportRequest;

// Opaque export session holding parsed ASE workspaces.
typedef struct OedSession OedSession;

// Create a session by parsing/converting ASE scene files (one per render LOD).
// The project is used to seed initial materials/lights/part animations.
OED_EXPORT OedStatus oed_session_create(const char **ase_paths,
                                        int ase_count,
                                        const TdpProject *project,
                                        OedSession **out_session);

// Export/re-export using the existing session and a fresh TdpProject view.
OED_EXPORT OedStatus oed_session_export(OedSession *session,
                                        const OedExportRequest *request);

// Build an in-memory 3DI3 model from the existing session and a fresh project
// view. The caller owns the returned allocations and must call
// threedi_3di3_free() when done.
OED_EXPORT OedStatus oed_session_build_model(OedSession *session,
                                             const TdpProject *project,
                                             uint8_t update_mask,
                                             const char *model_name,
                                             Threedi3di3 *out_model);

// Last session-specific diagnostic for export/build failures. The returned
// pointer remains owned by the session and is valid until the next session call
// or oed_session_destroy().
OED_EXPORT const char *oed_session_last_error(const OedSession *session);

OED_EXPORT void oed_session_destroy(OedSession *session);

#ifdef __cplusplus
}
#endif

#endif // OED_H
