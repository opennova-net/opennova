/* SBF audio bank reader implementation.

   [orig: Sbf_OpenFile_Gamemus @ 0x4ED6C0, AudioVM_OpenContextFile @ 0x672160, Sbf_StartEntry @ 0x4ED910, Audio_StreamNextChunk @ 0x4ED7D0]
   Header parser: jointops!Sbf_OpenFile_Gamemus @ 0x004ED6C0
                  jointops!AudioVM_OpenContextFile @ 0x00672160
   Per-entry stream init: jointops!Sbf_StartEntry @ 0x004ED910
   Chunk decode: jointops!Audio_StreamNextChunk @ 0x004ED7D0 +
                 jointops!audio_channel_compute_mix_coefficients @ 0x007BD4B0 */

#include "sbf/sbf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Witnessed: jointops!Sbf_OpenFile_Gamemus @ 0x004ED6C0 (magic + flags check).
   The bounds-vs-size check only fires when caller gave us more than just the
   header; passing exactly SBF_HEADER_SIZE bytes does header-only validation
   (used by sbf_open before it has read the index off disk). */
extern "C" int sbf_validate(const uint8_t *data, size_t size) {
    if (!data || size < SBF_HEADER_SIZE) return -1;
    SbfHeader h;
    memcpy(&h, data, SBF_HEADER_SIZE);
    if (h.magic != SBF_MAGIC) return -2;
    if (h.flags > 2) return -5;
    if (h.index_offset != SBF_HEADER_SIZE) return -3;
    if (size > SBF_HEADER_SIZE
        && (uint64_t)h.entry_count * SBF_ENTRY_SIZE > size - SBF_HEADER_SIZE) {
        return -4;
    }
    return 0;
}

/* Witnessed: jointops!Sbf_OpenFile_Gamemus @ 0x004ED6C0.
   Mirrors the engine's CreateFile+ReadFile pair: 24-byte header, then N×32-byte
   entries from index_offset. */
extern "C" int sbf_open(SbfArchive *arc, const char *path) {
    if (!arc || !path) return -1;
    memset(arc, 0, sizeof(*arc));

    FILE *f = fopen(path, "rb");
    if (!f) return -10;

    uint8_t header[SBF_HEADER_SIZE];
    if (fread(header, 1, SBF_HEADER_SIZE, f) != SBF_HEADER_SIZE) {
        fclose(f); return -11;
    }
    int v = sbf_validate(header, SBF_HEADER_SIZE);
    if (v != 0) { fclose(f); return v; }
    memcpy(&arc->header, header, SBF_HEADER_SIZE);
    snprintf(arc->_path, sizeof(arc->_path), "%s", path);

    uint32_t n = arc->header.entry_count;
    if (n > 0) {
        arc->entries = (SbfRawEntry *)malloc((size_t)n * SBF_ENTRY_SIZE);
        if (!arc->entries) { fclose(f); return -5; }
        if (fread(arc->entries, SBF_ENTRY_SIZE, n, f) != n) {
            free(arc->entries); arc->entries = NULL; fclose(f); return -12;
        }
    }
    arc->_file = f;
    return 0;
}

/* Witnessed: jointops!Sbf_OpenFile_Gamemus @ 0x004ED6C0 (header + entry-loop reads).
   Memory-mode counterpart: ingests header + index from a caller-provided buffer. */
extern "C" int sbf_open_memory(SbfArchive *arc, const uint8_t *data, size_t size) {
    if (!arc || !data) return -1;
    memset(arc, 0, sizeof(*arc));

    int v = sbf_validate(data, size);
    if (v != 0) return v;
    memcpy(&arc->header, data, SBF_HEADER_SIZE);

    uint32_t n = arc->header.entry_count;
    if (n > 0) {
        arc->entries = (SbfRawEntry *)malloc((size_t)n * SBF_ENTRY_SIZE);
        if (!arc->entries) return -5;
        memcpy(arc->entries, data + SBF_HEADER_SIZE, (size_t)n * SBF_ENTRY_SIZE);
    }
    arc->_file = NULL;
    return 0;
}

