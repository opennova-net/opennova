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

/* Read an entire file into a freshly malloc'd buffer (caller frees). Used by the deterministic
   write test. Returns 1 on success. */
static int read_whole_file(const char *path, uint8_t **out, long *out_len) {
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *buf;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return 0; }
    buf = (uint8_t *)malloc(n ? (size_t)n : 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return 0; }
    fclose(f);
    *out = buf;
    *out_len = n;
    return 1;
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

/* ---- write API (pff_write_archive / pff_container_xor) ---- */

/* F1: write a mixed archive (plain + empty + encrypted), reopen, and verify. */
static int test_write_then_open(void) {
    const uint8_t a[] = {1, 2, 3, 4, 5};
    const uint8_t c_plain[] = "encrypted payload contents";
    uint8_t c_cipher[sizeof(c_plain)];
    PffArchive ar;
    const PffEntry *empty;
    const char *path = "pff_w_open.pff";

    memcpy(c_cipher, c_plain, sizeof(c_plain));
    pff_container_xor(c_cipher, sizeof(c_cipher), 0x0312A4CEu);   /* store as ciphertext */
    PffWriteEntry e[3] = {
        { "alpha.txt",  a,        (uint32_t)sizeof(a),       0,                  0, 0 },
        { "empty.bin",  NULL,     0,                          0,                  0, 0 },
        { "secret.bin", c_cipher, (uint32_t)sizeof(c_cipher), PFF_FLAG_ENCRYPTED, 0, 0 },
    };

    CHECK(pff_write_archive(path, PFF_FORMAT_PFF3, e, 3) == PFF_WRITE_OK, "write archive");
    CHECK(pff_open(&ar, path) == 0, "open written archive");
    CHECK(ar.entry_count == 3, "entry count == 3");
    CHECK(entry_bytes_equal(&ar, "alpha.txt", a, sizeof(a)), "alpha bytes");
    CHECK(entry_bytes_equal(&ar, "secret.bin", c_plain, sizeof(c_plain)), "secret decrypts");
    empty = pff_find(&ar, "empty.bin");
    CHECK(empty != NULL && empty->size == 0, "empty entry size 0");
    pff_close(&ar);
    remove(path);
    return 1;
}

/* F2: writing identical input twice yields byte-identical files (deterministic layout). */
static int test_write_deterministic(void) {
    const uint8_t a[] = {1, 2, 3};
    const uint8_t b[] = {9, 8, 7, 6};
    PffWriteEntry e[2] = {
        { "bravo.dat", b, (uint32_t)sizeof(b), 0, 0, 0 },   /* deliberately out of sort order */
        { "alpha.txt", a, (uint32_t)sizeof(a), 0, 0, 0 },
    };
    const char *p1 = "pff_det1.pff", *p2 = "pff_det2.pff";
    uint8_t *b1 = NULL, *b2 = NULL;
    long l1 = 0, l2 = 0;
    int ok;

    CHECK(pff_write_archive(p1, PFF_FORMAT_PFF3, e, 2) == PFF_WRITE_OK, "write 1");
    CHECK(pff_write_archive(p2, PFF_FORMAT_PFF3, e, 2) == PFF_WRITE_OK, "write 2");
    CHECK(read_whole_file(p1, &b1, &l1), "read 1");
    CHECK(read_whole_file(p2, &b2, &l2), "read 2");
    ok = (l1 == l2 && l1 > 0 && memcmp(b1, b2, (size_t)l1) == 0);
    free(b1);
    free(b2);
    remove(p1);
    remove(p2);
    CHECK(ok, "two writes are byte-identical");
    return 1;
}

