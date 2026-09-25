// The .adm writer: the canonical stock row shape, pinned byte-for-byte on a
// small table, parse-equal through adm_parse_buffer, and refusing rows the
// parser could not read back.
#include <formats/adm/adm.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "common/test_expect.h"

using namespace opennova::adm;

int main() {
    std::string bytes;
    TEST_EXPECT(adm_write_buffer(nullptr, bytes) == -1);

    AdmEntry entries[3] = {};
    std::strcpy(entries[0].key, "anim_reset");
    entries[0].variant_count = 1;
    std::strcpy(entries[0].variants[0], "person_rst");
    std::strcpy(entries[1].key, "anim_wpn_reload");
    entries[1].variant_count = 3;
    std::strcpy(entries[1].variants[0], "m4_1r");
    std::strcpy(entries[1].variants[1], "m4_1r");
    std::strcpy(entries[1].variants[2], "m4_1r2");
    std::strcpy(entries[2].key, "anim_idle");
    entries[2].variant_count = 1;
    std::strcpy(entries[2].variants[0], "person_idle.bad");
    AdmFile table = {};
    table.entries = entries;
    table.count = 3;

    TEST_EXPECT(adm_write_buffer(&table, bytes) == 0);
    const std::string expected =
        std::string("\r\n"
                    "anim_reset\t\t\t\t\"person_rst\"\r\n"
                    "anim_wpn_reload\t\t\t\t\"m4_1r\" \"m4_1r\" \"m4_1r2\"\r\n"
                    "anim_idle\t\t\t\t\"person_idle.bad\"\r\n\r\n\r\n") + std::string(1, '\0');
    TEST_EXPECT(bytes == expected);

    AdmFile back = {};
    TEST_EXPECT(adm_parse_buffer(bytes.data(), bytes.size(), &back) == 0);
    TEST_EXPECT(back.count == 3);
    TEST_EXPECT(std::strcmp(back.entries[0].key, "anim_reset") == 0 && back.entries[0].variant_count == 1);
    TEST_EXPECT(std::strcmp(back.entries[1].variants[2], "m4_1r2") == 0 && back.entries[1].variant_count == 3);
    TEST_EXPECT(std::strcmp(back.entries[2].variants[0], "person_idle.bad") == 0);
    adm_free(&back);

    // An empty table is the bare frame.
    AdmFile empty = {};
    TEST_EXPECT(adm_write_buffer(&empty, bytes) == 0);
    TEST_EXPECT(bytes == std::string("\r\n\r\n\r\n\r\n") + std::string(1, '\0'));

    // Rows the parser would drop or misread are refused: a key without the
    // anim_ prefix, no variants, a quote inside a clip name.
    AdmEntry bad = {};
    std::strcpy(bad.key, "reset");
    bad.variant_count = 1;
    std::strcpy(bad.variants[0], "x");
    AdmFile one = {};
    one.entries = &bad;
    one.count = 1;
    TEST_EXPECT(adm_write_buffer(&one, bytes) == -1);
    std::strcpy(bad.key, "anim_reset");
    bad.variant_count = 0;
    TEST_EXPECT(adm_write_buffer(&one, bytes) == -1);
    bad.variant_count = 1;
    std::strcpy(bad.variants[0], "a\"b");
    TEST_EXPECT(adm_write_buffer(&one, bytes) == -1);

    // A key is one plain token: a space, tab or comma would split it, a quote
    // toggle quoting, and ';' or "//" cut the line.
    std::strcpy(bad.variants[0], "x");
    for (const char *key : {"anim_a b", "anim_a\tb", "anim_a,b", "anim_a\"b", "anim_a;b", "anim_a//b",
                            "anim_a\rb"}) {
        std::strcpy(bad.key, key);
        TEST_EXPECT(adm_write_buffer(&one, bytes) == -1);
    }
    // A variant starting with '/' ends the row, edge whitespace is trimmed on
    // read, and a line break splits the row.
    std::strcpy(bad.key, "anim_reset");
    for (const char *variant : {"/x", " x", "x\t", "x\r\ny"}) {
        std::strcpy(bad.variants[0], variant);
        TEST_EXPECT(adm_write_buffer(&one, bytes) == -1);
    }
    // Inside the quotes a comma, ';' and "//" are the name's own.
    std::strcpy(bad.variants[0], "a,b;c//d");
    TEST_EXPECT(adm_write_buffer(&one, bytes) == 0);
    AdmFile quoted = {};
    TEST_EXPECT(adm_parse_buffer(bytes.data(), bytes.size(), &quoted) == 0);
    TEST_EXPECT(quoted.count == 1 && std::strcmp(quoted.entries[0].variants[0], "a,b;c//d") == 0);
    adm_free(&quoted);

    std::printf("adm_write: canonical form pinned, parse-equal, refusals hold\n");
    return 0;
}
