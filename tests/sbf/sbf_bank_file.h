#pragma once

// A bank fixture as the SBF tests read it: the file's bytes whole, its index opened by the one reader the
// game and the editor use (sbf_open_memory), and an entry's audio and one of its chunks as slices of those
// bytes, where the stream pump reads them (an entry's block_size chunks from its data_offset).

#include <formats/sbf/sbf.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/file_io.h"

struct SbfBankFile {
	std::vector<uint8_t> bytes;
	opennova::sbf::SbfArchive arc{};

	SbfBankFile() = default;
	SbfBankFile(const SbfBankFile &) = delete;
	SbfBankFile &operator=(const SbfBankFile &) = delete;
	~SbfBankFile() { opennova::sbf::sbf_close(&arc); }

	// The file read whole and its index opened: 0, the reader's code, or -10 for a file not read.
	int open(const std::string &path) {
		opennova::sbf::sbf_close(&arc);
		if (!test_io::read_file(path, bytes)) return -10;
		return opennova::sbf::sbf_open_memory(&arc, bytes.data(), bytes.size());
	}

	// The entry's audio, null where it runs past the bank.
	const uint8_t *entry_bytes(const opennova::sbf::SbfRawEntry *entry) const {
		if (entry == nullptr || uint64_t(entry->data_offset) + entry->total_size > bytes.size()) return nullptr;
		return bytes.data() + entry->data_offset;
	}

	// Chunk `index` of the entry, its block_size bytes from index * block_size, null where it lies past the
	// entry's bytes.
	const uint8_t *chunk(const opennova::sbf::SbfRawEntry *entry, uint32_t index) const {
		const uint8_t *audio = entry_bytes(entry);
		if (audio == nullptr) return nullptr;
		const uint64_t at = uint64_t(index) * entry->block_size;
		if (at + entry->block_size > entry->total_size) return nullptr;
		return audio + at;
	}
};
