/* PFF archive writer (production). [orig: PFF_SortEntries @ 0x768280; the writer is a from-scratch
   inverse of the retail loader, docs/vfs/vfs-pff-mount-re.md, ADR 0008]
   Serializes an explicit, caller-provided set of stored entries
   into a modern PFF3/PFF4/BHD archive: header(20) | payloads | directory(36 each). Payloads are
   NEVER transformed — the supplied bytes are the exact stored bytes (container-XOR-encrypted iff
   flags & PFF_FLAG_ENCRYPTED). The directory is emitted sorted by normalized name (matching the
   engine's on-disk convention; PFF_SortEntries @ 0x768280 also re-sorts on load), and the write
   goes to a temp file then atomically renames, so a crash never leaves a half-written archive.

   The core is PffStreamWriter (pff_stream_writer.h), which writes a budget of payload bytes at a
   time; these single-call forms run it to the end. The streaming form (pff_write_archive_streamed)
   pulls one payload at a time via a callback, so a multi-hundred-MB archive resaves with only the
   largest single entry buffered; the flat-array form (pff_write_archive) is a thin wrapper for
   tests and the future dir packer. Mirrors the layout witnessed in docs/vfs/vfs-pff-mount-re.md
   (PFF_Open @ 0x7682e0). */

#include <formats/pff/pff.h>
#include <formats/pff/pff_stream_writer.h>

#include <stdint.h>
#include <string.h>

#include <vector>

namespace opennova::pff {

namespace {

/* The single-call forms run the stream writer to the end: a budget no payload set reaches. */
constexpr uint64_t kWholeBudget = UINT64_MAX;
/* The progress form's slice between two looks at the entries written. */
constexpr uint64_t kProgressBudget = uint64_t(1) << 20;

/* The streamed forms' callback reads a payload whole; the stream writer asks for chunks, served
   from one buffer that holds the payload being written (the largest one at most). */
struct WholeEntryReader {
	const PffWriteStreamEntry *entries;
	PffReadEntryFn read;
	void *ctx;
	std::vector<uint8_t> payload;
	uint32_t loaded;
};

int read_whole_entry_chunk(void *ctx, uint32_t index, uint32_t offset, uint8_t *out, uint32_t size) {
	WholeEntryReader *reader = static_cast<WholeEntryReader *>(ctx);
	if (offset == 0 || reader->loaded != index) {
		const uint32_t whole = reader->entries[index].size;
		reader->payload.resize(whole ? whole : 1);
		reader->loaded = UINT32_MAX;
		if (reader->read(reader->ctx, index, reader->payload.data(), whole) != 0) return -1;
		reader->loaded = index;
	}
	memcpy(out, reader->payload.data() + offset, size);
	return 0;
}

/* The flat-array form's payloads are in memory already: each chunk straight from them. */
int read_array_chunk(void *ctx, uint32_t index, uint32_t offset, uint8_t *out, uint32_t size) {
	const PffWriteEntry *entries = static_cast<const PffWriteEntry *>(ctx);
	if (entries[index].data == NULL) return -1;
	memcpy(out, entries[index].data + offset, size);
	return 0;
}

} // namespace

int pff_write_archive_streamed_progress(const char *path, PffFormat format,
                                        const PffWriteStreamEntry *entries, uint32_t n,
                                        PffReadEntryFn read_entry, void *ctx,
                                        PffWriteProgressFn progress, void *progress_ctx)
{
	if (n > 0 && read_entry == NULL)
		return PFF_WRITE_ERR_IO;
	WholeEntryReader reader{entries, read_entry, ctx, {}, UINT32_MAX};
	PffStreamWriter writer;
	int rc = writer.open(path, format, entries, n, read_whole_entry_chunk, &reader);
	if (rc != PFF_WRITE_OK)
		return rc;
	/* Tick once per entry as its payload is written (a zero-size one too), so `done` reaches `n`. */
	uint32_t reported = 0;
	while (!writer.payloads_written()) {
		rc = writer.write(progress ? kProgressBudget : kWholeBudget);
		if (rc != PFF_WRITE_OK)
			return rc;
		for (; progress && reported < writer.entries_written(); ++reported)
			progress(progress_ctx, reported + 1, n);
	}
	return writer.finish();
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

	/* Flatten to stream entries; a payload the array does not hold (NULL with a size) is refused
	   before anything is written. */
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
	PffStreamWriter writer;
	int rc = writer.open(path, format, n ? se.data() : NULL, n, read_array_chunk,
	                     const_cast<PffWriteEntry *>(entries));
	if (rc != PFF_WRITE_OK)
		return rc;
	rc = writer.write(kWholeBudget);
	if (rc != PFF_WRITE_OK)
		return rc;
	return writer.finish();
}

} // namespace opennova::pff
