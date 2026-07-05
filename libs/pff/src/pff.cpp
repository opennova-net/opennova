/* PFF archive format implementation for NovaLogic games (C99 port).
   [orig: PFF_LoadFileToMemory @ 0x768920, PFF_SortEntries @ 0x768280; docs/vfs/vfs-pff-mount-re.md (D-VFS)]
   Supports modern PFF3/PFF4/BHD archives and the legacy (pre-PFF3) format.
   Structural port of the Jointops.exe file subsystem; addresses are cited inline and
   the algorithms are documented in notes/vfs/phase0_ida_verification.md. */

#include "pff/pff.h"

#include "pff_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static int pff_valid_magic(uint32_t magic)
{
    return magic == PFF_MAGIC_PFF3
        || magic == PFF_MAGIC_PFF4
        || magic == PFF_MAGIC_BHD;
}

/* Reject obviously bogus entry counts so a corrupt header cannot overflow the
   allocation size computation (num_entries * 36). Real archives hold a few thousand. */
static int pff_count_sane(uint32_t num_entries)
{
    return num_entries <= (uint32_t)(0x7FFFFFFF / PFF_ENTRY_SIZE);
}

/* Normalize a PFF name into an uppercase, trailing-space-trimmed C string. Reads up to
   raw_cap bytes or until a NUL; result is capped to out_sz - 1 chars. Matches the engine's
   strupr + trailing-0x20 trim used for sort/lookup (PFF_SortEntries / PFF_FindEntry).
   Non-static: shared with pff_writer.cpp via pff_internal.h. */
void pff_norm_name(const char *raw, size_t raw_cap, char *out, size_t out_sz)
{
    size_t n = 0, i;
    for (i = 0; i < raw_cap && n + 1 < out_sz; ++i) {
        char c = raw[i];
        if (c == '\0') break;
        out[n++] = (char)toupper((unsigned char)c);
    }
    while (n > 0 && out[n - 1] == ' ') --n;
    out[n] = '\0';
}

/* qsort comparator: order entries by normalized (uppercased) name. */
static int pff_qsort_cmp(const void *a, const void *b)
{
    char na[PFF_NAME_SIZE + 1], nb[PFF_NAME_SIZE + 1];
    pff_norm_name(((const PffEntry *)a)->filename, PFF_NAME_SIZE, na, sizeof(na));
    pff_norm_name(((const PffEntry *)b)->filename, PFF_NAME_SIZE, nb, sizeof(nb));
    return strcmp(na, nb);
}

/* bsearch comparator: key is a pre-normalized query string, elem is a PffEntry. */
static int pff_bsearch_cmp(const void *key, const void *elem)
{
    char ne[PFF_NAME_SIZE + 1];
    pff_norm_name(((const PffEntry *)elem)->filename, PFF_NAME_SIZE, ne, sizeof(ne));
    return strcmp((const char *)key, ne);
}

/* In-place payload XOR for entries flagged PFF_FLAG_ENCRYPTED. Stateful rotating keystream: the
   32-bit key is rotated left 7 before each byte. Symmetric (XOR is its own inverse), so the same
   routine encrypts and decrypts. Verified against PFF_LoadFileToMemory @ 0x768920 (the retail
   read path hardcodes seed 0x0312A4CE; the seed is a parameter here so the game-profile container
   key can be threaded through, and so the public pff_container_xor can re-use it). */
static void pff_xor_buffer(uint8_t *buf, size_t size, uint32_t key)
{
    size_t i;
    for (i = 0; i < size; ++i) {
        key = (key << 7) | (key >> (32 - 7)); /* rotl32(key, 7) */
        buf[i] ^= (uint8_t)(key & 0xFFu);
    }
}

void pff_container_xor(uint8_t *buf, size_t size, uint32_t container_key)
{
    if (buf && size)
        pff_xor_buffer(buf, size, container_key);
}

static void pff_sort_entries(PffArchive *archive)
{
    if (archive->entries && archive->entry_count > 1)
        qsort(archive->entries, archive->entry_count, sizeof(PffEntry), pff_qsort_cmp);
}

