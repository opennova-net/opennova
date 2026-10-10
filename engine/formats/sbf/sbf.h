#pragma once

/* SBF (Sound Buffer File) audio bank format reader.

   Header parser witnessed at jointops!Sbf_OpenFile_Gamemus @ 0x004ED6C0
   (legacy gamemus path) and jointops!AudioVM_OpenContextFile @ 0x00672160
   (live AudioVM path); both implement identical reads. */

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace opennova::sbf {

/* --- Format constants --- */
inline constexpr uint32_t SBF_MAGIC = 0x30464253u;  /* 'SBF0' little-endian */
/* The header values every observed retail bank carries and the writer stamps:
   version 0x100 (never read by the engine) and the byte-paired-stereo
   channel-format selector (the engine accepts flags <= 2)
   [orig: AudioVM_OpenContextFile @ 0x672160 header reads]. */
inline constexpr uint32_t SBF_VERSION_DEFAULT = 0x00000100u;
inline constexpr uint32_t SBF_FLAGS_BYTE_PAIRED_STEREO = 0x00000001u;
inline constexpr int SBF_HEADER_SIZE = 24;
inline constexpr int SBF_ENTRY_SIZE = 32;
inline constexpr int SBF_NAME_SIZE = 16;
inline constexpr int SBF_CHUNK_HEADER = 8;
inline constexpr int SBF_CHUNK_AUDIO = 4096;  /* observed audio bytes per full chunk */
inline constexpr int SBF_CHUNK_TOTAL = SBF_CHUNK_HEADER + SBF_CHUNK_AUDIO;
inline constexpr int SBF_SAMPLE_RATE = 22050;
inline constexpr int SBF_CHANNELS = 2;
inline constexpr int SBF_BITS_PER_SAMPLE = 16;

/* --- On-disk structs (must be packed at natural u32 alignment) --- */

typedef struct SbfHeader {
    uint32_t magic;
    uint32_t version;        /* 0x00000100; engine never reads */
    uint32_t flags;          /* channel-format selector; engine accepts <= 2 */
    uint32_t reserved;
    uint32_t index_offset;   /* always 0x18 */
    uint32_t entry_count;
} SbfHeader;

typedef struct SbfRawEntry {
    char     name[SBF_NAME_SIZE];   /* null-padded ASCII */
    uint32_t data_offset;
    uint32_t total_size;
    uint32_t block_size;            /* 0x1008 in every observed file */
    uint32_t sample_length_hint;    /* engine copies to sample_length scheduler;
                                       observed shipped files store 0 */
} SbfRawEntry;

typedef struct SbfChunkHeader {
    uint32_t valid_samples;         /* count of AUDIO BYTES, not int16 samples */
    uint8_t  scale_a;               /* L-channel right-shift (0..7) */
    uint8_t  scale_b;               /* R-channel right-shift (0..7) */
    uint8_t  reserved_a;            /* never read: one value a bank, 0xFA in JO gamemus.sbf (menumus.sbf 0x1A, the expansion's 0x20 and 0xFB) */
    uint8_t  reserved_b;            /* never read: 0x00 or 0x01, one value a bank */
} SbfChunkHeader;

/* --- Open archive (lifetime-owning state) --- */

typedef struct SbfArchive {
    SbfHeader     header;
    SbfRawEntry  *entries;            /* malloc'd, length = header.entry_count */
    void         *_file;              /* opaque FILE* (NULL when memory mode) */
    char          _path[260];
} SbfArchive;

/* --- API --- */

/* Validate raw bytes look like an SBF header. 0 = OK, negative = error. */
int sbf_validate(const uint8_t *data, size_t size);

/* Open by file path. Streams chunk reads off the open handle in sbf_read_*. */
int sbf_open(SbfArchive *arc, const char *path);

/* Open from a contiguous buffer (caller retains ownership of `data`).
   Copies header + entry table into the archive struct. */
int sbf_open_memory(SbfArchive *arc, const uint8_t *data, size_t size);

/* Free entry table and close any open file handle. Idempotent. */
void sbf_close(SbfArchive *arc);

/* Lookup by entry name (case-insensitive, null-padded). NULL on miss. */
const SbfRawEntry *sbf_find_by_name (const SbfArchive *arc, const char *name);

/* Lookup by index. NULL on out-of-range. */
const SbfRawEntry *sbf_find_by_index(const SbfArchive *arc, uint32_t index);

/* Read entry's full audio bytes (entry->total_size) into out_buf.
   Returns bytes read on success, negative on error. File-mode only. */
int sbf_read_raw(const SbfArchive *arc, const SbfRawEntry *entry,
                 uint8_t *out_buf, size_t buf_size);

/* Streaming hot path: read a single block_size-sized chunk into out_buf. */
int sbf_read_chunk(const SbfArchive *arc, const SbfRawEntry *entry,
                   uint32_t chunk_index, uint8_t *out_buf, size_t buf_size);

/* Decode one offset-binary u8 sample to int16 PCM at the channel's mix-coeff
   shift. Algebraically equivalent at unity gain to the engine's mix-time path
   in AudioChannel_ComputeMixCoefficients @ 0x007BD4B0; we collapse the
   shift into the sample-value domain since engine/formats/sbf's output target is int16
   PCM rather than the engine's 8-bit mix buffer. Caller passes scale_a for
   even bytes (L) and scale_b for odd bytes (R). Scales above the format's
   supported 0..7 range clamp to 7. */
static inline int16_t sbf_decode_sample(uint8_t byte, uint8_t scale) {
    if (scale > 7) scale = 7;
    const int32_t s = (int32_t)byte - 128;
    return (int16_t)(s * (int32_t)(128u >> scale));
}

/* Decode one chunk to int16 PCM. Returns sample count (== chunk header
   valid_samples, capped to chunk_size minus the 8-byte header). */
int sbf_decode_chunk(const uint8_t *chunk_bytes, size_t chunk_size,
                     int16_t *out_samples, size_t out_capacity);

/* Decode every chunk in a contiguous raw_bytes buffer (typically a full entry
   read via sbf_read_raw). Returns total samples produced. */
int sbf_decode_all(const uint8_t *raw_bytes, size_t raw_size,
                   int16_t *out_samples, size_t out_capacity);

/* --- Encoder ---

   No engine equivalent: original game shipped pre-encoded SBFs. The encoder is
   the algebraic inverse of sbf_decode_sample so output bytes round-trip
   through the witnessed decoder within one int8 quantum. */

/* Encode one int16 PCM sample to offset-binary u8 at the given mix-coeff
   shift. Saturates rather than wrapping when the post-scale value would
   overflow [0, 255]. Scales above 7 clamp to 7. */
static inline uint8_t sbf_encode_sample(int16_t sample, uint8_t scale) {
    if (scale > 7) scale = 7;
    int32_t v = (int32_t)sample * (int32_t)(2u << scale);
    v = (v / 256) + 128;
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* Pick the largest scale 0..7 whose chunk-wide max byte stays inside [0,255].
   Smaller scale = more headroom; larger scale = more precision but clips
   sooner. Falls through to 0 when the chunk's amplitude exceeds even scale 0
   (encoded bytes will saturate via sbf_encode_sample). */
uint8_t sbf_pick_scale(const int16_t *samples, size_t count);

/* Encode `sample_count` int16 samples (treated as L/R interleaved bytes by the
   decoder) into one chunk: 8-byte header + `SBF_CHUNK_AUDIO` audio bytes.
   Both scale_a and scale_b take sbf_pick_scale's result; reserved bytes get
   gamemus.sbf's 0xFA / 0x00; trailing audio area is padded with 0x80
   (post-decode silence). Returns SBF_CHUNK_TOTAL on success, negative on
   parameter / capacity errors. */
int sbf_encode_chunk(const int16_t *samples, size_t sample_count,
                     uint8_t *out_buf, size_t out_capacity);

/* Build a complete .sbf in memory: 24-byte header (flags = 1, byte-paired
   stereo) + N x 32-byte index + audio chunks. `names[i]` is null-padded to
   SBF_NAME_SIZE; `pcm_samples[i]` provides `pcm_sample_counts[i]` int16
   samples that get split into SBF_CHUNK_AUDIO-sized chunks. Allocates
   `*out_buf` with malloc; caller frees via sbf_free. Returns 0 on success,
   negative on parameter errors. */
int sbf_encode_file(const char * const *names, uint32_t entry_count,
                    const int16_t * const *pcm_samples,
                    const size_t *pcm_sample_counts,
                    uint8_t **out_buf, size_t *out_size);

/* Free a buffer returned by sbf_encode_file. Forwards to free(). */
void sbf_free(void *p);

/* --- The bank as a model ---

   A bank as the fields its every byte is made from (ADR 0003): the header's
   words; the two never-read bytes every chunk header carries, one pair a
   bank; the rule the chunks' unread tails follow; and each stream (an index
   entry) with its chunks, each its two channel shifts and the bytes the
   stream plays (its header's valid count of them). A chunk on disk is a whole
   block (block_size bytes, as every shipped chunk is): its audio area past
   the bytes it plays is never read, and the writer makes it by the bank's
   tail rule. The engine opens the index and streams one entry's chunks from
   its offset, block_size bytes a read, the mixer taking the valid count
   [orig: AudioVM_OpenContextFile @ 0x672160; Sbf_StartEntry @ 0x4ED910;
   Audio_StreamNextChunk @ 0x4ED7D0]. What the editor's music bank document
   holds; sbf_write_bank writes it from scratch: the index with each stream's
   offset and size worked out, the streams one after another in index order,
   so every shipped bank (JO's menumus.sbf, gamemus.sbf and the expansion's
   MJox01.sbf, GJox01.sbf) reads into it and writes back byte for byte. The
   model is SbfFile, as the format libs name a file's model (lwf::File):
   SbfBank is the Godot resource over the streaming reader
   (godot/src/audio/sbf_bank.h). */

/* A name fills the index's 16 bytes; a shorter one ends at a NUL. The game
   plays a stream by its place, never its name (docs/audio/mus-sbf-re.md). */
inline constexpr size_t SBF_STREAM_NAME_MAX = SBF_NAME_SIZE;

/* How a chunk's audio area past the bytes it plays is filled. No reader reads
   those bytes; the rule is the encoder's, measured over JO:CA's four banks
   (2026-10-09: every stream's last chunk is its one partial chunk). */
enum class SbfTail : uint8_t {
    /* The retail encoder's one reused buffer: the stream's previous chunk's
       bytes at the same offsets, zeros under a stream's first chunk (every
       chunk of the shipped banks). */
    Residue,
    /* 0x80, which decodes to silence (sbf_encode_chunk's padding, the minted
       banks'). */
    Silence,
};

struct SbfChunk {
    uint8_t scale_a = 0;           /* the left channel's shift, 0..7 */
    uint8_t scale_b = 0;           /* the right channel's */
    std::vector<uint8_t> audio;    /* the bytes the stream plays (SbfChunkHeader::valid_samples), at most block_size - 8 */
};

struct SbfStream {
    std::string name;              /* at most SBF_STREAM_NAME_MAX bytes, no NUL */
    uint32_t block_size = SBF_CHUNK_TOTAL;
    uint32_t sample_length_hint = 0;
    std::vector<SbfChunk> chunks;
};

struct SbfFile {
    uint32_t version = SBF_VERSION_DEFAULT;
    uint32_t flags = SBF_FLAGS_BYTE_PAIRED_STEREO;
    uint32_t reserved = 0;
    /* Every chunk header's two never-read bytes (SbfChunkHeader::reserved_a,
       _b): one pair a bank, gamemus.sbf's by default. */
    uint8_t chunk_reserved_a = 0xFA;
    uint8_t chunk_reserved_b = 0x00;
    SbfTail tail = SbfTail::Residue;
    std::vector<SbfStream> streams;
};

/* What a read found that a write lays out otherwise: streams not one after
   another from the index's end in index order (a gap, another order), bytes
   past the last stream, a name with bytes after its terminator, chunks whose
   reserved pair is not the bank's (its first chunk's), chunk tails the bank's
   rule (the rule more of them follow) does not make, and short chunks (a last
   chunk shorter than its block, written whole; a valid count past its block,
   cut to it). A write keeps none of those. */
struct SbfFileLayout {
    bool packed = true;
    size_t trailing_bytes = 0;
    size_t names_with_tails = 0;
    size_t reserved_other = 0;
    size_t tails_other = 0;
    size_t short_chunks = 0;
};

/* The bank `data` holds: the header as sbf_validate takes it, every entry's
   bytes inside the file, its block_size at least a chunk header, its
   total_size whole chunks of it but a last one holding at least its header,
   and the entries' sizes adding up to no more than the file holds past the
   index (entries that share bytes are refused). False, with `error`, for one
   the model cannot hold. */
bool sbf_read_bank(const uint8_t *data, size_t size, SbfFile &out, std::string &error,
                   SbfFileLayout *layout = nullptr);

/* The bank's bytes from its fields alone, every chunk a whole block: false,
   with `error`, for a name past SBF_STREAM_NAME_MAX or holding a NUL, a
   block_size under a chunk header, a chunk playing more bytes than its block
   holds, a flags word the engine refuses (above 2), or a bank past 4 GiB. */
bool sbf_write_bank(const SbfFile &bank, std::vector<uint8_t> &out, std::string &error);

/* A stream's audio as int16 PCM, byte-paired stereo interleaved (left then
   right) at SBF_SAMPLE_RATE: each chunk's bytes decoded at its shifts, in
   order, the stream stopping at a chunk whose shift is past 7 (as the Godot
   player stops: sbf_decode_chunk refuses such a chunk). */
std::vector<int16_t> sbf_decode_stream(const SbfStream &stream);

/* A stream of `pcm` (int16, left/right interleaved), encoded chunk by chunk
   as sbf_encode_chunk encodes one: full SBF_CHUNK_AUDIO chunks, the last
   holding the rest, at least one chunk. */
SbfStream sbf_encode_stream(const std::string &name, const int16_t *pcm, size_t count);

} // namespace opennova::sbf
