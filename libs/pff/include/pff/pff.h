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

/* Entry flags. Bit 0 marks an ENCRYPTED payload (it is NOT a "deleted" marker): the
   engine XOR-decrypts such entries when reading them. Verified against Jointops.exe
   PFF_LoadFileToMemory @ 0x768920 (see notes/vfs/phase0_ida_verification.md). */
#define PFF_FLAG_ENCRYPTED 0x01u

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
    PffEntry *entries;          /* Sorted by uppercased name for binary-search lookup */
    uint32_t entry_count;
    /* opaque internals */
    void *_file;
    char _path[260];
} PffArchive;

/* --- API --- */

/* Open a modern PFF archive (PFF3/PFF4/BHD). Loads every directory entry (the engine does
   no deleted-entry filtering) and sorts them by uppercased name. Returns 0 on success.
   Models Jointops.exe PFF_Open @ 0x7682e0. */
PFF_EXPORT int pff_open(PffArchive *archive, const char *path);

/* Open a legacy (pre-PFF3) archive: entry count at file offset 12, 16-byte directory records
   {12-byte name across 3 dwords XOR 0xACEDDEAD, file offset}, sizes delta-encoded
   (size[i] = offset[i+1] - offset[i], so there are count+1 records on disk). Returns 0 on
   success. Models Jointops.exe PFF_OpenLegacyArchive @ 0x7683f0. Legacy archives carry no
   magic, so the caller must select this explicitly (auto-detecting a magic-less format is
   unsafe); retail JO uses only the modern format. */
PFF_EXPORT int pff_open_legacy(PffArchive *archive, const char *path);

/* Close and free internal resources. */
PFF_EXPORT void pff_close(PffArchive *archive);

/* Find entry by name (case-insensitive, binary search). Returns NULL if not found.
   Models Jointops.exe PFF_FindEntry @ 0x7685d0. */
PFF_EXPORT const PffEntry *pff_find(const PffArchive *archive, const char *name);

/* Extract a file entry into caller-provided buffer. out_buf must be at least entry->size
   bytes. If the entry is flagged PFF_FLAG_ENCRYPTED the payload is XOR-decrypted in place
   (models PFF_LoadFileToMemory @ 0x768920). Returns 0 on success. */
PFF_EXPORT int pff_extract(const PffArchive *archive, const PffEntry *entry,
                uint8_t *out_buf, size_t buf_size);

/* Like pff_extract but returns the raw archived bytes without decrypting, even when the
   entry is flagged encrypted. */
PFF_EXPORT int pff_extract_raw(const PffArchive *archive, const PffEntry *entry,
                uint8_t *out_buf, size_t buf_size);

/* Check if raw data starts with a valid PFF header. */
PFF_EXPORT int pff_is_pff(const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* PFF_H */