static void pff_store_path(PffArchive *archive, const char *path)
{
    size_t path_len = strlen(path);
    if (path_len >= sizeof(archive->_path))
        path_len = sizeof(archive->_path) - 1;
    memcpy(archive->_path, path, path_len);
    archive->_path[path_len] = '\0';
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

int pff_open(PffArchive *archive, const char *path)
{
    FILE *f;
    PffHeader hdr;
    PffEntry *entries;

    if (!archive || !path) return -1;

    memset(archive, 0, sizeof(*archive));

    f = fopen(path, "rb");
    if (!f) return -1;

    /* Read and validate header. */
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return -1;
    }
    if (!pff_valid_magic(hdr.magic) || hdr.entry_size != PFF_ENTRY_SIZE
        || !pff_count_sane(hdr.num_entries)) {
        fclose(f);
        return -1;
    }

    archive->header = hdr;

    if (fseek(f, (long)hdr.file_table_offset, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    /* Load every directory entry. Per PFF_Open @ 0x7682e0 the engine does NO deleted-entry
       filtering; flag bit 0 means ENCRYPTED, not deleted (handled at extract time). */
    entries = NULL;
    if (hdr.num_entries > 0) {
        entries = (PffEntry *)malloc((size_t)hdr.num_entries * sizeof(PffEntry));
        if (!entries) {
            fclose(f);
            return -1;
        }
        if (fread(entries, sizeof(PffEntry), hdr.num_entries, f) != hdr.num_entries) {
            free(entries);
            fclose(f);
            return -1;
        }
    }

    archive->entries = entries;
    archive->entry_count = hdr.num_entries;
    archive->_file = f;

    pff_sort_entries(archive);        /* PFF_SortEntries @ 0x768280 */
    pff_store_path(archive, path);
    return 0;
}

int pff_open_legacy(PffArchive *archive, const char *path)
{
    FILE *f;
    uint32_t count, i;
    PffEntry *entries;
    uint32_t raw[4];          /* 16-byte record: name dwords [0..2] (XOR'd), file offset [3] */
    uint32_t cur_offset, next_offset;

    if (!archive || !path) return -1;

    memset(archive, 0, sizeof(*archive));

    f = fopen(path, "rb");
    if (!f) return -1;

    /* num_entries lives at file offset 12 (PFF_OpenLegacyArchive @ 0x7683f0). */
    if (fseek(f, 12, SEEK_SET) != 0 || fread(&count, 4, 1, f) != 1
        || !pff_count_sane(count)) {
        fclose(f);
        return -1;
    }

    /* Synthesize a header in the modern shape (legacy archives carry no magic on disk). */
    archive->header.header_size = 16;
    archive->header.magic = 0;
    archive->header.num_entries = count;
    archive->header.entry_size = PFF_ENTRY_SIZE;
    archive->header.file_table_offset = 16;

    entries = NULL;
    if (count > 0) {
        entries = (PffEntry *)calloc(count, sizeof(PffEntry));
        if (!entries) {
            fclose(f);
            return -1;
        }

        /* The directory starts at offset 16. Records are 16 bytes; sizes are delta-encoded
           from the next record's offset, so there are count+1 records (the last supplies the
           terminating offset). Read one record ahead, mirroring the engine loop. */
        if (fread(raw, 4, 4, f) != 4) {        /* record 0 */
            free(entries);
            fclose(f);
            return -1;
        }
        next_offset = raw[3];
        for (i = 0; i < count; ++i) {
            uint32_t name0 = raw[0] ^ 0xACEDDEADu;
            uint32_t name1 = raw[1] ^ 0xACEDDEADu;
            uint32_t name2 = raw[2] ^ 0xACEDDEADu;
            char namebuf[12];
            size_t nl;

            cur_offset = next_offset;
            if (fread(raw, 4, 4, f) != 4) {     /* record i+1 */
                free(entries);
                fclose(f);
                return -1;
            }
            next_offset = raw[3];

            memcpy(namebuf + 0, &name0, 4);
            memcpy(namebuf + 4, &name1, 4);
            memcpy(namebuf + 8, &name2, 4);
            nl = 0;
            while (nl < sizeof(namebuf) && namebuf[nl] != '\0') ++nl;

            entries[i].flags = 0;
            entries[i].offset = cur_offset;
            entries[i].size = next_offset - cur_offset;
            memcpy(entries[i].filename, namebuf, nl); /* trailing bytes stay 0 (calloc) */
        }
    }

    archive->entries = entries;
    archive->entry_count = count;
    archive->_file = f;

    pff_sort_entries(archive);
    pff_store_path(archive, path);
    return 0;
}

void pff_close(PffArchive *archive)
{
    if (!archive) return;

    if (archive->_file) {
        fclose((FILE *)archive->_file);
        archive->_file = NULL;
    }
    free(archive->entries);
    archive->entries = NULL;
    archive->entry_count = 0;
}

const PffEntry *pff_find(const PffArchive *archive, const char *name)
{
    char query[PFF_NAME_SIZE * 2 + 1];
    if (!archive || !name || !archive->entries) return NULL;

    /* Normalize the query once; an over-long query stays longer than any (<=16 char) entry
       name, so it cannot produce a false match. */
    pff_norm_name(name, strlen(name), query, sizeof(query));
    return (const PffEntry *)bsearch(query, archive->entries, archive->entry_count,
                                     sizeof(PffEntry), pff_bsearch_cmp);
}

int pff_extract_raw(const PffArchive *archive, const PffEntry *entry,
                    uint8_t *out_buf, size_t buf_size)
{
    FILE *f;

    if (!archive || !entry || !out_buf) return -1;
    if (buf_size < entry->size) return -1;
    if (entry->size == 0) return 0;

    f = (FILE *)archive->_file;
    if (!f) return -1;

    if (fseek(f, (long)entry->offset, SEEK_SET) != 0) return -1;
    if (fread(out_buf, 1, entry->size, f) != entry->size) return -1;

    return 0;
}

int pff_extract(const PffArchive *archive, const PffEntry *entry,
                uint8_t *out_buf, size_t buf_size)
{
    int rc = pff_extract_raw(archive, entry, out_buf, buf_size);
    if (rc != 0) return rc;

    /* Decrypt encrypted payloads in place (PFF_LoadFileToMemory @ 0x768920, seed 0x0312A4CE). */
    if ((entry->flags & PFF_FLAG_ENCRYPTED) && entry->size > 0)
        pff_xor_buffer(out_buf, entry->size, 0x0312A4CEu);

    return 0;
}

int pff_is_pff(const uint8_t *data, size_t size)
{
    const PffHeader *hdr;
    if (!data || size < PFF_HEADER_SIZE) return 0;
    hdr = (const PffHeader *)data;
    return pff_valid_magic(hdr->magic) ? 1 : 0;
}
