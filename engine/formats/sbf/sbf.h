#pragma once

/* SBF (Sound Buffer File) audio bank format reader.

   Header parser witnessed at [orig: Sbf_OpenFile_Gamemus @ 0x004ED6C0]
   (legacy gamemus path) and [orig: AudioVM_OpenContextFile @ 0x00672160]
   (live AudioVM path); both implement identical reads. */

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace opennova::sbf {

/* --- Format constants --- */
inline constexpr uint32_t SBF_MAGIC = 0x30464253u;  /* 'SBF0' little-endian */
/* The header's version and reserved words: the engine reads the 24-byte header
   whole and never reads either (the version at +4, the reserved word at +12:
   no reader of the header copy [orig: AudioVM_OpenContextFile @ 0x672160, the
   ReadFile into g_AudioVMBankHeader @ 0x67221f, no xref to its +4 or +12; the dead
   Sbf_OpenFile_Gamemus @ 0x4ED6C0's copy stru_C60D68 likewise]). No original
   writer of a bank is witnessed, so the writer writes its own constants, never
   a bank's: 0x100, the version every observed bank carries, and 0. */
inline constexpr uint32_t SBF_VERSION_DEFAULT = 0x00000100u;
inline constexpr uint32_t SBF_HEADER_RESERVED = 0x00000000u;
/* The byte-paired-stereo channel-format selector every observed bank carries
   (the engine accepts flags <= 2) [orig: Sbf_OpenFile_Gamemus @ 0x4ed717;
   AudioStream_Update @ 0x671cca reads it]. */
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
    uint32_t flags;          /* the stream paths' form: 1 byte-paired, any other IMA ADPCM stereo */
    uint32_t reserved;
    uint32_t index_offset;   /* where the live open reads the index; the writer's 0x18 */
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
    uint8_t  scale_a;               /* L-channel right-shift, any byte (15 and up silent) */
    uint8_t  scale_b;               /* R-channel right-shift, likewise */
    uint8_t  reserved_a;            /* never read: one value a bank, 0xFA in JO gamemus.sbf (menumus.sbf 0x1A, the expansion's 0x20 and 0xFB) */
    uint8_t  reserved_b;            /* never read: 0x00 or 0x01, one value a bank */
} SbfChunkHeader;

/* --- Open bank index (the header and entry table of a bank held in memory) --- */

typedef struct SbfArchive {
    SbfHeader     header;
    SbfRawEntry  *entries;            /* malloc'd, length = header.entry_count */
} SbfArchive;

/* --- API --- */

/* Validate a bank's bytes: the SBF0 magic, the header and the whole entry index
   (at the header's index offset) inside `size`. 0 = OK, negative = error. */
int sbf_validate(const uint8_t *data, size_t size);

/* Open from a contiguous buffer (caller retains ownership of `data`).
   Copies header + entry table into the archive struct; an entry's audio is the
   buffer's bytes at its data_offset. */
int sbf_open_memory(SbfArchive *arc, const uint8_t *data, size_t size);

/* Free the entry table. Idempotent. */
void sbf_close(SbfArchive *arc);

/* Lookup by entry name (case-insensitive, null-padded). NULL on miss. */
const SbfRawEntry *sbf_find_by_name (const SbfArchive *arc, const char *name);

/* Lookup by index. NULL on out-of-range. */
const SbfRawEntry *sbf_find_by_index(const SbfArchive *arc, uint32_t index);

