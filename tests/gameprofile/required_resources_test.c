/* Pins for the ENG-6 required-resources manifest: the fatal set, ordering,
 * and per-row completeness against docs/required-resources.md (the witness
 * source — a change there lands here in the same change). */
#include <stdio.h>
#include <string.h>

#include "gameprofile/required_resources.h"

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

static int test_bounds(void) {
    CHECK(gameprofile_required_resource_count() > 0, "table non-empty");
    CHECK(gameprofile_required_resource_at(-1) == NULL, "negative index -> NULL");
    CHECK(gameprofile_required_resource_at(gameprofile_required_resource_count()) == NULL,
          "past-end index -> NULL");
    CHECK(gameprofile_required_resource_find(NULL) == NULL, "NULL name -> NULL");
    CHECK(gameprofile_required_resource_find("not-a-resource.xyz") == NULL,
          "unknown name -> NULL");
    return 1;
}

static int test_every_row_is_complete_and_phase_ordered(void) {
    int last_phase = NOVA_BOOT_PHASE_BOOT;
    for (int i = 0; i < gameprofile_required_resource_count(); ++i) {
        const NovaRequiredResource *row = gameprofile_required_resource_at(i);
        CHECK(row != NULL, "row not NULL");
        CHECK(row->name != NULL && row->name[0] != '\0', "row has a name");
        CHECK(row->failure != NULL && row->failure[0] != '\0', "row has failure behavior");
        CHECK(row->orig != NULL && strncmp(row->orig, "[orig:", 6) == 0,
              "row carries an [orig: ...] citation");
        CHECK(row->phase >= NOVA_BOOT_PHASE_BOOT && row->phase <= NOVA_BOOT_PHASE_MISSION,
              "phase in range");
        CHECK(row->phase >= last_phase, "rows are phase-major in load order");
        last_phase = row->phase;
        CHECK(row->severity >= NOVA_RES_FATAL && row->severity <= NOVA_RES_OPTIONAL,
              "severity in range");
    }
    return 1;
}

static int test_names_are_unique(void) {
    for (int i = 0; i < gameprofile_required_resource_count(); ++i) {
        const NovaRequiredResource *a = gameprofile_required_resource_at(i);
        CHECK(gameprofile_required_resource_find(a->name) == a,
              "find(name) resolves to the first (only) row with that name");
    }
    return 1;
}

static int test_the_witnessed_fatal_set(void) {
    /* The record's fatal set, exactly: the three boot-table archives
     * (all-missing fatal), the three string bins, items.def (fatal wired),
     * and main.mnu (silent dead-end). */
    static const char *fatal_names[] = {
        "resource.pff", "localres.pff", "language.pff",
        "gametext.bin", "vmacros.bin", "keyhelp.bin",
        "items.def", "main.mnu",
    };
    int fatal_count = 0;
    for (int i = 0; i < gameprofile_required_resource_count(); ++i) {
        if (gameprofile_required_resource_at(i)->severity == NOVA_RES_FATAL) {
            ++fatal_count;
        }
    }
    CHECK(fatal_count == 8, "exactly the eight witnessed fatal rows");
    for (int i = 0; i < 8; ++i) {
        const NovaRequiredResource *row = gameprofile_required_resource_find(fatal_names[i]);
        CHECK(row != NULL, "fatal row present");
        CHECK(row->severity == NOVA_RES_FATAL, "fatal severity");
    }
    /* The archive-table trio carries the any-of semantics; the file rows do not. */
    CHECK(gameprofile_required_resource_find("resource.pff")->flags & NOVA_RES_F_PFF_TABLE_ANY,
          "resource.pff is an any-of table member");
    CHECK(gameprofile_required_resource_find("language.pff")->flags & NOVA_RES_F_PFF_TABLE_ANY,
          "language.pff is an any-of table member");
    CHECK((gameprofile_required_resource_find("gametext.bin")->flags & NOVA_RES_F_PFF_TABLE_ANY) == 0,
          "gametext.bin is an individually fatal file");
    /* main.mnu is fatal in the MENU phase; the rest of the set is BOOT. */
    CHECK(gameprofile_required_resource_find("main.mnu")->phase == NOVA_BOOT_PHASE_MENU,
          "main.mnu is the menu-phase fatal");
    CHECK(gameprofile_required_resource_find("items.def")->phase == NOVA_BOOT_PHASE_BOOT,
          "items.def is boot-phase");
    return 1;
}

static int test_known_row_lookups(void) {
    const NovaRequiredResource *gameerr = gameprofile_required_resource_find("gameerr.bin");
    CHECK(gameerr != NULL && gameerr->severity == NOVA_RES_DIALOG,
          "gameerr.bin sits just below fatal (dialog, then continue)");
    const NovaRequiredResource *fonts = gameprofile_required_resource_find("ARIAL12B.FNT");
    CHECK(fonts != NULL, "lookups are case-insensitive");
    CHECK(fonts->severity == NOVA_RES_REQUIRED && fonts->phase == NOVA_BOOT_PHASE_MENU,
          "the hardcoded HUD font set is menu-phase required");
    const NovaRequiredResource *failsafe = gameprofile_required_resource_find("failsafe.bad");
    CHECK(failsafe != NULL && failsafe->phase == NOVA_BOOT_PHASE_MISSION &&
              failsafe->severity == NOVA_RES_REQUIRED,
          "failsafe.bad is the mission-phase anim fallback");
    const NovaRequiredResource *scan = gameprofile_required_resource_find("*.npj/*.npz");
    CHECK(scan != NULL && (scan->flags & NOVA_RES_F_PATTERN) != 0,
          "the mission-list wildcard scan is a pattern row");
    return 1;
}

int main(void) {
    RUN_TEST(test_bounds);
    RUN_TEST(test_every_row_is_complete_and_phase_ordered);
    RUN_TEST(test_names_are_unique);
    RUN_TEST(test_the_witnessed_fatal_set);
    RUN_TEST(test_known_row_lookups);
    printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
