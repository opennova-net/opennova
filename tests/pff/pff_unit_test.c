// Unit tests for PFF format — struct sizes, magic constants, header validation.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pff/pff.h"

static int passed = 0;
static int failed = 0;

#define RUN_TEST(fn) do { \
    printf("Running %s... ", #fn); \
    if (fn()) { printf("PASS\n"); ++passed; } \
    else { printf("FAIL\n"); ++failed; } \
} while (0)

#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); return 0; } \
} while (0)

/* Test PFF3 magic detection */
static int test_is_pff3_magic(void) {
    uint8_t pff3_header[20] = {
        0x14, 0x00, 0x00, 0x00,  /* header_size = 20 */
        0x50, 0x46, 0x46, 0x33,  /* magic = "PFF3" */
        0x01, 0x00, 0x00, 0x00,  /* num_entries = 1 */
        0x24, 0x00, 0x00, 0x00,  /* entry_size = 36 */
        0x14, 0x00, 0x00, 0x00,  /* file_table_offset = 20 */
    };
    CHECK(pff_is_pff(pff3_header, sizeof(pff3_header)), "PFF3 magic should be recognized");
    return 1;
}

/* Test PFF4 magic detection */
static int test_is_pff4_magic(void) {
    uint8_t pff4_header[20] = {
        0x14, 0x00, 0x00, 0x00,
        0x50, 0x46, 0x46, 0x34,  /* "PFF4" */
        0x01, 0x00, 0x00, 0x00,
        0x24, 0x00, 0x00, 0x00,
        0x14, 0x00, 0x00, 0x00,
    };
    CHECK(pff_is_pff(pff4_header, sizeof(pff4_header)), "PFF4 magic should be recognized");
    return 1;
}

/* Test invalid magic rejection */
static int test_rejects_invalid_magic(void) {
    uint8_t invalid_header[20] = {
        0x14, 0x00, 0x00, 0x00,
        0x50, 0x46, 0x46, 0x35,  /* "PFF5" — invalid */
        0x01, 0x00, 0x00, 0x00,
        0x24, 0x00, 0x00, 0x00,
        0x14, 0x00, 0x00, 0x00,
    };
    CHECK(!pff_is_pff(invalid_header, sizeof(invalid_header)), "PFF5 magic should be rejected");
    return 1;
}

/* Test too-small buffer rejection */
static int test_rejects_too_small_buffer(void) {
    uint8_t small_buffer[10] = {0};
    CHECK(!pff_is_pff(small_buffer, sizeof(small_buffer)), "Small buffer should be rejected");
    return 1;
}

/* Test header size constants */
static int test_header_size_constants(void) {
    CHECK(PFF_HEADER_SIZE == 20, "PFF_HEADER_SIZE should be 20");
    CHECK(PFF_ENTRY_SIZE == 36, "PFF_ENTRY_SIZE should be 36");
    CHECK(PFF_NAME_SIZE == 16, "PFF_NAME_SIZE should be 16");
    return 1;
}

/* Test magic constants */
static int test_magic_constants(void) {
    CHECK(PFF_MAGIC_PFF3 == 0x33464650u, "PFF_MAGIC_PFF3 should be 0x33464650");
    CHECK(PFF_MAGIC_PFF4 == 0x34464650u, "PFF_MAGIC_PFF4 should be 0x34464650");
    return 1;
}

/* Test deleted flag constant */
static int test_deleted_flag_constant(void) {
    CHECK(PFF_FLAG_DELETED == 0x01u, "PFF_FLAG_DELETED should be 0x01");
    return 1;
}

/* Test Header struct size */
static int test_header_struct_size(void) {
    CHECK(sizeof(PffHeader) == PFF_HEADER_SIZE, "PffHeader struct size mismatch");
    return 1;
}

/* Test Entry struct size */
static int test_entry_struct_size(void) {
    CHECK(sizeof(PffEntry) == PFF_ENTRY_SIZE, "PffEntry struct size mismatch");
    return 1;
}

int main(void) {
    RUN_TEST(test_is_pff3_magic);
    RUN_TEST(test_is_pff4_magic);
    RUN_TEST(test_rejects_invalid_magic);
    RUN_TEST(test_rejects_too_small_buffer);
    RUN_TEST(test_header_size_constants);
    RUN_TEST(test_magic_constants);
    RUN_TEST(test_deleted_flag_constant);
    RUN_TEST(test_header_struct_size);
    RUN_TEST(test_entry_struct_size);

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
