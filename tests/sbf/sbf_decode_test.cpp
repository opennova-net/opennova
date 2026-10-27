/* SBF decode: the sample/chunk/entry decoders over the synthetic bank
   fixtures/sbf/synth_gamemus.sbf (SILENCE = one partial chunk of 0x08A8
   samples, TONE01 = 4096 + 4096 + 1000 samples over three chunks). */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <formats/sbf/sbf.h>

#include <vector>

#include "sbf/sbf_bank_file.h"
#include "sbf/sbf_streaming_reference.h"

using namespace opennova::sbf;

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

#ifndef SBF_FIXTURE_DIR
#define SBF_FIXTURE_DIR "fixtures/sbf"
#endif
#define SYNTH_BANK SBF_FIXTURE_DIR "/synth_gamemus.sbf"

static int test_decode_sample_silence(void) {
    /* 0x80 (128) is centred, should decode to 0 at every scale */
    for (uint8_t scale = 0; scale <= 7; ++scale) {
        CHECK(sbf_decode_sample(0x80, scale) == 0, "0x80 -> 0");
    }
    return 1;
}

/* The 8-bit buffer the stereo split makes of a chunk, at the mixer's 16-bit
   scale: ((byte - 128) * 256) >> shift, as an 8-bit AUD1 wave of the same
   bytes decodes (C-162) [orig: Audio_SubmitStereoSampleSplit @ 0x7BD205;
   AudioChannel_BindSampleBuffer @ 0x7BD6A1]. */
static int test_decode_sample_extremes(void) {
    /* scale 0: byte 0xFF -> 127 * 256 = 32512, the loudest a stream plays */
    CHECK(sbf_decode_sample(0xFF, 0) == 32512, "0xFF scale 0");
    /* scale 0: byte 0x00 -> -128 * 256 = -32768 */
    CHECK(sbf_decode_sample(0x00, 0) == -32768, "0x00 scale 0");
    /* scale 7: byte 0xFF -> (127 * 256) >> 7 = 254 */
    CHECK(sbf_decode_sample(0xFF, 7) == 254, "0xFF scale 7");
    CHECK(sbf_decode_sample(0x00, 7) == -256, "0x00 scale 7");
    /* an 8-bit AUD1 sample's 16-bit value at the same shift */
    CHECK(sbf_decode_sample(0x81, 3) == (1 * 256) >> 3, "0x81 scale 3");
    return 1;
}

/* Any shift byte plays: the mixer shifts the channel's coefficients by it,
   any count, so past 7 a chunk plays quieter and from 15 on silent [orig:
   AudioChannel_ComputeMixCoefficients @ 0x7BD5E6..0x7BD5F3, psraw]. */
static int test_decode_chunk_takes_any_shift(void) {
    uint8_t chunk[SBF_CHUNK_HEADER + 2] = {0};
    int16_t decoded[2] = {0};
    SbfChunkHeader h = {};
    h.valid_samples = 2;
    chunk[SBF_CHUNK_HEADER] = 0x00;
    chunk[SBF_CHUNK_HEADER + 1] = 0xFF;

    h.scale_a = 8;
    h.scale_b = 14;
    memcpy(chunk, &h, SBF_CHUNK_HEADER);
    CHECK(sbf_decode_chunk(chunk, sizeof(chunk), decoded, 2) == 2,
          "a chunk with shifts past 7 decodes");
    CHECK(decoded[0] == -128 && decoded[1] == 1, "shift 8 and 14");

    h.scale_a = 15;
    h.scale_b = 255;
    memcpy(chunk, &h, SBF_CHUNK_HEADER);
    CHECK(sbf_decode_chunk(chunk, sizeof(chunk), decoded, 2) == 2,
          "a chunk with shifts of 15 and more decodes");
    CHECK(decoded[0] == 0 && decoded[1] == 0, "from 15 on silent");
    return 1;
}

