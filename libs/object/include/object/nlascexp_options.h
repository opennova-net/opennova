// libs/object/include/object/nlascexp_options.h
#pragma once

#ifdef _WIN32
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define NLASCEXP_EXPORT __declspec(dllexport)
#  else
#    define NLASCEXP_EXPORT
#  endif
#else
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define NLASCEXP_EXPORT __attribute__((visibility("default")))
#  else
#    define NLASCEXP_EXPORT
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

// User-facing subset of nlascexp.dle's export-context fields plus a few
// pyopennova-specific toggles. Defaults mirror the no-dialog branch of
// NLAsciiSceneExporter_DoExport @ 0x1000bb50 (Phase A audit, cross-cutting
// finding 1).
typedef struct NlascexpOptions {
    int include_collisions;     // default 1 (Python writer's default)
    int include_occlusion;      // default 1
    int include_lights;         // default 1
    float scaleby;              // default 1.0 — applied by ModSuperOED's ConvertToInternal
    int float_precision;        // default 4 (matches ctx+88)
} NlascexpOptions;

// Populate a NlascexpOptions struct with default values. Callers should call
// this before mutating individual fields.
NLASCEXP_EXPORT void object_nlascexp_options_init_defaults(NlascexpOptions* opts);

#ifdef __cplusplus
}
#endif
