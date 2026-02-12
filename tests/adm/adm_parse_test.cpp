// Test parsing a real .adm file (mp5_1st.adm) with expected entries.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adm/adm.h"
#include "common/test_paths.h"

int main(void) {
    const char *repo_root = test_paths_repo_root(__FILE__);
    char path[4096];
    snprintf(path, sizeof(path), "%s/fixtures/adm/mp5_1st.adm", repo_root);

    AdmFile adm;
    memset(&adm, 0, sizeof(adm));
    if (adm_parse(path, &adm) != 0) {
        fprintf(stderr, "FAIL: adm_parse failed for %s\n", path);
        return 1;
    }

    if (adm.count != 9) {
        fprintf(stderr, "FAIL: expected 9 entries, got %zu\n", adm.count);
        for (size_t i = 0; i < adm.count; ++i)
            fprintf(stderr, "  %zu: %s -> %s\n", i, adm.entries[i].key, adm.entries[i].value);
        adm_free(&adm);
        return 1;
    }

    if (strcmp(adm.entries[0].key, "anim_reset") != 0 ||
        strcmp(adm.entries[0].value, "mp5_RST") != 0) {
        fprintf(stderr, "FAIL: first entry mismatch: '%s' -> '%s'\n",
                adm.entries[0].key, adm.entries[0].value);
        adm_free(&adm);
        return 1;
    }

    if (strcmp(adm.entries[4].key, "anim_wpn_reload") != 0 ||
        strcmp(adm.entries[4].value, "mp5_1r") != 0) {
        fprintf(stderr, "FAIL: reload entry mismatch: '%s' -> '%s'\n",
                adm.entries[4].key, adm.entries[4].value);
        adm_free(&adm);
        return 1;
    }

    adm_free(&adm);
    printf("PASS: mp5_1st.adm parsed correctly (9 entries)\n");
    return 0;
}
