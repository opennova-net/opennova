/* A synthetic PFF3 archive of gigabytes written as a sparse file (a few KB on the disk): each
   payload at an offset of its own, the directory where it is placed, the retail footer at the end.
   For the tests of archives whose directory or payloads lie past 2 GiB (or near 4 GiB), which a
   32-bit seek cannot reach. Shared by the engine/formats/pff unit tests and the engine/base/vfs
   tests. */
#pragma once

#include <stdint.h>
#include <string.h>

#include <vector>

#include <formats/pff/pff.h>

#include "common/sparse_file.h"

typedef struct PffSparseEntry {
    const char *name;  /* logical name (<= 16) */
    uint32_t offset;   /* where its payload lies */
    const char *data;  /* the payload, its strlen the entry's size */
} PffSparseEntry;

static inline void pff_sparse_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* A PFF3 archive of `file_size` bytes at the UTF-8 `path`: the header, each entry's payload at its
   offset, the directory at `dir_offset` and the retail footer carrying `kept` as its second word in
   the last 12 bytes. 0 when the file system cannot make a sparse file (the caller skips), else 1. */
static inline int pff_sparse_write(const char *path, uint64_t file_size, uint32_t dir_offset,
                                   const PffSparseEntry *entries, uint32_t n, uint32_t kept)
{
    using namespace opennova::pff;
    uint8_t header[PFF_HEADER_SIZE];
    std::vector<uint8_t> dir((size_t)n * PFF_ENTRY_SIZE, 0);
    uint8_t footer[PFF_FOOTER_SIZE];
    uint32_t i;
    if (!test_sparse::write_sparse(path, file_size)) return 0;
    pff_sparse_put_le32(header + 0, PFF_HEADER_SIZE);
    pff_sparse_put_le32(header + 4, PFF_MAGIC_PFF3);
    pff_sparse_put_le32(header + 8, n);
    pff_sparse_put_le32(header + 12, PFF_ENTRY_SIZE);
    pff_sparse_put_le32(header + 16, dir_offset);
    if (!test_sparse::write_at(path, 0, header, sizeof(header))) return 0;
    for (i = 0; i < n; ++i) {
        uint8_t *rec = dir.data() + (size_t)i * PFF_ENTRY_SIZE;
        const uint32_t size = (uint32_t)strlen(entries[i].data);
        pff_sparse_put_le32(rec + 4, entries[i].offset);
        pff_sparse_put_le32(rec + 8, size);
        pff_sparse_put_le32(rec + 12, PFF_NEW_ENTRY_TIMESTAMP);
        memcpy(rec + 16, entries[i].name, strlen(entries[i].name));
        if (!test_sparse::write_at(path, entries[i].offset, entries[i].data, size)) return 0;
    }
    if (!test_sparse::write_at(path, dir_offset, dir.data(), dir.size())) return 0;
    pff_sparse_put_le32(footer + 0, 0x39a9a8c0u); /* 192.168.169.57, as the JO:CA archives end */
    pff_sparse_put_le32(footer + 4, kept);
    memcpy(footer + 8, "KING", 4);
    return test_sparse::write_at(path, file_size - PFF_FOOTER_SIZE, footer, sizeof(footer)) ? 1 : 0;
}
