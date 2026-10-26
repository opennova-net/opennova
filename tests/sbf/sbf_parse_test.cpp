/* SBF parse: the header/index/entry reads over the synthetic bank
   fixtures/sbf/synth_gamemus.sbf (minted by tests/fixtures/minimal_sbf_gen.cpp;
   13 entries, SILENCE first as one partial chunk of 0x08A8 samples, TONE01
   spanning three chunks). Both shipped bank forms are swept by
   sbf_bank_sweep_test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <formats/sbf/sbf.h>

#include "sbf/sbf_bank_file.h"

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

static uint8_t *slurp(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc((size_t)n);
    fread(buf, 1, (size_t)n, f);
    fclose(f);
    *out_size = (size_t)n;
    return buf;
}

static int test_open_memory(void) {
    size_t n;
    uint8_t *buf = slurp(SYNTH_BANK, &n);
    CHECK(buf, "fixture must exist");
    SbfArchive arc;
    CHECK(sbf_open_memory(&arc, buf, n) == 0, "open_memory should succeed");
    CHECK(arc.header.magic       == SBF_MAGIC, "magic");
    CHECK(arc.header.entry_count == 13,        "synth bank has 13 entries");
    CHECK(arc.entries != NULL,                  "entries allocated");
    CHECK(strncmp(arc.entries[0].name, "SILENCE", 7) == 0, "first entry SILENCE");
    sbf_close(&arc);
    free(buf);
    return 1;
}

static int test_find_by_name(void) {
    SbfBankFile bank;
    SbfArchive &arc = bank.arc;
    CHECK(bank.open(SYNTH_BANK) == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&arc, "SILENCE");
    CHECK(e != NULL, "SILENCE found");
    CHECK(strncmp(e->name, "SILENCE", 7) == 0, "name match");
    /* Engine string compare in jointops!Sbf_StartEntry's caller is uppercase-folded;
       we keep the same case-insensitive semantics. */
    const SbfRawEntry *e2 = sbf_find_by_name(&arc, "silence");
    CHECK(e2 == e, "case-insensitive match");
    const SbfRawEntry *miss = sbf_find_by_name(&arc, "NOPE");
    CHECK(miss == NULL, "miss returns NULL");
    sbf_close(&arc);
    return 1;
}

static int test_find_by_index(void) {
    SbfBankFile bank;
    SbfArchive &arc = bank.arc;
    CHECK(bank.open(SYNTH_BANK) == 0, "open");
    const SbfRawEntry *e = sbf_find_by_index(&arc, 0);
    CHECK(e != NULL, "index 0");
    CHECK(strncmp(e->name, "SILENCE", 7) == 0, "first is SILENCE");
    const SbfRawEntry *last = sbf_find_by_index(&arc, 12);
    CHECK(last != NULL && strncmp(last->name, "TONE12", 6) == 0, "index 12 is TONE12");
    const SbfRawEntry *oob = sbf_find_by_index(&arc, 999);
    CHECK(oob == NULL, "out-of-range returns NULL");
    sbf_close(&arc);
    return 1;
}

static int test_entry_bytes(void) {
    SbfBankFile bank;
    CHECK(bank.open(SYNTH_BANK) == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&bank.arc, "SILENCE");
    CHECK(e != NULL, "SILENCE found");
    CHECK(e->total_size == 0x1008, "SILENCE total_size = 4104 (one chunk)");
    const uint8_t *buf = bank.entry_bytes(e);
    CHECK(buf != NULL, "the entry's bytes lie in the bank");
    /* First 4 bytes are the chunk header's valid_samples: 0x08A8, the retail NULLS shape. */
    uint32_t valid_samples;
    memcpy(&valid_samples, buf, 4);
    CHECK(valid_samples == 0x08A8, "valid_samples in the SILENCE chunk");
    return 1;
}

static int test_entry_chunks(void) {
    SbfBankFile bank;
    CHECK(bank.open(SYNTH_BANK) == 0, "open");
    const SbfRawEntry *e = sbf_find_by_name(&bank.arc, "TONE01");
    CHECK(e != NULL, "TONE01 found");
    CHECK(e->total_size == 3 * 0x1008, "TONE01 spans three chunks");
    const uint8_t *buf = bank.chunk(e, 0);
    CHECK(buf != NULL, "the first chunk lies in the entry");
    uint32_t valid_samples;
    memcpy(&valid_samples, buf, 4);
    CHECK(valid_samples == 0x1000, "TONE01 first chunk full");
    buf = bank.chunk(e, 2);
    CHECK(buf != NULL, "the last chunk lies in the entry");
    memcpy(&valid_samples, buf, 4);
    CHECK(valid_samples == 1000, "TONE01 last chunk carries the 1000-sample tail");
    CHECK(bank.chunk(e, 3) == NULL, "no chunk past the entry");
    return 1;
}

/* A header whose entry count runs past the bytes is refused whatever the size, a header-only 24-byte
   bank too (the index would be read past the buffer), and a failed open leaves the index closable. */
static int test_open_bad_bytes_safe_close(void) {
    uint8_t buf[24] = {
        0x53,0x42,0x46,0x30,  /* SBF0 */
        0x00,0x01,0x00,0x00,  /* version */
        0x01,0x00,0x00,0x00,  /* flags */
        0x00,0x00,0x00,0x00,  /* reserved */
        0x18,0x00,0x00,0x00,  /* index_offset = 24 */
        0xE8,0x03,0x00,0x00,  /* entry_count = 1000, no index behind it */
    };
    SbfArchive arc;
    /* Garbage in struct memory before open */
    memset(&arc, 0xCD, sizeof(arc));
    CHECK(sbf_open_memory(&arc, buf, sizeof(buf)) == -4, "an index past the bytes is refused");
    /* sbf_close should be safe even after failed open */
    sbf_close(&arc);
    buf[0] = 'X';
    CHECK(sbf_open_memory(&arc, buf, sizeof(buf)) != 0, "bad magic is refused");
    sbf_close(&arc);
    return 1;
}

int main(void) {
    RUN_TEST(test_open_memory);
    RUN_TEST(test_find_by_name);
    RUN_TEST(test_find_by_index);
    RUN_TEST(test_entry_bytes);
    RUN_TEST(test_entry_chunks);
    RUN_TEST(test_open_bad_bytes_safe_close);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
