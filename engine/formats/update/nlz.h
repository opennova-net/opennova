#pragma once

// 1ZLN, the updater's compressed file: a 16-byte header and a zlib stream.
// docs/update/updater-re.md §9. [orig: Nlz_DecompressFile @ 0x401000 (UPDATE.EXE)]

#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::update {

inline constexpr uint32_t kNlzMagic = 0x4E4C5A31u; // bytes "1ZLN"
inline constexpr size_t kNlzHeaderBytes = 16;

// The header: {magic, the zlib stream's offset (16), the inflated size, the stream's size}.
struct NlzHeader {
	uint32_t magic = kNlzMagic;
	uint32_t data_offset = kNlzHeaderBytes;
	uint32_t raw_size = 0;
	uint32_t packed_size = 0;
};

// The header of `data` when it is at least 16 bytes and begins with the magic.
bool nlz_read_header(const uint8_t *data, size_t size, NlzHeader &out);

// `data` compressed as a 1ZLN file (the zlib stream at miniz's default level).
std::vector<uint8_t> nlz_encode(const uint8_t *data, size_t size);

enum class NlzDecode {
	Inflated, // a 1ZLN file, inflated to its raw size
	Raw, // no 1ZLN magic: the file is its own content, copied as it is
	Corrupt, // under 4 bytes, a stream past the file's end, or a stream that does not inflate
			 // to the header's raw size
};

// A file as the updater installs it: inflated when it is 1ZLN, else copied as it is.
NlzDecode nlz_decode(const uint8_t *data, size_t size, std::vector<uint8_t> &out);

} // namespace opennova::update
