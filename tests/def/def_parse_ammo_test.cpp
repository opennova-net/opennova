// Test parsing ammo.def — check ROCKET and AT_NULL entries.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "def/def.h"
#include "common/test_paths.h"

int main(void) {
    const char *repo_root = test_paths_repo_root(__FILE__);
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/def/ammo.def", repo_root);

    DefAmmoFile ammo;
    memset(&ammo, 0, sizeof(ammo));
    if (def_parse_ammo(path, &ammo) != 0) {
        fprintf(stderr, "FAIL: def_parse_ammo failed for %s\n", path);
        return 1;
    }

    if (ammo.count < 5) {
        fprintf(stderr, "FAIL: too few ammo entries: %zu\n", ammo.count);
        def_free_ammo(&ammo);
        return 1;
    }

    printf("Parsed %zu ammo types\n", ammo.count);

    /* Find the ROCKET entry */
    const DefAmmoDef *rocket = NULL;
    for (size_t i = 0; i < ammo.count; ++i) {
        if (strcmp(ammo.entries[i].name, "ROCKET") == 0) {
            rocket = &ammo.entries[i];
            break;
        }
    }

    if (!rocket) {
        fprintf(stderr, "FAIL: could not find ROCKET entry\n");
        def_free_ammo(&ammo);
        return 1;
    }

    if (rocket->velocity != 390) {
        fprintf(stderr, "FAIL: ROCKET velocity mismatch: %d\n", rocket->velocity);
        def_free_ammo(&ammo);
        return 1;
    }

    if (rocket->penetration_impact != 100) {
        fprintf(stderr, "FAIL: ROCKET penetration_impact mismatch: %d\n", rocket->penetration_impact);
        def_free_ammo(&ammo);
        return 1;
    }

    if (rocket->penetration_kz != 100) {
        fprintf(stderr, "FAIL: ROCKET penetration_kz mismatch: %d\n", rocket->penetration_kz);
        def_free_ammo(&ammo);
        return 1;
    }

    /* Fire-presentation fields (world-wac-ai-re §17.4): ai_launch sound-set name; a
       one-value `tracer_type rocket` fills BOTH slots (friendly copies into enemy). */
    if (strcmp(rocket->ai_launch, "GS_AT4") != 0) {
        fprintf(stderr, "FAIL: ROCKET ai_launch: got '%s' want 'GS_AT4'\n", rocket->ai_launch);
        def_free_ammo(&ammo);
        return 1;
    }
    if (rocket->tracer_type_friendly != 3 || rocket->tracer_type_enemy != 3) {
        fprintf(stderr, "FAIL: ROCKET tracer_type: got %d/%d want 3/3 (one-value copy)\n",
                rocket->tracer_type_friendly, rocket->tracer_type_enemy);
        def_free_ammo(&ammo);
        return 1;
    }
    /* The tracer round's item graphic id (raw type id kept, host resolves) and the
       in-flight glow: `frndlyTrcrID 4502`, `light_move 6.0 128 120 80`
       [orig: AmmoDef_ParseProperty @0x40a5f8 -> +16; light_move -> +120 fp16 /
       +124 = ((r<<8)+g)<<8 + b]. */
    if (rocket->frndly_trcr_type_id != 4502 || rocket->foe_trcr_type_id != 0) {
        fprintf(stderr, "FAIL: ROCKET frndlyTrcrID/foeTrcrID: got %d/%d want 4502/0\n",
                rocket->frndly_trcr_type_id, rocket->foe_trcr_type_id);
        def_free_ammo(&ammo);
        return 1;
    }
    if (rocket->light_move_radius_fp16 != 6 * 65536 ||
        rocket->light_move_color != ((128 << 16) | (120 << 8) | 80)) {
        fprintf(stderr, "FAIL: ROCKET light_move: got %d/0x%X want %d/0x%X\n",
                rocket->light_move_radius_fp16, rocket->light_move_color, 6 * 65536,
                (128 << 16) | (120 << 8) | 80);
        def_free_ammo(&ammo);
        return 1;
    }

    /* Find the AT_NULL entry — should have 0 velocity */
    const DefAmmoDef *null_ammo = NULL;
    for (size_t i = 0; i < ammo.count; ++i) {
        if (strcmp(ammo.entries[i].name, "AT_NULL") == 0) {
            null_ammo = &ammo.entries[i];
            break;
        }
    }

    if (!null_ammo) {
        fprintf(stderr, "FAIL: could not find AT_NULL entry\n");
        def_free_ammo(&ammo);
        return 1;
    }

    if (null_ammo->velocity != 0) {
        fprintf(stderr, "FAIL: AT_NULL velocity should be 0, got %d\n", null_ammo->velocity);
        def_free_ammo(&ammo);
        return 1;
    }

    /* The round-sim field set (net-re §5.60) against the real AMMO_CAR15_556MM block:
       velocity 854, max_age 3 s -> 186 ticks, arm_age 0, error 0, drag 0.255 -> 16712
       (16.16), weight 62, penetration 20, kztype rounds_kz_C4 = 5, tracerRate 3,
       bullet_radius 0.00278 -> 182. */
    const DefAmmoDef *car15 = NULL;
    for (size_t i = 0; i < ammo.count; ++i) {
        if (strcmp(ammo.entries[i].name, "AMMO_CAR15_556MM") == 0) {
            car15 = &ammo.entries[i];
            break;
        }
    }
    if (!car15) {
        fprintf(stderr, "FAIL: could not find AMMO_CAR15_556MM entry\n");
        def_free_ammo(&ammo);
        return 1;
    }
    struct { const char *what; long got, want; } checks[] = {
        {"velocity", car15->velocity, 854},
        {"max_age_ticks", car15->max_age_ticks, 186},
        {"arm_age_ticks", car15->arm_age_ticks, 0},
        {"error_fp16", car15->error_fp16, 0},
        {"drag_fp16", car15->drag_fp16, 16712},
        {"tumble_error_fp16", car15->tumble_error_fp16, 655},
        {"weight_in_grains", car15->weight_in_grains, 62},
        {"penetration_impact", car15->penetration_impact, 20},
        {"kztype", car15->kztype, DEF_AMMO_KZ_C4},
        {"tracer_rate", car15->tracer_rate, 3},
        {"bullet_radius_fp16", car15->bullet_radius_fp16, 182},
        {"max_damage", car15->max_damage, 0},
        /* The presentation fields: `Mf_Light 100` sets flag + value [orig: @0x40a81b/+40];
           `tracer_type stdred stdgreen` = the witnessed 1/2 ids. */
        {"mf_light", car15->mf_light, 1},
        {"mf_light_value", car15->mf_light_value, 100},
        {"tracer_type_friendly", car15->tracer_type_friendly, 1},
        {"tracer_type_enemy", car15->tracer_type_enemy, 2},
    };
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        if (checks[i].got != checks[i].want) {
            fprintf(stderr, "FAIL: AMMO_CAR15_556MM %s: got %ld want %ld\n", checks[i].what,
                    checks[i].got, checks[i].want);
            def_free_ammo(&ammo);
            return 1;
        }
    }
    /* The name tokens land case-preserved; `ai_Launcheffect` in the data is matched
       case-insensitively and must NOT be eaten by the `ai_launch` prefix branch. */
    if (strcmp(car15->ai_launch, "GS_M4AI") != 0 ||
        strcmp(car15->ai_launcheffect, "Effect_CAR15MF") != 0) {
        fprintf(stderr, "FAIL: AMMO_CAR15_556MM ai_launch/'effect: got '%s'/'%s'\n",
                car15->ai_launch, car15->ai_launcheffect);
        def_free_ammo(&ammo);
        return 1;
    }

    /* Flag bits via a LAW-style entry (flag LAWR / NoGravity / forcetracer). */
    const DefAmmoDef *law = NULL;
    for (size_t i = 0; i < ammo.count; ++i) {
        if (ammo.entries[i].flags & DEF_AMMO_FLAG_LAWR) {
            law = &ammo.entries[i];
            break;
        }
    }
    if (!law) {
        fprintf(stderr, "FAIL: no entry carries the LAWR flag\n");
        def_free_ammo(&ammo);
        return 1;
    }
    if ((law->flags & DEF_AMMO_FLAG_NOGRAVITY) == 0 ||
        (law->flags & DEF_AMMO_FLAG_FORCETRACER) == 0) {
        fprintf(stderr, "FAIL: LAWR entry %s missing NoGravity/forcetracer bits (0x%x)\n",
                law->name, law->flags);
        def_free_ammo(&ammo);
        return 1;
    }

    def_free_ammo(&ammo);
    printf("PASS: ammo parsing OK\n");
    return 0;
}
