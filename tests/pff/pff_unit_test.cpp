// Unit tests for PFF format — struct sizes, magic constants, header validation.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "pff/pff.h"
#include "pff/pff_test_writer.h"

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

/* Test encrypted flag constant (bit 0 = ENCRYPTED, verified vs PFF_LoadFileToMemory @ 0x768920) */
static int test_encrypted_flag_constant(void) {
    CHECK(PFF_FLAG_ENCRYPTED == 0x01u, "PFF_FLAG_ENCRYPTED should be 0x01");
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

/* ---- round-trip tests over synthetic archives (no game data needed) ---- */

static int entry_bytes_equal(const PffArchive *a, const char *name,
                             const uint8_t *expect, uint32_t n) {
    const PffEntry *e = pff_find(a, name);
    uint8_t *buf;
    int ok;
    if (!e || e->size != n) return 0;
    buf = (uint8_t *)malloc(n ? n : 1);
    if (pff_extract(a, e, buf, n) != 0) { free(buf); return 0; }
    ok = (memcmp(buf, expect, n) == 0);
    free(buf);
    return ok;
}

/* Modern open/find/extract, including transparent decryption of an encrypted entry. */
static int test_modern_roundtrip(void) {
    const uint8_t a_data[] = {1, 2, 3, 4, 5};
    const uint8_t b_data[] = {0xAA, 0xBB, 0xCC};
    const uint8_t c_data[] = "encrypted payload contents";
    PffTestEntry entries[3] = {
        { "alpha.txt",  a_data, (uint32_t)sizeof(a_data), 0 },
        { "Bravo.dat",  b_data, (uint32_t)sizeof(b_data), 0 },
        { "secret.bin", c_data, (uint32_t)sizeof(c_data), 1 },  /* encrypted */
    };
    const char *path = "pff_rt_modern.pff";
    PffArchive ar;

    CHECK(pff_test_write_modern(path, entries, 3) == 0, "write modern pff");
    CHECK(pff_open(&ar, path) == 0, "open modern pff");
    CHECK(ar.entry_count == 3, "entry count == 3");
    CHECK(entry_bytes_equal(&ar, "alpha.txt", a_data, sizeof(a_data)), "alpha bytes");
    CHECK(entry_bytes_equal(&ar, "Bravo.dat", b_data, sizeof(b_data)), "bravo bytes");
    CHECK(entry_bytes_equal(&ar, "secret.bin", c_data, sizeof(c_data)), "decrypted bytes");
    {
        const PffEntry *e = pff_find(&ar, "secret.bin");
        uint8_t raw[64];
        CHECK(e != NULL && (e->flags & PFF_FLAG_ENCRYPTED), "encrypted flag set");
        CHECK(pff_extract_raw(&ar, e, raw, sizeof(raw)) == 0, "extract_raw ok");
        CHECK(memcmp(raw, c_data, sizeof(c_data)) != 0, "raw bytes are ciphertext");
        pff_test_xor(raw, (uint32_t)sizeof(c_data));
        CHECK(memcmp(raw, c_data, sizeof(c_data)) == 0, "xor(raw) == plaintext");
    }
    pff_close(&ar);
    remove(path);
    return 1;
}

/* Lookup is case-insensitive (engine uppercases names) and misses return NULL. */
static int test_find_case_insensitive(void) {
    const uint8_t d[] = {9, 9, 9};
    PffTestEntry entries[1] = { { "MixedCase.TGA", d, 3, 0 } };
    const char *path = "pff_rt_case.pff";
    PffArchive ar;

    CHECK(pff_test_write_modern(path, entries, 1) == 0, "write");
    CHECK(pff_open(&ar, path) == 0, "open");
    CHECK(pff_find(&ar, "mixedcase.tga") != NULL, "lower-case query matches");
    CHECK(pff_find(&ar, "MIXEDCASE.TGA") != NULL, "upper-case query matches");
    CHECK(pff_find(&ar, "nope.tga") == NULL, "missing returns NULL");
    pff_close(&ar);
    remove(path);
    return 1;
}

/* Legacy format: delta-encoded sizes + 0xACEDDEAD name obfuscation. */
static int test_legacy_roundtrip(void) {
    const uint8_t a_data[] = {10, 20, 30, 40};
    const uint8_t b_data[] = {50, 60};
    PffTestEntry entries[2] = {
        { "leg1.bin", a_data, (uint32_t)sizeof(a_data), 0 },
        { "leg2.bin", b_data, (uint32_t)sizeof(b_data), 0 },
    };
    const char *path = "pff_rt_legacy.pff";
    PffArchive ar;

    CHECK(pff_test_write_legacy(path, entries, 2) == 0, "write legacy");
    CHECK(pff_open_legacy(&ar, path) == 0, "open legacy");
    CHECK(ar.entry_count == 2, "legacy entry count == 2");
    CHECK(entry_bytes_equal(&ar, "leg1.bin", a_data, sizeof(a_data)), "leg1 bytes");
    CHECK(entry_bytes_equal(&ar, "leg2.bin", b_data, sizeof(b_data)), "leg2 bytes (delta size)");
    pff_close(&ar);
    remove(path);
    return 1;
}

int main(void) {
    RUN_TEST(test_is_pff3_magic);
    RUN_TEST(test_is_pff4_magic);
    RUN_TEST(test_rejects_invalid_magic);
    RUN_TEST(test_rejects_too_small_buffer);
    RUN_TEST(test_header_size_constants);
    RUN_TEST(test_magic_constants);
    RUN_TEST(test_encrypted_flag_constant);
    RUN_TEST(test_header_struct_size);
    RUN_TEST(test_entry_struct_size);
    RUN_TEST(test_modern_roundtrip);
    RUN_TEST(test_find_case_insensitive);
    RUN_TEST(test_legacy_roundtrip);

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
