#include "object/lights_to_ase.h"

#include "ase/types.h"
#include "threedi/threedi_3di3.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        return false;
    }
    return true;
}

uint32_t fbits(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool nearly_equal(float a, float b, float eps = 1.0e-6f) {
    return std::fabs(a - b) <= eps;
}

Threedi3di3 make_model(ThreediLod* lod, ThreediRenderObject* parts, ThreediLight* lights) {
    std::memset(lod, 0, sizeof(*lod));
    std::memset(parts, 0, sizeof(ThreediRenderObject) * 2);
    std::memset(lights, 0, sizeof(*lights));

    lod->render_objects = parts;
    lod->render_object_count = 2;
    parts[1].abs[0] = -1.4901161193847656e-8f;

    lights[0].offset[0] = 0.17569996416568756f;
    lights[0].offset[1] = 1.2802000045776367f;
    lights[0].offset[2] = 3.9030001163482666f;
    lights[0].subobj_index = 1;
    lights[0].falloff_byte = 2;
    lights[0].rotation[2] = 1.0f;

    Threedi3di3 ir{};
    ir.lods = lod;
    ir.lod_count = 1;
    ir.lights = lights;
    ir.light_count = 1;
    return ir;
}

}  // namespace

int main() {
    ThreediLod lod{};
    ThreediRenderObject parts[2]{};
    ThreediLight lights[1]{};
    Threedi3di3 ir = make_model(&lod, parts, lights);

    ase_Light out[1]{};
    int count = object_emit_lights(&ir, out, 1);
    if (!expect(count == 1, "expected one emitted light")) return 1;
    if (!expect(fbits(out[0].pos[0]) == 0xBE33EAB1u,
                "tiny parent offset should not shift light x by one ULP")) return 1;

    parts[1].abs[0] = -0.25f;
    count = object_emit_lights(&ir, out, 1);
    if (!expect(count == 1, "expected one emitted light after parent move")) return 1;
    if (!expect(nearly_equal(out[0].pos[0], 0.25f - lights[0].offset[0]),
                "meaningful parent offset should still be applied")) return 1;

    return 0;
}
