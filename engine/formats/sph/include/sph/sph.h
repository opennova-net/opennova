#pragma once

#include <cstddef>
#include <cstdint>

namespace opennova::sph {

// The .sph `/PROFILE` server-log CONTAINER: a flat FOURCC chunk stream.
// On-disk chunk = [char[4] tag][u16 length][u16 pad][payload]; `length` is the
// TOTAL chunk size including the 8-byte header (the pad's two high bytes are
// always zero — the byte-trick writers strcpy only the low byte)
// [orig: the CServerLog_* writer cluster @ 0x4e1a10..0x4e1e00; the per-frame
//  loop Game_ProcessMainFrame @ 0x5263f0]. This lib owns only the container
// walk; the chunk PAYLOADS are replication records whose interpretation is
// net knowledge and stays in engine/net/npwire (serverlog_decode.h — the tag
// mnemonics, field maps, and cross-validation live there and in
// docs/net/novaworld-net-re.md §5.22).
struct Chunk {
    const uint8_t *tag = nullptr;      // 4 bytes, on-disk (reversed-mnemonic) order
    const uint8_t *payload = nullptr;
    size_t payload_len = 0;
    size_t total_len = 0;              // including the 8-byte header
};

enum class WalkResult {
    kChunk,      // `out` holds the chunk at `offset`
    kEndOfData,  // fewer than 8 bytes remain — the normal walk end
    kMalformed,  // a header is present but its length is short or runs past EOF
};

// Read the chunk starting at `offset` without consuming it; the caller
// advances by `out.total_len`.
WalkResult next_chunk(const uint8_t *data, size_t len, size_t offset, Chunk &out);

}  // namespace opennova::sph
