/* The PFF archive writer, a budget of payload bytes at a time. [orig: PFF_SortEntries @ 0x768280;
   the layout is a from-scratch inverse of the retail loader, PFF_Open @ 0x7682e0,
   docs/vfs/vfs-pff-mount-re.md, ADR 0008]
   header(20) | payloads in the directory's order | directory(36 each), the directory sorted by
   normalized name (uppercase, trailing spaces trimmed: the engine's own convention, which its
   loader sorts by again). Payloads are never transformed: the bytes the caller supplies are the
   stored bytes (container-XOR-encrypted iff flags & PFF_FLAG_ENCRYPTED). */

#include <formats/pff/pff_stream_writer.h>

#include <string.h>

#include <algorithm>

namespace opennova::pff {

namespace {

// The largest chunk one read asks the caller for: a budget spans several.
constexpr uint32_t kChunkBytes = 1u << 20;

uint32_t magic_for_format(PffFormat format) {
	switch (format) {
	case PFF_FORMAT_PFF4: return PFF_MAGIC_PFF4;
	case PFF_FORMAT_BHD: return PFF_MAGIC_BHD;
	case PFF_FORMAT_PFF3:
	default: return PFF_MAGIC_PFF3;
	}
}

void put_u32_le(uint8_t *out, uint32_t value) {
	out[0] = uint8_t(value);
	out[1] = uint8_t(value >> 8);
	out[2] = uint8_t(value >> 16);
	out[3] = uint8_t(value >> 24);
}

} // namespace

PffStreamWriter::~PffStreamWriter() {
	abort();
}

int PffStreamWriter::fail(int code) {
	abort();
	return code;
}

void PffStreamWriter::abort() {
	if (file_ != nullptr) {
		fclose(file_);
		file_ = nullptr;
		remove(tmp_path_.c_str());
	}
}

int PffStreamWriter::open(const char *path, PffFormat format, const PffWriteStreamEntry *entries,
		uint32_t n, ReadChunkFn read, void *ctx) {
	abort();
	if (path == nullptr) return PFF_WRITE_ERR_IO;
	if (n > 0 && (entries == nullptr || read == nullptr)) return PFF_WRITE_ERR_IO;

	// Every name checked (none cut to fit, none blank, none twice as the lookup compares
	// them), and the payloads' end kept inside the directory's 32-bit offsets: a wrapped
	// offset would make an archive nothing can read, so it is refused before a byte is written.
	std::vector<Entry> checked(n);
	std::vector<std::string> norm(n);
	uint64_t total = PFF_HEADER_SIZE;
	for (uint32_t i = 0; i < n; ++i) {
		const char *name = entries[i].name ? entries[i].name : "";
		const size_t raw_len = strlen(name);
		if (raw_len > PFF_NAME_SIZE) return PFF_WRITE_ERR_NAME_LEN;
		char buf[PFF_NAME_SIZE + 1];
		pff_norm_name(name, raw_len, buf, sizeof(buf));
		if (buf[0] == '\0') return PFF_WRITE_ERR_NAME_EMPTY;
		norm[i] = buf;
		total += entries[i].size;
		if (total > 0xFFFFFFFFull) return PFF_WRITE_ERR_TOO_LARGE;
		checked[i].name = name;
		checked[i].size = entries[i].size;
		checked[i].flags = entries[i].flags;
		checked[i].timestamp = entries[i].timestamp;
		checked[i].checksum = entries[i].checksum;
	}
	std::vector<uint32_t> order(n);
	for (uint32_t i = 0; i < n; ++i) order[i] = i;
	std::sort(order.begin(), order.end(), [&norm](uint32_t a, uint32_t b) { return norm[a] < norm[b]; });
	for (uint32_t k = 1; k < n; ++k)
		if (norm[order[k]] == norm[order[k - 1]]) return PFF_WRITE_ERR_DUP_NAME;

	path_ = path;
	tmp_path_ = path_ + ".tmp";
	file_ = fopen(tmp_path_.c_str(), "wb");
	if (file_ == nullptr) return PFF_WRITE_ERR_IO;
	format_ = format;
	entries_ = std::move(checked);
	order_ = std::move(order);
	read_ = read;
	ctx_ = ctx;
	next_ = 0;
	entry_done_ = 0;
	position_ = PFF_HEADER_SIZE;
	payload_bytes_ = total - PFF_HEADER_SIZE;
	payload_written_ = 0;

	// The header, its directory offset patched by finish().
	uint8_t header[PFF_HEADER_SIZE];
	put_u32_le(header + 0, PFF_HEADER_SIZE);
	put_u32_le(header + 4, magic_for_format(format_));
	put_u32_le(header + 8, n);
	put_u32_le(header + 12, PFF_ENTRY_SIZE);
	put_u32_le(header + 16, 0);
	if (fwrite(header, 1, sizeof(header), file_) != sizeof(header)) return fail(PFF_WRITE_ERR_IO);
	return PFF_WRITE_OK;
}

int PffStreamWriter::write(uint64_t budget) {
	if (file_ == nullptr) return PFF_WRITE_ERR_IO;
	uint64_t left = budget;
	while (next_ < order_.size()) {
		Entry &entry = entries_[order_[next_]];
		if (entry_done_ == 0) entry.offset = position_;
		const uint32_t remaining = entry.size - entry_done_;
		if (remaining == 0) {
			++next_;
			entry_done_ = 0;
			continue;
		}
		if (left == 0) break;
		const uint32_t chunk = uint32_t(std::min<uint64_t>({remaining, left, kChunkBytes}));
		if (chunk_.size() < chunk) chunk_.resize(chunk);
		if (read_(ctx_, order_[next_], entry_done_, chunk_.data(), chunk) != 0) return fail(PFF_WRITE_ERR_IO);
		if (fwrite(chunk_.data(), 1, chunk, file_) != chunk) return fail(PFF_WRITE_ERR_IO);
		entry_done_ += chunk;
		position_ += chunk;
		payload_written_ += chunk;
		left -= chunk;
	}
	return PFF_WRITE_OK;
}

int PffStreamWriter::finish() {
	if (!payloads_written()) return fail(PFF_WRITE_ERR_IO);
	// The directory, in the written order: flags, offset, size, timestamp, the name in its
	// own case zero-padded to 16 bytes, checksum.
	const uint32_t table_offset = position_;
	for (const uint32_t index : order_) {
		const Entry &entry = entries_[index];
		uint8_t record[PFF_ENTRY_SIZE];
		memset(record, 0, sizeof(record));
		put_u32_le(record + 0, entry.flags);
		put_u32_le(record + 4, entry.offset);
		put_u32_le(record + 8, entry.size);
		put_u32_le(record + 12, entry.timestamp);
		memcpy(record + 16, entry.name.data(), std::min<size_t>(entry.name.size(), PFF_NAME_SIZE));
		put_u32_le(record + 32, entry.checksum);
		if (fwrite(record, 1, sizeof(record), file_) != sizeof(record)) return fail(PFF_WRITE_ERR_IO);
	}
	uint8_t offset[4];
	put_u32_le(offset, table_offset);
	if (fseek(file_, 16, SEEK_SET) != 0 || fwrite(offset, 1, sizeof(offset), file_) != sizeof(offset))
		return fail(PFF_WRITE_ERR_IO);
	const bool closed = fclose(file_) == 0;
	file_ = nullptr;
	if (!closed) {
		remove(tmp_path_.c_str());
		return PFF_WRITE_ERR_IO;
	}
	/* rename() will not overwrite an existing file on Windows, so an existing target is moved
	   aside first, to "<path>.bak" rather than removed: a crash mid-swap always leaves a copy.
	   The backup goes once the new file is in place. */
	const std::string bak = path_ + ".bak";
	remove(bak.c_str()); /* a stale backup from an interrupted save */
	const bool had_original = rename(path_.c_str(), bak.c_str()) == 0;
	if (rename(tmp_path_.c_str(), path_.c_str()) != 0) {
		remove(tmp_path_.c_str());
		if (had_original) rename(bak.c_str(), path_.c_str()); /* the original back */
		return PFF_WRITE_ERR_IO;
	}
	if (had_original) remove(bak.c_str());
	return PFF_WRITE_OK;
}

} // namespace opennova::pff