static int test_decode_chunk_tone01(void) {
    SbfBankFile bank;
    SbfArchive &arc = bank.arc;
    CHECK(bank.open(SYNTH_BANK) == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "TONE01");
    CHECK(e != NULL, "TONE01 found");
    const uint8_t *chunk = bank.chunk(e, 0);
    CHECK(chunk != NULL && e->block_size == 0x1008, "the first chunk lies in the entry");

    int16_t decoded[SBF_CHUNK_AUDIO];
    int n = sbf_decode_chunk(chunk, e->block_size, decoded, SBF_CHUNK_AUDIO);
    CHECK(n == 0x1000, "full chunk decoded to 4096 int16 samples");

    /* Hand-verify the first audio byte using sbf_decode_sample with scale_a. */
    SbfChunkHeader h;
    memcpy(&h, chunk, SBF_CHUNK_HEADER);
    int16_t expected_first = sbf_decode_sample(chunk[SBF_CHUNK_HEADER], h.scale_a);
    CHECK(decoded[0] == expected_first, "first sample matches hand decode");

    /* A tone chunk is not silence. */
    int nonzero = 0;
    for (int i = 0; i < n; ++i) if (decoded[i] != 0) ++nonzero;
    CHECK(nonzero > 0, "tone chunk has non-silent samples");

    sbf_close(&arc);
    return 1;
}

static int test_decode_chunk_silence_partial(void) {
    SbfBankFile bank;
    SbfArchive &arc = bank.arc;
    CHECK(bank.open(SYNTH_BANK) == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "SILENCE");
    CHECK(e != NULL, "SILENCE found");
    const uint8_t *chunk = bank.chunk(e, 0);
    CHECK(chunk != NULL && e->block_size == 0x1008, "the chunk lies in the entry");

    int16_t decoded[SBF_CHUNK_AUDIO];
    int n = sbf_decode_chunk(chunk, e->block_size, decoded, SBF_CHUNK_AUDIO);
    CHECK(n == 0x08A8, "partial chunk: 2216 int16 samples");
    for (int i = 0; i < n; ++i) CHECK(decoded[i] == 0, "silence decodes to zero");

    sbf_close(&arc);
    return 1;
}

static int test_decode_all_silence(void) {
    SbfBankFile bank;
    SbfArchive &arc = bank.arc;
    CHECK(bank.open(SYNTH_BANK) == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "SILENCE");
    CHECK(e != NULL, "SILENCE found");
    const uint8_t *raw = bank.entry_bytes(e);
    CHECK(raw != NULL, "the entry's bytes lie in the bank");

    int16_t *out = (int16_t *)malloc(e->total_size * sizeof(int16_t));
    CHECK(out != NULL, "out alloc");
    int n = sbf_decode_all(raw, e->total_size, out, e->total_size);
    CHECK(n == 0x08A8, "SILENCE decodes to 2216 samples (single partial chunk)");

    free(out);
    sbf_close(&arc);
    return 1;
}

static int test_decode_all_tone01_multichunk(void) {
    SbfBankFile bank;
    SbfArchive &arc = bank.arc;
    CHECK(bank.open(SYNTH_BANK) == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "TONE01");
    CHECK(e != NULL, "TONE01 found");
    const uint8_t *raw = bank.entry_bytes(e);
    CHECK(raw != NULL, "the entry's bytes lie in the bank");

    int16_t *out = (int16_t *)malloc(e->total_size * sizeof(int16_t));
    CHECK(out != NULL, "out alloc");
    int n = sbf_decode_all(raw, e->total_size, out, e->total_size);
    /* Bounds: all-but-last chunks are full; the last is partial. */
    uint32_t chunks = e->total_size / e->block_size;
    CHECK(chunks == 3, "three chunks");
    int min_samples = (int)((chunks - 1) * SBF_CHUNK_AUDIO + 1);
    int max_samples = (int)(chunks * SBF_CHUNK_AUDIO);
    CHECK(n >= min_samples && n <= max_samples,
          "decoded count is bounded by chunk count");
    CHECK(n == 4096 + 4096 + 1000, "TONE01 decodes to its 9192 authored samples");

    free(out);
    sbf_close(&arc);
    return 1;
}

/* The entry decode the Godot player hands the mixer (C-164) is sample for sample
   the PCM the deleted streaming playback produced, for every entry of the
   synthetic bank (each full chunk, then its one short last chunk). */
