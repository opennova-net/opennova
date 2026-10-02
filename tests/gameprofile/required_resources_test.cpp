/* Pins for the ENG-6 required-resources manifest: the fatal set, ordering,
 * and per-row completeness against docs/required-resources.md (the witness
 * source — a change there lands here in the same change). */
#include <stdio.h>
#include <string.h>

#include <base/gameprofile/required_resources.h>

using namespace opennova::gameprofile;

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
    int last_phase = BOOT_PHASE_BOOT;
    for (int i = 0; i < gameprofile_required_resource_count(); ++i) {
        const RequiredResource *row = gameprofile_required_resource_at(i);
        CHECK(row != NULL, "row not NULL");
        CHECK(row->name != NULL && row->name[0] != '\0', "row has a name");
        CHECK(row->failure != NULL && row->failure[0] != '\0', "row has failure behavior");
        CHECK(row->orig != NULL && strncmp(row->orig, "[orig:", 6) == 0,
              "row carries an [orig: ...] citation");
        CHECK(row->phase >= BOOT_PHASE_BOOT && row->phase <= BOOT_PHASE_MISSION,
              "phase in range");
        CHECK(row->phase >= last_phase, "rows are phase-major in load order");
        last_phase = row->phase;
        CHECK(row->severity >= RES_FATAL && row->severity <= RES_OPTIONAL,
              "severity in range");
    }
    return 1;
}

static int test_names_are_unique(void) {
    for (int i = 0; i < gameprofile_required_resource_count(); ++i) {
        const RequiredResource *a = gameprofile_required_resource_at(i);
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
        if (gameprofile_required_resource_at(i)->severity == RES_FATAL) {
            ++fatal_count;
        }
    }
    CHECK(fatal_count == 8, "exactly the eight witnessed fatal rows");
    for (int i = 0; i < 8; ++i) {
        const RequiredResource *row = gameprofile_required_resource_find(fatal_names[i]);
        CHECK(row != NULL, "fatal row present");
        CHECK(row->severity == RES_FATAL, "fatal severity");
    }
    /* The archive-table trio carries the any-of semantics; the file rows do not. */
    CHECK(gameprofile_required_resource_find("resource.pff")->flags & RES_F_PFF_TABLE_ANY,
          "resource.pff is an any-of table member");
    CHECK(gameprofile_required_resource_find("language.pff")->flags & RES_F_PFF_TABLE_ANY,
          "language.pff is an any-of table member");
    CHECK((gameprofile_required_resource_find("gametext.bin")->flags & RES_F_PFF_TABLE_ANY) == 0,
          "gametext.bin is an individually fatal file");
    /* main.mnu is fatal in the MENU phase; the rest of the set is BOOT. */
    CHECK(gameprofile_required_resource_find("main.mnu")->phase == BOOT_PHASE_MENU,
          "main.mnu is the menu-phase fatal");
    CHECK(gameprofile_required_resource_find("items.def")->phase == BOOT_PHASE_BOOT,
          "items.def is boot-phase");
    return 1;
}

static int test_known_row_lookups(void) {
    const RequiredResource *gameerr = gameprofile_required_resource_find("gameerr.bin");
    CHECK(gameerr != NULL && gameerr->severity == RES_DIALOG,
          "gameerr.bin sits just below fatal (dialog, then continue)");
    const RequiredResource *fonts = gameprofile_required_resource_find("ARIAL12B.FNT");
    CHECK(fonts != NULL, "lookups are case-insensitive");
    CHECK(fonts->severity == RES_REQUIRED && fonts->phase == BOOT_PHASE_MENU,
          "the hardcoded HUD font set is menu-phase required");
    const RequiredResource *failsafe = gameprofile_required_resource_find("failsafe.bad");
    CHECK(failsafe != NULL && failsafe->phase == BOOT_PHASE_MISSION &&
              failsafe->severity == RES_OPTIONAL,
          "failsafe.bad is the mission-phase anim fallback, optional (retail JO ships none)");
    const RequiredResource *scan = gameprofile_required_resource_find("*.npj/*.npz");
    CHECK(scan != NULL && (scan->flags & RES_F_PATTERN) != 0,
          "the mission-list wildcard scan is a pattern row");
    /* The player's and this machine's own files carry RES_F_PLAYER_FILE, and only they. */
    static const char *const player_names[] = {"game.cfg", "assets.cd", "filter.txt", "gt.ssc", "hiscore.txt",
                                               "admin.cfg", "player.sav", "weapon.sav", "epass.bin", "passgen.bin"};
    int players = 0;
    for (int i = 0; i < gameprofile_required_resource_count(); ++i)
        players += (gameprofile_required_resource_at(i)->flags & RES_F_PLAYER_FILE) != 0;
    CHECK(players == 10, "exactly the ten player-file rows");
    for (const char *name : player_names)
        CHECK(gameprofile_required_resource_find(name)->flags & RES_F_PLAYER_FILE, "a player's own file is flagged");
    CHECK((gameprofile_required_resource_find("CC.BIN")->flags & RES_F_PLAYER_FILE) == 0 &&
              (gameprofile_required_resource_find("items.def")->flags & RES_F_PLAYER_FILE) == 0,
          "a resource the game ships is not");
    return 1;
}

static int test_roles_are_unique_snake_case_tokens(void) {
    /* The editor keys its requirements checklist on the role token (ADR 0046 d5/d7):
     * one per row, lower-case snake_case, never empty, never shared. */
    for (int i = 0; i < gameprofile_required_resource_count(); ++i) {
        const RequiredResource *row = gameprofile_required_resource_at(i);
        CHECK(row->role != NULL && row->role[0] != '\0', "row has a role token");
        for (const char *c = row->role; *c; ++c) {
            CHECK((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_',
                  "role token is lower-case snake_case");
        }
        CHECK(gameprofile_required_resource_by_role(row->role) == row,
              "by_role(role) resolves to its own row (tokens are unique)");
    }
    CHECK(gameprofile_required_resource_by_role(NULL) == NULL, "NULL role -> NULL");
    CHECK(gameprofile_required_resource_by_role("not_a_role") == NULL, "unknown role -> NULL");
    CHECK(gameprofile_required_resource_by_role("MAIN_MENU") == NULL, "roles match exactly");
    const RequiredResource *menu = gameprofile_required_resource_by_role("main_menu");
    CHECK(menu != NULL && strcmp(menu->name, "main.mnu") == 0, "main_menu is main.mnu");
    const RequiredResource *strings = gameprofile_required_resource_by_role("gametext");
    CHECK(strings != NULL && strcmp(strings->name, "gametext.bin") == 0, "gametext is gametext.bin");
    return 1;
}

int main(void) {
    RUN_TEST(test_bounds);
    RUN_TEST(test_every_row_is_complete_and_phase_ordered);
    RUN_TEST(test_names_are_unique);
    RUN_TEST(test_the_witnessed_fatal_set);
    RUN_TEST(test_known_row_lookups);
    RUN_TEST(test_roles_are_unique_snake_case_tokens);
    printf("%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
