#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>


namespace opennova::bfc1 {

inline constexpr uint32_t BFC1_MAGIC = 0x31434642u;  /* "BFC1" little-endian */
inline constexpr int BFC1_HEADER_SIZE = 8;

/* Check if data starts with BFC1 magic. */
int bfc1_is_bfc1(const uint8_t *data, size_t size);

/* Decompress BFC1 data from memory, as the game reads it: the header's size, the stream
   inflated into it, a stream that ends early or does not inflate keeping that size with the
   bytes past what inflated zero; a stream that reaches its own end gives its own length.
   out_buf must be at least *out_size bytes; on entry *out_size is the buffer capacity, on
   success the decompressed size. Non-zero when the data is no BFC1 (-1) or the buffer is
   smaller than the header's size (-2). [orig: AudioFile_DecompressBFC_Aligned @ 0x75AFB0] */
int bfc1_decompress(const uint8_t *data, size_t size,
                    uint8_t *out_buf, size_t *out_size);

/* Get the uncompressed size from a BFC1 header without decompressing.
   Returns 0 on success (writes to *out_size), non-zero if not BFC1. */
int bfc1_uncompressed_size(const uint8_t *data, size_t size,
                           uint32_t *out_size);

/* `data` unpacked in place when it is BFC1, left as it is when it is not: the
   mounted-file decode's second layer, and the models' TGA reader's unpack of a
   BFC1 file [orig: CTerrainTileData_LoadTGAFromArchive @ 0x56E570,
   AudioFile_DecompressBFC_Aligned @ 0x75AFB0], a damaged stream kept at the
   header's size as bfc1_decompress keeps it. True for every input: the game's read
   of a BFC1 file fails only when it cannot allocate the header's size, which is not
   modelled here. */
bool bfc1_unpack(std::vector<uint8_t> &data);

} // namespace opennova::bfc1
