#include <napi/envelope.h>

#include <novacrypto/crc32.h>

namespace opennova {

namespace {

constexpr size_t SCATTER_THRESHOLD = 32;
constexpr size_t HEADER_SIZE = 4;

inline uint32_t read_le32(const uint8_t *p) {
	return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
			(static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline void write_le32(uint8_t *p, uint32_t v) {
	p[0] = static_cast<uint8_t>(v & 0xFFu);
	p[1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
	p[2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
	p[3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
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

	write_le32(out, header);
	*out_size = src_len + HEADER_SIZE;
	return 0;
}

int napi_envelope_decode(const uint8_t *packet, size_t packet_len,
                         uint8_t *out, size_t out_cap, size_t *out_size) {
	if (!packet || !out || !out_size || packet_len < HEADER_SIZE) {
		return -1;
	}
	const size_t payload_len = packet_len - HEADER_SIZE;
	if (payload_len == 0 || payload_len > out_cap) {
		return -1;
	}

	// Reject variable-size-header mode explicitly. This port emits only
	// the 4-byte mode; variable-size inputs are out of scope for B.2.1
	// (see notes/net_verification_log.md; decoder living elsewhere may
	// accept both).
	if (read_le32(packet) == 0u) {
		return -1;
	}

	// Copy payload to out[]. We don't mutate the input packet.
	for (size_t i = 0; i < payload_len; ++i) {
		out[i] = packet[HEADER_SIZE + i];
	}

	const uint32_t header = read_le32(packet);
	uint32_t expected_crc;
	if (payload_len >= SCATTER_THRESHOLD) {
		// Unwind the scatter: restore original low bit from the header and
		// collect the CRC bits that were living in the low bits of the
		// output bytes.
		expected_crc = 0;
		for (size_t i = 0; i < SCATTER_THRESHOLD; ++i) {
			const uint8_t b = out[i];
			expected_crc |= static_cast<uint32_t>(b & 1u) << i;
			out[i] = static_cast<uint8_t>((b & 0xFEu) | static_cast<uint8_t>((header >> i) & 1u));
		}
	} else {
		expected_crc = header;
	}

	const uint32_t actual_crc = crc32_napi(out, payload_len);
	if (actual_crc != expected_crc) {
		return -1;
	}

	*out_size = payload_len;
	return 0;
}

} // namespace opennova
