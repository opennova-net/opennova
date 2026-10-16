#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

namespace opennova::pff {

/* --- Format constants --- */
inline constexpr int PFF_HEADER_SIZE = 20;
inline constexpr int PFF_ENTRY_SIZE = 36;
inline constexpr int PFF_NAME_SIZE = 16;

/* Known magic values */
inline constexpr uint32_t PFF_MAGIC_PFF3 = 0x33464650u;  /* "PFF3" - JO/DFX2 era */
inline constexpr uint32_t PFF_MAGIC_PFF4 = 0x34464650u;  /* "PFF4" - JO/DFX2 era */
inline constexpr uint32_t PFF_MAGIC_BHD = 0x34460001u;  /* BHD variant           */

/* Entry flags. Bit 0 marks an ENCRYPTED payload (it is NOT a "deleted" marker): the
   engine XOR-decrypts such entries when reading them. Verified against Jointops.exe
   PFF_LoadFileToMemory @ 0x768920 (see docs/vfs/vfs-pff-mount-re.md). */
inline constexpr uint32_t PFF_FLAG_ENCRYPTED = 0x01u;

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
    uint32_t timestamp;         /* a Unix time in retail's archives; 0 hides the entry from the
                                   effect loaders (PFF_NEW_ENTRY_TIMESTAMP) */
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
int pff_open(PffArchive *archive, const char *path);

/* Open a legacy (pre-PFF3) archive: entry count at file offset 12, 16-byte directory records
   {12-byte name across 3 dwords XOR 0xACEDDEAD, file offset}, sizes delta-encoded
   (size[i] = offset[i+1] - offset[i], so there are count+1 records on disk). Returns 0 on
   success. Models Jointops.exe PFF_OpenLegacyArchive @ 0x7683f0. Legacy archives carry no
   magic, so the caller must select this explicitly (auto-detecting a magic-less format is
   unsafe); retail JO uses only the modern format. */
int pff_open_legacy(PffArchive *archive, const char *path);

/* Close and free internal resources. */
void pff_close(PffArchive *archive);

/* Find entry by name (case-insensitive, binary search). Returns NULL if not found.
   Models Jointops.exe PFF_FindEntry @ 0x7685d0. */
const PffEntry *pff_find(const PffArchive *archive, const char *name);

/* Extract a file entry into caller-provided buffer. out_buf must be at least entry->size
   bytes. If the entry is flagged PFF_FLAG_ENCRYPTED the payload is XOR-decrypted in place
   (models PFF_LoadFileToMemory @ 0x768920). Returns 0 on success. */
int pff_extract(const PffArchive *archive, const PffEntry *entry,
                uint8_t *out_buf, size_t buf_size);

/* Like pff_extract but returns the raw archived bytes without decrypting, even when the
   entry is flagged encrypted. */
int pff_extract_raw(const PffArchive *archive, const PffEntry *entry,
                uint8_t *out_buf, size_t buf_size);

/* Check if raw data starts with a valid PFF header. */
int pff_is_pff(const uint8_t *data, size_t size);

/* An entry's 16-byte name as the directory stores it: to its first NUL, all sixteen bytes when it
   fills the field with none after it; no case change, no trim (pff_norm_name makes the lookup's form). */
std::string pff_entry_stored_name(const PffEntry &entry);

/* Normalize a PFF name into an uppercase, trailing-space-trimmed C string. The uppercase is
   the engine's sort and lookup key (PFF_SortEntries @ 0x768280 strupr's each name); the trim is
   OpenNova's own identity: retail trims nowhere, PFF_FindEntry @ 0x7685d0's loop starting on the
   NUL (D-VFS-7), so "PAD.DAT " and "PAD.DAT" are two names to the game and one here. Reads up
   to raw_cap bytes or until a NUL; result capped to out_sz - 1 chars.
   The reader's lookup, the writer's directory sort + duplicate detection and
   normalized_logical_name (ADR 0046 d6: one flat identity per logical name) all key on it. */
void pff_norm_name(const char *raw, size_t raw_cap, char *out, size_t out_sz);

/* OpenNova's identity for a logical name: the PFF normalization (uppercase, trailing spaces
   trimmed, pff_norm_name) that the reader's lookup and the writer's directory share, of the whole name to its
   first NUL however long (a filter compares a record's text by it, which may run past any name's
   length). */
std::string normalized_logical_name(std::string_view name);

/* The output-name rules a build must satisfy: at most PFF_NAME_SIZE bytes and not empty after
   normalization (the writer's PFF_WRITE_ERR_NAME_LEN and PFF_WRITE_ERR_NAME_EMPTY). */
bool logical_name_fits_archive(std::string_view name);

/* --- Write API --- */

/* The writers' own version: bumped whenever pff_write_archive and PffStreamWriter would write other
   bytes for the same entries (their order, the header, the directory, the footer), so a build that
   keys an archive by its entries' content keys it by the writer too, and never takes an archive the
   older writer packed for one this writer would pack (ADR 0046 S13 A8). 2: the directory in the
   entries' own order and the retail footer (1: sorted by name, no footer). */
inline constexpr uint32_t PFF_WRITER_VERSION = 2;

/* The 12 bytes the retail packer ends an archive with, after the directory: the packing machine's
   IPv4 address as gethostbyname(gethostname()) answers it, in network order
   [orig: sub_403360 @ 0x403360 (pack.exe)]; a second word kept from the archive's earlier footer
   (zero for a new archive), which the packer overwrites with the address only on a 65.211.238.x
   machine; and 'KING' [orig: sub_4040E0 @ 0x4042B7..0x40432F (pack.exe)]
   [orig: sub_4030E0 @ 0x40312B..0x40318A (pack.exe)]. No reader reads it (PFF_Open @ 0x7682e0
   reads the header and the directory). Every JO:CA archive ends with c0a8a939 00000000 'KING'
   (192.168.169.57, a private address, presumably the packing machine's); OpenNova writes that
   first word, never its own machine's address, so an archive is the same bytes on every machine
   and a JO:CA archive written again is its own bytes (D-VFS-15). The second word is the packer's:
   an archive written again or edited keeps its earlier footer's (pff_footer_kept_word), a new one
   writes 0. */
inline constexpr int PFF_FOOTER_SIZE = 12;
inline constexpr uint8_t PFF_RETAIL_FOOTER[PFF_FOOTER_SIZE] = { 0xC0, 0xA8, 0xA9, 0x39, 0x00, 0x00,
                                                                 0x00, 0x00, 'K',  'I',  'N',  'G' };

/* The footer's second word the packer keeps when it writes an archive again: the archive's last 12
   bytes read whole, and kept when they end in 'KING', else all three words zero
   [orig: sub_4030E0 @ 0x403133..0x40318A (pack.exe)]; its four bytes as a little-endian word. 0 for
   an archive with no such footer (one OpenNova's first writer packed, or none at all). */
uint32_t pff_footer_kept_word(const PffArchive *archive);

/* Container format selector for a written archive (legacy is read-only; not authored here). */
typedef enum PffFormat {
    PFF_FORMAT_PFF3 = 0,   /* magic "PFF3" */
    PFF_FORMAT_PFF4 = 1,   /* magic "PFF4" */
    PFF_FORMAT_BHD  = 2     /* BHD variant  */
} PffFormat;

/* The magic an archive of `format` is written with (PFF3 for a value of no format), and its inverse:
   the format an archive that carries `magic` is written again in (PFF3 for a magic of neither PFF4
   nor BHD). */
uint32_t pff_magic_for_format(PffFormat format);
PffFormat pff_format_for_magic(uint32_t magic);

/* The +12 word a writer stamps a NEW entry with (a retained entry keeps its source's): never 0, since
   the game's effect loaders walk each archive's directory and skip an entry whose +12 word is 0
   [orig: CEffectSystem_Init @ 0x5f64c0; HLSLEffect_LoadAllFromPFFArchive @ 0x5aff26], so a .ptl,
   .ptu or .fx packed with 0 never loads (D-VFS-12), and retail stamps every entry with a Unix time
   (the 11,576 entries of JO:CA's five archives: 473418166..1248404912). One fixed time, so the same
   files write the same bytes: 2004-06-15 00:00 UTC. */
inline constexpr uint32_t PFF_NEW_ENTRY_TIMESTAMP = 1087257600u;

/* One stored entry to serialize. `data` is the EXACT bytes that will live in the archive: it is
   already container-XOR-encrypted iff (flags & PFF_FLAG_ENCRYPTED). pff_write_archive applies NO
   SCR/BFC1/XOR transform to payloads. timestamp/checksum are written verbatim (PFF_Open and
   PFF_LoadFileToMemory read neither, the effect loaders the timestamp: use the source values for
   retained entries, and PFF_NEW_ENTRY_TIMESTAMP with a 0 checksum for new ones). */
typedef struct PffWriteEntry {
    const char    *name;       /* original-case logical name; > PFF_NAME_SIZE bytes is rejected */
    const uint8_t *data;       /* stored payload bytes, or NULL when size == 0                  */
    uint32_t       size;
    uint32_t       flags;      /* copied verbatim (bit 0 = PFF_FLAG_ENCRYPTED)                  */
    uint32_t       timestamp;  /* copied verbatim                                               */
    uint32_t       checksum;   /* copied verbatim                                               */
} PffWriteEntry;

/* pff_write_archive return codes. */
inline constexpr int PFF_WRITE_OK = 0;
inline constexpr int PFF_WRITE_ERR_IO = -1;  /* file open/write/rename failure, or NULL data with size  */
inline constexpr int PFF_WRITE_ERR_NAME_LEN = -2;  /* a name exceeds PFF_NAME_SIZE bytes                       */
inline constexpr int PFF_WRITE_ERR_NAME_EMPTY = -3;  /* a name normalizes to empty (blank / whitespace only)     */
inline constexpr int PFF_WRITE_ERR_DUP_NAME = -4;  /* two entries share a normalized (uppercased) name         */
inline constexpr int PFF_WRITE_ERR_TOO_LARGE = -5;  /* total payload size overflows the uint32 offset space     */

/* A PFF_WRITE_* code in words, for a message ("a name is too long for an archive"); an unknown code
   reads as PFF_WRITE_ERR_IO's. */
const char *pff_write_error_string(int code);

/* Write a modern archive in retail's layout: header(20) | payloads | directory(36 each) | the
   footer (PFF_RETAIL_FOOTER), the payloads and the directory both in the entries' own order, as
   every retail archive lists its entries in the order their payloads lie (our pff_open and the
   retail loader sort the directory on load). The write goes to "<path>.tmp" then atomically
   renames onto `path`, so a crash never leaves a half-written archive. Returns PFF_WRITE_OK on
   success or one of the PFF_WRITE_ERR_* codes. Models the on-disk layout of PFF_Open @ 0x7682e0. */
int pff_write_archive(const char *path, PffFormat format,
                                 const PffWriteEntry *entries, uint32_t n);

/* Streaming variant. The writer pulls each entry's stored bytes via `read_entry` as it serializes,
   so only one payload is buffered at a time (the editor's Save-As streams retained payloads
   straight from the still-open source archive instead of materializing the whole archive in RAM).
   read_entry must fill `out` with exactly `size` bytes for the entry at `index` (its index in the
   array, which is also its written place) and return 0 on success, non-zero on failure. Same
   layout, name validation and atomic temp-rename as pff_write_archive: both run PffStreamWriter
   (pff_stream_writer.h) to the end in one call. */
typedef int (*PffReadEntryFn)(void *ctx, uint32_t index, uint8_t *out, uint32_t size);

typedef struct PffWriteStreamEntry {
    const char *name;       /* original-case logical name; > PFF_NAME_SIZE bytes is rejected */
    uint32_t    size;
    uint32_t    flags;
    uint32_t    timestamp;
    uint32_t    checksum;
} PffWriteStreamEntry;

/* `footer_kept` is the footer's second word (pff_footer_kept_word of the archive this one writes
   again; 0 for a new archive). */
int pff_write_archive_streamed(const char *path, PffFormat format,
                                          const PffWriteStreamEntry *entries, uint32_t n,
                                          PffReadEntryFn read_entry, void *ctx,
                                          uint32_t footer_kept = 0);

/* The archive `source` written again to `path` with one entry's bytes replaced: every other entry's
   stored bytes as the source holds them (an encrypted one still encrypted, its flags, time and
   checksum kept, pff_extract_raw), the entry at `index` (into source->entries) `size` bytes of
   `data`, stored plain (flags 0) under its own name, time and checksum; every entry in the source's
   payload order (pff_payload_order), in the source's format (pff_format_for_magic of its magic);
   a stock packer's dead-space records (pff_entry_is_dead_space) left out. The write is pff_write_archive_streamed's (temp file, then the
   rename onto `path`), so `path` is never the source's own file while it is open. Returns
   PFF_WRITE_OK or a PFF_WRITE_ERR_* code (PFF_WRITE_ERR_IO for an index past the entries). */
int pff_rewrite_with_entry(const PffArchive *source, uint32_t index, const uint8_t *data, uint32_t size,
                           const char *path);

/* One entry an update writes into an archive: stored plain (flags 0) under `name`, stamped with
   `timestamp` (the command row's time, which UPDATE.EXE's precheck compares within 2 s) and, in
   the checksum word (+32), io::novalogic_file_crc of its bytes: the CRC the patch-era pack.exe
   stamps and the precheck compares with the row's crc (JO:CA's one patched entry carries it; the
   2005 build's entries the plain CRC-32/BZIP2; docs/update/updater-re.md §7, §10, ADR 0061 d8).
   [orig: Cupdate2Dlg_PrecheckCommands @ 0x407820 (UPDATE.EXE)] */
struct PffEditEntry {
	std::string name;
	std::vector<uint8_t> data;
	uint32_t timestamp = 0;
};

/* The archive at `path` with `adds` written in (an entry whose normalized name matches is
   replaced) and `removes` taken out (a name the archive lacks is nothing to do), as pack.exe's
   `/FORCE` add and `-name` remove leave its live entries (D-UPD-1: in process, never a child
   pack.exe; written compact, where pack.exe keeps a removed or replaced entry's bytes as a
   `<DEAD SPACE>` record and refills such holes). Every other entry keeps its stored bytes,
   flags, time and checksum and its place in the archive's payload order (pff_payload_order);
   the adds follow them; a stock packer's dead-space records are left out when it is written
   (pff_entry_is_dead_space; they alone never make it written). An archive that does not exist is created (PFF3, ADR 0008). Nothing is written when no entry would change (an add identical to the
   entry it replaces, a remove of a missing name), and `*rewritten` says whether it was. The new
   archive is written beside `path` ("<path>.new") and then put in its place. Returns PFF_WRITE_OK
   or a PFF_WRITE_ERR_* code. */
int pff_apply_edits(const char *path, const std::vector<PffEditEntry> &adds,
		const std::vector<std::string> &removes, bool *rewritten);

/* A record the retail packer keeps for the bytes an edit removed or replaced: its flags word
   exactly 1, which the packer's directory walk names `<DEAD SPACE>`
   [orig: sub_4040E0 @ 0x4041B0..0x4041C4 (pack.exe)]. A stock-updated archive holds one per
   removed or replaced entry (updater-re §14), so several share the name. pff_apply_edits,
   pff_write_archive_again and pff_rewrite_with_entry leave them out (the archive written
   compact, D-UPD-1); the game never reads one by name. */
bool pff_entry_is_dead_space(const PffEntry &entry);

/* pff_apply_edits with nothing to apply, written whatever: the archive at `path` written to
   `out_path` (never `path` itself) with every entry kept as an edit keeps it. A retail archive
   written so is its own bytes (ctest pff_retail_rewrite, over JO:CA's five). Returns PFF_WRITE_OK
   or a PFF_WRITE_ERR_* code (PFF_WRITE_ERR_IO for an archive that does not open). */
int pff_write_archive_again(const char *path, const char *out_path);

/* `archive`'s entries (indices into archive->entries, which pff_open sorts by name) in the order
   their payloads lie in the file: by offset, an empty entry before the payload that starts where it
   does. A retail archive's directory lists them so (the packer's list order), and the writers keep
   it. Two empty entries at one offset come back in name order, which pff_open's sort leaves no
   way to tell apart (no JO:CA archive holds an empty entry). */
std::vector<uint32_t> pff_payload_order(const PffArchive &archive);

/* Optional progress callback for the streaming writer: invoked once per entry as payloads are
   written, with `done` running 1..n and `total` == n (fires for zero-size entries too, so `done`
   always reaches n; never fires for a zero-entry archive). */
typedef void (*PffWriteProgressFn)(void *ctx, uint32_t done, uint32_t total);

/* As pff_write_archive_streamed, plus a progress callback. pff_write_archive_streamed forwards here
   with progress == NULL, so the layout/behavior is identical when no progress is wanted. */
int pff_write_archive_streamed_progress(const char *path, PffFormat format,
                                                   const PffWriteStreamEntry *entries, uint32_t n,
                                                   PffReadEntryFn read_entry, void *ctx,
                                                   PffWriteProgressFn progress, void *progress_ctx,
                                                   uint32_t footer_kept = 0);

/* In-place symmetric container XOR (PFF_FLAG_ENCRYPTED keystream). XOR is its own inverse, so the
   same call encrypts (before storing a payload) and decrypts (after reading one). `container_key`
   is the keystream seed; every reversed NovaLogic game uses 0x0312A4CE (PFF_LoadFileToMemory
   @ 0x768920). No-op when buf is NULL or size is 0. */
void pff_container_xor(uint8_t *buf, size_t size, uint32_t container_key);

} // namespace opennova::pff