/* Decode one offset-binary u8 sample to int16 PCM at its channel's shift, as
   the music stream mixes it. The stream pump hands each chunk to the stereo
   split, which makes it two 8-bit AUD1 channel buffers: each sample byte - 0x80,
   the width byte 1 and the shift byte the chunk's scale_a / scale_b [orig:
   AudioStream_Update @ 0x671D17..0x671D56 -> Audio_SubmitStereoSampleSplit
   @ 0x7BD205 -- the shifts @ 0x7BD23E / 0x7BD241, the width @ 0x7BD24A /
   0x7BD24D, the bias @ 0x7BD258 / 0x7BD25A]. The mixer reads an 8-bit buffer at
   the 16-bit scale, its four taps summed and shifted left 6, their average
   times 256 [orig: AudioChannel_BindSampleBuffer @ 0x7BD6A1, the patched `shl ebx, 6`], and shifts
   the channel's mix coefficients right by the shift byte, any count, so 15 or
   more plays silent [orig: AudioChannel_ComputeMixCoefficients
   @ 0x7BD5E6..0x7BD5F3, psraw]. Ours folds the shift onto the sample,
   ((byte - 128) * 256) >> n, as lwf::decode_aud1 decodes the same 8-bit buffer
   (the coefficient rounding is D-SND-51's). Caller passes scale_a for even
   bytes (L) and scale_b for odd bytes (R). */
static inline int16_t sbf_decode_sample(uint8_t byte, uint8_t scale) {
    if (scale >= 15) return 0;
    const int32_t s = ((int32_t)byte - 128) * 256;
    return (int16_t)(s >> scale);
}

/* Decode one chunk to int16 PCM. Returns sample count (== chunk header
   valid_samples, capped to chunk_size minus the 8-byte header). */
int sbf_decode_chunk(const uint8_t *chunk_bytes, size_t chunk_size,
                     int16_t *out_samples, size_t out_capacity);

/* Decode every chunk in a contiguous raw_bytes buffer (typically an entry's
   bytes in the bank, at its data_offset). Returns total samples produced. */
int sbf_decode_all(const uint8_t *raw_bytes, size_t raw_size,
                   int16_t *out_samples, size_t out_capacity);

/* An entry's audio as the stream pump plays it, int16 PCM, byte-paired stereo
   interleaved (left then right) at SBF_SAMPLE_RATE: from the entry's start, one
   block_size chunk at a time until the entry's bytes run out, each decoded at its
   shifts (sbf_decode_chunk) and kept as whole frames, the split counting a
   chunk's frames as its valid count >> 1 and queueing nothing for a chunk of
   under 4 frames or over 2048, the pump going on to the next block
   [orig: AudioStream_Update @ 0x671C60 -- the entry's end @ 0x671C7D, the block
   read @ 0x671CBC, the split @ 0x671D56, the countdown @ 0x671D61;
   Audio_SubmitStereoSampleSplit @ 0x7BD222..0x7BD233, `shr ecx,1; cmp ecx,3;
   jbe` / `cmp ecx,800h; ja` to the return before any store]. A chunk the
   decoder refuses, or one past the bytes, ends the stream. The Godot player
   hands this PCM to the mixer whole (godot/src/audio/sbf_audio_stream.cpp). */
std::vector<int16_t> sbf_decode_entry(const uint8_t *entry_bytes, size_t size,
                                      uint32_t block_size, uint32_t flags = SBF_FLAGS_BYTE_PAIRED_STEREO,
                                      uint32_t sample_length = 0);

