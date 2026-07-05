/* PFF archive writer (production). [orig: PFF_SortEntries @ 0x768280; the writer is a from-scratch
   inverse of the retail loader, docs/vfs/vfs-pff-mount-re.md, ADR 0008]
   Serializes an explicit, caller-provided set of stored entries
   into a modern PFF3/PFF4/BHD archive: header(20) | payloads | directory(36 each). Payloads are
   NEVER transformed — the supplied bytes are the exact stored bytes (container-XOR-encrypted iff
   flags & PFF_FLAG_ENCRYPTED). The directory is emitted sorted by normalized name (matching the
   engine's on-disk convention; PFF_SortEntries @ 0x768280 also re-sorts on load), and the write
   goes to a temp file then atomically renames, so a crash never leaves a half-written archive.

   The streaming form (pff_write_archive_streamed) is the core: it pulls one payload at a time via
   a callback, so a multi-hundred-MB archive resaves with only the largest single entry buffered.
   The flat-array form (pff_write_archive) is a thin wrapper for tests and the future dir packer.
   Mirrors the layout verified in notes/vfs/phase0_ida_verification.md (PFF_Open @ 0x7682e0). */

#include "pff/pff.h"

#include "pff_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

namespace {

uint32_t pff_magic_for_format(PffFormat format)
{
    switch (format) {
        case PFF_FORMAT_PFF4: return PFF_MAGIC_PFF4;
        case PFF_FORMAT_BHD:  return PFF_MAGIC_BHD;
        case PFF_FORMAT_PFF3:
        default:              return PFF_MAGIC_PFF3;
    }
}

/* Validate names and compute the sorted write order. Rejects over-long (> PFF_NAME_SIZE) and
   blank names outright (no silent truncation), and two entries that collapse to the same
   normalized name (binary-search lookup is otherwise nondeterministic). */
int pff_validate_and_order(const PffWriteStreamEntry *entries, uint32_t n,
                           std::vector<uint32_t> &order)
{
    std::vector<std::string> norm(n);
    order.resize(n);
    /* Entry offsets and file_table_offset are uint32. The last payload ends at
       PFF_HEADER_SIZE + sum(sizes); if that exceeds UINT32_MAX the offsets silently wrap and the
       archive is unreadable, so reject it up front rather than writing a corrupt file. */
    uint64_t total = PFF_HEADER_SIZE;
    for (uint32_t i = 0; i < n; ++i) {
        const char *name = entries[i].name ? entries[i].name : "";
        size_t raw_len = strlen(name);
        if (raw_len > PFF_NAME_SIZE)
            return PFF_WRITE_ERR_NAME_LEN;
        char buf[PFF_NAME_SIZE + 1];
        pff_norm_name(name, raw_len, buf, sizeof(buf));
        if (buf[0] == '\0')
            return PFF_WRITE_ERR_NAME_EMPTY;
        norm[i] = buf;
        order[i] = i;
        total += entries[i].size;
        if (total > 0xFFFFFFFFull)
            return PFF_WRITE_ERR_TOO_LARGE;
    }
    std::sort(order.begin(), order.end(), [&norm](uint32_t a, uint32_t b) {
        return norm[a] < norm[b];
    });
    for (uint32_t k = 1; k < n; ++k) {
        if (norm[order[k]] == norm[order[k - 1]])
            return PFF_WRITE_ERR_DUP_NAME;
    }
    return PFF_WRITE_OK;
}

/* Serialize into an already-open file using the streaming callback. `order` lists entry indices in
   write order (sorted by normalized name). `progress` (optional) is fired once per entry as the
   payloads are written. Returns PFF_WRITE_OK or PFF_WRITE_ERR_IO. */
int pff_write_body(FILE *f, PffFormat format, const PffWriteStreamEntry *entries, uint32_t n,
                   const std::vector<uint32_t> &order, PffReadEntryFn read_entry, void *ctx,
                   PffWriteProgressFn progress, void *progress_ctx)
{
    uint32_t hdr[5];
    hdr[0] = PFF_HEADER_SIZE;
    hdr[1] = pff_magic_for_format(format);
    hdr[2] = n;
    hdr[3] = PFF_ENTRY_SIZE;
    hdr[4] = 0;                 /* file_table_offset, patched below */
    if (fwrite(hdr, 4, 5, f) != 5)
        return PFF_WRITE_ERR_IO;

    /* Payloads, in sorted order. One reusable buffer sized to the largest entry, so peak memory is
       a single payload rather than the whole archive. */
    uint32_t max_size = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (entries[i].size > max_size)
            max_size = entries[i].size;
    }
    std::vector<uint8_t> payload(max_size ? max_size : 1);

