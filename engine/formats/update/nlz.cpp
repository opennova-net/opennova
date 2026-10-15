#include "nlz.h"

#include <base/io/le.h>

#include <miniz.h> // third_party/miniz: zlib's stream, as UPDATE.EXE's zlib 1.2.3 reads it

#include <cstring>

namespace opennova::update {

bool nlz_read_header(const uint8_t *data, size_t size, NlzHeader &out) {
	if (data == nullptr || size < kNlzHeaderBytes)
		return false;
	NlzHeader header;
	header.magic = io::read_u32_le(data);
	if (header.magic != kNlzMagic)
		return false;
	header.data_offset = io::read_u32_le(data + 4);
	header.raw_size = io::read_u32_le(data + 8);
	header.packed_size = io::read_u32_le(data + 12);
	out = header;
	return true;
}

std::vector<uint8_t> nlz_encode(const uint8_t *data, size_t size) {
	mz_ulong packed = mz_compressBound(static_cast<mz_ulong>(size));
	std::vector<uint8_t> out(kNlzHeaderBytes + packed);
	const uint8_t empty = 0;
	if (mz_compress2(out.data() + kNlzHeaderBytes, &packed, size ? data : &empty,
				static_cast<mz_ulong>(size), MZ_DEFAULT_COMPRESSION) != MZ_OK)
		return {};
	out.resize(kNlzHeaderBytes + packed);
	io::write_u32_le(out.data(), kNlzMagic);
	io::write_u32_le(out.data() + 4, static_cast<uint32_t>(kNlzHeaderBytes));
	io::write_u32_le(out.data() + 8, static_cast<uint32_t>(size));
	io::write_u32_le(out.data() + 12, static_cast<uint32_t>(packed));
	return out;
}

NlzDecode nlz_decode(const uint8_t *data, size_t size, std::vector<uint8_t> &out) {
	// Under four bytes the updater fails before it reads the magic. [orig: Nlz_DecompressFile
	// @ 0x401000 (UPDATE.EXE)]
	if (data == nullptr || size < 4)
		return NlzDecode::Corrupt;
	if (io::read_u32_le(data) != kNlzMagic) {
		out.assign(data, data + size);
		return NlzDecode::Raw;
	}
	NlzHeader header;
	if (!nlz_read_header(data, size, header))
		return NlzDecode::Corrupt;
	// The stream must lie inside the file; the updater checks its size against the file less
	// the header, and inflates from the header's offset. [orig: Nlz_DecompressFile @ 0x401000
	// (UPDATE.EXE)]
	if (header.packed_size > size - kNlzHeaderBytes || header.data_offset > size ||
			header.packed_size > size - header.data_offset)
		return NlzDecode::Corrupt;
	out.assign(header.raw_size, 0);
	mz_ulong inflated = header.raw_size;
	const uint8_t sink = 0;
	const int rc = mz_uncompress(header.raw_size ? out.data() : const_cast<uint8_t *>(&sink),
			&inflated, data + header.data_offset, header.packed_size);
	if (rc != MZ_OK || inflated != header.raw_size) {
		out.clear();
		return NlzDecode::Corrupt;
	}
	return NlzDecode::Inflated;
}

} // namespace opennova::update
