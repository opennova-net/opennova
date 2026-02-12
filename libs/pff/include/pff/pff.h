#ifndef PFF_H
#define PFF_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define PFF_EXPORT __declspec(dllexport)
#  else
#    define PFF_EXPORT
#  endif
#else
#  ifdef OPENNOVA_SHARED_EXPORTS
#    define PFF_EXPORT __attribute__((visibility("default")))
#  else
#    define PFF_EXPORT
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* --- Format constants --- */
#define PFF_HEADER_SIZE  20
#define PFF_ENTRY_SIZE   36
#define PFF_NAME_SIZE    16

/* Known magic values */
#define PFF_MAGIC_PFF3   0x33464650u  /* "PFF3" - JO/DFX2 era */
#define PFF_MAGIC_PFF4   0x34464650u  /* "PFF4" - JO/DFX2 era */
#define PFF_MAGIC_BHD    0x34460001u  /* BHD variant           */

/* Entry flags */
#define PFF_FLAG_DELETED 0x01u

/* --- Structs --- */

typedef struct PffHeader {
    uint32_t header_size;       /* Typically 0x14 (20) */
    uint32_t magic;
    uint32_t num_entries;
    uint32_t entry_size;        /* Always 36 */
    uint32_t file_table_offset;
} PffHeader;

typedef struct PffEntry {
    uint32_t flags;
    uint32_t offset;
    uint32_t size;
    uint32_t timestamp;         /* Unknown purpose */
    char     filename[16];      /* Null-padded ASCII */
    uint32_t checksum;          /* Unknown purpose */
} PffEntry;

typedef struct PffArchive {
    PffHeader header;
    PffEntry *entries;
    uint32_t entry_count;       /* Non-deleted entries only */
    /* opaque internals */
    void *_file;
    char _path[260];
} PffArchive;

/* --- API --- */

/* Open a PFF archive. Returns 0 on success, non-zero on error. */
PFF_EXPORT int pff_open(PffArchive *archive, const char *path);

/* Close and free internal resources. */
PFF_EXPORT void pff_close(PffArchive *archive);

/* Find entry by name (case-insensitive). Returns NULL if not found. */
PFF_EXPORT const PffEntry *pff_find(const PffArchive *archive, const char *name);

/* Extract a file entry into caller-provided buffer.
   out_buf must be at least entry->size bytes. Returns 0 on success. */
PFF_EXPORT int pff_extract(const PffArchive *archive, const PffEntry *entry,
                uint8_t *out_buf, size_t buf_size);

/* Check if raw data starts with a valid PFF header. */
PFF_EXPORT int pff_is_pff(const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* PFF_H */
