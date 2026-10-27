#pragma once

// JoinerConnection's C2S bodies of one or two little-endian dwords (the file-chunk re-requests,
// the probes' answers), shared by its TUs (joiner_connection.cpp, joiner_connection_records.cpp).

#include <base/io/le.h>

#include <cstdint>
#include <vector>

namespace opennova::inmatch {

inline std::vector<uint8_t> le32_pair(uint32_t first, uint32_t second) {
	std::vector<uint8_t> body;
	body.reserve(8);
	io::append_u32_le(body, first);
	io::append_u32_le(body, second);
	return body;
}

inline std::vector<uint8_t> le32_value(uint32_t value) {
	std::vector<uint8_t> body;
	body.reserve(4);
	io::append_u32_le(body, value);
	return body;
}

} // namespace opennova::inmatch