static int test_decode_entry_matches_streaming(void) {
    FILE *f = fopen(SYNTH_BANK, "rb");
    CHECK(f != NULL, "open the fixture");
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> bytes((size_t)size);
    const size_t got = fread(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    CHECK(got == bytes.size(), "read the fixture whole");
    SbfArchive arc;
    CHECK(sbf_open_memory(&arc, bytes.data(), bytes.size()) == 0, "open memory");
    for (uint32_t i = 0; i < arc.header.entry_count; ++i) {
        const SbfRawEntry &e = arc.entries[i];
        const std::vector<int16_t> entry =
                sbf_decode_entry(bytes.data() + e.data_offset, e.total_size, e.block_size);
        CHECK(!entry.empty(), "every entry decodes to samples");
        CHECK(entry == sbf_streaming_reference(bytes.data() + e.data_offset, e.total_size, e.block_size),
              "the entry decode is the streaming playback's PCM");
    }
    const SbfRawEntry *tone01 = sbf_find_by_name(&arc, "TONE01");
    CHECK(tone01 != NULL &&
                  sbf_decode_entry(bytes.data() + tone01->data_offset, tone01->total_size, tone01->block_size).size() ==
                          4096 + 4096 + 1000,
          "TONE01's three chunks");
    sbf_close(&arc);
    return 1;
}

/* Chunk by chunk to the entry's end, each chunk's whole frames, a chunk of under
   4 frames queueing nothing: an odd count's last byte is not played, and neither
   it nor a short chunk ends the stream (the split counts frames as count >> 1 and
   returns before any store under 4 frames or over 2048; the pump reads the next
   block until the entry's bytes run out) [orig: Audio_SubmitStereoSampleSplit
   @ 0x7BD222..0x7BD233; AudioStream_Update @ 0x671D61]. */
static int test_decode_entry_walks_every_chunk(void) {
    const uint32_t block = SBF_CHUNK_HEADER + 10;
    uint8_t entry[3 * (SBF_CHUNK_HEADER + 10)] = {0};
    SbfChunkHeader h = {};
    h.valid_samples = 9; /* 4 frames and an odd byte */
    memcpy(entry, &h, SBF_CHUNK_HEADER);
    for (int i = 0; i < 10; ++i) entry[SBF_CHUNK_HEADER + i] = (uint8_t)(0x81 + i);
    h.valid_samples = 6; /* 3 frames: the split queues nothing */
    memcpy(entry + block, &h, SBF_CHUNK_HEADER);
    for (int i = 0; i < 10; ++i) entry[block + SBF_CHUNK_HEADER + i] = (uint8_t)(0xA1 + i);
    h.valid_samples = 8; /* 4 frames */
    memcpy(entry + 2 * block, &h, SBF_CHUNK_HEADER);
    for (int i = 0; i < 10; ++i) entry[2 * block + SBF_CHUNK_HEADER + i] = (uint8_t)(0x91 + i);
    const std::vector<int16_t> pcm = sbf_decode_entry(entry, sizeof(entry), block);
    CHECK(pcm.size() == 16, "four whole frames from the first and the last chunk, none from the 3-frame one");
    CHECK(pcm.size() == 16 && pcm[7] == sbf_decode_sample(0x88, 0) && pcm[8] == sbf_decode_sample(0x91, 0),
          "the odd byte dropped, the short chunk skipped, the last chunk on the left again");
    return 1;
}

/* The bank model's stream as the game plays it (the editor's preview): the same
   split rule over its chunks [orig: Audio_SubmitStereoSampleSplit
   @ 0x7BD222..0x7BD233]. */
static int test_play_stream_takes_the_split_rule(void) {
    SbfStream stream;
    for (size_t count : {(size_t)9, (size_t)6, (size_t)8}) {
        SbfChunk chunk;
        chunk.audio.assign(count, 0x90);
        stream.chunks.push_back(chunk);
    }
    CHECK(sbf_decode_stream(stream).size() == 23, "the model's decode keeps every byte");
    CHECK(sbf_play_stream(stream).size() == 16, "the played stream keeps the split's whole frames");
    return 1;
}

/* A bank of flags 0 or 2 streams stereo IMA ADPCM: 1024-byte blocks, the right header at +0 and the
   left at +4 (a predictor, then a signed index byte), then 127 pairs of 4-byte words, right then left,
   eight nibbles a word low first, through the wave loader's step; the +4 channel fills the buffer the
   byte-paired split's left byte does [orig: Audio_PlayStereoSample @ 0x7BCF95 - off_79CDB0
   @ 0x7BCFA4, off_79CDD0 @ 0x7BCFAB; Audio_AdpcmDecodeNibbleStep @ 0x7BF250]. */
static int test_adpcm_chunk_decodes_both_channels(void) {
    std::vector<uint8_t> block(4096, 0);
    const int16_t right = 1000, left = -1000;
    memcpy(block.data(), &right, 2);
    memcpy(block.data() + 4, &left, 2);
    block[8] = 0x07; /* the right's first word: nibble 7, then 0 */
    std::vector<int16_t> out;
    sbf_decode_adpcm_chunk(block.data(), block.size(), 3, out);
    CHECK(out.size() == 6, "three frames, left then right");
    /* Nibble 7 at index 0: 7 * 0x1E000 >> 16 = 13; then nibble 0 at index 8 (step 16): 16 * 0x2000 >> 16 = 2. */
    CHECK(out[1] == 1000 && out[3] == 1013 && out[5] == 1015, "the +0 channel plays right: its predictor, then its nibbles");
    CHECK(out[0] == -1000 && out[2] == -1000 && out[4] == -1000, "the +4 channel plays left: its own header and words");
    out.clear();
    sbf_decode_adpcm_chunk(block.data(), block.size(), 0, out);
    sbf_decode_adpcm_chunk(block.data(), block.size(), SBF_ADPCM_FRAMES + 1, out);
    CHECK(out.empty(), "a count outside 1..4068 plays nothing");
    /* A chunk's second block: the right's header 1024 bytes on, its 1018th sample. */
    const int16_t second = 2000;
    memcpy(block.data() + 1024, &second, 2);
    sbf_decode_adpcm_chunk(block.data(), block.size(), 1018, out);
    CHECK(out.size() == 2 * 1018 && out[2 * 1017 + 1] == 2000, "a block makes 1017 samples, the next its own header");
    return 1;
}

/* Each block_size read plays min(4068, the samples left) frames, the count unsigned against the
   samples left, which fall 4068 a block [orig: AudioStream_Update @ 0x671CF8..0x671D09, @ 0x671D72]. */
static int test_adpcm_entry_counts_by_the_samples_left(void) {
    std::vector<uint8_t> entry(3 * 4096, 0);
    const std::vector<int16_t> five = sbf_decode_entry(entry.data(), entry.size(), 4096, 0, 5000);
    /* 4068, 932, then the samples left below 0: a count of 4068 again. */
    CHECK(five.size() == 2 * (4068 + 932 + 4068), "the third block plays 4068 past the samples");
    CHECK(sbf_decode_entry(entry.data(), entry.size(), 4096, 2, 0).size() == 2 * (0 + 4068 + 4068),
            "no samples: the first block plays none, the ones after it 4068 each");
    CHECK(sbf_decode_entry(entry.data(), entry.size(), 4096, 0, 4068).size() == 2 * (4068 + 0 + 4068),
            "exactly one block's samples: the next count is 0, the one after 4068 again");
    /* The length the playback reads counts the same frames without decoding them. */
    const uint32_t lengths[] = {5000, 0, 4068, 100, 20000};
    for (const uint32_t length : lengths) {
        CHECK(sbf_adpcm_entry_frames(entry.size(), 4096, length) * 2 ==
                        sbf_decode_entry(entry.data(), entry.size(), 4096, 0, length).size(),
                "the counted frames are the decoded ones");
    }
    CHECK(sbf_adpcm_entry_frames(entry.size() - 1000, 4096, 20000) == 3 * 4068,
            "a short last block still plays its count");
    CHECK(sbf_adpcm_entry_frames(entry.size(), 0, 20000) == 0, "a zero block size plays nothing");
    return 1;
}

int main(void) {
    RUN_TEST(test_decode_sample_silence);
    RUN_TEST(test_decode_sample_extremes);
    RUN_TEST(test_decode_chunk_takes_any_shift);
    RUN_TEST(test_decode_chunk_tone01);
    RUN_TEST(test_decode_chunk_silence_partial);
    RUN_TEST(test_decode_all_silence);
    RUN_TEST(test_decode_all_tone01_multichunk);
    RUN_TEST(test_decode_entry_matches_streaming);
    RUN_TEST(test_decode_entry_walks_every_chunk);
    RUN_TEST(test_play_stream_takes_the_split_rule);
    RUN_TEST(test_adpcm_chunk_decodes_both_channels);
    RUN_TEST(test_adpcm_entry_counts_by_the_samples_left);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
