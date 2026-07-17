#ifndef SBF_H
#define SBF_H

/* SBF (Sound Buffer File) audio bank format reader.

   Header parser witnessed at jointops!Sbf_OpenFile_Gamemus @ 0x004ED6C0
   (legacy gamemus path) and jointops!AudioVM_OpenContextFile @ 0x00672160
   (live AudioVM path); both implement identical reads. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Format constants --- */
#define SBF_MAGIC          0x30464253u      /* 'SBF0' little-endian */
#define SBF_HEADER_SIZE    24
#define SBF_ENTRY_SIZE     32
#define SBF_NAME_SIZE      16
#define SBF_CHUNK_HEADER   8
#define SBF_CHUNK_AUDIO    4096             /* observed audio bytes per full chunk */
#define SBF_CHUNK_TOTAL    (SBF_CHUNK_HEADER + SBF_CHUNK_AUDIO)
#define SBF_SAMPLE_RATE    22050
#define SBF_CHANNELS       2
#define SBF_BITS_PER_SAMPLE 16

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
    uint8_t  reserved_a;            /* always 0xFA on disk; never read */
    uint8_t  reserved_b;            /* always 0x00 on disk; never read */
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
   in audio_channel_compute_mix_coefficients @ 0x007BD4B0; we collapse the
   shift into the sample-value domain since libs/sbf's output target is int16
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
   the on-disk constants 0xFA / 0x00; trailing audio area is padded with 0x80
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

#ifdef __cplusplus
}
#endif

#endif /* SBF_H */