extern "C" void sbf_close(SbfArchive *arc) {
    if (!arc) return;
    free(arc->entries);
    arc->entries = NULL;
    if (arc->_file) {
        fclose((FILE *)arc->_file);
        arc->_file = NULL;
    }
}

/* Case-insensitive null-padded compare across SBF_NAME_SIZE. */
static int sbf_iequals_name(const char *entry_name, const char *search) {
    for (size_t i = 0; i < SBF_NAME_SIZE; ++i) {
        unsigned char a = (unsigned char)entry_name[i];
        unsigned char b = (unsigned char)search[i];
        if (a == 0) return b == 0 ? 1 : 0;
        if (b == 0) return 0;
        if (tolower(a) != tolower(b)) return 0;
    }
    return search[SBF_NAME_SIZE] == 0 ? 1 : 0;
}

extern "C" const SbfRawEntry *sbf_find_by_name(const SbfArchive *arc, const char *name) {
    if (!arc || !name || !arc->entries) return NULL;
    for (uint32_t i = 0; i < arc->header.entry_count; ++i) {
        if (sbf_iequals_name(arc->entries[i].name, name)) {
            return &arc->entries[i];
        }
    }
    return NULL;
}

extern "C" const SbfRawEntry *sbf_find_by_index(const SbfArchive *arc, uint32_t index) {
    if (!arc || !arc->entries) return NULL;
    if (index >= arc->header.entry_count) return NULL;
    return &arc->entries[index];
}

/* Witnessed: jointops!Sbf_StartEntry @ 0x004ED910 (SetFilePointer to data_offset)
   + jointops!Audio_StreamNextChunk @ 0x004ED7D0 (per-chunk ReadFile).
   This function reads the entry's full audio range in one shot. */
extern "C" int sbf_read_raw(const SbfArchive *arc, const SbfRawEntry *entry,
                             uint8_t *out_buf, size_t buf_size) {
    if (!arc || !entry || !out_buf) return -1;
    if (buf_size < entry->total_size) return -2;
    if (!arc->_file) return -3;
    FILE *f = (FILE *)arc->_file;
    if (fseek(f, (long)entry->data_offset, SEEK_SET) != 0) return -4;
    if (fread(out_buf, 1, entry->total_size, f) != entry->total_size) return -5;
    return (int)entry->total_size;
}

/* Witnessed: jointops!Audio_SubmitStereoSampleSplit inner loop @ 0x007BD252.
   Bytes are byte-paired stereo: even byte (i&1==0) is the LEFT channel sample
   shifted by scale_a; odd byte is RIGHT shifted by scale_b. valid_samples is
   the count of audio BYTES to consume (and matches the int16 output count). */
extern "C" int sbf_decode_chunk(const uint8_t *chunk_bytes, size_t chunk_size,
                                 int16_t *out_samples, size_t out_capacity) {
    if (!chunk_bytes || !out_samples) return -1;
    if (chunk_size < SBF_CHUNK_HEADER) return -2;

    SbfChunkHeader h;
    memcpy(&h, chunk_bytes, SBF_CHUNK_HEADER);
    if (h.scale_a > 7 || h.scale_b > 7) {
        return -4;
    }

    uint32_t want = h.valid_samples;
    if (want > chunk_size - SBF_CHUNK_HEADER) want = (uint32_t)(chunk_size - SBF_CHUNK_HEADER);
    if (want > out_capacity) return -3;

    const uint8_t *audio = chunk_bytes + SBF_CHUNK_HEADER;
    for (uint32_t i = 0; i < want; ++i) {
        uint8_t scale = (i & 1) ? h.scale_b : h.scale_a;
        out_samples[i] = sbf_decode_sample(audio[i], scale);
    }
    return (int)want;
}

/* Iterate chunks across a raw entry buffer. Walks SBF_CHUNK_TOTAL strides;
   short trailing chunk (e.g. JO NULLS' single 0x1008 block) decodes naturally
   via sbf_decode_chunk's valid_samples honor. */
