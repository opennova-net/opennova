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
    if (strcmp(m4->loadout_menu_textid, "WEAP_SHORT_M4") != 0) {
        fprintf(stderr, "FAIL: M4AUTO loadout_menu_textid mismatch: '%s'\n", m4->loadout_menu_textid);
        def_free_weapons(&wf); return 1;
    }
    printf("Loadout fields OK\n");

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

    /* Weapon flags mask (WeaponDef+8 bits via the token table: auto = 0x100, Sighted =
       0x2, WhileSwimming = 0x4; LaserBeam is unmapped -> raw_lines) and the ADS zoom
       magnification [orig: WeaponSlot_CanFireInCurrentState @ 0x53f0b0 auto gate;
       Player_ToggleWeaponScope @ 0x4df0c0 Flags & 3 gate + FOV 80/zoom @ 0x4df401]. */
    if (m4->flags != (0x100 | 0x2 | 0x4)) {
        fprintf(stderr, "FAIL: M4AUTO flags mismatch: 0x%x\n", m4->flags);
        def_free_weapons(&wf);
        return 1;
    }
    if (fabsf(m4->scope_max_mag - 2.0f) > FEPS) {
        fprintf(stderr, "FAIL: M4AUTO scope_max_mag mismatch: %.3f\n", m4->scope_max_mag);
        def_free_weapons(&wf);
        return 1;
    }
    printf("flags + scope_max_mag OK\n");

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
    printf("PASS: weapon parsing OK\n");
    return 0;
}
