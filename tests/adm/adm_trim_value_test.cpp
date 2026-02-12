// Test that whitespace around quoted values is trimmed during ADM parsing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adm/adm.h"
#include "common/test_paths.h"

int main(void) {
    const char *test_content =
        "anim_idle                                \"Dt1IdleA.bad    \"\n"
        "anim_run\t\t\"RunAnim.bad\"\n"
        "anim_walk  \"  WalkAnim.bad  \"\n";

    char temp_path[4096];
    snprintf(temp_path, sizeof(temp_path), "%s/adm_trim_test.adm", test_paths_temp_dir());
    FILE *f = fopen(temp_path, "wb");
    if (!f) {
        fprintf(stderr, "FAIL: could not create temp file\n");
        return 1;
    }
    fwrite(test_content, 1, strlen(test_content), f);
    fclose(f);

    AdmFile adm;
    memset(&adm, 0, sizeof(adm));
    if (adm_parse(temp_path, &adm) != 0) {
        fprintf(stderr, "FAIL: adm_parse failed\n");
        remove(temp_path);
        return 1;
    }

    if (adm.count != 3) {
        fprintf(stderr, "FAIL: expected 3 entries, got %zu\n", adm.count);
        adm_free(&adm);
        remove(temp_path);
        return 1;
    }

    if (strcmp(adm.entries[0].key, "anim_idle") != 0) {
        fprintf(stderr, "FAIL: entry 0 key mismatch: '%s'\n", adm.entries[0].key);
        adm_free(&adm);
        remove(temp_path);
        return 1;
    }
    if (strcmp(adm.entries[0].value, "Dt1IdleA.bad") != 0) {
        fprintf(stderr, "FAIL: entry 0 value not trimmed: '%s'\n", adm.entries[0].value);
        adm_free(&adm);
        remove(temp_path);
        return 1;
    }

    if (strcmp(adm.entries[1].key, "anim_run") != 0 ||
        strcmp(adm.entries[1].value, "RunAnim.bad") != 0) {
        fprintf(stderr, "FAIL: entry 1 mismatch: '%s' -> '%s'\n",
                adm.entries[1].key, adm.entries[1].value);
        adm_free(&adm);
        remove(temp_path);
        return 1;
    }

    if (strcmp(adm.entries[2].key, "anim_walk") != 0) {
        fprintf(stderr, "FAIL: entry 2 key mismatch: '%s'\n", adm.entries[2].key);
        adm_free(&adm);
        remove(temp_path);
        return 1;
    }
    if (strcmp(adm.entries[2].value, "WalkAnim.bad") != 0) {
        fprintf(stderr, "FAIL: entry 2 value not trimmed: '%s'\n", adm.entries[2].value);
        adm_free(&adm);
        remove(temp_path);
        return 1;
    }

    adm_free(&adm);
    remove(temp_path);
    printf("PASS: values trimmed correctly\n");
    return 0;
}
