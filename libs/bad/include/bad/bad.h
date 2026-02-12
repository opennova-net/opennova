// BAD animation file parser — pure C API.
// Flat structs suitable for FFI (ctypes, etc.).

#ifndef BAD_H
#define BAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define BAD_EXPORT __declspec(dllexport)
#  else
#    define BAD_EXPORT
#  endif
#else
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define BAD_EXPORT __attribute__((visibility("default")))
#  else
#    define BAD_EXPORT
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BadBone {
    char name[33];          // Null-terminated bone name (max 32 chars)
    int32_t num_children;
    int32_t child_offset;
    int32_t parent_offset;
    int32_t parent_index;   // -1 if root
    float length;
    float position[3];
    float rotation[9];      // 3x3 rotation matrix, row-major
} BadBone;

typedef struct BadQuaternion {
    float x, y, z, w;
} BadQuaternion;

typedef struct BadChannel {
    uint32_t frame_count;
    uint32_t frame_lengths_offset;
    uint32_t rotations_offset;
    uint16_t *frame_lengths;        // Array of frame_count durations
    BadQuaternion *rotations;       // Array of frame_count quaternions
} BadChannel;

typedef struct BadEvent {
    float velocity[3];
    float bottom;
    float top;
    int32_t trigger;
} BadEvent;

typedef struct BadFile {
    // Header fields
    uint32_t version;
    uint32_t header_size;
    uint32_t fps;
    uint32_t frame_count;
    uint32_t flags;
    uint32_t bone_count;

    // Bones
    BadBone *bones;
    size_t num_bones;

    // Channels (per-bone animation data)
    BadChannel *channels;
    size_t num_channels;

    // Events
    BadEvent *events;
    size_t num_events;

    // Translations (flattened [frame][bone], only when flags & 2)
    float (*translations)[3];       // Array of float[3]
    size_t num_translations;
} BadFile;

// Parse a BAD file. Returns 0 on success, -1 on error.
// On success, caller must eventually call bad_free().
BAD_EXPORT int bad_parse(const char *path, BadFile *out);

// Free all allocations inside a BadFile.
BAD_EXPORT void bad_free(BadFile *bf);

#ifdef __cplusplus
}
#endif

#endif // BAD_H
