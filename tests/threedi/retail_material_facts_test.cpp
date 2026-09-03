// Asset-gated (OPENNOVA_JO_DIR): the retail MTRL authoring facts the GUT
// projected-shadow witness keys on, read off the shipped model through the
// engine's own 3DI3 parser (the retired MaterialInfo ClassDB record served
// them to GDScript, ADR 0043 d10). Scrate1 authors retail's constant AlphaGen
// style (24, start 128 -> 128/255 material alpha) on an opaque FF_ST_OP
// material with no alpha test — the row an opaque projected shadow must not
// reject (terrain_static_shadow_runtime_test.gd).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/retail_paths.h"

#include <base/resource_index/resource_index.h>
#include <formats/threedi/threedi_3di3.h>

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

int main() {
    RETAIL_REQUIRE_OR_SKIP(install, retail::install(),
            "OPENNOVA_JO_DIR (a packed retail JO install serving Scrate1.3di)");
    opennova::ResourceIndex index;
    std::vector<uint8_t> bytes;
    bool served = index.scan(install) && index.has_file("Scrate1.3di") &&
                  index.read_file("Scrate1.3di", bytes);
    for (const std::string &expansion : retail::expansions()) {
        if (served) break;
        served = index.scan(install, expansion) && index.has_file("Scrate1.3di") &&
                 index.read_file("Scrate1.3di", bytes);
    }
    if (!served) return retail::skip("a retail mount serving Scrate1.3di");

    Threedi3di3 model{};
    CHECK(threedi_3di3_read_memory(bytes.data(), bytes.size(), &model) == 0);
    if (failures != 0) return 1;
    CHECK(model.material_count >= 1);
    if (model.material_count >= 1) {
        const ThreediMaterial &m = model.materials[0];
        CHECK(std::strcmp(m.shader_name, "FF_ST_OP") == 0);
        CHECK(m.alpha_gen.style == 24);  // retail's constant AlphaGen style
        CHECK(m.alpha_gen.start == 128); // the constant generator supplies 128/255 material alpha
        CHECK((m.material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) == 0);
    }
    threedi_3di3_free(&model);
    if (failures == 0) std::printf("OK: threedi_retail_material_facts (Scrate1)\n");
    return failures == 0 ? 0 : 1;
}
