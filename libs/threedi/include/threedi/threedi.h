#ifndef THREEDI_H
#define THREEDI_H

// Minimal 3DI chunk reader/builder API.
// Chunks are represented as a simple tree mirroring the 3DI on-disk layout.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef _WIN32
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define THREEDI_EXPORT __declspec(dllexport)
#  else
#    define THREEDI_EXPORT
#  endif
#else
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define THREEDI_EXPORT __attribute__((visibility("default")))
#  else
#    define THREEDI_EXPORT
#  endif
#endif

typedef struct ThreediChunk {
    char id[5];                // FourCC, null-terminated.
    int is_parent;             // Non-zero if the chunk has children.
    const uint8_t *data;       // Pointer into the backing buffer for leaves (no copy).
    size_t data_len;           // Payload length for leaves.
    size_t content_len;        // Declared length from the chunk header (children span or payload size).
    size_t offset;             // Offset into the backing buffer where this chunk header starts.
    struct ThreediChunk *children;
    size_t child_count;
} ThreediChunk;

typedef struct ThreediFile {
    uint32_t version;          // e.g., 259 in ModSuperOED.
    ThreediChunk *root;
    uint8_t *buffer;           // Owning buffer for the entire file.
    size_t buffer_len;
} ThreediFile;

// Utility/version info.
THREEDI_EXPORT const char *threedi_version(void);
THREEDI_EXPORT int threedi_smoke_self_check(void);

// Load a 3DI file from disk into a chunk tree. Returns 0 on success.
THREEDI_EXPORT int threedi_read_file(const char *path, ThreediFile *out_file);

// Write a previously-read 3DI chunk tree back to disk.
THREEDI_EXPORT int threedi_write_file(const char *path, const ThreediFile *file);

// Recursively free a ThreediFile and its chunks.
THREEDI_EXPORT void threedi_free_file(ThreediFile *file);

#ifdef __cplusplus
}
#endif

#endif // THREEDI_H
