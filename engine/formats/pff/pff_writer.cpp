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

#include <base/io/crc32_mpeg2.h>
#include <base/io/file_io.h>
#include <base/io/os_path.h>

#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <system_error>
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

/* pff_rewrite_with_entry's payloads: each entry's stored bytes from the open source, the replaced
   entry's from the caller. */
struct Rewrite {
	const PffArchive *source;
	uint32_t replaced;
	const uint8_t *data;
	uint32_t size;
};

int read_rewrite_entry(void *ctx, uint32_t index, uint8_t *out, uint32_t size) {
	const Rewrite &rewrite = *static_cast<const Rewrite *>(ctx);
	if (index == rewrite.replaced) {
		if (size != rewrite.size) return 1;
		if (size) memcpy(out, rewrite.data, size);
		return 0;
	}
	return pff_extract_raw(rewrite.source, &rewrite.source->entries[index], out, size);
}

/* pff_apply_edits' payloads: a retained entry's stored bytes from the open source, an add's from
   the caller. origin[i] >= 0 is a source index, < 0 the add -(origin[i] + 1). */
struct EditPayloads {
	const PffArchive *source;
	const std::vector<PffEditEntry> *adds;
	std::vector<int64_t> origin;
};

int read_edit_entry(void *ctx, uint32_t index, uint8_t *out, uint32_t size) {
	const EditPayloads &edit = *static_cast<const EditPayloads *>(ctx);
	const int64_t origin = edit.origin[index];
	if (origin >= 0)
		return pff_extract_raw(edit.source, &edit.source->entries[origin], out, size);
	const PffEditEntry &add = (*edit.adds)[size_t(-origin - 1)];
	if (size != add.data.size())
		return 1;
	if (size)
		memcpy(out, add.data.data(), size);
	return 0;
}

/* True when the source entry already holds what `add` would write: plain, the same time, the same
   checksum, the same bytes. */
bool entry_holds(const PffArchive *source, const PffEntry &entry, const PffEditEntry &add,
		uint32_t checksum) {
	if (entry.flags != 0 || entry.size != add.data.size() || entry.timestamp != add.timestamp ||
			entry.checksum != checksum)
		return false;
	std::vector<uint8_t> stored(entry.size ? entry.size : 1);
	if (pff_extract_raw(source, &entry, stored.data(), entry.size) != 0)
		return false;
	return entry.size == 0 || memcmp(stored.data(), add.data.data(), entry.size) == 0;
}

} // namespace

