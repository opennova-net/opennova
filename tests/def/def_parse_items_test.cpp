// Test parsing items.def — spot-check "dbuggy1" and "Player #1" entries.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "def/def.h"
#include "common/test_paths.h"

int main(void) {
    const char *repo_root = test_paths_repo_root(__FILE__);
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/def/items.def", repo_root);

    DefItemsFile items;
    memset(&items, 0, sizeof(items));
    if (def_parse_items(path, &items) != 0) {
        fprintf(stderr, "FAIL: def_parse_items failed for %s\n", path);
        return 1;
    }

    if (items.count < 10) {
        fprintf(stderr, "FAIL: too few entries: %zu\n", items.count);
        def_free_items(&items);
        return 1;
    }

    printf("Parsed %zu items\n", items.count);

    /* Find the Dune Buggy entry by sid */
    const DefItemDef *buggy = NULL;
    for (size_t i = 0; i < items.count; ++i) {
        if (strcmp(items.entries[i].sid, "dbuggy1") == 0) {
            buggy = &items.entries[i];
            break;
        }
    }

    if (!buggy) {
        fprintf(stderr, "FAIL: could not find dbuggy1 entry\n");
        def_free_items(&items);
        return 1;
    }

    if (strcmp(buggy->display_name, "Drivable Dune Buggy") != 0) {
        fprintf(stderr, "FAIL: buggy display_name mismatch: '%s'\n", buggy->display_name);
        def_free_items(&items);
        return 1;
    }

    /* type 2 = Vehicle */
    if (buggy->type != 2) {
        fprintf(stderr, "FAIL: buggy type mismatch: expected 2 (Vehicle), got %d\n", buggy->type);
        def_free_items(&items);
        return 1;
    }

    if (strcmp(buggy->graphic, "Dbuggy1") != 0) {
        fprintf(stderr, "FAIL: buggy graphic mismatch: '%s'\n", buggy->graphic);
        def_free_items(&items);
        return 1;
    }

    if (strcmp(buggy->husk, "Dbuggy1X") != 0) {
        fprintf(stderr, "FAIL: buggy husk mismatch: '%s'\n", buggy->husk);
        def_free_items(&items);
        return 1;
    }

    if (buggy->hp != 3000) {
        fprintf(stderr, "FAIL: buggy hp mismatch: expected 3000, got %d\n", buggy->hp);
        def_free_items(&items);
        return 1;
    }

    if (buggy->id != 101291) {
        fprintf(stderr, "FAIL: buggy id mismatch: expected 101291, got %d\n", buggy->id);
        def_free_items(&items);
        return 1;
    }

    /* Find "Player #1, Single player" by id */
    const DefItemDef *player1 = NULL;
    for (size_t i = 0; i < items.count; ++i) {
        if (items.entries[i].id == 105310) {
            player1 = &items.entries[i];
            break;
        }
    }

    if (!player1) {
        fprintf(stderr, "FAIL: could not find Player #1, Single player (id 105310)\n");
        def_free_items(&items);
        return 1;
    }

    if (strcmp(player1->display_name, "Player #1, Single player") != 0) {
        fprintf(stderr, "FAIL: player1 display_name mismatch: '%s'\n", player1->display_name);
        def_free_items(&items);
        return 1;
    }

    /* type 3 = Person */
    if (player1->type != 3) {
        fprintf(stderr, "FAIL: player1 type mismatch: expected 3 (Person), got %d\n", player1->type);
        def_free_items(&items);
        return 1;
    }

    /* graphic should be "US01", not overridden by "graphicenemy Indo01" */
    if (strcmp(player1->graphic, "US01") != 0) {
        fprintf(stderr, "FAIL: player1 graphic mismatch: expected 'US01', got '%s'\n",
                player1->graphic);
        fprintf(stderr, "  (bug: 'graphicenemy' may have overridden 'graphic')\n");
        def_free_items(&items);
        return 1;
    }

    if (strcmp(player1->anim_def, "US01") != 0) {
        fprintf(stderr, "FAIL: player1 anim_def mismatch: '%s'\n", player1->anim_def);
        def_free_items(&items);
        return 1;
    }

    /* sound_profile should be "SP_JO_SP_PlayerM1", not overridden by sound_profileFemale */
    if (strcmp(player1->sound_profile, "SP_JO_SP_PlayerM1") != 0) {
        fprintf(stderr, "FAIL: player1 sound_profile mismatch: expected 'SP_JO_SP_PlayerM1', got '%s'\n",
                player1->sound_profile);
        def_free_items(&items);
        return 1;
    }

    if (player1->hp != 150) {
        fprintf(stderr, "FAIL: player1 hp mismatch: expected 150, got %d\n", player1->hp);
        def_free_items(&items);
        return 1;
    }

    def_free_items(&items);

    snprintf(path, sizeof(path), "%s/fixtures/lw/dflw/badguy/ITEMS.DEF", repo_root);
    memset(&items, 0, sizeof(items));
    if (def_parse_items(path, &items) != 0) {
        fprintf(stderr, "FAIL: def_parse_items failed for %s\n", path);
        return 1;
    }

    const DefItemDef *badguy = NULL;
    for (size_t i = 0; i < items.count; ++i) {
        if (items.entries[i].id == 105130) {
            badguy = &items.entries[i];
            break;
        }
    }

    if (!badguy) {
        fprintf(stderr, "FAIL: could not find DFLW badguy item id 105130\n");
        def_free_items(&items);
        return 1;
    }
    if (strcmp(badguy->display_name, "Change me please!!!") != 0 ||
        strcmp(badguy->graphic, "badguy") != 0 ||
        strcmp(badguy->chr_file, "player01") != 0 ||
        strcmp(badguy->anim_def, "enemy00") != 0 ||
        badguy->type != 3) {
        fprintf(stderr,
                "FAIL: DFLW badguy mismatch name='%s' graphic='%s' chr_file='%s' anim_def='%s' type=%d\n",
                badguy->display_name,
                badguy->graphic,
                badguy->chr_file,
                badguy->anim_def,
                badguy->type);
        def_free_items(&items);
        return 1;
    }

    def_free_items(&items);
    printf("PASS: items parsing OK\n");
    return 0;
}
