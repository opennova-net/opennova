// BAD animation file parser — pure C API.
// Flat structs suitable for FFI (ctypes, etc.).

#ifndef BAD_H
#define BAD_H

#include <stddef.h>
#include <stdint.h>

#include <io/export.h>
#define BAD_EXPORT OPENNOVA_API

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

// Parse a BAD file from an in-memory buffer (does not take ownership of `data`).
// Returns 0 on success, -1 on error. On success, caller must call bad_free().
// Used by embedders that read assets from a VFS (PFF archive) rather than disk.
BAD_EXPORT int bad_parse_buffer(const uint8_t *data, size_t data_size, BadFile *out);

// Free all allocations inside a BadFile.
BAD_EXPORT void bad_free(BadFile *bf);

// --------------------------------------------------------------------------
// Writer (inverse of bad_parse — see bad_write.cpp)
// --------------------------------------------------------------------------

// Header/policy fields the parser does not read back but a faithful file must
// carry. All have stock defaults (see bad_write_options_default); the parser
// ignores them, so they never affect a parse->write->parse round-trip. Carried
// here (not on BadFile) so the import-side ctypes BadFile mirror stays stable.
typedef struct BadWriteOptions {
    uint32_t header_size;            // 80
    uint32_t unknown1;               // 0   (header field @0x20)
    uint32_t num_event_blocks;       // 0   (@0x24, "NumEventBlocks"; suspect vs engine)
    uint32_t unknown3;               // 8   (@0x28)
    uint32_t bone_stride;            // 100 (@0x2C)
    uint32_t frame_rec_stride;       // 12  (@0x30; parser reads this as trans_stride)
    uint32_t rot_stride;             // 16  (@0x34)
    uint32_t pos_offsets_frame_len;  // 1   (@0x38)
    uint32_t unknown6;               // 1   (@0x44)
    uint32_t unknown7;               // 0   (@0x48)
    uint32_t unknown8;               // 0   (@0x4C)
    int zero_bn01_position;          // 0   (off: serializer does not mutate positions)
    int write_bone_index_byte;       // 1   (write i&0xFF into the bone's +35 unknown byte)
} BadWriteOptions;

// Fill opts with stock defaults.
BAD_EXPORT void bad_write_options_default(BadWriteOptions *opts);

// Serialize bf to a .bad file at path. Returns 0 on success, -1 on error.
// opts may be NULL (stock defaults are used).
BAD_EXPORT int bad_write(const char *path, const BadFile *bf, const BadWriteOptions *opts);

// Serialize bf into a freshly malloc'd buffer (*out / *out_size). Caller frees
// with bad_write_buffer_free. Returns 0 on success, -1 on error.
BAD_EXPORT int bad_write_buffer(const BadFile *bf, const BadWriteOptions *opts,
                                uint8_t **out, size_t *out_size);

// Free a buffer returned by bad_write_buffer.
BAD_EXPORT void bad_write_buffer_free(uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif // BAD_H