    std::vector<uint32_t> offsets(n);
    uint32_t off = PFF_HEADER_SIZE;
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t i = order[k];
        offsets[i] = off;
        if (entries[i].size) {
            if (read_entry(ctx, i, payload.data(), entries[i].size) != 0)
                return PFF_WRITE_ERR_IO;
            if (fwrite(payload.data(), 1, entries[i].size, f) != entries[i].size)
                return PFF_WRITE_ERR_IO;
        }
        off += entries[i].size;
        /* Tick after each entry (including zero-size ones) so `done` reaches `n`. */
        if (progress)
            progress(progress_ctx, k + 1, n);
    }

    /* Directory table. */
    uint32_t table_off = off;
    for (uint32_t k = 0; k < n; ++k) {
        uint32_t i = order[k];
        uint8_t rec[PFF_ENTRY_SIZE];
        size_t nl = strlen(entries[i].name ? entries[i].name : "");
        if (nl > PFF_NAME_SIZE)
            nl = PFF_NAME_SIZE;
        memset(rec, 0, sizeof(rec));
        memcpy(rec + 0,  &entries[i].flags, 4);
        memcpy(rec + 4,  &offsets[i], 4);
        memcpy(rec + 8,  &entries[i].size, 4);
        memcpy(rec + 12, &entries[i].timestamp, 4);
        if (nl)
            memcpy(rec + 16, entries[i].name, nl);   /* original case, zero-padded */
        memcpy(rec + 32, &entries[i].checksum, 4);
        if (fwrite(rec, 1, sizeof(rec), f) != sizeof(rec))
            return PFF_WRITE_ERR_IO;
    }

    /* Patch file_table_offset (header field at byte 16). */
    if (fseek(f, 16, SEEK_SET) != 0)
        return PFF_WRITE_ERR_IO;
    if (fwrite(&table_off, 4, 1, f) != 1)
        return PFF_WRITE_ERR_IO;
    return PFF_WRITE_OK;
}

/* Adapter so the flat-array form can reuse the streaming core: copies bytes from the array's
   `data` pointers. NULL-data-with-size is rejected up front by pff_write_archive. */
struct ArrayReadCtx {
    const PffWriteEntry *entries;
};

int array_read_entry(void *ctx, uint32_t index, uint8_t *out, uint32_t size)
{
    const PffWriteEntry *e = static_cast<ArrayReadCtx *>(ctx)->entries;
    if (size == 0)
        return 0;
    if (e[index].data == NULL)
        return -1;
    memcpy(out, e[index].data, size);
    return 0;
}

} // namespace

int pff_write_archive_streamed_progress(const char *path, PffFormat format,
                                        const PffWriteStreamEntry *entries, uint32_t n,
                                        PffReadEntryFn read_entry, void *ctx,
                                        PffWriteProgressFn progress, void *progress_ctx)
{
    if (!path)
        return PFF_WRITE_ERR_IO;
    if (n > 0 && (!entries || !read_entry))
        return PFF_WRITE_ERR_IO;

    std::vector<uint32_t> order;
    int rc = pff_validate_and_order(entries, n, order);
    if (rc != PFF_WRITE_OK)
        return rc;

    /* Write to "<path>.tmp", then atomically rename onto the target. */
    std::string tmp = std::string(path) + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f)
        return PFF_WRITE_ERR_IO;
    rc = pff_write_body(f, format, entries, n, order, read_entry, ctx, progress, progress_ctx);
    fclose(f);
    if (rc != PFF_WRITE_OK) {
        remove(tmp.c_str());
        return rc;
    }
    /* rename() will not overwrite an existing file on Windows, so an existing target must be moved
       aside first. Rather than remove() it outright (which would lose the original if the process
       died before the rename completed), move it to "<path>.bak" so a crash mid-swap always leaves
       a recoverable copy; the backup is deleted only once the new file is in place. The caller
       (e.g. NovaPffArchive::save_as) guarantees `path` is not the still-open source. */
    std::string bak = std::string(path) + ".bak";
    remove(bak.c_str()); /* clear any stale backup from a previous interrupted save */
    const bool had_original = (rename(path, bak.c_str()) == 0);
    if (rename(tmp.c_str(), path) != 0) {
        remove(tmp.c_str());
        if (had_original)
            rename(bak.c_str(), path); /* restore the original on failure */
        return PFF_WRITE_ERR_IO;
    }
    if (had_original)
        remove(bak.c_str());
    return PFF_WRITE_OK;
}

int pff_write_archive_streamed(const char *path, PffFormat format,
                               const PffWriteStreamEntry *entries, uint32_t n,
                               PffReadEntryFn read_entry, void *ctx)
{
    return pff_write_archive_streamed_progress(path, format, entries, n, read_entry, ctx,
                                               NULL, NULL);
}

int pff_write_archive(const char *path, PffFormat format,
                      const PffWriteEntry *entries, uint32_t n)
{
    if (!path)
        return PFF_WRITE_ERR_IO;
    if (n > 0 && !entries)
        return PFF_WRITE_ERR_IO;

    /* Flatten to stream entries; the streamed core can't see `data`, so reject NULL-with-size here. */
    std::vector<PffWriteStreamEntry> se(n);
    for (uint32_t i = 0; i < n; ++i) {
        if (entries[i].size != 0 && entries[i].data == NULL)
            return PFF_WRITE_ERR_IO;
        se[i].name      = entries[i].name;
        se[i].size      = entries[i].size;
        se[i].flags     = entries[i].flags;
        se[i].timestamp = entries[i].timestamp;
        se[i].checksum  = entries[i].checksum;
    }
    ArrayReadCtx actx;
    actx.entries = entries;
    return pff_write_archive_streamed(path, format, n ? se.data() : NULL, n, array_read_entry, &actx);
}