int pff_apply_edits(const char *path, const std::vector<PffEditEntry> &adds,
		const std::vector<std::string> &removes, bool *rewritten) {
	if (rewritten)
		*rewritten = false;
	if (path == NULL)
		return PFF_WRITE_ERR_IO;
	PffArchive source{};
	bool have_source = false;
	std::error_code ec;
	if (std::filesystem::exists(io::os_path(path), ec)) {
		if (pff_open(&source, path) != 0)
			return PFF_WRITE_ERR_IO;
		have_source = true;
	}
	/* pack.exe's remove jobs run before its add jobs [orig: Pack_Tick @ 0x402a70 (UPDATE.EXE): the
	   remove queue first], so an add of a removed name puts it back; of two adds of one name the
	   last stands. */
	std::vector<std::string> removed;
	for (const std::string &name : removes)
		removed.push_back(normalized_logical_name(name));
	std::vector<std::string> add_key(adds.size());
	for (size_t i = 0; i < adds.size(); ++i)
		add_key[i] = normalized_logical_name(adds[i].name);
	auto last_add = [&](const std::string &key) -> int64_t {
		for (size_t i = adds.size(); i-- > 0;)
			if (add_key[i] == key)
				return int64_t(i);
		return -1;
	};
	bool changed = false;
	EditPayloads payloads{ have_source ? &source : NULL, &adds, {} };
	std::vector<PffWriteStreamEntry> entries;
	std::vector<std::string> names;
	const uint32_t count = have_source ? source.entry_count : 0;
	names.reserve(count + adds.size());
	for (uint32_t i = 0; i < count; ++i) {
		const PffEntry &entry = source.entries[i];
		const std::string stored = pff_entry_stored_name(entry);
		const std::string key = normalized_logical_name(stored);
		const int64_t add = last_add(key);
		const bool is_removed = std::find(removed.begin(), removed.end(), key) != removed.end();
		if (add >= 0) {
			const PffEditEntry &edit = adds[size_t(add)];
			const uint32_t checksum = io::novalogic_file_crc(edit.data.data(), edit.data.size());
			if (is_removed || !entry_holds(&source, entry, edit, checksum))
				changed = true;
			continue; /* written with the adds below */
		}
		if (is_removed) {
			changed = true;
			continue;
		}
		names.push_back(stored);
		PffWriteStreamEntry out{};
		out.size = entry.size;
		out.flags = entry.flags;
		out.timestamp = entry.timestamp;
		out.checksum = entry.checksum;
		entries.push_back(out);
		payloads.origin.push_back(int64_t(i));
	}
	for (size_t i = 0; i < adds.size(); ++i) {
		if (last_add(add_key[i]) != int64_t(i))
			continue;
		bool existed = false;
		for (uint32_t e = 0; e < count && !existed; ++e)
			existed =
					normalized_logical_name(pff_entry_stored_name(source.entries[e])) == add_key[i];
		if (!existed)
			changed = true;
		if (adds[i].data.size() > UINT32_MAX) {
			if (have_source)
				pff_close(&source);
			return PFF_WRITE_ERR_TOO_LARGE;
		}
		names.push_back(adds[i].name);
		PffWriteStreamEntry out{};
		out.size = uint32_t(adds[i].data.size());
		out.flags = 0;
		out.timestamp = adds[i].timestamp;
		out.checksum = io::novalogic_file_crc(adds[i].data.data(), adds[i].data.size());
		entries.push_back(out);
		payloads.origin.push_back(-int64_t(i) - 1);
	}
	if (!changed) {
		if (have_source)
			pff_close(&source);
		return PFF_WRITE_OK;
	}
	for (size_t i = 0; i < entries.size(); ++i)
		entries[i].name = names[i].c_str();
	const PffFormat format =
			have_source ? pff_format_for_magic(source.header.magic) : PFF_FORMAT_PFF3;
	const std::string staged = std::string(path) + ".new";
	const int rc = pff_write_archive_streamed(staged.c_str(), format, entries.data(),
			uint32_t(entries.size()), read_edit_entry, &payloads);
	if (have_source)
		pff_close(&source);
	if (rc != PFF_WRITE_OK) {
		std::filesystem::remove(io::os_path(staged), ec);
		return rc;
	}
	std::string error;
	if (!io::replace_file(staged, path, error)) {
		std::filesystem::remove(io::os_path(staged), ec);
		return PFF_WRITE_ERR_IO;
	}
	if (rewritten)
		*rewritten = true;
	return PFF_WRITE_OK;
}

uint32_t pff_magic_for_format(PffFormat format)
{
	switch (format) {
	case PFF_FORMAT_PFF4: return PFF_MAGIC_PFF4;
	case PFF_FORMAT_BHD: return PFF_MAGIC_BHD;
	case PFF_FORMAT_PFF3:
	default: return PFF_MAGIC_PFF3;
	}
}

PffFormat pff_format_for_magic(uint32_t magic)
{
	if (magic == PFF_MAGIC_PFF4) return PFF_FORMAT_PFF4;
	if (magic == PFF_MAGIC_BHD) return PFF_FORMAT_BHD;
	return PFF_FORMAT_PFF3;
}

const char *pff_write_error_string(int code)
{
	switch (code) {
	case PFF_WRITE_OK: return "written";
	case PFF_WRITE_ERR_NAME_LEN: return "a name is too long for an archive";
	case PFF_WRITE_ERR_NAME_EMPTY: return "a name is blank";
	case PFF_WRITE_ERR_DUP_NAME: return "two files share a name";
	case PFF_WRITE_ERR_TOO_LARGE: return "the archive would exceed 4 GB";
	case PFF_WRITE_ERR_IO:
	default: return "the archive could not be written";
	}
}

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

int pff_rewrite_with_entry(const PffArchive *source, uint32_t index, const uint8_t *data, uint32_t size,
                           const char *path)
{
	if (source == NULL || path == NULL || index >= source->entry_count || (size != 0 && data == NULL))
		return PFF_WRITE_ERR_IO;
	std::vector<PffWriteStreamEntry> entries(source->entry_count);
	std::vector<std::string> names(source->entry_count);
	for (uint32_t i = 0; i < source->entry_count; ++i) {
		const PffEntry &entry = source->entries[i];
		names[i] = pff_entry_stored_name(entry);
		entries[i].name = names[i].c_str();
		entries[i].size = i == index ? size : entry.size;
		entries[i].flags = i == index ? 0u : entry.flags;
		entries[i].timestamp = entry.timestamp;
		entries[i].checksum = entry.checksum;
	}
	Rewrite rewrite{source, index, data, size};
	return pff_write_archive_streamed(path, pff_format_for_magic(source->header.magic), entries.data(),
	                                  uint32_t(entries.size()), read_rewrite_entry, &rewrite);
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
