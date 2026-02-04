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

    def_free_ammo(&ammo);
    printf("PASS: ammo parsing OK\n");
    return 0;
}
