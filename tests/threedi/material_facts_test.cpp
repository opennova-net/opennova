// The MTRL authoring facts the GUT projected-shadow witness keys on, read off
// the minted fixtures/threedi/crate_alphagen24.3di through the engine's own
// 3DI3 parser (the retired MaterialInfo ClassDB record served them to
// GDScript, ADR 0043 d10). The crate wears the row JOTAC's SCrate1.3di
// carries (witnessed 2026-10-19, the retired retail row): retail's constant
// AlphaGen style (24, start 128 -> 128/255 material alpha) on an opaque
// FF_ST_OP material with no alpha test, the row an opaque projected shadow
// must not reject (terrain_static_shadow_runtime_test.gd).
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/test_paths.h"

#include <formats/threedi/threedi_3di3.h>

using namespace opennova::threedi;

static int failures = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

int main() {
    const std::vector<uint8_t> bytes = test_io::read_file(test_paths_fixture("threedi/crate_alphagen24.3di"));
    CHECK(!bytes.empty());
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
    if (failures == 0) std::printf("OK: threedi_material_facts (crate_alphagen24)\n");
    return failures == 0 ? 0 : 1;
}
