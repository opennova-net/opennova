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

    def_free_weapons(&wf);
    printf("PASS: weapon parsing OK\n");
    return 0;
}
