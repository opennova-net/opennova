#include <net/napi/envelope.h>
#include <base/io/le.h>

#include <net/novacrypto/crc32.h>

#include <cstring>

namespace opennova {

namespace {

constexpr size_t SCATTER_THRESHOLD = 32;
constexpr size_t HEADER_SIZE = 4;

// Decode one of retail's two header layouts without mutating the packet. The
// extended layout keeps its scatter carrier (or direct CRC for a short body)
// at +4; the ordinary layout keeps it at +0.
// [orig: NapiNP_UnpackPacket @0x62ca20]
bool decode_with_header(const uint8_t *packet, size_t packet_len,
		size_t header_size, uint8_t *out, size_t out_cap,
		size_t *out_size) {
	if (header_size < HEADER_SIZE || header_size >= packet_len) return false;
	const size_t payload_len = packet_len - header_size;
	if (payload_len == 0 || payload_len > out_cap) return false;
	const size_t crc_offset = header_size == HEADER_SIZE ? 0 : HEADER_SIZE;
	if (crc_offset + sizeof(uint32_t) > header_size) return false;

	std::memcpy(out, packet + header_size, payload_len);
	const uint32_t carrier = io::read_u32_le(packet + crc_offset);
	uint32_t expected_crc = carrier;
	if (payload_len >= SCATTER_THRESHOLD) {
		expected_crc = 0;
		for (size_t i = 0; i < SCATTER_THRESHOLD; ++i) {
			const uint8_t byte = out[i];
			expected_crc |= static_cast<uint32_t>(byte & 1u) << i;
			out[i] = static_cast<uint8_t>(
					(byte & 0xFEu) | static_cast<uint8_t>((carrier >> i) & 1u));
		}
	}

	if (crc32_napi(out, payload_len) != expected_crc) return false;
	*out_size = payload_len;
	return true;
}

} // namespace

int napi_envelope_encode(const uint8_t *src, size_t src_len,
                         uint8_t *out, size_t out_cap, size_t *out_size) {
	if (!src || !out || !out_size || src_len == 0) {
		return -1;
	}
	if (src_len > out_cap || out_cap - src_len < HEADER_SIZE) {
		return -1;
	}

	// Copy src to out[4..], computing CRC over the original bytes along
	// the way. The copy happens before scatter so the CRC is of the
	// unscrambled payload (which is what the decoder will reconstruct).
	uint32_t crc = CRC32_INIT;
	for (size_t i = 0; i < src_len; ++i) {
		out[HEADER_SIZE + i] = src[i];
		crc = crc32_napi_update(crc, src[i]);
	}

	uint32_t header;
	if (src_len >= SCATTER_THRESHOLD) {
		// Steganographic scatter: bit i of header carries the original low
		// bit of out[4+i]; low bit of out[4+i] carries bit i of the CRC.
		header = 0;
		for (size_t i = 0; i < SCATTER_THRESHOLD; ++i) {
			const uint8_t b = out[HEADER_SIZE + i];
			header |= static_cast<uint32_t>(b & 1u) << i;
			out[HEADER_SIZE + i] = static_cast<uint8_t>((b & 0xFEu) | static_cast<uint8_t>((crc >> i) & 1u));
		}
	} else {
		header = crc;
	}

	io::write_u32_le(out, header);
	*out_size = src_len + HEADER_SIZE;
	return 0;
}

int napi_envelope_decode(const uint8_t *packet, size_t packet_len,
                         uint8_t *out, size_t out_cap, size_t *out_size) {
	if (!packet || !out || !out_size || packet_len < HEADER_SIZE) {
		return -1;
	}

	const uint32_t first_dword = io::read_u32_le(packet);
	if (first_dword != 0u) {
		return decode_with_header(packet, packet_len, HEADER_SIZE,
				out, out_cap, out_size) ? 0 : -1;
	}

	// A zero first dword selects the extended form. Retail reads the unsigned
	// header size from +9. Only a value greater than the ordinary four-byte
	// header enters the extended-attempt/fallback arm; smaller values reject.
	// The fallback is required because zero is also a legitimate scatter
	// carrier when the first 32 original payload bytes all had clear low bits.
	// [orig: NapiNP_UnpackPacket @0x62ca29..0x62cd43]
	if (packet_len <= 9) return -1;
	const size_t variable_header_size = packet[9];
	if (variable_header_size <= HEADER_SIZE) return -1;
	// Retail assumes a well-formed packet here. Keep the structural port
	// bounded: +9 and the +4 CRC dword must both belong to the header.
	if (variable_header_size >= 10 && variable_header_size < packet_len &&
			decode_with_header(packet, packet_len, variable_header_size,
					out, out_cap, out_size)) {
		return 0;
	}
	return decode_with_header(packet, packet_len, HEADER_SIZE,
			out, out_cap, out_size) ? 0 : -1;
}

} // namespace opennova