/* A bank of any flags word but 1 (0 or 2, which the header test takes) streams stereo IMA
   ADPCM: each block_size read plays min(4068, the entry's samples left) frames, the count
   unsigned against the samples left (+0x1C of its index entry, less 4068 a block, so a stream
   past them plays 4068 a block), none under 1 [orig: AudioVM_SelectStream @ 0x671BC0 - the
   count @ 0x671BF8, 4068 @ 0x671BFE; AudioStream_Update @ 0x671C60 - the flags test
   @ 0x671CCA, the count @ 0x671CF8..0x671D09, the samples left less 4068 @ 0x671D72;
   Audio_StreamNextChunk @ 0x4ED7D0 alike]. A chunk of 1024-byte blocks interleaved by
   channel: each block the right channel's 4-byte header (its predictor a sample, its step
   index a signed byte) at +0 and the left's at +4, then 127 pairs of 4-byte words, the right's
   then the left's, each word eight nibbles low first through the wave loader's own step; a
   block makes 1017 samples. The right decodes (count + 0x1C + 0x3FF) >> 10 blocks, the left
   always four. The +0 channel fills the buffer mixer channel 2 plays at pan 0xC0, the +4
   channel channel 1's at pan 0x40, the buffer the byte-paired split fills with its first
   (left) byte, so both forms play on mixer channels 1 and 2 alike (D-SND-8)
   [orig: Audio_PlayStereoSample @ 0x7BCF95 - g_MusicStreamLeftBuffers @ 0x7BCFA4 and
   g_MusicStreamRightBuffers @ 0x7BCFAB, the count's bounds @ 0x7BCFB2..0x7BCFC1, the +0
   channel's blocks @ 0x7BCFEA..0x7BCFF3 into g_MusicStreamRightBuffers, the +4 channel's four
   @ 0x7BD095 into g_MusicStreamLeftBuffers (the pushes @ 0x7BCFE0..0x7BCFE9, the pops
   @ 0x7BD090..0x7BD091), the blocks @ 0x7BD001..0x7BD07F and @ 0x7BD0A5..0x7BD123, each nibble
   Audio_AdpcmDecodeNibbleStep @ 0x7BF250; Audio_SubmitStereoSampleSplit @ 0x7BD205 - the first
   byte into g_MusicStreamLeftBuffers @ 0x7BD214..0x7BD25D]. `count` frames, left then right, appended to `out`; none for a
   count outside 1..4068. */
inline constexpr uint32_t SBF_ADPCM_FRAMES = 4068;
void sbf_decode_adpcm_chunk(const uint8_t *block, size_t size, uint32_t count, std::vector<int16_t> &out);

/* The stereo frames sbf_decode_entry makes of an ADPCM entry of `size` bytes, counted without
   decoding: each block_size read's count as above, summed (a playback length). */
uint64_t sbf_adpcm_entry_frames(size_t size, uint32_t block_size, uint32_t sample_length);

/* --- Encoder ---

   No engine equivalent: original game shipped pre-encoded SBFs. The encoder is
   the algebraic inverse of sbf_decode_sample so output bytes round-trip
   through the witnessed decoder within one int8 quantum (256 >> scale). */

/* Encode one int16 PCM sample to offset-binary u8 at the given shift:
   sample * 2^scale / 256 + 128, the inverse of sbf_decode_sample. Saturates
   rather than wrapping when the post-scale value would overflow [0, 255].
   The encoder writes shifts 0..7 alone (scales above 7 clamp to 7). */
