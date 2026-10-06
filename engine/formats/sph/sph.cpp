#include <formats/sph/sph.h>

namespace opennova::sph {

WalkResult next_chunk(const uint8_t *data, size_t len, size_t offset, Chunk &out) {
    if (!data || offset + 8 > len) {
        return WalkResult::kEndOfData;
    }
    // Length is the u16 at +4; the two pad bytes above it are always zero
    // [orig: the CServerLog_* writers' zero-initialised header buffer].
    const uint16_t clen =
        uint16_t(data[offset + 4]) | uint16_t(data[offset + 5]) << 8;
    if (clen < 8 || offset + clen > len) {
        return WalkResult::kMalformed;
    }
    out.tag = data + offset;
    out.payload = data + offset + 8;
    out.payload_len = size_t(clen) - 8;
    out.total_len = clen;
    return WalkResult::kChunk;
}

bool append_chunk(std::vector<uint8_t> &out, const char tag[4], const uint8_t *payload,
		size_t payload_len) {
    const size_t total = payload_len + 8;
    if (total > 0xFFFF || (payload_len != 0 && payload == nullptr)) return false;
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(tag[i]));
    // The u16 total length at +4, then the two pad bytes no writer stores.
    out.push_back(static_cast<uint8_t>(total & 0xFF));
    out.push_back(static_cast<uint8_t>(total >> 8));
    out.push_back(0);
    out.push_back(0);
    if (payload_len != 0) out.insert(out.end(), payload, payload + payload_len);
    return true;
}

}  // namespace opennova::sph
