#include "sph/sph.h"

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

}  // namespace opennova::sph
