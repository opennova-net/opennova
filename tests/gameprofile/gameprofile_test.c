/* Smoke tests for libs/gameprofile: table size, lookups, and the universal container key. */
#include <stdio.h>
#include <string.h>

#include "gameprofile/gameprofile.h"

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

static int test_count(void) {
    CHECK(gameprofile_count() == NOVA_GAME_COUNT, "count == NOVA_GAME_COUNT");
    CHECK(gameprofile_count() == 5, "five shipping profiles");
    return 1;
}

static int test_by_id(void) {
    const NovaGameProfile *jo = gameprofile_by_id(NOVA_GAME_JO);
    const NovaGameProfile *bhd = gameprofile_by_id(NOVA_GAME_BHD);
    CHECK(jo != NULL, "JO profile exists");
    CHECK(jo->id == NOVA_GAME_JO, "JO id");
    CHECK(bhd != NULL && bhd->id == NOVA_GAME_BHD, "BHD profile exists");
    CHECK(gameprofile_by_id(-1) == NULL, "unknown id -> NULL");
    CHECK(gameprofile_by_id(NOVA_GAME_COUNT) == NULL, "out-of-range id -> NULL");
    return 1;
}

static int test_at_bounds(void) {
    CHECK(gameprofile_at(-1) == NULL, "negative index -> NULL");
    CHECK(gameprofile_at(gameprofile_count()) == NULL, "past-end index -> NULL");
    CHECK(gameprofile_at(0) != NULL && gameprofile_at(0)->id == NOVA_GAME_JO, "index 0 is JO");
    return 1;
}

static int test_universal_key_and_labels(void) {
    int i;
    for (i = 0; i < gameprofile_count(); ++i) {
        const NovaGameProfile *p = gameprofile_at(i);
        CHECK(p != NULL, "profile not null");
        CHECK(p->container_key == 0x0312A4CEu, "container key is the universal 0x0312A4CE");
        CHECK(p->scr_policy == SCR_POLICY_VERSION_DETECT, "scr policy version-detect");
        CHECK(p->bfc1_compress == 0, "no bfc1 compression");
        CHECK(p->default_format == 0, "default format PFF3");
        CHECK(p->display_name != NULL && p->display_name[0] != '\0', "display name non-empty");
    }
    return 1;
}

int main(void) {
    RUN_TEST(test_count);
    RUN_TEST(test_by_id);
    RUN_TEST(test_at_bounds);
    RUN_TEST(test_universal_key_and_labels);

    printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
