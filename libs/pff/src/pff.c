/* PFF archive format implementation for NovaLogic games (C99 port).
   Supports PFF3, PFF4, and BHD-era archives. */

#include "pff/pff.h"

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

/* Case-insensitive compare of a PFF filename (up to 16 bytes, null-padded,
   possibly with trailing spaces) against a search string. */
static int pff_name_match(const char *entry_name, const char *search)
{
    size_t i;
    for (i = 0; i < PFF_NAME_SIZE; ++i) {
        char a = entry_name[i];
        char b = search[i];
        if (a == '\0' || a == ' ') {
            /* Treat trailing spaces same as null terminator in the entry. */
            while (a == ' ' && i < PFF_NAME_SIZE) {
                ++i;
                a = (i < PFF_NAME_SIZE) ? entry_name[i] : '\0';
            }
            return b == '\0' ? 1 : 0;
        }
        if (b == '\0') return 0;
        if (toupper((unsigned char)a) != toupper((unsigned char)b)) return 0;
    }
    /* entry_name consumed all 16 chars — match only if search also ends */
    return search[PFF_NAME_SIZE] == '\0' ? 1 : 0;
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

int pff_open(PffArchive *archive, const char *path)
{
    FILE *f;
    PffHeader hdr;
    uint32_t i, count;
    PffEntry raw;
    PffEntry *entries;
    size_t path_len;

    if (!archive || !path) return -1;

    memset(archive, 0, sizeof(*archive));

    f = fopen(path, "rb");
    if (!f) return -1;

    /* Read header */
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return -1;
    }

    if (!pff_valid_magic(hdr.magic)) {
        fclose(f);
        return -1;
    }

    if (hdr.entry_size != PFF_ENTRY_SIZE) {
        fclose(f);
        return -1;
    }

    archive->header = hdr;

    /* Seek to file table */
    if (fseek(f, (long)hdr.file_table_offset, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }

    /* First pass: count non-deleted entries */
    count = 0;
    for (i = 0; i < hdr.num_entries; ++i) {
        if (fread(&raw, sizeof(raw), 1, f) != 1) {
            fclose(f);
            return -1;
        }
        if (!(raw.flags & PFF_FLAG_DELETED)) {
            ++count;
        }
    }

    /* Allocate entries array */
    entries = NULL;
    if (count > 0) {
        entries = (PffEntry *)malloc(count * sizeof(PffEntry));
        if (!entries) {
            fclose(f);
            return -1;
        }
    }

    /* Second pass: read non-deleted entries */
    if (fseek(f, (long)hdr.file_table_offset, SEEK_SET) != 0) {
        free(entries);
        fclose(f);
        return -1;
    }

    {
        uint32_t idx = 0;
        for (i = 0; i < hdr.num_entries; ++i) {
            if (fread(&raw, sizeof(raw), 1, f) != 1) {
                free(entries);
                fclose(f);
                return -1;
            }
            if (!(raw.flags & PFF_FLAG_DELETED)) {
                entries[idx++] = raw;
            }
        }
    }

    archive->entries = entries;
    archive->entry_count = count;
    archive->_file = f;

    path_len = strlen(path);
    if (path_len >= sizeof(archive->_path))
        path_len = sizeof(archive->_path) - 1;
    memcpy(archive->_path, path, path_len);
    archive->_path[path_len] = '\0';

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
    uint32_t i;
    if (!archive || !name || !archive->entries) return NULL;

    for (i = 0; i < archive->entry_count; ++i) {
        if (pff_name_match(archive->entries[i].filename, name)) {
            return &archive->entries[i];
        }
    }
    return NULL;
}

int pff_extract(const PffArchive *archive, const PffEntry *entry,
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

int pff_is_pff(const uint8_t *data, size_t size)
{
    const PffHeader *hdr;
    if (!data || size < PFF_HEADER_SIZE) return 0;
    hdr = (const PffHeader *)data;
    return pff_valid_magic(hdr->magic) ? 1 : 0;
}