extern "C" int sbf_decode_all(const uint8_t *raw, size_t raw_size,
                               int16_t *out, size_t cap) {
    if (!raw || !out) return -1;
    size_t off = 0, produced = 0;
    while (off + SBF_CHUNK_HEADER <= raw_size) {
        size_t remaining = raw_size - off;
        size_t this_chunk = remaining < SBF_CHUNK_TOTAL ? remaining : SBF_CHUNK_TOTAL;
        int n = sbf_decode_chunk(raw + off, this_chunk, out + produced, cap - produced);
        if (n < 0) return n;
        produced += (size_t)n;
        off += this_chunk;
    }
    return (int)produced;
}

/* Witnessed: jointops!Audio_StreamNextChunk @ 0x004ED7D0 (per-chunk ReadFile of
   block_size bytes). Hot path for streaming AudioStreamPlayback. */
extern "C" int sbf_read_chunk(const SbfArchive *arc, const SbfRawEntry *entry,
                               uint32_t chunk_index, uint8_t *out_buf,
                               size_t buf_size) {
    if (!arc || !entry || !out_buf) return -1;
    if (buf_size < entry->block_size) return -2;
    if (!arc->_file) return -3;
    uint64_t chunk_off = (uint64_t)chunk_index * entry->block_size;
    if (chunk_off >= entry->total_size) return -4;
    uint64_t off = (uint64_t)entry->data_offset + chunk_off;
    FILE *f = (FILE *)arc->_file;
    if (fseek(f, (long)off, SEEK_SET) != 0) return -5;
    size_t got = fread(out_buf, 1, entry->block_size, f);
    if (got != entry->block_size) return -6;
    return (int)entry->block_size;
}

/* --- Encoder ---

   No engine equivalent: original game shipped pre-encoded SBFs and never wrote
   them at runtime. Encoder math is the algebraic inverse of sbf_decode_sample
   so a chunk produced here decodes back through the witnessed reader within
   one int8 quantum. */

/* No engine equivalent: original game shipped pre-encoded SBFs.
   Iterates 7 -> 0 because larger scale = more precision but clips sooner;
   we want the largest scale where the chunk's loudest sample still fits. */
extern "C" uint8_t sbf_pick_scale(const int16_t *s, size_t count) {
    if (!s || count == 0) return 0;
    int32_t max_abs = 0;
    for (size_t i = 0; i < count; ++i) {
        int32_t a = s[i] < 0 ? -(int32_t)s[i] : (int32_t)s[i];
        if (a > max_abs) max_abs = a;
    }
    for (int scale = 7; scale >= 0; --scale) {
        int32_t enc = ((max_abs << 1) << scale) / 256;
        if (enc <= 127) return (uint8_t)scale;
    }
    return 0;
}

/* No engine equivalent: original game shipped pre-encoded SBFs.
   Mirror of sbf_decode_chunk: lay down the 8-byte header (valid_samples,
   scale pair, reserved constants), then encode each int16 to one offset-
   binary u8 byte; pad the rest with 0x80 (decodes to silence). */
extern "C" int sbf_encode_chunk(const int16_t *samples, size_t sample_count,
                                 uint8_t *out_buf, size_t out_capacity) {
    if (!samples || !out_buf) return -1;
    if (sample_count > SBF_CHUNK_AUDIO) return -2;
    if (out_capacity < SBF_CHUNK_TOTAL) return -3;

    uint8_t scale = sbf_pick_scale(samples, sample_count);
    SbfChunkHeader h = {};
    h.valid_samples = (uint32_t)sample_count;
    h.scale_a = scale;
    h.scale_b = scale;
    h.reserved_a = 0xFA;
    h.reserved_b = 0x00;
    memcpy(out_buf, &h, SBF_CHUNK_HEADER);

    uint8_t *audio_out = out_buf + SBF_CHUNK_HEADER;
    for (size_t i = 0; i < sample_count; ++i) {
        audio_out[i] = sbf_encode_sample(samples[i], scale);
    }
    for (size_t i = sample_count; i < SBF_CHUNK_AUDIO; ++i) {
        audio_out[i] = 0x80;
    }
    return SBF_CHUNK_TOTAL;
}

