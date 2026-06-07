// ADM writer round-trip test.
// Parse a stock .adm -> adm_write -> parse again -> parse-equality of entries.
// (Byte-equality is not the bar: the parser trims whitespace, so the achievable
// contract is that the key/value pairs survive a write/read cycle.)
#include "adm/adm.h"

#include <cstdio>
#include <cstring>

#include "common/test_expect.h"
#include "common/test_paths.h"

int main() {
    char fixture[4096];
    std::snprintf(fixture, sizeof(fixture), "%s%cfixtures%cadm%cmp5_1st.adm",
                  test_paths_repo_root(__FILE__), TEST_PATHS_SEP, TEST_PATHS_SEP, TEST_PATHS_SEP);

    char out_path[4096];
    std::snprintf(out_path, sizeof(out_path), "%s%cadm_write_roundtrip.adm",
                  test_paths_temp_dir(), TEST_PATHS_SEP);

    AdmFile orig = {};
    TEST_EXPECT(adm_parse(fixture, &orig) == 0);
    TEST_EXPECT(orig.count >= 2);
    // sanity: the fixture leads with the reset entry
    TEST_EXPECT(std::strcmp(orig.entries[0].key, "anim_reset") == 0);

    TEST_EXPECT(adm_write(out_path, orig.entries, orig.count) == 0);

    AdmFile rt = {};
    TEST_EXPECT(adm_parse(out_path, &rt) == 0);
    TEST_EXPECT(rt.count == orig.count);
    for (size_t i = 0; i < orig.count; ++i) {
        TEST_EXPECT(std::strcmp(rt.entries[i].key, orig.entries[i].key) == 0);
        TEST_EXPECT(std::strcmp(rt.entries[i].value, orig.entries[i].value) == 0);
    }

    // Writing zero entries is valid and re-parses to an empty file.
    char empty_path[4096];
    std::snprintf(empty_path, sizeof(empty_path), "%s%cadm_write_empty.adm",
                  test_paths_temp_dir(), TEST_PATHS_SEP);
    TEST_EXPECT(adm_write(empty_path, nullptr, 0) == 0);
    AdmFile empty = {};
    TEST_EXPECT(adm_parse(empty_path, &empty) == 0);
    TEST_EXPECT(empty.count == 0);

    adm_free(&orig);
    adm_free(&rt);
    adm_free(&empty);
    std::remove(out_path);
    std::remove(empty_path);
    return 0;
}
