#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "mus/mus.h"

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

static int test_constants(void) {
    CHECK(MUS_MAGIC_SCR0      == 0x30524353u, "SCR0 magic");
    CHECK(MUS_CHUNK_TAG_MU01  == 0x3130554Du, "MU01 tag");
    CHECK(MUS_NAME_SIZE       == 16,           "name size");
    CHECK(MUS_GLOBALS_BYTES   == 68,           "globals area");
    CHECK(MUS_OPCODE_COUNT    == 65,           "opcode count");
    CHECK(MUS_INTRINSIC_NAMES == 11,           "intrinsic name count");
    return 1;
}

static int test_struct_sizes(void) {
    CHECK(sizeof(MusFileHeader)  == 44, "MusFileHeader 44 bytes");
    CHECK(sizeof(MusChunkHeader) == 72, "MusChunkHeader 72 bytes");
    return 1;
}

static int test_validate_valid_header(void) {
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    /* Field offsets per MusFileHeader. */
    *(uint32_t *)(buf + 0x00) = MUS_MAGIC_SCR0;
    *(uint32_t *)(buf + 0x04) = 0x00000100;
    *(uint32_t *)(buf + 0x08) = 0;          /* chunk_count = 0 (degenerate but valid) */
    *(uint32_t *)(buf + 0x0C) = 0x2C;       /* chunk_table_offset */
    CHECK(mus_validate(buf, sizeof(buf)) == 0, "valid header");
    return 1;
}

static int test_validate_bad_magic(void) {
    uint8_t buf[44];
    memset(buf, 0, sizeof(buf));
    buf[0] = 'X';
    CHECK(mus_validate(buf, sizeof(buf)) != 0, "bad magic");
    return 1;
}

static int test_validate_too_small(void) {
    uint8_t buf[20] = {0};
    CHECK(mus_validate(buf, sizeof(buf)) != 0, "too small");
    return 1;
}

static int test_validate_chunk_count_overflow(void) {
    uint8_t buf[44];
    memset(buf, 0, sizeof(buf));
    *(uint32_t *)(buf + 0x00) = MUS_MAGIC_SCR0;
    *(uint32_t *)(buf + 0x08) = 0x10000000;  /* implausible */
    *(uint32_t *)(buf + 0x0C) = 0x2C;
    CHECK(mus_validate(buf, sizeof(buf)) != 0, "chunk_count rejected");
    return 1;
}

int main(void) {
    RUN_TEST(test_constants);
    RUN_TEST(test_struct_sizes);
    RUN_TEST(test_validate_valid_header);
    RUN_TEST(test_validate_bad_magic);
    RUN_TEST(test_validate_too_small);
    RUN_TEST(test_validate_chunk_count_overflow);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
