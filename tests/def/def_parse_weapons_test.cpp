// Test parsing weapon.def — check WPN_M4AUTO fields, pos/tpos, sights, actions.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <formats/def/def.h>
#include <formats/def/def_notes.h>

#include <string>

#include "common/retail_paths.h"

using namespace opennova::def;

#define FEPS 0.01f

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
    // The default matters for shipped defs: no stability key still enables
    // drift. Each authored column retains Math_ParseFixedPoint16 precision.
    {
        static const char kStability[] =
            "weapon \"WPN_DEFAULT_STABILITY\"\r\nend\r\n"
            "weapon \"WPN_CUSTOM_STABILITY\"\r\nStability 0.5, 2, 1.5\r\nend\r\n"
            "weapon \"WPN_ZERO_STABILITY\"\r\nstability 0, 0, 0\r\nend\r\n";
        DefWeaponsFile parsed{};
        if (def_parse_weapons_memory(reinterpret_cast<const unsigned char *>(kStability),
                sizeof(kStability) - 1, &parsed) != 0 || parsed.count != 3) return 1;
        const int expected[3][3] = {{65536,65536,65536}, {32768,131072,98304}, {0,0,0}};
        for (size_t row = 0; row < 3; ++row) {
            for (int stance = 0; stance < 3; ++stance) {
                if (parsed.entries[row].stability_fp16[stance] != expected[row][stance]) {
                    fprintf(stderr, "FAIL: stability[%zu][%d]\n", row, stance);
                    def_free_weapons(&parsed);
                    return 1;
                }
            }
        }
        def_free_weapons(&parsed);
    }
    // The loadout list's team mask is the loadout reader's: yellow as blue, violet as red, an
    // unknown team none [orig: WeaponDef_ParseProperty @ 0x54d730, teamfilter
    // @ 0x54daae..0x54db08]. (The host's mask, from the weapon def reader's red / blue table,
    // is world::teamfilter_bit's.) The tokens stay as written.
    {
        static const char kTeams[] =
            "weapon \"WPN_YELLOW\"\r\nteamfilter yellow\r\nend\r\n"
            "weapon \"WPN_VIOLET\"\r\nteamfilter violet\r\nteamfilter green\r\nend\r\n"
            "weapon \"WPN_BOTH\"\r\nteamfilter RED\r\nteamfilter blue\r\nend\r\n";
        DefWeaponsFile parsed{};
        if (def_parse_weapons_memory(reinterpret_cast<const unsigned char *>(kTeams), sizeof(kTeams) - 1, &parsed) != 0 ||
            parsed.count != 3) return 1;
        const bool masks = parsed.entries[0].teamfilter_mask == 2 && parsed.entries[1].teamfilter_mask == 1 &&
                           parsed.entries[2].teamfilter_mask == 3 && parsed.entries[1].teamfilter_count == 2 &&
                           strcmp(parsed.entries[1].teamfilter[1], "green") == 0;
        def_free_weapons(&parsed);
        if (!masks) {
            fprintf(stderr, "FAIL: the loadout reader's team masks\n");
            return 1;
        }
    }

    /* A weapon.def with no weapon rows is an empty table, never a failure: a
       comment-only file and a zero-length one both parse to zero rows
       [orig: WeaponDef_LoadAll @0x54dd10 seeds the "None" row and never reads
       the walk's result]. */
    {
        static const char kCommentOnly[] = "// Weapon definitions\r\n\r\n";
        DefWeaponsFile parsed{};
        if (def_parse_weapons_memory(reinterpret_cast<const unsigned char *>(kCommentOnly),
                sizeof(kCommentOnly) - 1, &parsed) != 0 || parsed.count != 0) {
            fprintf(stderr, "FAIL: a comment-only weapon.def is an empty table\n");
            def_free_weapons(&parsed);
            return 1;
        }
        def_free_weapons(&parsed);
        DefWeaponsFile empty{};
        if (def_parse_weapons_memory(nullptr, 0, &empty) != 0 || empty.count != 0) {
            fprintf(stderr, "FAIL: a zero-length weapon.def is an empty table\n");
            def_free_weapons(&empty);
            return 1;
        }
        def_free_weapons(&empty);
        // The editor's noted read takes the same rule.
        DefTextNotes notes;
        DefWeaponsFile noted{};
        if (def_parse_weapons_memory(nullptr, 0, &noted, nullptr, notes) != 0 || noted.count != 0) {
            fprintf(stderr, "FAIL: a zero-length weapon.def read with notes is an empty table\n");
            def_free_weapons(&noted);
            return 1;
        }
        def_free_weapons(&noted);
        DefWeaponsFile refused{};
        if (def_parse_weapons_memory(nullptr, 4, &refused) == 0) {
            fprintf(stderr, "FAIL: a null buffer with a length is refused\n");
            def_free_weapons(&refused);
            return 1;
        }
        def_free_weapons(&refused);
    }

    /* The shipped weapon.def from the reference fixture set (OPENNOVA_JO_ASSETS):
       its field/pos/sights/actions pins and the memory-parse parity are the
       SKIP-LEG retail leg; the inline blocks run unconditionally. */
    const std::string fixture = retail::reference_fixture("def/weapon.def");
    const bool have_retail = !fixture.empty();
    const char *path = fixture.c_str();

    DefWeaponsFile wf;
    memset(&wf, 0, sizeof(wf));
    if (have_retail) {
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
    if (fabsf((m4->pos_rotation_deg_q16[0] / 65536.0f) - 2.500f) > FEPS ||
        fabsf((m4->pos_rotation_deg_q16[1] / 65536.0f) - 0.750f) > FEPS ||
        fabsf((m4->pos_rotation_deg_q16[2] / 65536.0f) - 356.750f) > FEPS) {
        fprintf(stderr, "FAIL: M4AUTO pos rotation mismatch: %.3f, %.3f, %.3f\n",
                (m4->pos_rotation_deg_q16[0] / 65536.0f), (m4->pos_rotation_deg_q16[1] / 65536.0f), (m4->pos_rotation_deg_q16[2] / 65536.0f));
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
       unmapped -> authoring diagnostic), NoAmmoTypes = flags2 0x40.
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

    /* The scope-zero table [orig: 'scope_max_zero' @ 0x544e8b..0x544efd]: the
       M16/M203 authors `10 50 0 0` and carries the ONE shipped `slide` SIGHTS row
       (m16203b.tga 112 -68 892 703, 33 frames, its third row); the M4 has no key. */
    {
        const DefWeaponDef *m203 = NULL;
        for (size_t i = 0; i < wf.count; ++i) {
            if (strcmp(wf.entries[i].weapon_name, "WPN_M16M203HE") == 0) {
                m203 = &wf.entries[i];
                break;
            }
        }
        if (!m203 || m203->scope_max_zero_steps != 10 || m203->scope_zero_step != 50 ||
            m203->scope_zero_default != 0 || m203->scope_zero_extra != 0 ||
            m4->scope_max_zero_steps != 0 || m4->scope_zero_step != 0 ||
            m4->scope_zero_default != 0 || m4->scope_zero_extra != 0) {
            fprintf(stderr, "FAIL: scope_max_zero: m203=%d/%d/%d/%d m4=%d/%d/%d/%d\n",
                    m203 ? m203->scope_max_zero_steps : -1, m203 ? m203->scope_zero_step : -1,
                    m203 ? m203->scope_zero_default : -1, m203 ? m203->scope_zero_extra : -1,
                    m4->scope_max_zero_steps, m4->scope_zero_step, m4->scope_zero_default,
                    m4->scope_zero_extra);
            def_free_weapons(&wf);
            return 1;
        }
        if (m203->sights_count < 3 || !m203->sights[2].slide ||
            m203->sights[2].slide_frames != 33 || m203->sights[2].scale ||
            m203->sights[2].x1 != 112 || m203->sights[2].y1 != -68 ||
            m203->sights[2].x2 != 892 || m203->sights[2].y2 != 703) {
            fprintf(stderr, "FAIL: M16M203HE slide row: count=%zu\n", m203->sights_count);
            def_free_weapons(&wf);
            return 1;
        }
        /* The parallax key [orig: 'scope_paralax_distance' @ 0x544e4e..0x544e80]:
           atof * 65535.0 then ftol. The M1 turret authors `.814` -> 53345, the T80
           turret `-.574` -> -37617; the M4 and the M203 carry no key. */
        const DefWeaponDef *m1 = NULL;
        const DefWeaponDef *t80 = NULL;
        for (size_t i = 0; i < wf.count; ++i) {
            if (strcmp(wf.entries[i].weapon_name, "WPN_M1TURRET") == 0) m1 = &wf.entries[i];
            if (strcmp(wf.entries[i].weapon_name, "WPN_T80TURRET") == 0) t80 = &wf.entries[i];
        }
        if (!m1 || !t80 || m1->scope_paralax_distance_fp16 != 53345 ||
            t80->scope_paralax_distance_fp16 != -37617 ||
            m4->scope_paralax_distance_fp16 != 0 || m203->scope_paralax_distance_fp16 != 0) {
            fprintf(stderr, "FAIL: scope_paralax_distance: m1=%d t80=%d m4=%d m203=%d\n",
                    m1 ? m1->scope_paralax_distance_fp16 : -1,
                    t80 ? t80->scope_paralax_distance_fp16 : -1,
                    m4->scope_paralax_distance_fp16, m203->scope_paralax_distance_fp16);
            def_free_weapons(&wf);
            return 1;
        }
    }
    printf("scope_max_zero + scope_paralax_distance + the M203 slide row OK\n");
    }  /* retail leg */
    {
        /* WeaponDef_CreateBlendNamedMaterial recognizes all six tokens at
           0x5401b9..0x540257. Keep the established Blend/Add/BlendAt values
           stable while assigning distinct transport values to the three
           previously dropped spellings. A weapon record holds four SIGHTS
           rows, so the six spellings span two weapons. */
        static const char kSightBlendDef[] =
            "weapon \"WPN_SIGHT_BLEND_TEST\"\r\n"
            "\tsights blend.tga 0 0 8 8 blend\r\n"
            "\tsights add.tga 0 0 8 8 add\r\n"
            "\tsights blendat.tga 0 0 8 8 blendat\r\n"
            "end\r\n"
            "weapon \"WPN_SIGHT_BLEND_TEST_2\"\r\n"
            "\tsights multiply.tga 0 0 8 8 multiply\r\n"
            "\tsights addat.tga 0 0 8 8 addat\r\n"
            "\tsights multiplyat.tga 0 0 8 8 multiplyat\r\n"
            "end\r\n";
        static const int expected[] = {
            DEF_SIGHT_BLEND_BLEND,
            DEF_SIGHT_BLEND_ADD,
            DEF_SIGHT_BLEND_BLEND_AT,
            DEF_SIGHT_BLEND_MULTIPLY,
            DEF_SIGHT_BLEND_ADD_AT,
            DEF_SIGHT_BLEND_MULTIPLY_AT,
        };
        DefWeaponsFile sf;
        memset(&sf, 0, sizeof(sf));
        if (def_parse_weapons_memory((const unsigned char *)kSightBlendDef,
                                     sizeof(kSightBlendDef) - 1, &sf) != 0 ||
            sf.count != 2 || sf.entries[0].sights_count != 3 ||
            sf.entries[1].sights_count != 3) {
            fprintf(stderr, "FAIL: sight blend-token inline parse failed\n");
            def_free_weapons(&sf);
            def_free_weapons(&wf);
            return 1;
        }
        for (size_t i = 0; i < 6; ++i) {
            const int got = sf.entries[i / 3].sights[i % 3].blend;
            if (got != expected[i]) {
                fprintf(stderr,
                        "FAIL: sight blend token %zu mapped to %d, expected %d\n",
                        i, got, expected[i]);
                def_free_weapons(&sf);
                def_free_weapons(&wf);
                return 1;
            }
        }
        def_free_weapons(&sf);
        printf("sight blend-token map OK\n");
    }
    {
        /* renderfov is atol then fild: a whole number of degrees [orig:
           WeaponDefs_ParseLineCallback, `call j__atol` @0x544840, `fild`
           @0x54484C]. */
        static const char kFov[] =
            "weapon \"WPN_FOV\"\r\n"
            "\trenderfov 80.5\r\n"
            "end\r\n";
        DefWeaponsFile ff;
        memset(&ff, 0, sizeof(ff));
        const int rc = def_parse_weapons_memory((const unsigned char *)kFov, sizeof(kFov) - 1, &ff);
        const bool correct = rc == 0 && ff.count == 1 && ff.entries[0].renderfov == 80.0f;
        if (!correct)
            fprintf(stderr, "FAIL: renderfov 80.5 read %g\n",
                    ff.count == 1 ? (double)ff.entries[0].renderfov : -1.0);
        def_free_weapons(&ff);
        if (!correct) {
            def_free_weapons(&wf);
            return 1;
        }
        printf("renderfov atol OK\n");
    }
    {
        /* A weapon's name keeps its first 32 characters [orig:
           WeaponDefs_ParseLineCallback, strncpy(def+0x14, tokens[2], 0x20)
           @0x543737]. */
        static const char kLongName[] =
            "weapon \"WPN_ABCDEFGHIJKLMNOPQRSTUVWXYZ_0123456789\"\r\n"
            "end\r\n";
        DefWeaponsFile nf;
        memset(&nf, 0, sizeof(nf));
        const int rc = def_parse_weapons_memory((const unsigned char *)kLongName,
                                                sizeof(kLongName) - 1, &nf);
        const bool correct = rc == 0 && nf.count == 1 &&
                strcmp(nf.entries[0].weapon_name, "WPN_ABCDEFGHIJKLMNOPQRSTUVWXYZ_0") == 0;
        if (!correct)
            fprintf(stderr, "FAIL: long weapon name kept as '%s'\n",
                    nf.count == 1 ? nf.entries[0].weapon_name : "");
        def_free_weapons(&nf);
        if (!correct) {
            def_free_weapons(&wf);
            return 1;
        }
        printf("weapon name cut to 32 OK\n");
    }
    {
        /* A SIGHTS row reads by position: the blend mode is token 6 only, and
           token 7 is the one `scale` or `slide` flag, `slide`'s frame count token
           8, read only when the line carries 8 tokens or more; a flag word
           anywhere else is nothing [orig: WeaponDefs_ParseLineCallback @0x543680,
           the sights arm @0x544AC8: WeaponDef_CreateBlendNamedMaterial(tokens[7])
           @0x544B3F, the `cmp [esi],8; jl` @0x544B7A, "scale" @0x544B86 and
           "slide" @0x544BA2 against tokens[8], atol(tokens[9]) @0x544BC3]. */
        static const char kSightOrderDef[] =
            "weapon \"WPN_SIGHT_ORDER_TEST\"\r\n"
            "\tsights a.tga 0 0 8 8 slide 33\r\n"       /* `slide` is the blend name */
            "\tsights b.tga 0 0 8 8 add blend scale\r\n" /* token 7 is `blend` */
            "\tsights c.tga 0 0 8 8 add scale\r\n"
            "\tsights d.tga 0 0 8 8 blendat slide 12\r\n"
            "end\r\n";
        DefWeaponsFile of;
        memset(&of, 0, sizeof(of));
        if (def_parse_weapons_memory((const unsigned char *)kSightOrderDef,
                                     sizeof(kSightOrderDef) - 1, &of) != 0 ||
            of.count != 1 || of.entries[0].sights_count != 4) {
            fprintf(stderr, "FAIL: positional sights inline parse failed\n");
            def_free_weapons(&of);
            def_free_weapons(&wf);
            return 1;
        }
        const DefSightEntry *s = of.entries[0].sights;
        const bool ok = s[0].blend == DEF_SIGHT_BLEND_BLEND && !s[0].slide && !s[0].scale &&
                s[1].blend == DEF_SIGHT_BLEND_ADD && !s[1].scale &&
                s[2].blend == DEF_SIGHT_BLEND_ADD && s[2].scale &&
                s[3].blend == DEF_SIGHT_BLEND_BLEND_AT && s[3].slide && s[3].slide_frames == 12;
        if (!ok) {
            fprintf(stderr, "FAIL: positional sights: %d/%d/%d %d/%d %d/%d %d/%d/%d\n", s[0].blend,
                    s[0].slide, s[0].scale, s[1].blend, s[1].scale, s[2].blend, s[2].scale,
                    s[3].blend, s[3].slide, s[3].slide_frames);
            def_free_weapons(&of);
            def_free_weapons(&wf);
            return 1;
        }
        def_free_weapons(&of);
        printf("positional sight rows OK\n");
    }
    {
        /* `charfilter` and `teamfilter` read token 1 only, one class or team a
           line, and `ammobucket` and the `ammoclass` count are abs(atol)
           [orig: WeaponDefs_ParseLineCallback @0x543680 — charfilter's table walk
           over tokens[2] @0x543F40..0x543F6E, teamfilter's @0x543FB5..0x543FE3,
           ammobucket's cdq/xor/sub @0x544037, the ammoclass count's @0x54423D]. */
        static const char kFilterDef[] =
            "weapon \"WPN_FILTER_TEST\"\r\n"
            "\tcharfilter medic sniper\r\n"
            "\tteamfilter blue red\r\n"
            "\tammobucket -3\r\n"
            "\tammoclass CLASS_TEST -2\r\n"
            "end\r\n";
        DefWeaponsFile ff;
        memset(&ff, 0, sizeof(ff));
        if (def_parse_weapons_memory((const unsigned char *)kFilterDef, sizeof(kFilterDef) - 1,
                                     &ff) != 0 || ff.count != 1) {
            fprintf(stderr, "FAIL: filter inline parse failed\n");
            def_free_weapons(&ff);
            def_free_weapons(&wf);
            return 1;
        }
        const DefWeaponDef &w = ff.entries[0];
        const bool ok = w.charfilter_count == 1 && strcmp(w.charfilter[0], "medic") == 0 &&
                w.teamfilter_count == 1 && strcmp(w.teamfilter[0], "blue") == 0 &&
                w.charfilter_mask == 1 && w.teamfilter_mask == 2 && w.ammobucket == 3 &&
                w.ammo_class_count == 2;
        if (!ok) {
            fprintf(stderr, "FAIL: filters read %zu/%zu tokens, masks %d/%d, bucket %d, class count %d\n",
                    w.charfilter_count, w.teamfilter_count, w.charfilter_mask, w.teamfilter_mask,
                    w.ammobucket, w.ammo_class_count);
            def_free_weapons(&ff);
            def_free_weapons(&wf);
            return 1;
        }
        def_free_weapons(&ff);
        printf("charfilter/teamfilter token 1 and the abs values OK\n");
    }
    {
        /* SIGHTS rows: a row whose texture is no file is refused, a short line is
           a row whose missing corners read 0, and a weapon keeps four rows, the
           record's room ahead of its count [orig: WeaponDefs_ParseLineCallback's
           sights arm @0x544AC8 — FileSystem_FileExists @0x544AE2, atol of
           tokens[3..6] @0x544B48..0x544B6C, the count @+0x258 after four 36-byte
           rows from +0x1C8 @0x544B11..0x544B32]. */
        static const char kRowsDef[] =
            "weapon \"WPN_SIGHT_ROWS_TEST\"\r\n"
            "\tsights short.tga 7\r\n"
            "\tsights missing.tga 0 0 8 8\r\n"
            "\tsights two.tga 1 2 3 4\r\n"
            "\tsights three.tga 1 2 3 4\r\n"
            "\tsights four.tga 1 2 3 4\r\n"
            "\tsights five.tga 1 2 3 4\r\n"
            "end\r\n";
        const DefFileProbe probe = {
                [](const void *, const char *name) { return strcmp(name, "missing.tga") != 0; },
                nullptr};
        DefWeaponsFile rf;
        memset(&rf, 0, sizeof(rf));
        if (def_parse_weapons_memory((const unsigned char *)kRowsDef, sizeof(kRowsDef) - 1, &rf,
                                     nullptr, &probe) != 0 || rf.count != 1) {
            fprintf(stderr, "FAIL: sight rows inline parse failed\n");
            def_free_weapons(&rf);
            def_free_weapons(&wf);
            return 1;
        }
        const DefWeaponDef &w = rf.entries[0];
        const bool ok = w.sights_count == 4 && strcmp(w.sights[0].texture, "short.tga") == 0 &&
                w.sights[0].x1 == 7 && w.sights[0].y1 == 0 && w.sights[0].y2 == 0 &&
                strcmp(w.sights[1].texture, "two.tga") == 0 &&
                strcmp(w.sights[3].texture, "four.tga") == 0;
        if (!ok) {
            fprintf(stderr, "FAIL: sight rows: %zu rows, first '%s', second '%s'\n", w.sights_count,
                    w.sights_count > 0 ? w.sights[0].texture : "",
                    w.sights_count > 1 ? w.sights[1].texture : "");
            def_free_weapons(&rf);
            def_free_weapons(&wf);
            return 1;
        }
        def_free_weapons(&rf);
        printf("sight rows: the texture check, short lines and the four-row record OK\n");
    }
    {
        /* The scope-zero table's token forms [orig: 'scope_max_zero'
           @ 0x544e8b..0x544efd]: three values store +0x84/+0x9C/+0xA0; a fourth stores
           +0x88 only when the line carries four (`cmp dword ptr [esi],4; jle`
           @ 0x544edf counts the key); an absent key leaves the entry memset's zeros.
           The parallax key [orig: 'scope_paralax_distance' @ 0x544e4e..0x544e80]:
           a case-insensitive match (stricmp), atof * 65535.0 then ftol, so `2.0`
           stores 131070 and `-.5` truncates -32767.5 toward zero to -32767. */
        static const char kZeroDef[] =
            "weapon \"WPN_ZERO_FOUR\"\r\n"
            "\tscope_max_zero 10 100 200 1\r\n"
            "end\r\n"
            "weapon \"WPN_ZERO_THREE\"\r\n"
            "\tscope_max_zero  1 300 300\r\n"
            "end\r\n"
            "weapon \"WPN_ZERO_NONE\"\r\n"
            "end\r\n"
            "weapon \"WPN_ZERO_PARALAX\"\r\n"
            "\tscope_max_zero 10 100 300 1\r\n"
            "\tScope_Paralax_Distance\t\t2.0\r\n"
            "end\r\n"
            "weapon \"WPN_ZERO_PARALAX_NEG\"\r\n"
            "\tscope_paralax_distance -.5\r\n"
            "end\r\n";
        DefWeaponsFile zf;
        memset(&zf, 0, sizeof(zf));
        if (def_parse_weapons_memory((const unsigned char *)kZeroDef,
                                     sizeof(kZeroDef) - 1, &zf) != 0 ||
            zf.count != 5) {
            fprintf(stderr, "FAIL: scope_max_zero inline parse failed\n");
            def_free_weapons(&zf);
            def_free_weapons(&wf);
            return 1;
        }
        const DefWeaponDef *four = &zf.entries[0];
        const DefWeaponDef *three = &zf.entries[1];
        const DefWeaponDef *none = &zf.entries[2];
        const DefWeaponDef *paralax = &zf.entries[3];
        const DefWeaponDef *paralax_neg = &zf.entries[4];
        if (paralax->scope_paralax_distance_fp16 != 131070 ||
            paralax->scope_max_zero_steps != 10 || paralax->scope_zero_default != 300 ||
            paralax_neg->scope_paralax_distance_fp16 != -32767 ||
            four->scope_paralax_distance_fp16 != 0 || three->scope_paralax_distance_fp16 != 0 ||
            none->scope_paralax_distance_fp16 != 0) {
            fprintf(stderr,
                    "FAIL: scope_paralax_distance forms: paralax=%d (%d/%d) neg=%d "
                    "four=%d three=%d none=%d\n",
                    paralax->scope_paralax_distance_fp16, paralax->scope_max_zero_steps,
                    paralax->scope_zero_default, paralax_neg->scope_paralax_distance_fp16,
                    four->scope_paralax_distance_fp16, three->scope_paralax_distance_fp16,
                    none->scope_paralax_distance_fp16);
            def_free_weapons(&zf);
            def_free_weapons(&wf);
            return 1;
        }
        if (four->scope_max_zero_steps != 10 || four->scope_zero_step != 100 ||
            four->scope_zero_default != 200 || four->scope_zero_extra != 1 ||
            three->scope_max_zero_steps != 1 || three->scope_zero_step != 300 ||
            three->scope_zero_default != 300 || three->scope_zero_extra != 0 ||
            none->scope_max_zero_steps != 0 || none->scope_zero_step != 0 ||
            none->scope_zero_default != 0 || none->scope_zero_extra != 0) {
            fprintf(stderr,
                    "FAIL: scope_max_zero forms: four=%d/%d/%d/%d three=%d/%d/%d/%d "
                    "none=%d/%d/%d/%d\n",
                    four->scope_max_zero_steps, four->scope_zero_step,
                    four->scope_zero_default, four->scope_zero_extra,
                    three->scope_max_zero_steps, three->scope_zero_step,
                    three->scope_zero_default, three->scope_zero_extra,
                    none->scope_max_zero_steps, none->scope_zero_step,
                    none->scope_zero_default, none->scope_zero_extra);
            def_free_weapons(&zf);
            def_free_weapons(&wf);
            return 1;
        }
        def_free_weapons(&zf);
        printf("scope_max_zero token forms OK\n");
    }
    {
        static const char kFovDef[] =
            "weapon \"WPN_FOVTEST\"\r\n"
            "\trenderfov 40\r\n"
            "end\r\n";
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
            "weapon \"WPN_HEAT_50\"\r\n"
            "\theat_values 2,4\r\n"
            "\theat_effect heat, .5, 30, 60\r\n"
            "end\r\n"
            "weapon \"WPN_HEAT_MINI\"\r\n"
            "\theat_values  .8,5\r\n"
            "end\r\n"
            "weapon \"WPN_HEAT_GRND\"\r\n"
            "\theat_values  7,5\r\n"
            "end\r\n"
            "weapon \"WPN_HEAT_QUAD\"\r\n"
            "\theat_values .5,7\r\n"
            "end\r\n"
            "weapon \"WPN_HEAT_NONE\"\r\n"
            "\tclipsize 30\r\n"
            "end\r\n";
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
            "weapon \"WPN_HEAT_LSB\"\r\n"
            "\theat_values 0.07,0.07\r\n"
            "end\r\n";
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
            "weapon \"WPN_HEAT_THR\"\r\n"
            "\theat_effect heat, 0.07\r\n"
            "end\r\n";
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
    if (have_retail) {
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
            "weapon \"WPN_MIXED\"\r\n"
            "\tflags auto\r\n"
            "\taction \"idle\"\r\n"
            "\t\tdelayend auto\r\n"
            "\t\tanim anim_wpn_idle\r\n"
            "\taction \"emptyidle\"\r\n"
            "\t\tdelayend auto\r\n"
            "\t\tanim anim_wpn_idle\r\n"
            "\taction \"fire\"\r\n"
            "\t\tdelayend 6\r\n"
            "\t\tanim anim_wpn_fire\r\n"
            "\tend\r\n"
            "\taction \"recoil\"\r\n"
            "\t\tdelay 4\r\n"
            "\tend\r\n"
            "end\r\n";
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

    /* A block's name is the line's second token, quotes optional: the shipped
       AT4 and RPG entries open their scopeup rows with a bare `ACTION SCOPEUP`
       [orig: WeaponDefs_ParseLineCallback @0x543680 over the tokens
       Terrain_TokenizeConfigLine @0x53CB60 cuts]. */
    {
        static const char bare[] =
            "weapon WPN_BARE // a comment\r\n"
            "\tACTION\tSCOPEUP\r\n"
            "\tDELAYSTART\t1\r\n"
            "\tEND\r\n"
            "\tACTION\t\"SCOPEDOWN\"\r\n"
            "\tDELAYSTART\t2\r\n"
            "\tEND\r\n"
            "end\r\n";
        DefWeaponsFile wb;
        const int ok = def_parse_weapons_memory((const uint8_t *)bare, sizeof(bare) - 1, &wb) == 0 &&
                wb.count == 1 && strcmp(wb.entries[0].weapon_name, "WPN_BARE") == 0 &&
                wb.entries[0].actions_count == 2 &&
                strcmp(wb.entries[0].actions[0].name, "SCOPEUP") == 0 &&
                wb.entries[0].actions[0].delaystart == 1 &&
                strcmp(wb.entries[0].actions[1].name, "SCOPEDOWN") == 0;
        def_free_weapons(&wb);
        if (!ok) {
            fprintf(stderr, "FAIL: bare weapon/action names\n");
            return 1;
        }
    }

    /* A second block of a suffix finds the same row and re-initializes it, so
       it replaces the first wholesale: its own keys over the defaults, nothing
       of the first kept (not a key merge), whatever the name's case.
       [orig: ActionDef_ParseScriptLine @0x4024A1 -> ActionDef_InitDefaults
       @0x4024DA] */
    {
        static const char twice[] =
            "weapon \"WPN_TWICE\"\r\n"
            "\tACTION \"FIRE\"\r\n\t\tANIM anim_wpn_fire\r\n\t\tDELAYSTART 4\r\n\t\tDELAYEND 6\r\n"
            "\t\tSOUNDSETEND GS_ONE\r\n\tEND\r\n"
            "\tACTION \"RELOAD\"\r\n\t\tDELAYEND 9\r\n\tEND\r\n"
            "\taction \"fire\"\r\n\t\tdelayend 2\r\n\tend\r\n"
            "end\r\n";
        DefWeaponsFile wt;
        int ok = def_parse_weapons_memory((const uint8_t *)twice, sizeof(twice) - 1, &wt) == 0 &&
                wt.count == 1 && wt.entries[0].actions_count == 2;
        if (ok) {
            const DefWeaponAction &fire = wt.entries[0].actions[0];
            ok = strcmp(fire.name, "fire") == 0 && fire.anim[0] == '\0' && fire.delaystart == 0 &&
                 fire.delayend == 2 && fire.soundsetend[0] == '\0' &&
                 wt.entries[0].actions[1].delayend == 9;
        }
        def_free_weapons(&wt);
        if (!ok) {
            fprintf(stderr, "FAIL: a repeated ACTION block replaces the row\n");
            return 1;
        }
    }


    {
        const char text[] =
                "weapon TEST_SOUNDS\r\n"
                " soundhead MINI_HEAD\r\n soundfireloop MINI_LOOP\r\n"
                " soundtrailoff MINI_TAIL\r\n soundlockedtone TARGET_LOCK\r\n"
                " heat_sound OVERHEAT extra\r\nend\r\n";
        DefWeaponsFile parsed{};
        if (def_parse_weapons_memory(reinterpret_cast<const uint8_t *>(text),
                sizeof(text) - 1, &parsed) != 0 || parsed.count != 1) return 1;
        const DefWeaponDef &w = parsed.entries[0];
        /* heat_sound reads its first value token alone [orig: WeaponDefs_ParseLineCallback
           'heat_sound' @ 0x543e85, tokens[1] @ 0x543e97 -> +0x368 @ 0x543eac]. */
        const bool ok = strcmp(w.soundhead, "MINI_HEAD") == 0 &&
                strcmp(w.soundfireloop, "MINI_LOOP") == 0 &&
                strcmp(w.soundtrailoff, "MINI_TAIL") == 0 &&
                strcmp(w.soundlockedtone, "TARGET_LOCK") == 0 &&
                strcmp(w.heat_sound, "OVERHEAT") == 0;
        def_free_weapons(&parsed);
        if (!ok) { fprintf(stderr, "FAIL: weapon-level sound names\n"); return 1; }
    }

    /* The scope zoom range keys [orig: WeaponDefs_ParseLineCallback 'scope_max_mag'
       @ 0x544f08 -> +0x90 @ 0x544f29 / +0x94 @ 0x544f44, 'scope_min_mag' @ 0x544f4f
       -> +0x98 @ 0x544f7a; AdmDef_InitEntryDefaults def[38] = 2 @ 0x53ff73]: atol'd
       ints, the second scope_max_mag value (the slot's initial zoom) 0 when the row
       carries one value, scope_min_mag 2 when the key is absent. The first three
       rows are the shipped JOX forms verbatim (WPN_M1TURRET `10<tab>2`, WPN_EMP50BD
       `8 8` + `scope_min_mag 2`, the common `2`). */
    {
        static const char kScopeDef[] =
            "weapon \"WPN_ZOOM_TURRET\"\r\n"
            "\tscope_max_mag\t10\t2\r\n"
            "end\r\n"
            "weapon \"WPN_ZOOM_EMP\"\r\n"
            "\tscope_max_mag\t8 8\r\n"
            "\tscope_min_mag\t2\r\n"
            "end\r\n"
            "weapon \"WPN_ZOOM_PLAIN\"\r\n"
            "\tscope_max_mag\t2\r\n"
            "end\r\n"
            "weapon \"WPN_ZOOM_FLOOR\"\r\n"
            "\tscope_max_mag\t16\r\n"
            "\tscope_min_mag\t4\r\n"
            "end\r\n"
            "weapon \"WPN_ZOOM_NONE\"\r\n"
            "\tclipsize 30\r\n"
            "end\r\n";
        struct { float max; int arg2; int min; } expect[5] = {
            { 10.0f, 2, 2 }, { 8.0f, 8, 2 }, { 2.0f, 0, 2 }, { 16.0f, 0, 4 }, { 0.0f, 0, 2 },
        };
        DefWeaponsFile zf;
        memset(&zf, 0, sizeof(zf));
        if (def_parse_weapons_memory((const unsigned char *)kScopeDef, sizeof(kScopeDef) - 1, &zf) != 0 ||
            zf.count != 5) {
            fprintf(stderr, "FAIL: scope zoom inline parse failed (count=%zu)\n", zf.count);
            def_free_weapons(&zf);
            def_free_weapons(&wf);
            return 1;
        }
        for (size_t i = 0; i < 5; ++i) {
            if (fabsf(zf.entries[i].scope_max_mag - expect[i].max) > FEPS ||
                zf.entries[i].scope_max_mag_arg2 != expect[i].arg2 ||
                zf.entries[i].scope_min_mag != expect[i].min) {
                fprintf(stderr, "FAIL: %s scope_max_mag %.1f/%d scope_min_mag %d, expected %.1f/%d/%d\n",
                        zf.entries[i].weapon_name, zf.entries[i].scope_max_mag,
                        zf.entries[i].scope_max_mag_arg2, zf.entries[i].scope_min_mag,
                        expect[i].max, expect[i].arg2, expect[i].min);
                def_free_weapons(&zf);
                def_free_weapons(&wf);
                return 1;
            }
        }
        def_free_weapons(&zf);
        printf("scope_max_mag arg2 + scope_min_mag OK\n");
    }

    /* A `pos`/`tpos` line with fewer than six values is refused whole: the
       original's token-count gate (key + 6) warns "too few params" and returns
       before the first store, so the row keeps its earlier values. A line with
       extra values still stores its first six. [orig: WeaponDefs_ParseLineCallback
       @0x543680, pos gate @0x5445EE..0x544613, tpos gate @0x544735..0x54475A] */
    {
        static const char kShortPoseDef[] =
            "weapon \"WPN_SHORT_POSE\"\r\n"
            "\tpos 1.5, 2.5, 3.5, 10, 20, 30\r\n"
            "\ttpos 4.5, 5.5, 6.5, 40, 50, 60\r\n"
            "\tpos 9, 9, 9, 9, 9\r\n"
            "\ttpos 8, 8\r\n"
            "end\r\n"
            "weapon \"WPN_LONG_POSE\"\r\n"
            "\tpos 1, 2, 3, 4, 5, 6, 7\r\n"
            "end\r\n";
        DefWeaponsFile pf;
        memset(&pf, 0, sizeof(pf));
        if (def_parse_weapons_memory((const unsigned char *)kShortPoseDef, sizeof(kShortPoseDef) - 1, &pf) != 0 ||
            pf.count != 2) {
            fprintf(stderr, "FAIL: short pose inline parse failed (count=%zu)\n", pf.count);
            def_free_weapons(&pf);
            def_free_weapons(&wf);
            return 1;
        }
        const DefWeaponDef &sp = pf.entries[0];
        const DefWeaponDef &lp = pf.entries[1];
        if (fabsf(sp.pos[0] - 1.5f) > FEPS || fabsf(sp.pos[2] - 3.5f) > FEPS ||
            sp.pos_rotation_deg_q16[0] != 10 * 65536 || sp.pos_rotation_deg_q16[2] != 30 * 65536 ||
            fabsf(sp.tpos[0] - 4.5f) > FEPS || fabsf(sp.tpos[1] - 5.5f) > FEPS ||
            sp.tpos_rotation_deg_q16[1] != 50 * 65536 ||
            fabsf(lp.pos[0] - 1.0f) > FEPS || fabsf(lp.pos[2] - 3.0f) > FEPS ||
            lp.pos_rotation_deg_q16[2] != 6 * 65536) {
            fprintf(stderr, "FAIL: short pos/tpos line was not refused whole: pos %.2f/%.2f/%.2f rot %d, "
                            "tpos %.2f/%.2f rot %d; long pos %.2f/%.2f rot %d\n",
                    sp.pos[0], sp.pos[1], sp.pos[2], sp.pos_rotation_deg_q16[0],
                    sp.tpos[0], sp.tpos[1], sp.tpos_rotation_deg_q16[1],
                    lp.pos[0], lp.pos[2], lp.pos_rotation_deg_q16[2]);
            def_free_weapons(&pf);
            def_free_weapons(&wf);
            return 1;
        }
        def_free_weapons(&pf);
        printf("short pos/tpos refusal OK\n");
    }

    /* `sameas <weapon>`: the first value token, at most 32 characters (retail's
       strncpy into AdmDef+0x34 caps at 0x20); absent = empty.
       [orig: WeaponDefs_ParseLineCallback @0x544056..0x544072] */
    {
        static const char kSameAsDef[] =
            "weapon \"WPN_A\"\r\n"
            "\tsameas WPN_M4AUTO\r\n"
            "end\r\n"
            "weapon \"WPN_B\"\r\n"
            "\tsameas WPN_0123456789012345678901234567890123\r\n"
            "end\r\n"
            "weapon \"WPN_C\"\r\n"
            "end\r\n";
        DefWeaponsFile sf;
        memset(&sf, 0, sizeof(sf));
        if (def_parse_weapons_memory((const unsigned char *)kSameAsDef, sizeof(kSameAsDef) - 1,
                                     &sf) != 0 ||
            sf.count != 3) {
            fprintf(stderr, "FAIL: sameas inline parse failed\n");
            def_free_weapons(&sf);
            def_free_weapons(&wf);
            return 1;
        }
        if (strcmp(sf.entries[0].sameas, "WPN_M4AUTO") != 0 ||
            strcmp(sf.entries[1].sameas, "WPN_0123456789012345678901234567") != 0 ||
            sf.entries[2].sameas[0] != '\0') {
            fprintf(stderr, "FAIL: sameas: '%s' / '%s' / '%s'\n", sf.entries[0].sameas,
                    sf.entries[1].sameas, sf.entries[2].sameas);
            def_free_weapons(&sf);
            def_free_weapons(&wf);
            return 1;
        }
        def_free_weapons(&sf);
        printf("sameas OK\n");
    }

    /* An entry no `end` closes keeps the slot its `weapon` line claimed: a
       final `end` with no CR LF reads `en` and the last weapon stays, marked
       unclosed and ending where the walk ended; an action block left open is a
       row of its entry too [orig: WeaponDefs_ParseLineCallback @0x543680, the
       slot @0x5436E7..0x543737, `end` @0x5437CF; ActionDef_ParseScriptLine's
       row at its `action` line @0x4024A1..0x4024DA; File_ParseASCIIFile's tail
       leg @0x53D8E9 / @0x53D8EC]. */
    {
        static const char kTailDef[] =
            "weapon \"WPN_SHUT\"\r\n"
            "\tclipsize 5\r\n"
            "end\r\n"
            "weapon \"WPN_TAIL\"\r\n"
            "\tclipsize 7\r\n"
            "\taction \"FIRE\"\r\n"
            "\t\tdelayend 3\r\n"
            "\tEND\r\n"
            "end";
        static const char kOpenActionDef[] =
            "weapon \"WPN_OPEN_ACTION\"\r\n"
            "\taction \"FIRE\"\r\n"
            "\t\tdelayend 4\r\n";
        DefWeaponsFile tf, of;
        memset(&tf, 0, sizeof(tf));
        memset(&of, 0, sizeof(of));
        const bool parsed =
            def_parse_weapons_memory((const unsigned char *)kTailDef, sizeof(kTailDef) - 1, &tf) == 0 &&
            def_parse_weapons_memory((const unsigned char *)kOpenActionDef, sizeof(kOpenActionDef) - 1,
                                     &of) == 0;
        const bool ok = parsed && tf.count == 2 && tf.entries[0].unclosed == 0 &&
                tf.entries[0].end_line == 2 && tf.entries[1].unclosed == 1 &&
                strcmp(tf.entries[1].weapon_name, "WPN_TAIL") == 0 && tf.entries[1].clipsize == 7 &&
                tf.entries[1].end_line == 9 && tf.entries[1].actions_count == 1 &&
                tf.entries[1].actions[0].delayend == 3 && !tf.stopped &&
                of.count == 1 && of.entries[0].unclosed == 1 && of.entries[0].actions_count == 1 &&
                strcmp(of.entries[0].actions[0].name, "FIRE") == 0 &&
                of.entries[0].actions[0].delayend == 4 && of.entries[0].actions[0].end_line == 3;
        if (!ok) {
            fprintf(stderr, "FAIL: unclosed entries: %zu entries (last unclosed %d, end_line %zu), "
                            "open action: %zu entries, %zu actions\n",
                    tf.count, tf.count > 1 ? tf.entries[1].unclosed : -1,
                    tf.count > 1 ? tf.entries[1].end_line : (size_t)0, of.count,
                    of.count > 0 ? of.entries[0].actions_count : (size_t)0);
            def_free_weapons(&tf);
            def_free_weapons(&of);
            def_free_weapons(&wf);
            return 1;
        }
        def_free_weapons(&tf);
        def_free_weapons(&of);
        printf("unclosed entries keep their slot OK\n");
    }

    /* A `weapon` line inside an open entry, its action block included, logs
       "weapon didn't have an end" and returns 1, which ends the walk: the
       open entry stays unclosed and nothing after the line is read
       [orig: WeaponDefs_ParseLineCallback @0x5436AD..0x5436D2; the walk's exit
       @0x53D942]. */
    {
        static const char kNestedDef[] =
            "weapon \"WPN_OPEN\"\r\n"
            "\tclipsize 3\r\n"
            "\taction \"FIRE\"\r\n"
            "\tweapon \"WPN_LATER\"\r\n"
            "\tclipsize 9\r\n"
            "\tend\r\n"
            "end\r\n"
            "weapon \"WPN_AFTER\"\r\n"
            "end\r\n";
        DefWeaponsFile nf;
        memset(&nf, 0, sizeof(nf));
        const bool ok = def_parse_weapons_memory((const unsigned char *)kNestedDef,
                                                 sizeof(kNestedDef) - 1, &nf) == 0 &&
                nf.count == 1 && nf.stopped == 1 && nf.stop_line == 3 &&
                nf.entries[0].unclosed == 1 && nf.entries[0].clipsize == 3 &&
                nf.entries[0].end_line == 3 && nf.entries[0].actions_count == 1 &&
                nf.entries[0].actions[0].unmodeled_count == 0;
        if (!ok) {
            fprintf(stderr, "FAIL: nested weapon line: %zu entries, stopped %d at %zu, clipsize %d\n",
                    nf.count, nf.stopped, nf.stop_line, nf.count > 0 ? nf.entries[0].clipsize : -1);
            def_free_weapons(&nf);
            def_free_weapons(&wf);
            return 1;
        }
        def_free_weapons(&nf);
        printf("a weapon line inside an open entry ends the walk OK\n");
    }

    /* A short line reads every slot its arm reads, past the line's count:
       slots 1 and 2 reset to "" each line and slots 3 on keep what an earlier,
       longer line left (io::ConfigTokens::slot). stability reads slots 1..3,
       error 1..6, heat_values and heat_effect 1..2, classrounds 1..2 and
       scope_max_zero 1..3 (slot 4 only on a line carrying it) [orig:
       WeaponDefs_ParseLineCallback — stability @0x54412A..0x544169, error
       @0x543B37..0x543BB5, heat_effect @0x543E48..0x543E7A, heat_values
       @0x543EC9..0x543F1B, classrounds @0x543AC4..0x543B16, scope_max_zero
       @0x544E9D..0x544EFD]. crosshair, hudclipgfx and hudrndgfx store only
       when their texture is a file; hudclipgfx's texture is slot 3 and
       hudrndgfx's slot 6, whatever the count [orig: FileSystem_FileExists
       @0x54493E, @0x544295, @0x544316]. */
    {
        static const char kSlotsDef[] =
            "weapon \"WPN_SLOTS\"\r\n"
            "\terror 1 2 3 4 5 6\r\n"
            "\tstability 9\r\n"
            "\terror 7\r\n"
            "\theat_values 200 6200\r\n"
            "\theat_values 100\r\n"
            "\theat_effect hot 3\r\n"
            "\theat_effect glow\r\n"
            "\tclassrounds medic 5\r\n"
            "\tclassrounds medic\r\n"
            "\tscope_max_zero 10 20 30 40\r\n"
            "\tscope_max_zero 11\r\n"
            "\tcrosshair a.tga b.tga\r\n"
            "\tcrosshair missing.tga\r\n"
            "\tcrosshair c.tga missing.tga\r\n"
            "\thudclipgfx 1 2 clip.tga\r\n"
            "\thudclipgfx 5 6 missing.tga\r\n"
            "\thudrndgfx 1 2 3 4 5 rnd.tga\r\n"
            "\thudrndgfx 1 2 3 4 5 missing.tga\r\n"
            "end\r\n";
        const DefFileProbe probe = {
                [](const void *, const char *name) { return strcmp(name, "missing.tga") != 0; },
                nullptr};
        DefWeaponsFile sf;
        memset(&sf, 0, sizeof(sf));
        if (def_parse_weapons_memory((const unsigned char *)kSlotsDef, sizeof(kSlotsDef) - 1, &sf,
                                     nullptr, &probe) != 0 || sf.count != 1) {
            fprintf(stderr, "FAIL: slot-read inline parse failed\n");
            def_free_weapons(&sf);
            def_free_weapons(&wf);
            return 1;
        }
        const DefWeaponDef &w = sf.entries[0];
        /* stability 9: slot 1 "9", slot 2 "", slot 3 the first error line's
           pointer, at offset 10, where this line wrote "9". error 7: slot 3
           still "9", slots 4..6 the first error line's "4".."6". heat_values
           100: slot 2 "" -> 0. heat_effect glow: slot 2 "" -> 0. classrounds
           medic: slot 2 "" -> 0. scope_max_zero 11: slot 3 is the previous
           line's "30", past this line's end; the fourth value stays 40. */
        const bool ok = w.stability_fp16[0] == 9 * 65536 && w.stability_fp16[1] == 0 &&
                w.stability_fp16[2] == 9 * 65536 &&
                w.error_fp16[0] == 7 * 65536 && w.error_fp16[1] == 0 && w.error_fp16[2] == 9 * 65536 &&
                w.error_fp16[3] == 4 * 65536 && w.error_fp16[5] == 6 * 65536 &&
                w.heat_per_shot == (100 * 65536) / 100 && w.heat_decay_per_tick == 0 &&
                strcmp(w.heat_effect, "glow") == 0 && w.heat_glow_threshold == 0 &&
                w.classrounds[1] == 0 &&
                w.scope_max_zero_steps == 11 && w.scope_zero_step == 0 && w.scope_zero_default == 30 &&
                w.scope_zero_extra == 40 &&
                strcmp(w.crosshair, "c.tga") == 0 && strcmp(w.crosshair_secondary, "b.tga") == 0 &&
                w.hudclipgfx_offset[0] == 1 && strcmp(w.hudclipgfx_texture, "clip.tga") == 0 &&
                w.hudrndgfx_offset[0] == 1 && strcmp(w.hudrndgfx_texture, "rnd.tga") == 0;
        if (!ok) {
            fprintf(stderr,
                    "FAIL: slot reads: stability %d/%d/%d, error %d/%d/%d/%d, heat %d/%d '%s' %d, "
                    "classrounds %d, zero %d/%d/%d/%d, crosshair '%s'/'%s', clip %d '%s', rnd %d '%s'\n",
                    w.stability_fp16[0], w.stability_fp16[1], w.stability_fp16[2], w.error_fp16[0],
                    w.error_fp16[1], w.error_fp16[2], w.error_fp16[5], w.heat_per_shot,
                    w.heat_decay_per_tick, w.heat_effect, w.heat_glow_threshold, w.classrounds[1],
                    w.scope_max_zero_steps, w.scope_zero_step, w.scope_zero_default,
                    w.scope_zero_extra, w.crosshair, w.crosshair_secondary, w.hudclipgfx_offset[0],
                    w.hudclipgfx_texture, w.hudrndgfx_offset[0], w.hudrndgfx_texture);
            def_free_weapons(&sf);
            def_free_weapons(&wf);
            return 1;
        }
        def_free_weapons(&sf);
        printf("short lines read retail's slots; crosshair/hudclipgfx/hudrndgfx file gates OK\n");
    }

    def_free_weapons(&wf);
    if (!have_retail)
        return retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/def/weapon.def (the shipped weapon table)");
    printf("PASS: weapon parsing OK\n");
    return 0;
}
