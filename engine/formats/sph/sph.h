#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

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

// The chunk WRITER, built from scratch (ADR 0003): append one chunk with the
// on-disk `tag` (the reversed mnemonic, 4 bytes), its u16 total length (the
// 8-byte header included) and `payload`. Every retail writer stores the tag
// as a u32 multichar constant or strcpy's the reversed literal, then the
// length word at +4; nothing stores +6..+7, which stay whatever the 1 MiB
// heap buffer held. The writer zeroes them (and any other byte retail leaves
// unwritten), the value the decoder's container walk and the zero-filled
// first pages of the buffer both give [orig: ServerLog_OpenForWrite
// @0x4e2056/@0x4e205c; CServerLog_WriteTimestampRecord @0x4e1ade/@0x4e1ae4;
// CServerLog_WritePositionRecord strcpy "TADP," @0x4e1b3d;
// CServerLog_CloseAndFree strcpy "DNE." @0x4e1a4e]. Returns false (and
// appends nothing) when the chunk would not fit the u16 length.
bool append_chunk(std::vector<uint8_t> &out, const char tag[4], const uint8_t *payload,
		size_t payload_len);

}  // namespace opennova::sph
