// Test parsing weapon.def — check WPN_M4AUTO fields, pos/tpos, sights, actions.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "def/def.h"
#include "common/test_paths.h"

#define FEPS 0.01f

int main(void) {
    const char *repo_root = test_paths_repo_root(__FILE__);
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/def/weapon.def", repo_root);

    DefWeaponsFile wf;
    memset(&wf, 0, sizeof(wf));
    if (def_parse_weapons(path, &wf) != 0) {
        fprintf(stderr, "FAIL: def_parse_weapons failed for %s\n", path);
        return 1;
    }

    if (wf.count < 5) {
        fprintf(stderr, "FAIL: too few weapon entries: %zu\n", wf.count);
        def_free_weapons(&wf);
        return 1;
    }

    printf("Parsed %zu weapons\n", wf.count);

    /* Find WPN_M4AUTO */
    const DefWeaponDef *m4 = NULL;
    for (size_t i = 0; i < wf.count; ++i) {
        if (strcmp(wf.entries[i].weapon_name, "WPN_M4AUTO") == 0) {
            m4 = &wf.entries[i];
            break;
        }
    }

    if (!m4) {
        fprintf(stderr, "FAIL: could not find WPN_M4AUTO entry\n");
        def_free_weapons(&wf);
        return 1;
    }

    /* Basic fields */
    if (m4->category != 3) {
        fprintf(stderr, "FAIL: M4AUTO category mismatch: %d\n", m4->category);
        def_free_weapons(&wf);
        return 1;
    }

    if (m4->clipsize != 30) {
        fprintf(stderr, "FAIL: M4AUTO clipsize mismatch: %d\n", m4->clipsize);
        def_free_weapons(&wf);
        return 1;
    }

    if (strcmp(m4->round_type, "AMMO_CAR15_556MM") != 0) {
        fprintf(stderr, "FAIL: M4AUTO round_type mismatch: '%s'\n", m4->round_type);
        def_free_weapons(&wf);
        return 1;
    }

    printf("Basic fields OK\n");

    /* pos parsing (comma-separated): -26.55, 23.25, -165.00, 2.500, 0.750, 356.750 */
    if (fabsf(m4->pos[0] - (-26.55f)) > FEPS ||
        fabsf(m4->pos[1] - 23.25f) > FEPS ||
        fabsf(m4->pos[2] - (-165.00f)) > FEPS) {
        fprintf(stderr, "FAIL: M4AUTO pos XYZ mismatch: %.3f, %.3f, %.3f\n",
                m4->pos[0], m4->pos[1], m4->pos[2]);
        def_free_weapons(&wf);
        return 1;
    }
    if (fabsf(m4->pos[3] - 2.500f) > FEPS ||
        fabsf(m4->pos[4] - 0.750f) > FEPS ||
        fabsf(m4->pos[5] - 356.750f) > FEPS) {
        fprintf(stderr, "FAIL: M4AUTO pos rotation mismatch: %.3f, %.3f, %.3f\n",
                m4->pos[3], m4->pos[4], m4->pos[5]);
        def_free_weapons(&wf);
        return 1;
    }
    printf("pos parsing OK\n");

    /* tpos parsing */
    if (fabsf(m4->tpos[0] - (-52.55f)) > FEPS ||
        fabsf(m4->tpos[1] - 40.50f) > FEPS ||
        fabsf(m4->tpos[2] - (-158.75f)) > FEPS) {
        fprintf(stderr, "FAIL: M4AUTO tpos XYZ mismatch: %.3f, %.3f, %.3f\n",
                m4->tpos[0], m4->tpos[1], m4->tpos[2]);
        def_free_weapons(&wf);
        return 1;
    }
    printf("tpos parsing OK\n");

    /* SIGHTS — should have 3 entries */
    if (m4->sights_count != 3) {
        fprintf(stderr, "FAIL: M4AUTO sights_count mismatch: %zu\n", m4->sights_count);
        def_free_weapons(&wf);
        return 1;
    }

    if (strcmp(m4->sights[0].texture, "car15aim.tga") != 0) {
        fprintf(stderr, "FAIL: M4AUTO sights[0] texture mismatch: '%s'\n", m4->sights[0].texture);
        def_free_weapons(&wf);
        return 1;
    }
    printf("SIGHTS parsing OK\n");

    /* HUDCLIPGFX */
    if (strcmp(m4->hudclipgfx_texture, "H_clip.tga") != 0) {
        fprintf(stderr, "FAIL: M4AUTO hudclipgfx_texture mismatch: '%s'\n", m4->hudclipgfx_texture);
        def_free_weapons(&wf);
        return 1;
    }
    printf("HUDCLIPGFX OK\n");

    /* Actions — should have fire and reload */
    if (m4->actions_count < 2) {
        fprintf(stderr, "FAIL: M4AUTO too few actions: %zu\n", m4->actions_count);
        def_free_weapons(&wf);
        return 1;
    }

    const DefWeaponAction *fire_action = NULL;
    const DefWeaponAction *reload_action = NULL;
    for (size_t i = 0; i < m4->actions_count; ++i) {
        if (strcmp(m4->actions[i].name, "fire") == 0) fire_action = &m4->actions[i];
        if (strcmp(m4->actions[i].name, "reload") == 0) reload_action = &m4->actions[i];
    }

    if (!fire_action || !reload_action) {
        fprintf(stderr, "FAIL: M4AUTO missing fire or reload action\n");
        def_free_weapons(&wf);
        return 1;
    }

    if (fire_action->delayend != 3) {
        fprintf(stderr, "FAIL: fire delayend mismatch: %d\n", fire_action->delayend);
        def_free_weapons(&wf);
        return 1;
    }

    if (strcmp(fire_action->soundsetend, "GS_M4") != 0) {
        fprintf(stderr, "FAIL: fire soundsetend mismatch: '%s'\n", fire_action->soundsetend);
        def_free_weapons(&wf);
        return 1;
    }

    if (strcmp(fire_action->particle, "Effect_CAR15MF") != 0) {
        fprintf(stderr, "FAIL: fire particle mismatch: '%s'\n", fire_action->particle);
        def_free_weapons(&wf);
        return 1;
    }

    if (reload_action->delaystart != 100) {
        fprintf(stderr, "FAIL: reload delaystart mismatch: %d\n", reload_action->delaystart);
        def_free_weapons(&wf);
        return 1;
    }

    /* delayend auto = -1 */
    if (reload_action->delayend != -1) {
        fprintf(stderr, "FAIL: reload delayend mismatch (expected -1 for auto): %d\n",
                reload_action->delayend);
        def_free_weapons(&wf);
        return 1;
    }
    printf("Action parsing OK\n");

    /* Loadout fields (D-PLAYERINFO-11). WPN_M4AUTO: selectable PRIMARY, blue team,
       rifleman|medic|engineer. */
    if (m4->weapon_class_slot != 1) { /* primary */
        fprintf(stderr, "FAIL: M4AUTO weapon_class_slot mismatch: %d\n", m4->weapon_class_slot);
        def_free_weapons(&wf); return 1;
    }
    if (m4->loadout_selectable != 1) {
        fprintf(stderr, "FAIL: M4AUTO loadout_selectable mismatch: %d\n", m4->loadout_selectable);
        def_free_weapons(&wf); return 1;
    }
    if (m4->teamfilter_mask != 2) { /* blue */
        fprintf(stderr, "FAIL: M4AUTO teamfilter_mask mismatch: %d\n", m4->teamfilter_mask);
        def_free_weapons(&wf); return 1;
    }
    if (m4->charfilter_mask != (1 | 8 | 16)) { /* medic|rifleman|engineer */
        fprintf(stderr, "FAIL: M4AUTO charfilter_mask mismatch: %d\n", m4->charfilter_mask);
        def_free_weapons(&wf); return 1;
    }
    if (m4->maxclips != 10) {
        fprintf(stderr, "FAIL: M4AUTO maxclips mismatch: %d\n", m4->maxclips);
        def_free_weapons(&wf); return 1;
    }
    if (fabsf(m4->weaponweight - 5.5f) > FEPS || fabsf(m4->clipweight - 1.5f) > FEPS) {
        fprintf(stderr, "FAIL: M4AUTO weight mismatch: %.3f / %.3f\n", m4->weaponweight, m4->clipweight);
        def_free_weapons(&wf); return 1;
    }
    if (m4->weaponweight_fp16 != 360448 || m4->clipweight_fp16 != 98304) {
        fprintf(stderr, "FAIL: M4AUTO exact weight mismatch: %d / %d\n",
                m4->weaponweight_fp16, m4->clipweight_fp16);
        def_free_weapons(&wf); return 1;
    }
    {
        static const int expected_error[6] = {1081, 13107, 16384, 1081, 2097, 3080};
        for (int i = 0; i < 6; ++i) {
            if (m4->error_fp16[i] != expected_error[i]) {
                fprintf(stderr, "FAIL: M4AUTO exact ERROR[%d]: got %d want %d\n",
                        i, m4->error_fp16[i], expected_error[i]);
                def_free_weapons(&wf); return 1;
            }
        }
        if (fabsf(m4->error[0] - 0.0165f) > FEPS ||
            fabsf(m4->error[5] - 0.047f) > FEPS) {
            fprintf(stderr, "FAIL: M4AUTO float ERROR view changed: %.4f / %.4f\n",
                    m4->error[0], m4->error[5]);
            def_free_weapons(&wf); return 1;
        }
    }
    /* The retail digit walker is observably different from float requantization
       for .35: 22938, not trunc(.35f * 65536) == 22937. */
    {
        const DefWeaponDef *magnum = NULL;
        const DefWeaponDef *mortar = NULL;
        for (size_t i = 0; i < wf.count; ++i) {
            if (strcmp(wf.entries[i].weapon_name, "WPN_357") == 0) magnum = &wf.entries[i];
            if (strcmp(wf.entries[i].weapon_name, "WPN_MORTAR") == 0) mortar = &wf.entries[i];
        }
        if (!magnum || magnum->clipweight_fp16 != 22938) {
            fprintf(stderr, "FAIL: WPN_357 exact clipweight: found=%d value=%d\n",
                    magnum != NULL, magnum ? magnum->clipweight_fp16 : -1);
            def_free_weapons(&wf); return 1;
        }
        if (!mortar || mortar->error_hip_theta_fp16 != 262144 ||
            mortar->error_up_theta_fp16 != 262144) {
            fprintf(stderr, "FAIL: WPN_MORTAR exact theta: found=%d values=%d/%d\n",
                    mortar != NULL, mortar ? mortar->error_hip_theta_fp16 : -1,
                    mortar ? mortar->error_up_theta_fp16 : -1);
            def_free_weapons(&wf); return 1;
        }
    }
    if (strcmp(m4->loadout_menu_textid, "WEAP_SHORT_M4") != 0) {
        fprintf(stderr, "FAIL: M4AUTO loadout_menu_textid mismatch: '%s'\n", m4->loadout_menu_textid);
        def_free_weapons(&wf); return 1;
    }
    printf("Loadout + exact spread/weight fields OK\n");

    /* Also find WPN_KNIFE to verify first entry */
    const DefWeaponDef *knife = NULL;
    for (size_t i = 0; i < wf.count; ++i) {
        if (strcmp(wf.entries[i].weapon_name, "WPN_KNIFE") == 0) {
            knife = &wf.entries[i];
            break;
        }
    }

    if (!knife) {
        fprintf(stderr, "FAIL: could not find WPN_KNIFE entry\n");
        def_free_weapons(&wf);
        return 1;
    }
    printf("WPN_KNIFE found OK\n");

    /* Loadout/armory fields (§5.57) — KNIFE: no-ammo weapon, all-class blue kit item */
    if (knife->statid != 100 || knife->clipsize != -1 || knife->startrounds != -1 ||
        knife->maxclips != 0 || knife->loadout_selectable != 0 || knife->loadout_subclasses != 0) {
        fprintf(stderr, "FAIL: KNIFE armory scalars: statid=%d clipsize=%d startrounds=%d "
                        "maxclips=%d sel=%d sub=%d\n",
                knife->statid, knife->clipsize, knife->startrounds, knife->maxclips,
                knife->loadout_selectable, knife->loadout_subclasses);
        def_free_weapons(&wf);
        return 1;
    }
    if (knife->teamfilter_count != 1 || strcmp(knife->teamfilter[0], "blue") != 0 ||
        knife->charfilter_count != 5 || strcmp(knife->charfilter[0], "medic") != 0 ||
        strcmp(knife->charfilter[4], "engineer") != 0) {
        fprintf(stderr, "FAIL: KNIFE filters: team n=%zu '%s', char n=%zu '%s'..'%s'\n",
                knife->teamfilter_count, knife->teamfilter[0],
                knife->charfilter_count, knife->charfilter[0], knife->charfilter[4]);
        def_free_weapons(&wf);
        return 1;
    }
    if (knife->ammo_class[0] != '\0' || knife->weapon_class[0] != '\0') {
        fprintf(stderr, "FAIL: KNIFE unexpected ammo_class '%s' / weapon_class '%s'\n",
                knife->ammo_class, knife->weapon_class);
        def_free_weapons(&wf);
        return 1;
    }
    printf("KNIFE armory fields OK\n");

    /* colt45: two-team secondary with ammoclass */
    const DefWeaponDef *colt = NULL;
    for (size_t i = 0; i < wf.count; ++i) {
        if (strcmp(wf.entries[i].weapon_name, "WPN_colt45") == 0) { colt = &wf.entries[i]; break; }
    }
    if (!colt) {
        fprintf(stderr, "FAIL: could not find WPN_colt45 entry\n");
        def_free_weapons(&wf);
        return 1;
    }
    if (colt->statid != 200 || colt->maxclips != 5 || colt->clipsize != 7 ||
        colt->startrounds != 35 || colt->loadout_selectable != 1 ||
        colt->loadout_subclasses != 0 || colt->ammo_class_count != 1 ||
        strcmp(colt->ammo_class, "CLASS_45cal") != 0 ||
        strcmp(colt->weapon_class, "secondary") != 0 ||
        colt->teamfilter_count != 2 || strcmp(colt->teamfilter[0], "red") != 0 ||
        strcmp(colt->teamfilter[1], "blue") != 0 || colt->charfilter_count != 4) {
        fprintf(stderr, "FAIL: colt45 armory fields: statid=%d mc=%d cs=%d sr=%d sel=%d sub=%d "
                        "ammo='%s'x%d wclass='%s' team n=%zu char n=%zu\n",
                colt->statid, colt->maxclips, colt->clipsize, colt->startrounds,
                colt->loadout_selectable, colt->loadout_subclasses, colt->ammo_class,
                colt->ammo_class_count, colt->weapon_class, colt->teamfilter_count,
                colt->charfilter_count);
        def_free_weapons(&wf);
        return 1;
    }
    printf("colt45 armory fields OK\n");

    /* M4AUTO: selectable primary with one subclass + ammobucket; M4: bucket-only variant */
    if (m4->statid != 300 || m4->maxclips != 10 || m4->loadout_selectable != 1 ||
        m4->loadout_subclasses != 1 || m4->ammobucket != 1 ||
        strcmp(m4->ammo_class, "CLASS_556MM") != 0 || m4->ammo_class_count != 1 ||
        strcmp(m4->weapon_class, "primary") != 0 ||
        m4->teamfilter_count != 1 || strcmp(m4->teamfilter[0], "blue") != 0 ||
        m4->charfilter_count != 3 || strcmp(m4->charfilter[0], "rifleman") != 0) {
        fprintf(stderr, "FAIL: M4AUTO armory fields: statid=%d mc=%d sel=%d sub=%d bucket=%d "
                        "ammo='%s'x%d wclass='%s' team n=%zu char n=%zu\n",
                m4->statid, m4->maxclips, m4->loadout_selectable, m4->loadout_subclasses,
                m4->ammobucket, m4->ammo_class, m4->ammo_class_count, m4->weapon_class,
                m4->teamfilter_count, m4->charfilter_count);
        def_free_weapons(&wf);
        return 1;
    }
    const DefWeaponDef *m4plain = NULL;
    for (size_t i = 0; i < wf.count; ++i) {
        if (strcmp(wf.entries[i].weapon_name, "WPN_M4") == 0) { m4plain = &wf.entries[i]; break; }
    }
    if (!m4plain || m4plain->statid != 301 || m4plain->ammobucket != 1) {
        fprintf(stderr, "FAIL: WPN_M4 armory fields (found=%d)\n", m4plain != NULL);
        def_free_weapons(&wf);
        return 1;
    }
    printf("M4AUTO/M4 armory fields OK\n");

    /* attachtextid — the attach-label Overlays key on emplaced guns; absent
       everywhere else [orig: WeaponDefs_ParseLineCallback @ 0x544d6c -> AdmDef+0x3A0] */
    {
        const DefWeaponDef *empl = NULL;
        for (size_t i = 0; i < wf.count; ++i) {
            if (strcmp(wf.entries[i].weapon_name, "WPN_EMPLCD50") == 0) {
                empl = &wf.entries[i];
                break;
            }
        }
        if (!empl || strcmp(empl->attach_text_id, "attach_50cal") != 0) {
            fprintf(stderr, "FAIL: EMPLCD50 attach_text_id: '%s' (found=%d)\n",
                    empl ? empl->attach_text_id : "", empl != NULL);
            def_free_weapons(&wf);
            return 1;
        }
        if (m4->attach_text_id[0] != '\0') {
            fprintf(stderr, "FAIL: M4AUTO attach_text_id should be empty: '%s'\n",
                    m4->attach_text_id);
            def_free_weapons(&wf);
            return 1;
        }
    }
    printf("attachtextid OK\n");

    /* Weapon flags — the FULL witnessed token table, both dwords [orig: the
       16-B-stride {name, 0, flags1, flags2} table @ 0x830bf0]: auto = 0x100,
       Sighted = 0x2, WhileSwimming = 0x1000000 (the old 7-entry table aliased it
       onto Underwater's 0x4 — corrected), LaserBeam = 0x40000000 (previously
       unmapped -> raw_lines only), NoAmmoTypes = flags2 0x40.
       [orig: WeaponSlot_CanFireInCurrentState @ 0x53f0b0 auto gate;
       Player_ToggleWeaponScope @ 0x4df0c0 Flags & 3 gate + FOV 80/zoom @ 0x4df401]. */
    if (m4->flags != (0x100 | 0x2 | 0x1000000 | 0x40000000)) {
        fprintf(stderr, "FAIL: M4AUTO flags mismatch: 0x%x\n", m4->flags);
        def_free_weapons(&wf);
        return 1;
    }
    if (m4->flags2 != 0) {
        fprintf(stderr, "FAIL: M4AUTO flags2 mismatch: 0x%x\n", m4->flags2);
        def_free_weapons(&wf);
        return 1;
    }
    /* flags2 tokens land in the SECOND dword (the table's fourth column):
       the shotgun's NoAmmoTypes = flags2 0x40, flags1 untouched by it. */
    {
        const DefWeaponDef *sg = NULL;
        for (size_t i = 0; i < wf.count; ++i) {
            if (strcmp(wf.entries[i].weapon_name, "WPN_RemmingtonSG") == 0) {
                sg = &wf.entries[i];
                break;
            }
        }
        if (!sg || sg->flags2 != 0x40) {
            fprintf(stderr, "FAIL: RemmingtonSG flags2 mismatch (found=%d flags2=0x%x)\n",
                    sg != NULL, sg ? sg->flags2 : 0);
            def_free_weapons(&wf);
            return 1;
        }
    }
    if (fabsf(m4->scope_max_mag - 2.0f) > FEPS) {
        fprintf(stderr, "FAIL: M4AUTO scope_max_mag mismatch: %.3f\n", m4->scope_max_mag);
        def_free_weapons(&wf);
        return 1;
    }
    printf("flags + scope_max_mag OK\n");

    /* 3P body-channel kinds [orig: 'special_hold' -> +0xA4 @ 0x543cb7, 'attack_anim'
       -> +0xA8 @ 0x543ce9]: the knife carries 1/1, the colt pistol 2 with no
       attack_anim key (0), and the rifle carries neither (0/0). */
    {
        const DefWeaponDef *knife = NULL;
        for (size_t i = 0; i < wf.count; ++i) {
            if (strcmp(wf.entries[i].weapon_name, "WPN_KNIFE") == 0) {
                knife = &wf.entries[i];
                break;
            }
        }
        if (!knife || knife->special_hold != 1 || knife->attack_anim != 1 ||
            colt->special_hold != 2 || colt->attack_anim != 0 ||
            m4->special_hold != 0 || m4->attack_anim != 0) {
            fprintf(stderr,
                    "FAIL: body-channel kinds: knife=%d/%d colt=%d/%d m4=%d/%d\n",
                    knife ? knife->special_hold : -1, knife ? knife->attack_anim : -1,
                    colt->special_hold, colt->attack_anim, m4->special_hold,
                    m4->attack_anim);
            def_free_weapons(&wf);
            return 1;
        }
        /* run-gait class [orig: 'run_anim' -> +0xAC @ 0x543d15]: the knife family
           ships 1; rifles omit the key (0). */
        if (knife->run_anim != 1 || m4->run_anim != 0) {
            fprintf(stderr, "FAIL: run_anim: knife=%d m4=%d\n", knife->run_anim,
                    m4->run_anim);
            def_free_weapons(&wf);
            return 1;
        }
    }
    printf("special_hold + attack_anim + run_anim OK\n");

    /* renderfov: no shipped JO weapon.def sets the key, so every entry carries the
       record default 80.0 [orig: AdmDef_InitEntryDefaults @ 0x53ff31]; the parser
       key overrides it [orig: 'renderfov' @ 0x54482a]. */
    if (fabsf(m4->renderfov - 80.0f) > FEPS) {
        fprintf(stderr, "FAIL: M4AUTO renderfov default mismatch: %.3f\n", m4->renderfov);
        def_free_weapons(&wf);
        return 1;
    }
    {
        static const char kFovDef[] =
            "weapon \"WPN_FOVTEST\"\n"
            "\trenderfov 40\n"
            "end\n";
        DefWeaponsFile ff;
        memset(&ff, 0, sizeof(ff));
        if (def_parse_weapons_memory((const unsigned char *)kFovDef, sizeof(kFovDef) - 1, &ff) != 0 ||
            ff.count != 1) {
            fprintf(stderr, "FAIL: renderfov inline parse failed\n");
            def_free_weapons(&ff);
            def_free_weapons(&wf);
            return 1;
        }
        if (fabsf(ff.entries[0].renderfov - 40.0f) > FEPS) {
            fprintf(stderr, "FAIL: renderfov override mismatch: %.3f\n", ff.entries[0].renderfov);
            def_free_weapons(&ff);
            def_free_weapons(&wf);
            return 1;
        }
        def_free_weapons(&ff);
        printf("renderfov default/override OK\n");
    }

    /* The heat model [orig: 'heat_values' @ 0x543eb7 -> +0x36C / +0x370,
       'heat_effect' @ 0x543e36 -> +0x358 / +0x374; Math_ParseFixedPoint16
       @ 0x6131f0]. These four lines are verbatim shipped JOX weapon.def rows
       (WPN_EMPLCD50, WPN_EMPLCDMINI, WPN_EMPLCDGRND, WPN_QUAD50) with their
       authored whitespace, so the expectations below double as a pin on the two
       truncating divides: value/100 for percent-per-shot and value/6200 for
       percent-per-second at the 62 Hz logic rate. Getting either divide wrong
       shifts the per-shot tick count and therefore the whole overheat curve. */
    {
        static const char kHeatDef[] =
            "weapon \"WPN_HEAT_50\"\n"
            "\theat_values 2,4\n"
            "\theat_effect heat, .5, 30, 60\n"
            "end\n"
            "weapon \"WPN_HEAT_MINI\"\n"
            "\theat_values  .8,5\n"
            "end\n"
            "weapon \"WPN_HEAT_GRND\"\n"
            "\theat_values  7,5\n"
            "end\n"
            "weapon \"WPN_HEAT_QUAD\"\n"
            "\theat_values .5,7\n"
            "end\n"
            "weapon \"WPN_HEAT_NONE\"\n"
            "\tclipsize 30\n"
            "end\n";
        struct { int per_shot, decay; } expect[5] = {
            { 1310, 42 },  /* 2 -> 131072/100,  4 -> 262144/6200 */
            {  524, 52 },  /* .8 -> 52429/100,  5 -> 327680/6200 */
            { 4587, 52 },  /* 7 -> 458752/100 */
            {  327, 73 },  /* .5 -> 32768/100, 7 -> 458752/6200 */
            {    0,  0 },  /* no key -> the model stays off */
        };
        DefWeaponsFile hf;
        memset(&hf, 0, sizeof(hf));
        if (def_parse_weapons_memory((const unsigned char *)kHeatDef, sizeof(kHeatDef) - 1, &hf) != 0 ||
            hf.count != 5) {
            fprintf(stderr, "FAIL: heat inline parse failed (count=%zu)\n", hf.count);
            def_free_weapons(&hf);
            def_free_weapons(&wf);
            return 1;
        }
        for (size_t i = 0; i < 5; ++i) {
            if (hf.entries[i].heat_per_shot != expect[i].per_shot ||
                hf.entries[i].heat_decay_per_tick != expect[i].decay) {
                fprintf(stderr, "FAIL: %s heat_values %d/%d, expected %d/%d\n",
                        hf.entries[i].weapon_name, hf.entries[i].heat_per_shot,
                        hf.entries[i].heat_decay_per_tick, expect[i].per_shot,
                        expect[i].decay);
                def_free_weapons(&hf);
                def_free_weapons(&wf);
                return 1;
            }
        }
        /* heat_effect keeps the name and the raw 16.16 threshold, and consumes only
           the first two values — the authored "30, 60" tail is unread in retail too. */
        if (strcmp(hf.entries[0].heat_effect, "heat") != 0 ||
            hf.entries[0].heat_glow_threshold != 0x8000 ||
            hf.entries[1].heat_glow_threshold != 0) {
            fprintf(stderr, "FAIL: heat_effect '%s' threshold %d (mini %d)\n",
                    hf.entries[0].heat_effect, hf.entries[0].heat_glow_threshold,
                    hf.entries[1].heat_glow_threshold);
            def_free_weapons(&hf);
            def_free_weapons(&wf);
            return 1;
        }
        def_free_weapons(&hf);
        printf("heat_values + heat_effect OK\n");
    }

    /* The heat keys go through the engine's digit walker, NOT a round-half-up
       conversion, and the two disagree by one 16.16 LSB on ~4% of decimal forms
       because the original's per-digit scale (419430/2^22) drifts a hair under 1/10.
       "0.07" is one such form: the walker yields 4587, round-half-up yields 4588.
       Per shot that is the difference between 45 and 46 heat, and because the runtime
       divides heat_per_shot by heat_decay_per_tick it also moves the per-shot tick
       count. Pin the walker's answer so a future "simplification" onto the shared
       helper cannot pass silently. [orig: Math_ParseFixedPoint16 @ 0x6131f0] */
    {
        static const char kLsbDef[] =
            "weapon \"WPN_HEAT_LSB\"\n"
            "\theat_values 0.07,0.07\n"
            "end\n";
        DefWeaponsFile lf;
        memset(&lf, 0, sizeof(lf));
        if (def_parse_weapons_memory((const unsigned char *)kLsbDef, sizeof(kLsbDef) - 1, &lf) != 0 ||
            lf.count != 1) {
            fprintf(stderr, "FAIL: heat LSB inline parse failed\n");
            def_free_weapons(&lf);
            def_free_weapons(&wf);
            return 1;
        }
        /* 4587/100 = 45 (round-half-up's 4588 would also give 45), but the decay
           divide by 6200 is where the walker's answer is observable on its own:
           4587/6200 = 0 either way, so assert the per-shot value against the walker
           and the raw threshold, which carries the LSB undivided. */
        if (lf.entries[0].heat_per_shot != 45) {
            fprintf(stderr, "FAIL: heat_values 0.07 per_shot=%d, expected 45\n",
                    lf.entries[0].heat_per_shot);
            def_free_weapons(&lf);
            def_free_weapons(&wf);
            return 1;
        }
        def_free_weapons(&lf);
    }
    {
        static const char kThrDef[] =
            "weapon \"WPN_HEAT_THR\"\n"
            "\theat_effect heat, 0.07\n"
            "end\n";
        DefWeaponsFile tf;
        memset(&tf, 0, sizeof(tf));
        if (def_parse_weapons_memory((const unsigned char *)kThrDef, sizeof(kThrDef) - 1, &tf) != 0 ||
            tf.count != 1 || tf.entries[0].heat_glow_threshold != 4587) {
            fprintf(stderr, "FAIL: heat_effect 0.07 threshold=%d, expected 4587 "
                            "(the digit walker's answer, not round-half-up's 4588)\n",
                    tf.count == 1 ? tf.entries[0].heat_glow_threshold : -1);
            def_free_weapons(&tf);
            def_free_weapons(&wf);
            return 1;
        }
        def_free_weapons(&tf);
        printf("heat fixed-point digit-walker LSB OK\n");
    }

    /* def_parse_weapons_memory parity: same bytes, same result (covers both the
       string armory fields and the D-PLAYERINFO-11 loadout slot/masks). */
    {
        FILE *fp = fopen(path, "rb");
        if (!fp) {
            fprintf(stderr, "FAIL: could not reopen fixture for memory parity\n");
            def_free_weapons(&wf);
            return 1;
        }
        fseek(fp, 0, SEEK_END);
        long fsz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        unsigned char *bytes = (unsigned char *)malloc((size_t)fsz);
        if (!bytes || fread(bytes, 1, (size_t)fsz, fp) != (size_t)fsz) {
            fprintf(stderr, "FAIL: could not read fixture bytes\n");
            fclose(fp);
            free(bytes);
            def_free_weapons(&wf);
            return 1;
        }
        fclose(fp);

        DefWeaponsFile wm;
        memset(&wm, 0, sizeof(wm));
        if (def_parse_weapons_memory(bytes, (size_t)fsz, &wm) != 0) {
            fprintf(stderr, "FAIL: def_parse_weapons_memory failed\n");
            free(bytes);
            def_free_weapons(&wf);
            return 1;
        }
        free(bytes);

        int parity_ok = wm.count == wf.count;
        if (parity_ok) {
            const DefWeaponDef *mm = NULL;
            for (size_t i = 0; i < wm.count; ++i) {
                if (strcmp(wm.entries[i].weapon_name, "WPN_M4AUTO") == 0) { mm = &wm.entries[i]; break; }
            }
            parity_ok = mm && mm->maxclips == 10 && mm->clipsize == 30 &&
                        strcmp(mm->weapon_class, "primary") == 0 &&
                        mm->weapon_class_slot == 1 && mm->loadout_selectable == 1;
        }
        def_free_weapons(&wm);
        if (!parity_ok) {
            fprintf(stderr, "FAIL: memory-parse parity mismatch\n");
            def_free_weapons(&wf);
            return 1;
        }
        printf("memory-parse parity OK\n");
    }

    def_free_weapons(&wf);
    /* A nested `action` line while one is open is REFUSED by the original: it
       logs "forgot an end", keeps the current row open (subsequent keys
       overwrite it, last writer wins) and never creates the new row — the
       never-created suffixes become zeroed generated defaults at bind time.
       [orig: ActionDef_ParseScriptLine @ 0x402409 "forgot an end"; the driver
       WeaponDefs_ParseLineCallback @ 0x54388d forwards in-ACTION lines before
       its own `action` dispatch]. Every shipped weapon.def corpus (JOX, JOTAC
       localres, RevX02, JO:CA, jox01, demo) is fully END-terminated, so only
       malformed data reaches this. Also covers the bare `delay` alias
       (= delayend) [orig: @ 0x40279a / @ 0x402b2c]. */
    {
        static const char mixed[] =
            "weapon \"WPN_MIXED\"\n"
            "\tflags auto\n"
            "\taction \"idle\"\n"
            "\t\tdelayend auto\n"
            "\t\tanim anim_wpn_idle\n"
            "\taction \"emptyidle\"\n"
            "\t\tdelayend auto\n"
            "\t\tanim anim_wpn_idle\n"
            "\taction \"fire\"\n"
            "\t\tdelayend 6\n"
            "\t\tanim anim_wpn_fire\n"
            "\tend\n"
            "\taction \"recoil\"\n"
            "\t\tdelay 4\n"
            "\tend\n"
            "end\n";
        DefWeaponsFile wx;
        if (def_parse_weapons_memory((const uint8_t *)mixed, sizeof(mixed) - 1, &wx) != 0) {
            fprintf(stderr, "FAIL: mixed-terminator parse errored\n");
            return 1;
        }
        const DefWeaponDef *w = NULL;
        for (size_t i = 0; i < wx.count; ++i)
            if (strcmp(wx.entries[i].weapon_name, "WPN_MIXED") == 0) w = &wx.entries[i];
        /* idle stays open through both refused `action` lines and ends up
           carrying fire's keys; emptyidle/fire rows are never created. */
        int ok = w != NULL && w->actions_count == 2;
        if (ok) {
            const DefWeaponAction *idle = &w->actions[0];
            const DefWeaponAction *recoil = &w->actions[1];
            ok = strcmp(idle->name, "idle") == 0 && idle->delaystart == 0 &&
                 idle->delayend == 6 && strcmp(idle->anim, "anim_wpn_fire") == 0 &&
                 strcmp(recoil->name, "recoil") == 0 && recoil->delaystart == 0 &&
                 recoil->delayend == 4;
        }
        def_free_weapons(&wx);
        if (!ok) {
            fprintf(stderr, "FAIL: nested-action refusal (swallow) semantics\n");
            return 1;
        }
        printf("nested-action refusal + delay alias OK\n");
    }

    printf("PASS: weapon parsing OK\n");
    return 0;
}
