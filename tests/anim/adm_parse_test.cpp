// Parse the minted first-person table fixtures/anim/vm40.adm (the stock
// mp5_1st.adm's shape: the reset and the eight weapon slots in the stock order;
// tests/fixtures/minimal_bad_gen.cpp) and pin its entries. adm_variants and the
// anim clip index cover the parser's other forms.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <formats/adm/adm.h>

#include <string>

#include "common/test_paths.h"

using namespace opennova::adm;

int main(void) {
    const std::string fixture = test_paths_fixture("anim/vm40.adm");
    const char *path = fixture.c_str();

    AdmFile adm;
    memset(&adm, 0, sizeof(adm));
    if (adm_parse(path, &adm) != 0) {
        fprintf(stderr, "FAIL: adm_parse failed for %s\n", path);
        return 1;
    }

    if (adm.count != 9) {
        fprintf(stderr, "FAIL: expected 9 entries, got %zu\n", adm.count);
        for (size_t i = 0; i < adm.count; ++i)
            fprintf(stderr, "  %zu: %s -> %s\n", i, adm.entries[i].key,
                    adm.entries[i].variants.size() ? adm.entries[i].variants[0] : "");
        adm_free(&adm);
        return 1;
    }

    if (strcmp(adm.entries[0].key, "anim_reset") != 0 ||
        adm.entries[0].variants.size() != 1 ||
        strcmp(adm.entries[0].variants[0], "vm40_rst") != 0) {
        fprintf(stderr, "FAIL: first entry mismatch: '%s' -> '%s'\n",
                adm.entries[0].key,
                adm.entries[0].variants.size() ? adm.entries[0].variants[0] : "");
        adm_free(&adm);
        return 1;
    }

    if (strcmp(adm.entries[4].key, "anim_wpn_reload") != 0 ||
        adm.entries[4].variants.size() != 1 ||
        strcmp(adm.entries[4].variants[0], "vm40_r") != 0) {
        fprintf(stderr, "FAIL: reload entry mismatch: '%s' -> '%s'\n",
                adm.entries[4].key,
                adm.entries[4].variants.size() ? adm.entries[4].variants[0] : "");
        adm_free(&adm);
        return 1;
    }

    adm_free(&adm);
    printf("PASS: vm40.adm parsed correctly (9 entries)\n");
    return 0;
}