static inline uint8_t sbf_encode_sample(int16_t sample, uint8_t scale) {
    if (scale > 7) scale = 7;
    int32_t v = (int32_t)sample * (int32_t)(1u << scale);
    v = (v / 256) + 128;
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

/* Pick the largest scale 0..7 whose chunk-wide max byte stays inside [0,255].
   Smaller scale = more headroom; larger scale = more precision but clips
   sooner. Falls through to 0 when the chunk's amplitude exceeds even scale 0
   (only the most negative 16-bit sample, which encodes to the 0x00 rail
   exactly). */
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
   flags word (its version and reserved words, which no reader reads, are the
   writer's constants, SBF_VERSION_DEFAULT and SBF_HEADER_RESERVED) and each
   stream (an index entry) with its chunks, each its two channel shifts and the
   bytes the stream plays (its header's valid count of them). A chunk on disk is a whole block (block_size bytes, as every shipped
   chunk is); its header's two reserved bytes and its audio area past the
   bytes it plays are never read, and the writer fills them with its own
   constants (SBF_CHUNK_RESERVED_A, _B, SBF_CHUNK_TAIL), never a bank's. The
   engine opens the index and streams one entry's chunks from its offset,
   block_size bytes a read, the mixer taking the valid count [orig:
   AudioVM_OpenContextFile @ 0x672160; Sbf_StartEntry @ 0x4ED910;
   Audio_StreamNextChunk @ 0x4ED7D0]. What the editor's music bank document
   holds; sbf_write_bank writes it from scratch: the index with each stream's
   offset and size worked out, the streams one after another in index order,
   so every shipped bank (JO's menumus.sbf, gamemus.sbf and the expansion's
   MJox01.sbf, GJox01.sbf) reads into it and reads back from its write as
   the same fields. The model is SbfFile, as the format libs name a file's
   model (lwf::File): SbfBank is the Godot resource over the streaming reader
   (godot/src/audio/sbf_bank.h). */

/* A name fills the index's 16 bytes; a shorter one ends at a NUL. The game
   plays a stream by its place, never its name (docs/audio/mus-sbf-re.md). */
inline constexpr size_t SBF_STREAM_NAME_MAX = SBF_NAME_SIZE;

/* What the writer puts in the bytes of a chunk no reader reads: its header's
   two reserved bytes (SbfChunkHeader::reserved_a, _b) and its audio area past
   the bytes it plays. No original writer of a bank is witnessed, so these are
   the writer's own: gamemus.sbf's reserved pair, which sbf_encode_chunk has
   always written, and 0x80, which decodes to silence (the shipped banks carry
   one pair a bank and the retail encoder's reused buffer in their tails,
   docs/audio/mus-sbf-re.md; a write keeps neither). */
inline constexpr uint8_t SBF_CHUNK_RESERVED_A = 0xFA;
inline constexpr uint8_t SBF_CHUNK_RESERVED_B = 0x00;
inline constexpr uint8_t SBF_CHUNK_TAIL = 0x80;

/* A chunk of a byte-paired bank (flags 1): its two shifts and the bytes it plays. A chunk of a
   bank of any other flags (the IMA ADPCM form, sbf_decode_adpcm_chunk) has no header: its
   `audio` is the block's bytes, at most block_size, and its shifts are 0. */
struct SbfChunk {
    uint8_t scale_a = 0;           /* the left channel's shift, any byte (15 and up silent) */
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
    uint32_t flags = SBF_FLAGS_BYTE_PAIRED_STEREO;
    std::vector<SbfStream> streams;
};

/* What a read found that a write lays out otherwise: the header's version or
   reserved word not the writer's (each counted), streams not one after another
   from the index's end in index order (a gap, another order), bytes past the
   last stream, a name with bytes after its terminator, chunks whose reserved
   pair is not the writer's, chunk tails not the writer's SBF_CHUNK_TAIL, and
   short chunks (a last chunk shorter than its block, written whole; a valid
   count past its block, cut to it), and an index the header places past byte
   24 (written at 24; the bytes past the last stream are then not counted). A
   write keeps none of those. */
struct SbfFileLayout {
    size_t header_words_other = 0;
    bool index_moved = false;
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
   holds, or a bank past 4 GiB. Any flags word is written: the game's stream
   paths play 1 as byte-paired and any other as IMA ADPCM stereo. */
bool sbf_write_bank(const SbfFile &bank, std::vector<uint8_t> &out, std::string &error);

/* A stream's audio as int16 PCM, byte-paired stereo interleaved (left then
   right) at SBF_SAMPLE_RATE: each chunk's bytes decoded at its shifts, in
   order (sbf_decode_sample: a shift of 15 or more decodes to silence). */
std::vector<int16_t> sbf_decode_stream(const SbfStream &stream, uint32_t flags = SBF_FLAGS_BYTE_PAIRED_STEREO);

/* The same stream as the game plays it: each chunk through the stereo split's
   frame rule, as sbf_decode_entry plays an entry's blocks (the editor's
   preview). */
std::vector<int16_t> sbf_play_stream(const SbfStream &stream, uint32_t flags = SBF_FLAGS_BYTE_PAIRED_STEREO);

/* A stream of `pcm` (int16, left/right interleaved), encoded chunk by chunk
   as sbf_encode_chunk encodes one: full SBF_CHUNK_AUDIO chunks, the last
   holding the rest, at least one chunk. */
SbfStream sbf_encode_stream(const std::string &name, const int16_t *pcm, size_t count);

} // namespace opennova::sbf