/* F3: pff_container_xor is its own inverse (default + custom key) and matches the test keystream. */
static int test_container_xor_roundtrip(void) {
    uint8_t d[] = "the quick brown fox";
    uint8_t orig[sizeof(d)];
    uint8_t d2[] = "a different buffer!!";
    uint8_t o2[sizeof(d2)];
    uint8_t d3[] = "keystream compare ok";
    uint8_t o3[sizeof(d3)];

    memcpy(orig, d, sizeof(d));
    pff_container_xor(d, sizeof(d), 0x0312A4CEu);
    CHECK(memcmp(d, orig, sizeof(d)) != 0, "xor changes bytes");
    pff_container_xor(d, sizeof(d), 0x0312A4CEu);
    CHECK(memcmp(d, orig, sizeof(d)) == 0, "involution (default key)");

    memcpy(o2, d2, sizeof(d2));
    pff_container_xor(d2, sizeof(d2), 0xDEADBEEFu);
    pff_container_xor(d2, sizeof(d2), 0xDEADBEEFu);
    CHECK(memcmp(d2, o2, sizeof(d2)) == 0, "involution (custom key)");

    memcpy(o3, d3, sizeof(d3));
    pff_container_xor(d3, sizeof(d3), 0x0312A4CEu);
    pff_test_xor(o3, (uint32_t)sizeof(o3));          /* test writer uses the same 0x0312A4CE seed */
    CHECK(memcmp(d3, o3, sizeof(d3)) == 0, "matches PFF_LoadFileToMemory keystream");
    return 1;
}

/* F4 + F7: load an archive, add+delete entries, resave, and confirm retained entries (including
   an encrypted one) are copied as VERBATIM stored bytes + metadata. */
static int test_add_delete_resave(void) {
    const uint8_t a[] = {1, 2, 3, 4, 5};
    const uint8_t b[] = {0xAA, 0xBB, 0xCC};
    const uint8_t c_plain[] = "encrypted payload contents";
    const uint8_t g[] = {7, 7, 7, 7};
    uint8_t c_cipher[sizeof(c_plain)];
    const char *pa = "pff_wr_a.pff", *pb = "pff_wr_b.pff";
    PffArchive ar, br;
    const PffEntry *ae, *se, *se2;
    uint8_t alpha_raw[64], secret_raw[64], secret_raw2[64];
    uint32_t secret_size, secret_flags, secret_ts, secret_cs;

    memcpy(c_cipher, c_plain, sizeof(c_plain));
    pff_container_xor(c_cipher, sizeof(c_cipher), 0x0312A4CEu);
    PffWriteEntry ents[3] = {
        { "alpha.txt",  a,        (uint32_t)sizeof(a),       0,                  0,          0 },
        { "secret.bin", c_cipher, (uint32_t)sizeof(c_cipher), PFF_FLAG_ENCRYPTED, 0x11223344, 0x55667788 },
        { "bravo.dat",  b,        (uint32_t)sizeof(b),       0,                  0,          0 },
    };
    CHECK(pff_write_archive(pa, PFF_FORMAT_PFF3, ents, 3) == PFF_WRITE_OK, "write A");
    CHECK(pff_open(&ar, pa) == 0, "open A");

    ae = pff_find(&ar, "alpha.txt");
    se = pff_find(&ar, "secret.bin");
    CHECK(ae != NULL && se != NULL, "find retained entries");
    CHECK(pff_extract_raw(&ar, ae, alpha_raw, sizeof(alpha_raw)) == 0, "raw alpha");
    CHECK(pff_extract_raw(&ar, se, secret_raw, sizeof(secret_raw)) == 0, "raw secret");
    secret_size = se->size; secret_flags = se->flags; secret_ts = se->timestamp; secret_cs = se->checksum;

    /* B = alpha (untouched) + secret (untouched, verbatim ciphertext) - bravo (deleted) + gamma (new) */
    {
        PffWriteEntry bents[3] = {
            { "alpha.txt",  alpha_raw,  ae->size,    ae->flags, ae->timestamp, ae->checksum },
            { "secret.bin", secret_raw, secret_size, secret_flags, secret_ts, secret_cs },
            { "gamma.new",  g,          (uint32_t)sizeof(g), 0, 0, 0 },
        };
        pff_close(&ar);
        CHECK(pff_write_archive(pb, PFF_FORMAT_PFF3, bents, 3) == PFF_WRITE_OK, "write B");
    }

    CHECK(pff_open(&br, pb) == 0, "open B");
    CHECK(br.entry_count == 3, "B count == 3");
    CHECK(pff_find(&br, "bravo.dat") == NULL, "bravo deleted");
    CHECK(entry_bytes_equal(&br, "gamma.new", g, sizeof(g)), "gamma added");
    CHECK(entry_bytes_equal(&br, "secret.bin", c_plain, sizeof(c_plain)), "secret still decrypts");
    se2 = pff_find(&br, "secret.bin");
    CHECK(se2 != NULL && se2->size == secret_size, "secret size preserved");
    CHECK(pff_extract_raw(&br, se2, secret_raw2, sizeof(secret_raw2)) == 0, "raw secret B");
    CHECK(memcmp(secret_raw, secret_raw2, secret_size) == 0, "secret ciphertext byte-identical (F7)");
    CHECK(se2->flags == secret_flags && se2->timestamp == secret_ts && se2->checksum == secret_cs,
          "secret flags/timestamp/checksum preserved");
    pff_close(&br);
    remove(pa);
    remove(pb);
    return 1;
}

