/* Synthetic PFF archive writer for tests. Header-only; builds in-memory archives so tests
   never depend on copyrighted game data. Shared by the libs/pff unit tests and the libs/vfs
   tests. Mirrors the on-disk layout verified in notes/vfs/phase0_ida_verification.md. */
#ifndef PFF_TEST_WRITER_H
#define PFF_TEST_WRITER_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pff/pff.h"

typedef struct PffTestEntry {
    const char *name;       /* logical name (<=16 modern, <=12 legacy) */
    const uint8_t *data;    /* payload bytes */
    uint32_t size;          /* payload length */
    int encrypted;          /* modern only: store XOR-encrypted + set the encrypted flag */
} PffTestEntry;

/* Stateful ROL7 XOR keystream (PFF_LoadFileToMemory @ 0x768920). XOR is its own inverse, so
   the same routine encrypts (test side) and decrypts (pff_extract). */
static inline void pff_test_xor(uint8_t *buf, size_t size)
{
    uint32_t key = 0x0312A4CEu;
    size_t i;
    for (i = 0; i < size; ++i) {
        key = (key << 7) | (key >> (32 - 7));
        buf[i] ^= (uint8_t)(key & 0xFFu);
    }
}

/* Write a modern PFF3 archive: header(20) | payloads | entry table(36 each). Returns 0 on
   success. Payloads of entries marked encrypted are stored XOR-encrypted. */
static inline int pff_test_write_modern(const char *path, const PffTestEntry *entries,
                                        uint32_t n)
{
    FILE *f;
    uint32_t i, off, table_off, hdr[5];
    uint32_t *offs;

    f = fopen(path, "wb");
    if (!f) return -1;

    hdr[0] = PFF_HEADER_SIZE;   /* header_size */
    hdr[1] = PFF_MAGIC_PFF3;    /* magic */
    hdr[2] = n;                 /* num_entries */
    hdr[3] = PFF_ENTRY_SIZE;    /* entry_size */
    hdr[4] = 0;                 /* file_table_offset (patched below) */
    fwrite(hdr, 4, 5, f);

    offs = (uint32_t *)malloc((n ? n : 1) * sizeof(uint32_t));
    off = PFF_HEADER_SIZE;
    for (i = 0; i < n; ++i) {
        offs[i] = off;
        if (entries[i].size) {
            if (entries[i].encrypted) {
                uint8_t *tmp = (uint8_t *)malloc(entries[i].size);
                memcpy(tmp, entries[i].data, entries[i].size);
                pff_test_xor(tmp, entries[i].size);
                fwrite(tmp, 1, entries[i].size, f);
                free(tmp);
            } else {
                fwrite(entries[i].data, 1, entries[i].size, f);
            }
        }
        off += entries[i].size;
    }

    table_off = off;
    for (i = 0; i < n; ++i) {
        uint8_t rec[PFF_ENTRY_SIZE];
        uint32_t flags = entries[i].encrypted ? PFF_FLAG_ENCRYPTED : 0u;
        size_t nl = strlen(entries[i].name);
        if (nl > PFF_NAME_SIZE) nl = PFF_NAME_SIZE;
        memset(rec, 0, sizeof(rec));
        memcpy(rec + 0, &flags, 4);
        memcpy(rec + 4, &offs[i], 4);
        memcpy(rec + 8, &entries[i].size, 4);
        memcpy(rec + 16, entries[i].name, nl);  /* name field, zero-padded */
        fwrite(rec, 1, sizeof(rec), f);
    }
    free(offs);

    fseek(f, 16, SEEK_SET);     /* patch file_table_offset */
    fwrite(&table_off, 4, 1, f);
    fclose(f);
    return 0;
}

/* Write a legacy archive: header(16, count@12) | (n+1) directory records(16) | payloads.
   Records hold a 12-byte name XOR'd with 0xACEDDEAD and an absolute payload offset; the
   (n+1)th record supplies the terminating offset (delta-encoded sizes). */
static inline int pff_test_write_legacy(const char *path, const PffTestEntry *entries,
                                        uint32_t n)
{
    FILE *f;
    uint32_t i, payload_start, cur;
    uint32_t *offs;
    uint8_t header[16];

    f = fopen(path, "wb");
    if (!f) return -1;

    memset(header, 0, sizeof(header));
    memcpy(header + 12, &n, 4);
    fwrite(header, 1, sizeof(header), f);

    payload_start = 16 + (n + 1) * 16;
    offs = (uint32_t *)malloc((n + 1) * sizeof(uint32_t));
    cur = payload_start;
    for (i = 0; i < n; ++i) { offs[i] = cur; cur += entries[i].size; }
    offs[n] = cur;  /* terminator */

    for (i = 0; i <= n; ++i) {
        uint8_t rec[16];
        uint32_t w[3];
        char namebuf[12];
        int k;
        memset(namebuf, 0, sizeof(namebuf));
        if (i < n) {
            size_t nl = strlen(entries[i].name);
            if (nl > 12) nl = 12;
            memcpy(namebuf, entries[i].name, nl);
        }
        memcpy(w, namebuf, 12);
        for (k = 0; k < 3; ++k) w[k] ^= 0xACEDDEADu;
        memset(rec, 0, sizeof(rec));
        memcpy(rec + 0, w, 12);
        memcpy(rec + 12, &offs[i], 4);
        fwrite(rec, 1, sizeof(rec), f);
    }

    for (i = 0; i < n; ++i)
        if (entries[i].size) fwrite(entries[i].data, 1, entries[i].size, f);

    free(offs);
    fclose(f);
    return 0;
}

#endif /* PFF_TEST_WRITER_H */
