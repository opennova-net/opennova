#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sbf/sbf.h"

static int passed = 0, failed = 0;
#define RUN_TEST(fn) do { printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } } while (0)

static int test_struct_sizes(void) {
    CHECK(sizeof(SbfHeader)       == 24, "SbfHeader must be 24 bytes");
    CHECK(sizeof(SbfRawEntry)     == 32, "SbfRawEntry must be 32 bytes");
    CHECK(sizeof(SbfChunkHeader)  ==  8, "SbfChunkHeader must be 8 bytes");
    return 1;
}

static int test_constants(void) {
    CHECK(SBF_MAGIC          == 0x30464253u, "magic");
    CHECK(SBF_HEADER_SIZE    == 24,          "header size");
    CHECK(SBF_ENTRY_SIZE     == 32,          "entry size");
    CHECK(SBF_NAME_SIZE      == 16,          "name size");
    CHECK(SBF_CHUNK_HEADER   ==  8,          "chunk header size");
    CHECK(SBF_SAMPLE_RATE    == 22050,       "sample rate");
    CHECK(SBF_CHANNELS       ==  2,          "channels");
    return 1;
}

static int test_validate_valid_header(void) {
    uint8_t buf[24] = {
        0x53,0x42,0x46,0x30,  /* SBF0 */
        0x00,0x01,0x00,0x00,  /* version */
        0x01,0x00,0x00,0x00,  /* flags */
        0x00,0x00,0x00,0x00,  /* reserved */
        0x18,0x00,0x00,0x00,  /* index_offset = 24 */
        0x00,0x00,0x00,0x00,  /* entry_count = 0 */
    };
    CHECK(sbf_validate(buf, sizeof(buf)) == 0, "valid header should pass");
    return 1;
}

static int test_validate_rejects_engine_unsupported_flags(void) {
    uint8_t buf[24] = {
        0x53,0x42,0x46,0x30,  /* SBF0 */
        0x00,0x01,0x00,0x00,  /* version */
        0x03,0x00,0x00,0x00,  /* flags = 3; engine accepts only <= 2 */
        0x00,0x00,0x00,0x00,  /* reserved */
        0x18,0x00,0x00,0x00,  /* index_offset = 24 */
        0x00,0x00,0x00,0x00,  /* entry_count = 0 */
    };
    CHECK(sbf_validate(buf, sizeof(buf)) != 0, "flags > 2 should fail");
    return 1;
}

static int test_validate_bad_magic(void) {
    uint8_t buf[24] = {0};
    buf[0] = 'X';
    CHECK(sbf_validate(buf, sizeof(buf)) != 0, "bad magic should fail");
    return 1;
}

static int test_validate_too_small(void) {
    uint8_t buf[10] = {0};
    CHECK(sbf_validate(buf, sizeof(buf)) != 0, "buf < header size should fail");
    return 1;
}

int main(void) {
    RUN_TEST(test_struct_sizes);
    RUN_TEST(test_constants);
    RUN_TEST(test_validate_valid_header);
    RUN_TEST(test_validate_rejects_engine_unsupported_flags);
    RUN_TEST(test_validate_bad_magic);
    RUN_TEST(test_validate_too_small);
    printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