/* F5: PFF3/PFF4/BHD magic survives a write/reopen round-trip. */
static int test_format_preservation(void) {
    const uint8_t d[] = {1, 2};
    PffWriteEntry e[1] = { { "x.bin", d, 2, 0, 0, 0 } };
    PffFormat fmts[3]  = { PFF_FORMAT_PFF3, PFF_FORMAT_PFF4, PFF_FORMAT_BHD };
    uint32_t  mags[3]  = { PFF_MAGIC_PFF3,  PFF_MAGIC_PFF4,  PFF_MAGIC_BHD  };
    const char *path = "pff_fmt.pff";
    int i;
    for (i = 0; i < 3; ++i) {
        PffArchive ar;
        CHECK(pff_write_archive(path, fmts[i], e, 1) == PFF_WRITE_OK, "write format");
        CHECK(pff_open(&ar, path) == 0, "open format");
        CHECK(ar.header.magic == mags[i], "magic preserved");
        pff_close(&ar);
        remove(path);
    }
    return 1;
}

/* F6a: zero-entry archive is valid. */
static int test_write_zero_entries(void) {
    const char *path = "pff_zero.pff";
    PffArchive ar;
    CHECK(pff_write_archive(path, PFF_FORMAT_PFF3, NULL, 0) == PFF_WRITE_OK, "write empty");
    CHECK(pff_open(&ar, path) == 0, "open empty");
    CHECK(ar.entry_count == 0, "zero entries");
    pff_close(&ar);
    remove(path);
    return 1;
}

/* F6b: an over-long (>16 byte) name is rejected, not silently truncated. */
static int test_write_rejects_overlong_name(void) {
    const uint8_t d[] = {1};
    PffWriteEntry e[1] = { { "seventeen_chars!!", d, 1, 0, 0, 0 } };
    CHECK(strlen("seventeen_chars!!") == 17, "fixture name is 17 chars");
    CHECK(pff_write_archive("pff_bad.pff", PFF_FORMAT_PFF3, e, 1) == PFF_WRITE_ERR_NAME_LEN,
          "over-long name rejected");
    return 1;
}

/* F6c: two entries with the same normalized name are rejected. */
static int test_write_rejects_duplicate_names(void) {
    const uint8_t d[] = {1};
    PffWriteEntry e[2] = { { "dup.bin", d, 1, 0, 0, 0 }, { "DUP.BIN", d, 1, 0, 0, 0 } };
    CHECK(pff_write_archive("pff_dup.pff", PFF_FORMAT_PFF3, e, 2) == PFF_WRITE_ERR_DUP_NAME,
          "duplicate name rejected");
    return 1;
}

/* F6d: original-case name is stored on disk; lookup is still case-insensitive. */
static int test_write_preserves_name_case(void) {
    const uint8_t d[] = {1, 2, 3};
    PffWriteEntry e[1] = { { "Bravo.dat", d, 3, 0, 0, 0 } };
    const char *path = "pff_case.pff";
    PffArchive ar;
    const PffEntry *fe;
    CHECK(pff_write_archive(path, PFF_FORMAT_PFF3, e, 1) == PFF_WRITE_OK, "write");
    CHECK(pff_open(&ar, path) == 0, "open");
    CHECK(pff_find(&ar, "bravo.dat") != NULL, "case-insensitive lookup");
    fe = pff_find(&ar, "Bravo.dat");
    CHECK(fe != NULL && memcmp(fe->filename, "Bravo.dat", 9) == 0, "original case stored on disk");
    pff_close(&ar);
    remove(path);
    return 1;
}

