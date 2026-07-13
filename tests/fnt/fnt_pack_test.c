/* Unit tests for fnt_pack_shelf — the deterministic authoring-side shelf
 * packer (ENG-4). The layout walk expectations are hand-computed from the
 * documented policy: first-fit shelves, FNT_PACK_PAD gutters, input order. */
#include <stdio.h>
#include <string.h>

#include "fnt/fnt.h"

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

static int test_null_args(void) {
    fnt_pack_size_t size = { 4, 4 };
    fnt_pack_rect_t rect;
    uint32_t pages = 0;
    CHECK(fnt_pack_shelf(NULL, 1, &rect, &pages) == FNT_ERR_NULL_POINTER, "NULL sizes");
    CHECK(fnt_pack_shelf(&size, 1, NULL, &pages) == FNT_ERR_NULL_POINTER, "NULL rects");
    CHECK(fnt_pack_shelf(&size, 1, &rect, NULL) == FNT_ERR_NULL_POINTER, "NULL page count");
    return 1;
}

static int test_uniform_glyph_layout(void) {
    /* 224 cells of 10x12: 23 cells per shelf (cx = 1+11k <= 245), 19 shelves
     * per page (cy = 1+13m <= 243) -> all fit on page 0. */
    enum { N = FNT_GLYPH_COUNT };
    fnt_pack_size_t sizes[N];
    fnt_pack_rect_t rects[N];
    uint32_t pages = 0;
    for (int i = 0; i < N; ++i) {
        sizes[i].width = 10;
        sizes[i].height = 12;
    }
    CHECK(fnt_pack_shelf(sizes, N, rects, &pages) == FNT_OK, "pack ok");
    CHECK(pages == 1, "one page");
    CHECK(rects[0].page == 0 && rects[0].x == 1 && rects[0].y == 1, "first cell at (1,1)");
    CHECK(rects[0].width == 10 && rects[0].height == 12, "size preserved");
    CHECK(rects[22].x == 1 + 11 * 22 && rects[22].y == 1, "last cell of shelf 0");
    CHECK(rects[23].x == 1 && rects[23].y == 14, "shelf 1 starts at (1,14)");
    for (int i = 0; i < N; ++i) {
        CHECK(rects[i].x + rects[i].width + FNT_PACK_PAD <= FNT_TEXTURE_WIDTH, "right bound");
        CHECK(rects[i].y + rects[i].height + FNT_PACK_PAD <= FNT_TEXTURE_HEIGHT, "bottom bound");
    }
    return 1;
}

static int test_empty_cells_consume_no_space(void) {
    fnt_pack_size_t sizes[3] = { { 5, 5 }, { 0, 0 }, { 7, 5 } };
    fnt_pack_rect_t rects[3];
    uint32_t pages = 0;
    CHECK(fnt_pack_shelf(sizes, 3, rects, &pages) == FNT_OK, "pack ok");
    CHECK(rects[1].page == 0 && rects[1].x == 0 && rects[1].y == 0 &&
          rects[1].width == 0 && rects[1].height == 0, "empty cell -> zero rect");
    CHECK(rects[2].x == 7 && rects[2].y == 1, "cell after empty continues the shelf");
    CHECK(pages == 1, "one page");
    return 1;
}

static int test_oversize_clamps_and_paginates(void) {
    /* A 300x300 cell clamps to 254x254 and fills page 0; the next cell
     * cannot fit a new shelf on page 0 and opens page 1. */
    fnt_pack_size_t sizes[2] = { { 300, 300 }, { 10, 10 } };
    fnt_pack_rect_t rects[2];
    uint32_t pages = 0;
    CHECK(fnt_pack_shelf(sizes, 2, rects, &pages) == FNT_OK, "pack ok");
    CHECK(rects[0].width == FNT_TEXTURE_WIDTH - 2 * FNT_PACK_PAD, "width clamped");
    CHECK(rects[0].height == FNT_TEXTURE_HEIGHT - 2 * FNT_PACK_PAD, "height clamped");
    CHECK(rects[0].page == 0 && rects[0].x == 1 && rects[0].y == 1, "clamped cell at (0,1,1)");
    CHECK(rects[1].page == 1 && rects[1].x == 1 && rects[1].y == 1, "next cell opens page 1");
    CHECK(pages == 2, "two pages");
    return 1;
}

static int test_page_overflow_is_an_error(void) {
    /* One full-page cell per page: 16 fit exactly; the 17th exceeds
     * FNT_MAX_PAGES and must fail. */
    enum { FULL = FNT_MAX_PAGES + 1 };
    fnt_pack_size_t sizes[FULL];
    fnt_pack_rect_t rects[FULL];
    uint32_t pages = 0;
    for (int i = 0; i < FULL; ++i) {
        sizes[i].width = 254;
        sizes[i].height = 254;
    }
    CHECK(fnt_pack_shelf(sizes, FNT_MAX_PAGES, rects, &pages) == FNT_OK, "16 pages ok");
    CHECK(pages == FNT_MAX_PAGES, "exactly FNT_MAX_PAGES pages");
    CHECK(fnt_pack_shelf(sizes, FULL, rects, &pages) == FNT_ERR_INVALID_PAGE_COUNT,
          "17th full page -> error");
    return 1;
}

static int test_reference_layout_walk(void) {
    /* Hand-walked irregular sequence (the GDScript _pack parity vector):
     * (100,20) -> (0,1,1); (100,30) -> (0,102,1); (60,10) wraps to shelf 1
     * at cy=32; (200,40) wraps to shelf 2 at cy=43; empty; (30,50) continues
     * shelf 2 at x=202. */
    fnt_pack_size_t sizes[6] = {
        { 100, 20 }, { 100, 30 }, { 60, 10 }, { 200, 40 }, { 0, 0 }, { 30, 50 }
    };
    fnt_pack_rect_t rects[6];
    uint32_t pages = 0;
    CHECK(fnt_pack_shelf(sizes, 6, rects, &pages) == FNT_OK, "pack ok");
    CHECK(rects[0].x == 1 && rects[0].y == 1, "c0");
    CHECK(rects[1].x == 102 && rects[1].y == 1, "c1");
    CHECK(rects[2].x == 1 && rects[2].y == 32, "c2 wraps below the 30-high shelf");
    CHECK(rects[3].x == 1 && rects[3].y == 43, "c3 wraps below the 10-high shelf");
    CHECK(rects[4].width == 0, "c4 empty");
    CHECK(rects[5].x == 202 && rects[5].y == 43, "c5 continues shelf 2");
    CHECK(pages == 1, "one page");
    return 1;
}

int main(void) {
    RUN_TEST(test_null_args);
    RUN_TEST(test_uniform_glyph_layout);
    RUN_TEST(test_empty_cells_consume_no_space);
    RUN_TEST(test_oversize_clamps_and_paginates);
    RUN_TEST(test_page_overflow_is_an_error);
    RUN_TEST(test_reference_layout_walk);
    printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
