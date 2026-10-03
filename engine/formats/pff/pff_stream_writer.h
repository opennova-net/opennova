#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <string>
#include <vector>

#include <formats/pff/pff.h>

namespace opennova::pff {

/* A modern PFF archive written a budget of payload bytes at a time: the one writer behind
   pff_write_archive and its streamed forms, which run it to the end in one call, and behind the
   OpenNova Editor's build, which steps it across frames (ADR 0046 d8, S13 A1). open() checks the
   entries (the codes pff_write_archive returns), orders them by normalized name and writes the
   header to "<path>.tmp"; write() writes at most a budget of payload bytes, pulling each payload
   from the caller a chunk at a time; finish() writes the directory, patches the header and moves
   the temp file onto `path`. What lands is the header, the payloads as the caller supplies them
   and the directory, the layout the retail loader reads [orig: PFF_Open @ 0x7682e0], in the
   order its sort keeps [orig: PFF_SortEntries @ 0x768280]: the same bytes however the payloads
   were chunked. A writer destroyed or aborted before finish() removes its temp file and leaves
   `path` as it was. */
class PffStreamWriter {
public:
	/* Fills `out` with `size` bytes of the payload of entry `index` (its index among the entries
	   open() took, not its sorted place) from byte `offset` of that payload; 0 on success. Asked
	   for one entry at a time, in the written order, at increasing offsets from 0. */
	typedef int (*ReadChunkFn)(void *ctx, uint32_t index, uint32_t offset, uint8_t *out, uint32_t size);

	PffStreamWriter() = default;
	~PffStreamWriter();
	PffStreamWriter(const PffStreamWriter &) = delete;
	PffStreamWriter &operator=(const PffStreamWriter &) = delete;

	/* Checks and orders `entries` (names over PFF_NAME_SIZE, blank or repeated normalized names,
	   payloads past the 32-bit offsets are refused before anything is written) and writes the
	   header to "<path>.tmp", `path` a UTF-8 path (base/io/os_path.h). PFF_WRITE_OK, or a
	   PFF_WRITE_ERR_* code with nothing left open. */
	int open(const char *path, PffFormat format, const PffWriteStreamEntry *entries, uint32_t n,
			ReadChunkFn read, void *ctx);
	/* Writes payload bytes in the written order, at most `budget` of them; an empty payload is
	   passed as it is reached. PFF_WRITE_OK, or PFF_WRITE_ERR_IO (a failed read or write: the
	   temp file is removed). */
	int write(uint64_t budget);
	/* True once every payload is written: finish() may run. */
	bool payloads_written() const { return is_open() && next_ == order_.size(); }
	/* Writes the directory, patches the header's directory offset and replaces `path` with the
	   temp file (an existing `path` kept aside as "<path>.bak" until the new one is in place).
	   PFF_WRITE_OK, or PFF_WRITE_ERR_IO with `path` as it was. */
	int finish();
	/* Stops: the temp file removed, `path` untouched. */
	void abort();

	bool is_open() const { return file_ != nullptr; }
	/* Every payload's bytes, and those written so far. */
	uint64_t payload_bytes() const { return payload_bytes_; }
	uint64_t payload_written() const { return payload_written_; }
	/* The payloads written whole (an empty one counts once it is reached). */
	uint32_t entries_written() const { return uint32_t(next_); }

private:
	struct Entry {
		std::string name;
		uint32_t size = 0;
		uint32_t flags = 0;
		uint32_t timestamp = 0;
		uint32_t checksum = 0;
		uint32_t offset = 0;
	};

	int fail(int code);

	std::string path_;
	std::string tmp_path_;
	PffFormat format_ = PFF_FORMAT_PFF3;
	std::vector<Entry> entries_;
	std::vector<uint32_t> order_; // entry indices in the written order
	ReadChunkFn read_ = nullptr;
	void *ctx_ = nullptr;
	FILE *file_ = nullptr;
	size_t next_ = 0;         // the written order's place of the payload being written
	uint32_t entry_done_ = 0; // its bytes written so far
	uint32_t position_ = 0;   // the file offset the next payload byte lands at
	uint64_t payload_bytes_ = 0;
	uint64_t payload_written_ = 0;
	std::vector<uint8_t> chunk_;
};

} // namespace opennova::pff