/* No engine equivalent: a thin wrapper so callers freeing buffers from
   sbf_encode_file don't reach into the lib's allocator directly. */
extern "C" void sbf_free(void *p) {
    free(p);
}

/* No engine equivalent: original game shipped pre-encoded SBFs.
   Builds the full header + index + audio image in one malloc'd buffer.
   Header.flags = 1 selects the byte-paired stereo path the decoder accepts;
   block_size mirrors the SBF_CHUNK_TOTAL stride that every observed file
   uses. */
extern "C" int sbf_encode_file(const char * const *names, uint32_t n,
                                const int16_t * const *pcm,
                                const size_t *counts,
                                uint8_t **out_buf, size_t *out_size) {
    if (!out_buf || !out_size) return -1;
    /* A zero-entry bank is a valid 24-byte header-only file; the per-entry
       input arrays are unused in that case, so don't require them (an empty
       bank authored from scratch and saved before any track is added). */
    if (n != 0 && (!names || !pcm || !counts)) return -1;

    typedef struct { uint8_t *bytes; size_t size; } Buf;
    Buf *bufs = (Buf *)calloc(n ? n : 1, sizeof(Buf));
    if (!bufs) return -2;

    for (uint32_t i = 0; i < n; ++i) {
        size_t samples = counts[i];
        size_t chunks = (samples + SBF_CHUNK_AUDIO - 1) / SBF_CHUNK_AUDIO;
        if (chunks == 0) chunks = 1;
        bufs[i].size = chunks * SBF_CHUNK_TOTAL;
        bufs[i].bytes = (uint8_t *)malloc(bufs[i].size);
        if (!bufs[i].bytes) {
            for (uint32_t j = 0; j < i; ++j) free(bufs[j].bytes);
            free(bufs);
            return -3;
        }

        for (size_t c = 0; c < chunks; ++c) {
            size_t off = c * SBF_CHUNK_AUDIO;
            size_t this_samples = samples > off ? samples - off : 0;
            if (this_samples > SBF_CHUNK_AUDIO) this_samples = SBF_CHUNK_AUDIO;
            sbf_encode_chunk(pcm[i] + off, this_samples,
                             bufs[i].bytes + c * SBF_CHUNK_TOTAL, SBF_CHUNK_TOTAL);
        }
    }

    size_t total_audio = 0;
    for (uint32_t i = 0; i < n; ++i) total_audio += bufs[i].size;
    size_t total = SBF_HEADER_SIZE + (size_t)n * SBF_ENTRY_SIZE + total_audio;
    uint8_t *out = (uint8_t *)malloc(total);
    if (!out) {
        for (uint32_t i = 0; i < n; ++i) free(bufs[i].bytes);
        free(bufs);
        return -4;
    }

    SbfHeader hdr = {};
    hdr.magic = SBF_MAGIC;
    hdr.version = 0x00000100;
    hdr.flags = 0x00000001;
    hdr.reserved = 0;
    hdr.index_offset = SBF_HEADER_SIZE;
    hdr.entry_count = n;
    memcpy(out, &hdr, SBF_HEADER_SIZE);

    size_t cursor = SBF_HEADER_SIZE + (size_t)n * SBF_ENTRY_SIZE;
    for (uint32_t i = 0; i < n; ++i) {
        SbfRawEntry e = {};
        size_t name_len = strlen(names[i]);
        if (name_len > SBF_NAME_SIZE - 1) name_len = SBF_NAME_SIZE - 1;
        memcpy(e.name, names[i], name_len);
        e.data_offset   = (uint32_t)cursor;
        e.total_size    = (uint32_t)bufs[i].size;
        e.block_size    = SBF_CHUNK_TOTAL;
        e.sample_length_hint = 0;
        memcpy(out + SBF_HEADER_SIZE + (size_t)i * SBF_ENTRY_SIZE, &e, SBF_ENTRY_SIZE);
        memcpy(out + cursor, bufs[i].bytes, bufs[i].size);
        cursor += bufs[i].size;
        free(bufs[i].bytes);
    }
    free(bufs);

    *out_buf = out;
    *out_size = total;
    return 0;
}