/* Streaming-writer read callback over an array of payload pointers (NULL allowed for size 0). */
static int prog_read_entry(void *ctx, uint32_t index, uint8_t *out, uint32_t size) {
    const uint8_t **datas = (const uint8_t **)ctx;
    if (size) memcpy(out, datas[index], size);
    return 0;
}

typedef struct { int calls; uint32_t last_done; uint32_t last_total; } ProgCount;
static void prog_count_cb(void *ctx, uint32_t done, uint32_t total) {
    ProgCount *p = (ProgCount *)ctx;
    p->calls++;
    p->last_done = done;
    p->last_total = total;
}

/* The streaming writer's progress callback fires once per entry (including a zero-size one), and
   `done` reaches `total == n`. The written archive must still be valid. */
static int test_write_progress_callback(void) {
    const uint8_t a[] = {1, 2, 3};
    const uint8_t b[] = {4, 4};
    const uint8_t *datas[3] = { a, NULL, b };   /* entry 1 is empty */
    PffWriteStreamEntry se[3] = {
        { "a.bin",     3, 0, 0, 0 },
        { "empty.bin", 0, 0, 0, 0 },
        { "b.bin",     2, 0, 0, 0 },
    };
    ProgCount pc = { 0, 0, 0 };
    const char *path = "pff_prog.pff";
    PffArchive ar;

    CHECK(pff_write_archive_streamed_progress(path, PFF_FORMAT_PFF3, se, 3,
              prog_read_entry, (void *)datas, prog_count_cb, &pc) == PFF_WRITE_OK, "write w/ progress");
    CHECK(pc.calls == 3, "progress fires once per entry (incl. the empty one)");
    CHECK(pc.last_total == 3, "total == n");
    CHECK(pc.last_done == 3, "done reaches n");

    CHECK(pff_open(&ar, path) == 0, "open progress-written archive");
    CHECK(ar.entry_count == 3, "entry count == 3");
    CHECK(entry_bytes_equal(&ar, "a.bin", a, sizeof(a)), "a bytes");
    CHECK(entry_bytes_equal(&ar, "b.bin", b, sizeof(b)), "b bytes");
    pff_close(&ar);
    remove(path);
    return 1;
}

/* A payload set whose total size overflows the uint32 offset space is rejected up front, before
   any read callback runs, rather than silently wrapping offsets into a corrupt archive. */
static int test_write_rejects_offset_overflow(void) {
    /* Three ~1.5GB entries sum to >4GB. The read callback is never reached (validation fails
       first), so no memory is actually allocated for these sizes. */
    PffWriteStreamEntry se[3] = {
        { "a.bin", 0x60000000u, 0, 0, 0 },
        { "b.bin", 0x60000000u, 0, 0, 0 },
        { "c.bin", 0x60000000u, 0, 0, 0 },
    };
    const uint8_t *datas[3] = { NULL, NULL, NULL };
    CHECK(pff_write_archive_streamed("pff_huge.pff", PFF_FORMAT_PFF3, se, 3,
              prog_read_entry, (void *)datas) == PFF_WRITE_ERR_TOO_LARGE,
          "total payload over 4GB rejected");
    remove("pff_huge.pff");
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
    RUN_TEST(test_write_then_open);
    RUN_TEST(test_write_deterministic);
    RUN_TEST(test_container_xor_roundtrip);
    RUN_TEST(test_add_delete_resave);
    RUN_TEST(test_format_preservation);
    RUN_TEST(test_write_zero_entries);
    RUN_TEST(test_write_rejects_overlong_name);
    RUN_TEST(test_write_rejects_duplicate_names);
    RUN_TEST(test_write_preserves_name_case);
    RUN_TEST(test_write_progress_callback);
    RUN_TEST(test_write_rejects_offset_overflow);

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
