#ifndef PFF_H
#define PFF_H

#include <stddef.h>
#include <stdint.h>

#include <io/export.h>
#define PFF_EXPORT OPENNOVA_API

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

/* --- Write API --- */

/* Container format selector for a written archive (legacy is read-only; not authored here). */
typedef enum PffFormat {
    PFF_FORMAT_PFF3 = 0,   /* magic "PFF3" */
    PFF_FORMAT_PFF4 = 1,   /* magic "PFF4" */
    PFF_FORMAT_BHD  = 2     /* BHD variant  */
} PffFormat;

/* One stored entry to serialize. `data` is the EXACT bytes that will live in the archive: it is
   already container-XOR-encrypted iff (flags & PFF_FLAG_ENCRYPTED). pff_write_archive applies NO
   SCR/BFC1/XOR transform to payloads. timestamp/checksum are written verbatim (the engine reads
   neither, verified vs PFF_Open/PFF_LoadFileToMemory; use the source values for retained entries
   and 0 for new ones). */
typedef struct PffWriteEntry {
    const char    *name;       /* original-case logical name; > PFF_NAME_SIZE bytes is rejected */
    const uint8_t *data;       /* stored payload bytes, or NULL when size == 0                  */
    uint32_t       size;
    uint32_t       flags;      /* copied verbatim (bit 0 = PFF_FLAG_ENCRYPTED)                  */
    uint32_t       timestamp;  /* copied verbatim                                               */
    uint32_t       checksum;   /* copied verbatim                                               */
} PffWriteEntry;

/* pff_write_archive return codes. */
#define PFF_WRITE_OK             0
#define PFF_WRITE_ERR_IO        (-1)  /* file open/write/rename failure, or NULL data with size  */
#define PFF_WRITE_ERR_NAME_LEN  (-2)  /* a name exceeds PFF_NAME_SIZE bytes                       */
#define PFF_WRITE_ERR_NAME_EMPTY (-3) /* a name normalizes to empty (blank / whitespace only)     */
#define PFF_WRITE_ERR_DUP_NAME  (-4)  /* two entries share a normalized (uppercased) name         */
#define PFF_WRITE_ERR_TOO_LARGE (-5)  /* total payload size overflows the uint32 offset space     */

/* Write a modern archive: header(20) | payloads | directory(36 each). Entries are emitted sorted
   by normalized name (uppercase + trailing-space trim), matching the engine's on-disk convention
   so naive readers that bsearch without re-sorting still resolve; our pff_open and the retail
   loader both re-sort on load regardless. The write goes to "<path>.tmp" then atomically renames
   onto `path`, so a crash never leaves a half-written archive. Returns PFF_WRITE_OK on success or
   one of the PFF_WRITE_ERR_* codes. Models the on-disk layout of PFF_Open @ 0x7682e0. */
PFF_EXPORT int pff_write_archive(const char *path, PffFormat format,
                                 const PffWriteEntry *entries, uint32_t n);

/* Streaming variant. The writer pulls each entry's stored bytes via `read_entry` as it serializes,
   so only one payload is buffered at a time (the editor's Save-As streams retained payloads
   straight from the still-open source archive instead of materializing the whole archive in RAM).
   read_entry must fill `out` with exactly `size` bytes for the entry at `index` (the ORIGINAL
   array index, not the sorted write position) and return 0 on success, non-zero on failure. Same
   header/payload/directory layout, name validation, sort, and atomic temp-rename as
   pff_write_archive (which is a thin wrapper over this). */
typedef int (*PffReadEntryFn)(void *ctx, uint32_t index, uint8_t *out, uint32_t size);

typedef struct PffWriteStreamEntry {
    const char *name;       /* original-case logical name; > PFF_NAME_SIZE bytes is rejected */
    uint32_t    size;
    uint32_t    flags;
    uint32_t    timestamp;
    uint32_t    checksum;
} PffWriteStreamEntry;

PFF_EXPORT int pff_write_archive_streamed(const char *path, PffFormat format,
                                          const PffWriteStreamEntry *entries, uint32_t n,
                                          PffReadEntryFn read_entry, void *ctx);

/* Optional progress callback for the streaming writer: invoked once per entry as payloads are
   written, with `done` running 1..n and `total` == n (fires for zero-size entries too, so `done`
   always reaches n; never fires for a zero-entry archive). */
typedef void (*PffWriteProgressFn)(void *ctx, uint32_t done, uint32_t total);

/* As pff_write_archive_streamed, plus a progress callback. pff_write_archive_streamed forwards here
   with progress == NULL, so the layout/behavior is identical when no progress is wanted. */
PFF_EXPORT int pff_write_archive_streamed_progress(const char *path, PffFormat format,
                                                   const PffWriteStreamEntry *entries, uint32_t n,
                                                   PffReadEntryFn read_entry, void *ctx,
                                                   PffWriteProgressFn progress, void *progress_ctx);

/* In-place symmetric container XOR (PFF_FLAG_ENCRYPTED keystream). XOR is its own inverse, so the
   same call encrypts (before storing a payload) and decrypts (after reading one). `container_key`
   is the keystream seed; every reversed NovaLogic game uses 0x0312A4CE (PFF_LoadFileToMemory
   @ 0x768920). No-op when buf is NULL or size is 0. */
PFF_EXPORT void pff_container_xor(uint8_t *buf, size_t size, uint32_t container_key);

#ifdef __cplusplus
}
#endif

#endif /* PFF_H */
