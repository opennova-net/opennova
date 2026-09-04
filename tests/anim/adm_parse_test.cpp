// Parse the shipped mp5_1st.adm (the reference fixture set, OPENNOVA_JO_ASSETS;
// the whole test is gated on it) and pin its entries. The authored rig maps
// under fixtures/anim are the unconditional coverage (adm_variants, the
// simassets clip index).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <formats/adm/adm.h>

#include <string>

#include "common/retail_paths.h"

using namespace opennova::adm;

int main(void) {
    const std::string fixture = retail::reference_fixture("adm/mp5_1st.adm");
    if (fixture.empty())
        return retail::skip("OPENNOVA_JO_ASSETS/fixtures/adm/mp5_1st.adm (the shipped MP5 rig map)");
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
                    adm.entries[i].variant_count ? adm.entries[i].variants[0] : "");
        adm_free(&adm);
        return 1;
    }

    if (strcmp(adm.entries[0].key, "anim_reset") != 0 ||
        adm.entries[0].variant_count != 1 ||
        strcmp(adm.entries[0].variants[0], "mp5_RST") != 0) {
        fprintf(stderr, "FAIL: first entry mismatch: '%s' -> '%s'\n",
                adm.entries[0].key,
                adm.entries[0].variant_count ? adm.entries[0].variants[0] : "");
        adm_free(&adm);
        return 1;
    }

    if (strcmp(adm.entries[4].key, "anim_wpn_reload") != 0 ||
        adm.entries[4].variant_count != 1 ||
        strcmp(adm.entries[4].variants[0], "mp5_1r") != 0) {
        fprintf(stderr, "FAIL: reload entry mismatch: '%s' -> '%s'\n",
                adm.entries[4].key,
                adm.entries[4].variant_count ? adm.entries[4].variants[0] : "");
        adm_free(&adm);
        return 1;
    }

    adm_free(&adm);
    printf("PASS: mp5_1st.adm parsed correctly (9 entries)\n");
    return 0;
}
