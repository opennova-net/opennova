/* SBF audio bank reader implementation.

   [orig: Sbf_OpenFile_Gamemus @ 0x4ED6C0, AudioVM_OpenContextFile @ 0x672160, Sbf_StartEntry @ 0x4ED910, Audio_StreamNextChunk @ 0x4ED7D0]
   Header parser: jointops!Sbf_OpenFile_Gamemus @ 0x004ED6C0
                  jointops!AudioVM_OpenContextFile @ 0x00672160
   Per-entry stream init: jointops!Sbf_StartEntry @ 0x004ED910
   Chunk decode: jointops!Audio_StreamNextChunk @ 0x004ED7D0 +
                 jointops!AudioChannel_ComputeMixCoefficients @ 0x007BD4B0 */

#include <formats/sbf/sbf.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <utility>
#include <vector>

namespace opennova::sbf {

/* Witnessed: jointops!Sbf_OpenFile_Gamemus @ 0x004ED6C0 (magic + flags check).
   The bounds-vs-size check only fires when caller gave us more than just the
   header; passing exactly SBF_HEADER_SIZE bytes does header-only validation
   (used by sbf_open before it has read the index off disk). */
int sbf_validate(const uint8_t *data, size_t size) {
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
int sbf_open(SbfArchive *arc, const char *path) {
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
int sbf_open_memory(SbfArchive *arc, const uint8_t *data, size_t size) {
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

void sbf_close(SbfArchive *arc) {
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

const SbfRawEntry *sbf_find_by_name(const SbfArchive *arc, const char *name) {
    if (!arc || !name || !arc->entries) return NULL;
    for (uint32_t i = 0; i < arc->header.entry_count; ++i) {
        if (sbf_iequals_name(arc->entries[i].name, name)) {
            return &arc->entries[i];
        }
    }
    return NULL;
}

const SbfRawEntry *sbf_find_by_index(const SbfArchive *arc, uint32_t index) {
    if (!arc || !arc->entries) return NULL;
    if (index >= arc->header.entry_count) return NULL;
    return &arc->entries[index];
}

/* Witnessed: jointops!Sbf_StartEntry @ 0x004ED910 (SetFilePointer to data_offset)
   + jointops!Audio_StreamNextChunk @ 0x004ED7D0 (per-chunk ReadFile).
   This function reads the entry's full audio range in one shot. */
int sbf_read_raw(const SbfArchive *arc, const SbfRawEntry *entry,
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
int sbf_decode_chunk(const uint8_t *chunk_bytes, size_t chunk_size,
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
int sbf_decode_all(const uint8_t *raw, size_t raw_size,
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
int sbf_read_chunk(const SbfArchive *arc, const SbfRawEntry *entry,
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
uint8_t sbf_pick_scale(const int16_t *s, size_t count) {
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
int sbf_encode_chunk(const int16_t *samples, size_t sample_count,
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
void sbf_free(void *p) {
    free(p);
}

/* No engine equivalent: original game shipped pre-encoded SBFs.
   Each entry a stream of its PCM (sbf_encode_stream), the bank written by
   sbf_write_bank: the header at the default version, flags = 1 (the
   byte-paired stereo path the decoder accepts), block_size the
   SBF_CHUNK_TOTAL stride every observed file uses, the chunks' reserved
   pair gamemus.sbf's and their tails 0x80, as sbf_encode_chunk lays a
   chunk out. */
int sbf_encode_file(const char * const *names, uint32_t n,
                                const int16_t * const *pcm,
                                const size_t *counts,
                                uint8_t **out_buf, size_t *out_size) {
    if (!out_buf || !out_size) return -1;
    /* A zero-entry bank is a valid 24-byte header-only file; the per-entry
       input arrays are unused in that case, so don't require them (an empty
       bank authored from scratch and saved before any track is added). */
    if (n != 0 && (!names || !pcm || !counts)) return -1;

    SbfFile bank;
    bank.tail = SbfTail::Silence;
    bank.streams.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        std::string name(names[i]);
        if (name.size() > SBF_STREAM_NAME_MAX) name.resize(SBF_STREAM_NAME_MAX);
        bank.streams.push_back(sbf_encode_stream(name, pcm[i], counts[i]));
    }
    std::vector<uint8_t> bytes;
    std::string error;
    if (!sbf_write_bank(bank, bytes, error)) return -2;
    uint8_t *out = (uint8_t *)malloc(bytes.empty() ? 1 : bytes.size());
    if (!out) return -4;
    memcpy(out, bytes.data(), bytes.size());
    *out_buf = out;
    *out_size = bytes.size();
    return 0;
}

/* --- The bank as a model --- */

namespace {

/* The bytes a chunk's audio area holds past its valid ones under the bank's
   tail rule: the previous chunk's area at those offsets (zeros under a
   stream's first chunk), or 0x80. `previous` is the stream's previous chunk's
   whole area, empty for its first. */
void append_tail(SbfTail tail, const std::vector<uint8_t> &previous, size_t from, size_t area,
                 std::vector<uint8_t> &out) {
    for (size_t i = from; i < area; ++i)
        out.push_back(tail == SbfTail::Silence ? (uint8_t)0x80 : i < previous.size() ? previous[i] : (uint8_t)0);
}

} // namespace

bool sbf_read_bank(const uint8_t *data, size_t size, SbfFile &out, std::string &error,
                   SbfFileLayout *layout) {
    error.clear();
    const int valid = sbf_validate(data, size);
    if (valid != 0) {
        error = valid == -2 ? "not an SBF0 bank" : valid == -5 ? "its flags word is above 2, which the engine refuses"
              : valid == -3 ? "its index is not at byte 24" : "its header or its index is cut short";
        return false;
    }
    SbfFile bank;
    SbfHeader header;
    memcpy(&header, data, SBF_HEADER_SIZE);
    bank.version = header.version;
    bank.flags = header.flags;
    bank.reserved = header.reserved;
    const size_t index_end = SBF_HEADER_SIZE + (size_t)header.entry_count * SBF_ENTRY_SIZE;
    std::vector<SbfRawEntry> entries(header.entry_count);
    uint64_t streamed = 0;
    for (uint32_t i = 0; i < header.entry_count; ++i) {
        SbfRawEntry &entry = entries[i];
        memcpy(&entry, data + SBF_HEADER_SIZE + (size_t)i * SBF_ENTRY_SIZE, SBF_ENTRY_SIZE);
        const std::string at = "entry " + std::to_string(i + 1);
        if ((uint64_t)entry.data_offset + entry.total_size > size) {
            error = at + "'s audio runs past the end of the file";
            return false;
        }
        if (entry.block_size < (uint32_t)SBF_CHUNK_HEADER) {
            error = at + "'s block size is under a chunk's 8-byte header";
            return false;
        }
        if (entry.total_size % entry.block_size != 0 &&
            entry.total_size % entry.block_size < (uint32_t)SBF_CHUNK_HEADER) {
            error = at + "'s last chunk is shorter than its 8-byte header";
            return false;
        }
        streamed += entry.total_size;
    }
    /* Streams that share bytes: the packed writer could only copy them, and a
       few kilobytes of entries over one range would be gigabytes of chunks. */
    if (streamed > (uint64_t)(size - index_end)) {
        error = "its streams' sizes add up past the file: entries share bytes";
        return false;
    }

    /* Each stream's chunks, every chunk's reserved pair and tail checked
       against the bank's: the reserved pair its first chunk's, the tail rule
       the one more of its tails follow (a tie Residue, the shipped banks'). */
    SbfFileLayout found;
    size_t expected = index_end;
    bool have_pair = false;
    size_t residue_tails = 0, silence_tails = 0, tails = 0;
    bank.streams.reserve(header.entry_count);
    for (uint32_t i = 0; i < header.entry_count; ++i) {
        const SbfRawEntry &entry = entries[i];
        SbfStream stream;
        size_t length = 0;
        while (length < (size_t)SBF_NAME_SIZE && entry.name[length]) ++length;
        stream.name.assign(entry.name, length);
        for (size_t k = length; k < (size_t)SBF_NAME_SIZE; ++k)
            if (entry.name[k]) {
                ++found.names_with_tails;
                break;
            }
        stream.block_size = entry.block_size;
        stream.sample_length_hint = entry.sample_length_hint;
        const uint8_t *audio = data + entry.data_offset;
        const size_t area = entry.block_size - SBF_CHUNK_HEADER;
        const uint8_t *previous = NULL;
        size_t previous_size = 0;
        for (uint32_t offset = 0; offset < entry.total_size; offset += entry.block_size) {
            const uint32_t left = entry.total_size - offset;
            const size_t held = (left < entry.block_size ? left : entry.block_size) - SBF_CHUNK_HEADER;
            const uint8_t *bytes = audio + offset + SBF_CHUNK_HEADER;
            SbfChunkHeader h;
            memcpy(&h, audio + offset, SBF_CHUNK_HEADER);
            if (!have_pair) {
                bank.chunk_reserved_a = h.reserved_a;
                bank.chunk_reserved_b = h.reserved_b;
                have_pair = true;
            } else if (h.reserved_a != bank.chunk_reserved_a || h.reserved_b != bank.chunk_reserved_b) {
                ++found.reserved_other;
            }
            const size_t plays = h.valid_samples < held ? h.valid_samples : held;
            if (held < area || h.valid_samples > held) ++found.short_chunks;
            if (plays < held) {
                ++tails;
                bool residue = true, silence = true;
                for (size_t k = plays; k < held && (residue || silence); ++k) {
                    residue = residue && bytes[k] == (k < previous_size ? previous[k] : 0);
                    silence = silence && bytes[k] == 0x80;
                }
                residue_tails += residue ? 1 : 0;
                silence_tails += silence ? 1 : 0;
            }
            SbfChunk chunk;
            chunk.scale_a = h.scale_a;
            chunk.scale_b = h.scale_b;
            chunk.audio.assign(bytes, bytes + plays);
            stream.chunks.push_back(std::move(chunk));
            previous = bytes;
            previous_size = held;
        }
        if (entry.data_offset != expected) found.packed = false;
        expected = (size_t)entry.data_offset + entry.total_size;
        bank.streams.push_back(std::move(stream));
    }
    bank.tail = silence_tails > residue_tails ? SbfTail::Silence : SbfTail::Residue;
    found.tails_other = tails - (bank.tail == SbfTail::Silence ? silence_tails : residue_tails);
    if (found.packed && expected < size) found.trailing_bytes = size - expected;
    out = std::move(bank);
    if (layout) *layout = found;
    return true;
}

bool sbf_write_bank(const SbfFile &bank, std::vector<uint8_t> &out, std::string &error) {
    error.clear();
    out.clear();
    if (bank.flags > 2) {
        error = "the flags word is above 2, which the engine refuses";
        return false;
    }
    uint64_t total = SBF_HEADER_SIZE + (uint64_t)bank.streams.size() * SBF_ENTRY_SIZE;
    for (size_t i = 0; i < bank.streams.size(); ++i) {
        const SbfStream &stream = bank.streams[i];
        const std::string at = stream.name.empty() ? "stream " + std::to_string(i + 1) : stream.name;
        if (stream.name.size() > SBF_STREAM_NAME_MAX) {
            error = at + "'s name is longer than " + std::to_string(SBF_STREAM_NAME_MAX) + " characters";
            return false;
        }
        if (stream.name.find('\0') != std::string::npos) {
            error = "stream " + std::to_string(i + 1) + "'s name holds a NUL, which ends a name in the index";
            return false;
        }
        if (stream.block_size < (uint32_t)SBF_CHUNK_HEADER) {
            error = at + "'s block size is under a chunk's 8-byte header";
            return false;
        }
        const size_t area = stream.block_size - SBF_CHUNK_HEADER;
        for (size_t c = 0; c < stream.chunks.size(); ++c) {
            const size_t held = stream.chunks[c].audio.size();
            if (held > area) {
                error = at + "'s chunk " + std::to_string(c + 1) + " holds " + std::to_string(held) +
                        " audio bytes where its block holds " + std::to_string(area);
                return false;
            }
        }
        total += (uint64_t)stream.block_size * stream.chunks.size();
    }
    if (total > UINT32_MAX) {
        error = "the bank is past the 4 GiB its offsets reach";
        return false;
    }
    out.reserve((size_t)total);
    const auto word = [&](uint32_t value) {
        for (int b = 0; b < 4; ++b) out.push_back((uint8_t)(value >> (8 * b)));
    };
    word(SBF_MAGIC);
    word(bank.version);
    word(bank.flags);
    word(bank.reserved);
    word((uint32_t)SBF_HEADER_SIZE);
    word((uint32_t)bank.streams.size());
    uint32_t cursor = (uint32_t)(SBF_HEADER_SIZE + bank.streams.size() * SBF_ENTRY_SIZE);
    for (const SbfStream &stream : bank.streams) {
        char name[SBF_NAME_SIZE] = {};
        memcpy(name, stream.name.data(), stream.name.size());
        out.insert(out.end(), name, name + SBF_NAME_SIZE);
        const uint32_t size = stream.block_size * (uint32_t)stream.chunks.size();
        word(cursor);
        word(size);
        word(stream.block_size);
        word(stream.sample_length_hint);
        cursor += size;
    }
    /* Every chunk a whole block: its header (the valid count, the shifts, the
       bank's reserved pair), the bytes it plays, then its tail by the bank's
       rule. */
    std::vector<uint8_t> previous;
    for (const SbfStream &stream : bank.streams) {
        const size_t area = stream.block_size - SBF_CHUNK_HEADER;
        previous.clear();
        for (const SbfChunk &chunk : stream.chunks) {
            word((uint32_t)chunk.audio.size());
            out.push_back(chunk.scale_a);
            out.push_back(chunk.scale_b);
            out.push_back(bank.chunk_reserved_a);
            out.push_back(bank.chunk_reserved_b);
            const size_t start = out.size();
            out.insert(out.end(), chunk.audio.begin(), chunk.audio.end());
            append_tail(bank.tail, previous, chunk.audio.size(), area, out);
            previous.assign(out.begin() + (ptrdiff_t)start, out.end());
        }
    }
    return true;
}

std::vector<int16_t> sbf_decode_stream(const SbfStream &stream) {
    std::vector<int16_t> out;
    for (const SbfChunk &chunk : stream.chunks) {
        /* A shift past 7 stops the stream at its chunk, as the Godot player
           does (sbf_decode_chunk refuses the chunk; what the original's mixer
           does with such a shift is not witnessed, and no shipped chunk has
           one). */
        if (chunk.scale_a > 7 || chunk.scale_b > 7) break;
        for (size_t i = 0; i < chunk.audio.size(); ++i)
            out.push_back(sbf_decode_sample(chunk.audio[i], (i & 1) ? chunk.scale_b : chunk.scale_a));
    }
    return out;
}

SbfStream sbf_encode_stream(const std::string &name, const int16_t *pcm, size_t count) {
    SbfStream stream;
    stream.name = name;
    size_t chunks = (count + SBF_CHUNK_AUDIO - 1) / SBF_CHUNK_AUDIO;
    if (chunks == 0) chunks = 1;
    for (size_t c = 0; c < chunks; ++c) {
        const size_t off = c * SBF_CHUNK_AUDIO;
        size_t these = count > off ? count - off : 0;
        if (these > (size_t)SBF_CHUNK_AUDIO) these = SBF_CHUNK_AUDIO;
        SbfChunk chunk;
        const uint8_t scale = these ? sbf_pick_scale(pcm + off, these) : 0;
        chunk.scale_a = scale;
        chunk.scale_b = scale;
        chunk.audio.resize(these);
        for (size_t i = 0; i < these; ++i) chunk.audio[i] = sbf_encode_sample(pcm[off + i], scale);
        stream.chunks.push_back(std::move(chunk));
    }
    return stream;
}

} // namespace opennova::sbf
