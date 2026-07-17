// Test parsing items.def — spot-check "dbuggy1" and "Player #1" entries, plus
// the per-item particle-effect keys over an inline snippet.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "def/def.h"
#include "common/test_paths.h"

static int expect_str(const char *what, const char *got, const char *want) {
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL: %s mismatch: expected '%s', got '%s'\n", what, want, got);
        return 1;
    }
    return 0;
}

/* Per-item particle-effect keys [orig: ItemDef_ParseProperty @ 0x49eb00,
   particlefx chain @ 0x4a13ad..0x4a179d]: anchored slots take
   <effect> <userpoint> (particlefxs/particlefxw1/particlefxw2 read an optional
   third <secondary_effect> token; particlefx/particlefxw3/particlefxw4 never
   do), the death/h2odeath/fire/other/spawn/finale keys take the effect name
   only, keys match case-insensitively, and extra tokens are ignored. The
   snippet mirrors the retail "Drivable Dune Buggy" rows (JOX ITEMS.DEF). */
static int test_particle_keys(void) {
    static const char snippet[] =
        "begin \"Drivable Dune Buggy\"\n"
        "  id 101291\n"
        "  sid dbuggy1\n"
        "  type vehicle\n"
        /* Leading tab + multi-space separator + trailing spaces, exactly as the
           retail rows are formatted. */
        "\tparticlefx   Effect_whiteExhaust FX00 stray_token\n"
        "  ParticleFXW1 Effect_W1 FX03 Effect_W1S\n"
        "\tparticlefxw2 Effect_DirtWake FX01  \n"
        "  particlefxw3 Effect_W3 FX02 Effect_IgnoredW3\n"
        "  particlefxw4 Effect_W4 FX04\n"
        "  particlefxs Effect_DirtWakeS FX01 Effect_DirtWakeS2\n"
        "  particledeath Effect_Fuelxp3\n"
        "  particleh2odeath Effect_H2OVeExp\n"
        "  particlefire Effect_VehFire extra junk\n"
        "  particleother Effect_SmkNStemNP\n"
        "  particlespawn Effect_Spawn\n"
        "  particlefinale Effect_VehDrtPuftrk\n"
        "end\n"
        "begin \"No Particles\"\n"
        "  id 5\n"
        "  sid plain1\n"
        "  type object\n"
        "end\n"
        "begin \"FXS Two Arg\"\n"
        "  id 6\n"
        "  sid fxs2\n"
        "  particlefxs Effect_OnlyTwo FX07\n"
        "end\n";

    DefItemsFile items;
    memset(&items, 0, sizeof(items));
    if (def_parse_items_memory((const unsigned char *)snippet, sizeof(snippet) - 1, &items) != 0) {
        fprintf(stderr, "FAIL: def_parse_items_memory failed for particle snippet\n");
        return 1;
    }
    int fails = 0;
    if (items.count != 3) {
        fprintf(stderr, "FAIL: particle snippet: expected 3 entries, got %zu\n", items.count);
        def_free_items(&items);
        return 1;
    }

    const DefItemDef *b = &items.entries[0];
    if (b->id != 101291) {
        fprintf(stderr, "FAIL: particle snippet buggy id mismatch: got %d\n", b->id);
        ++fails;
    }
    /* Every line above is a recognized key — none may fall through to raw_lines. */
    if (b->raw_lines_count != 0) {
        fprintf(stderr, "FAIL: particle keys fell through to raw_lines (%zu)\n",
                b->raw_lines_count);
        ++fails;
    }

    /* Slot A two-arg; the third token is IGNORED (particlefx has no secondary). */
    fails += expect_str("particlefx.effect", b->particlefx.effect, "Effect_whiteExhaust");
    fails += expect_str("particlefx.userpoint", b->particlefx.userpoint, "FX00");
    fails += expect_str("particlefx.secondary_effect", b->particlefx.secondary_effect, "");

    /* fxw1 with the optional secondary, matched case-insensitively. */
    fails += expect_str("particlefxw1.effect", b->particlefxw1.effect, "Effect_W1");
    fails += expect_str("particlefxw1.userpoint", b->particlefxw1.userpoint, "FX03");
    fails += expect_str("particlefxw1.secondary_effect", b->particlefxw1.secondary_effect,
                        "Effect_W1S");

    /* fxw2 without the optional secondary. */
    fails += expect_str("particlefxw2.effect", b->particlefxw2.effect, "Effect_DirtWake");
    fails += expect_str("particlefxw2.userpoint", b->particlefxw2.userpoint, "FX01");
    fails += expect_str("particlefxw2.secondary_effect", b->particlefxw2.secondary_effect, "");

    /* fxw3/fxw4 never read a third token [orig: @ 0x4a158b / @ 0x4a15eb]. */
    fails += expect_str("particlefxw3.effect", b->particlefxw3.effect, "Effect_W3");
    fails += expect_str("particlefxw3.userpoint", b->particlefxw3.userpoint, "FX02");
    fails += expect_str("particlefxw3.secondary_effect", b->particlefxw3.secondary_effect, "");
    fails += expect_str("particlefxw4.effect", b->particlefxw4.effect, "Effect_W4");
    fails += expect_str("particlefxw4.userpoint", b->particlefxw4.userpoint, "FX04");
    fails += expect_str("particlefxw4.secondary_effect", b->particlefxw4.secondary_effect, "");

    /* fxs with the third token consumed as the secondary effect. */
    fails += expect_str("particlefxs.effect", b->particlefxs.effect, "Effect_DirtWakeS");
    fails += expect_str("particlefxs.userpoint", b->particlefxs.userpoint, "FX01");
    fails += expect_str("particlefxs.secondary_effect", b->particlefxs.secondary_effect,
                        "Effect_DirtWakeS2");

    /* Effect-only keys; extra tokens after the effect name are ignored. */
    fails += expect_str("particledeath", b->particledeath, "Effect_Fuelxp3");
    fails += expect_str("particleh2odeath", b->particleh2odeath, "Effect_H2OVeExp");
    fails += expect_str("particlefire", b->particlefire, "Effect_VehFire");
    fails += expect_str("particleother", b->particleother, "Effect_SmkNStemNP");
    fails += expect_str("particlespawn", b->particlespawn, "Effect_Spawn");
    fails += expect_str("particlefinale", b->particlefinale, "Effect_VehDrtPuftrk");

    /* An item without any particle keys leaves every field empty. */
    const DefItemDef *plain = &items.entries[1];
    fails += expect_str("plain particlefx.effect", plain->particlefx.effect, "");
    fails += expect_str("plain particlefx.userpoint", plain->particlefx.userpoint, "");
    fails += expect_str("plain particlefxs.effect", plain->particlefxs.effect, "");
    fails += expect_str("plain particlefxs.secondary_effect",
                        plain->particlefxs.secondary_effect, "");
    fails += expect_str("plain particlefxw1.effect", plain->particlefxw1.effect, "");
    fails += expect_str("plain particlefxw2.effect", plain->particlefxw2.effect, "");
    fails += expect_str("plain particlefxw3.effect", plain->particlefxw3.effect, "");
    fails += expect_str("plain particlefxw4.effect", plain->particlefxw4.effect, "");
    fails += expect_str("plain particledeath", plain->particledeath, "");
    fails += expect_str("plain particleh2odeath", plain->particleh2odeath, "");
    fails += expect_str("plain particlefire", plain->particlefire, "");
    fails += expect_str("plain particleother", plain->particleother, "");
    fails += expect_str("plain particlespawn", plain->particlespawn, "");
    fails += expect_str("plain particlefinale", plain->particlefinale, "");

    /* fxs with only two args: the secondary stays empty. */
    const DefItemDef *fxs2 = &items.entries[2];
    fails += expect_str("fxs2 particlefxs.effect", fxs2->particlefxs.effect, "Effect_OnlyTwo");
    fails += expect_str("fxs2 particlefxs.userpoint", fxs2->particlefxs.userpoint, "FX07");
    fails += expect_str("fxs2 particlefxs.secondary_effect",
                        fxs2->particlefxs.secondary_effect, "");

    def_free_items(&items);
    if (fails != 0) {
        fprintf(stderr, "FAIL: %d particle-key check(s) failed\n", fails);
        return 1;
    }
    return 0;
}

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

    /* vehicle = 1, the witnessed engine value [orig: ItemDef_ParseProperty
       @ 0x49eb00; docs/world/itemdef-re.md D-ITEMDEF-1] */
    if (buggy->type != DEF_ITEM_TYPE_VEHICLE) {
        fprintf(stderr, "FAIL: buggy type mismatch: expected 1 (vehicle), got %d\n", buggy->type);
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

    /* Shipped JOX data marks the drivable buggy PlayerControl and anchors its
       running exhaust at FX00. Keep the fixture chain intact so runtime tests
       exercise the same startup gate as retail data. */
    if ((buggy->attrib & 0x40u) == 0) {
        fprintf(stderr, "FAIL: buggy attrib missing PlayerControl (0x40): got 0x%x\n",
                buggy->attrib);
        def_free_items(&items);
        return 1;
    }
    if (expect_str("buggy particlefx.effect", buggy->particlefx.effect,
                   "Effect_whiteExhaust") != 0 ||
        expect_str("buggy particlefx.userpoint", buggy->particlefx.userpoint, "FX00") != 0) {
        def_free_items(&items);
        return 1;
    }

    /* §5.10b class-tag directives. The buggy uses ai_function chel (the engine's
       AI-helicopter family — buggies inherit aircraft-like driving in retail).
       render_function/move_function = cveh. No disk_function on the buggy. */
    if (strcmp(buggy->ai_function, "chel") != 0) {
        fprintf(stderr, "FAIL: buggy ai_function mismatch: expected 'chel', got '%s'\n",
                buggy->ai_function);
        def_free_items(&items);
        return 1;
    }
    if (strcmp(buggy->move_function, "cveh") != 0) {
        fprintf(stderr, "FAIL: buggy move_function mismatch: expected 'cveh', got '%s'\n",
                buggy->move_function);
        def_free_items(&items);
        return 1;
    }
    if (strcmp(buggy->render_function, "cveh") != 0) {
        fprintf(stderr, "FAIL: buggy render_function mismatch: expected 'cveh', got '%s'\n",
                buggy->render_function);
        def_free_items(&items);
        return 1;
    }
    if (buggy->disk_function[0] != '\0') {
        fprintf(stderr, "FAIL: buggy disk_function should be empty, got '%s'\n",
                buggy->disk_function);
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

    /* person = 3 [orig: ItemDef_ParseProperty @ 0x49eb00] */
    if (player1->type != DEF_ITEM_TYPE_PERSON) {
        fprintf(stderr, "FAIL: player1 type mismatch: expected 3 (person), got %d\n", player1->type);
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

    /* The player's ai_function is `plyr` — the §5.10b dispatch tag that selects
       NetPacket_SerializePlayerState. move_function is `org2` (a movement-family
       variant); disk_function is the load-class string `PLAYER`. */
    if (strcmp(player1->ai_function, "plyr") != 0) {
        fprintf(stderr, "FAIL: player1 ai_function mismatch: expected 'plyr', got '%s'\n",
                player1->ai_function);
        def_free_items(&items);
        return 1;
    }
    if (strcmp(player1->move_function, "org2") != 0) {
        fprintf(stderr, "FAIL: player1 move_function mismatch: expected 'org2', got '%s'\n",
                player1->move_function);
        def_free_items(&items);
        return 1;
    }
    if (strcmp(player1->disk_function, "PLAYER") != 0) {
        fprintf(stderr, "FAIL: player1 disk_function mismatch: expected 'PLAYER', got '%s'\n",
                player1->disk_function);
        def_free_items(&items);
        return 1;
    }

    /* AI infantry: Generic Soldier (id 105311) uses ai_function org1 = the
       §5.10b dispatch tag that selects NetPacket_SerializeInfantryEntityState. */
    const DefItemDef *soldier = NULL;
    for (size_t i = 0; i < items.count; ++i) {
        if (items.entries[i].id == 105311) {
            soldier = &items.entries[i];
            break;
        }
    }
    if (!soldier) {
        fprintf(stderr, "FAIL: could not find Generic Soldier (id 105311)\n");
        def_free_items(&items);
        return 1;
    }
    if (strcmp(soldier->ai_function, "org1") != 0) {
        fprintf(stderr, "FAIL: soldier ai_function mismatch: expected 'org1', got '%s'\n",
                soldier->ai_function);
        def_free_items(&items);
        return 1;
    }
    if (strcmp(soldier->move_function, "org1") != 0) {
        fprintf(stderr, "FAIL: soldier move_function mismatch: expected 'org1', got '%s'\n",
                soldier->move_function);
        def_free_items(&items);
        return 1;
    }

    /* The anim-fire weapon family + clipsize default: absent keys leave the fields
       zeroed (the fixture predates the JO character-item authoring). */
    if (soldier->ammo_closeattack[0] != '\0' || soldier->clipsize != 0) {
        fprintf(stderr, "FAIL: soldier ammo/clipsize should be unset, got '%s'/%d\n",
                soldier->ammo_closeattack, soldier->clipsize);
        def_free_items(&items);
        return 1;
    }

    def_free_items(&items);

    if (test_particle_keys() != 0) {
        return 1;
    }


    /* The person-item anim-fire weapon family (world-wac-ai-re §17.4, D-AI-5):
       'ammo_closeattack' name -> def+0x56B, 'clipsize' atol -> def+0x894
       [orig: ItemDef_ParseProperty @ 0x4a1823 / @ 0x49fa1c]. The tracked fixture
       has no character items, so pin the parse on an inline JOX-shaped block
       (retail "Indonesian Soldier #1 with AK47", id 101798). */
    static const char rifleman_def[] =
        "begin \"Indonesian Soldier #1 with AK47\"\n"
        "  id 101798\n"
        "  type person\n"
        "  ai_function org1\n"
        "  move_function org1\n"
        "  clipsize 30\n"
        "  ammo_closeattack    AMMO_AK47_556MM\n"
        "  ammo_easyrocket     AMMO_AK47_556MM\n"
        "  ammo_advancedrocket AMMO_AK47_556MM\n"
        "  ammo_marker3        AMMO_AK47_556MM\n"
        "  launchups_closeattack    mflash01\n"
        "end\n";
    DefItemsFile rifle_items;
    memset(&rifle_items, 0, sizeof(rifle_items));
    if (def_parse_items_memory((const uint8_t *)rifleman_def, sizeof(rifleman_def) - 1,
                               &rifle_items) != 0 ||
        rifle_items.count != 1) {
        fprintf(stderr, "FAIL: inline rifleman block did not parse\n");
        def_free_items(&rifle_items);
        return 1;
    }
    if (strcmp(rifle_items.entries[0].ammo_closeattack, "AMMO_AK47_556MM") != 0) {
        fprintf(stderr, "FAIL: rifleman ammo_closeattack mismatch: '%s'\n",
                rifle_items.entries[0].ammo_closeattack);
        def_free_items(&rifle_items);
        return 1;
    }
    if (rifle_items.entries[0].clipsize != 30) {
        fprintf(stderr, "FAIL: rifleman clipsize mismatch: expected 30, got %d\n",
                rifle_items.entries[0].clipsize);
        def_free_items(&rifle_items);
        return 1;
    }
    def_free_items(&rifle_items);

    /* 'primary_weapon' — the ewep emplacement's mounted weapon.def entry (the attach
       label's text source) [orig: -> ItemDef+0x54B primaryWeapon, itemdef-re.md]. The
       fixture carries the retail "NON-Armored Emplaced 50cal for FAV" block (id 101419). */
    {
        DefItemsFile items2;
        memset(&items2, 0, sizeof(items2));
        snprintf(path, sizeof(path), "%s/fixtures/def/items.def", repo_root);
        if (def_parse_items(path, &items2) != 0) {
            fprintf(stderr, "FAIL: reparse for primary_weapon failed\n");
            return 1;
        }
        const DefItemDef *ewep = NULL;
        for (size_t i = 0; i < items2.count; ++i) {
            if (items2.entries[i].id == 101419) { ewep = &items2.entries[i]; break; }
        }
        if (!ewep || strcmp(ewep->primary_weapon, "WPN_EMPLCD50NA") != 0 ||
            strcmp(ewep->ai_function, "ewep") != 0) {
            fprintf(stderr, "FAIL: ewep primary_weapon: '%s' ai='%s' (found=%d)\n",
                    ewep ? ewep->primary_weapon : "", ewep ? ewep->ai_function : "",
                    ewep != NULL);
            def_free_items(&items2);
            return 1;
        }
        def_free_items(&items2);
    }

    printf("PASS: items parsing OK\n");
    return 0;
}
