#include <runtime/renderer/model_panm_cache.h>
#include <formats/threedi/threedi_panm.h>
#include "common/test_expect.h"

#include <cmath>
#include <cstring>

using namespace opennova::renderer;
using namespace opennova::threedi;

namespace {
bool near(float a, float b) { return std::fabs(a - b) < 0.0001f; }
ThreediPartAnimation spinner(uint8_t part) {
    ThreediPartAnimation node{};
    node.flags = 1u << 8;
    node.parent_subobject = 0xff;
    node.subobject_index = part;
    const float coefficient = 1.0f;
    std::memcpy(&node.rotation_y.control, &coefficient, sizeof(coefficient));
    return node;
}
}

int main() {
    ThreediRenderObject parts[3]{};
    parts[0].abs[0] = 1;
    parts[1].abs[1] = 2;
    parts[2].abs[2] = 3;
    ThreediLod lods[2]{};
    for (auto &lod : lods) { lod.render_objects = parts; lod.render_object_count = 3; }
    auto model_node = spinner(1);
    auto lod_node = spinner(2);
    lods[1].part_animations = &lod_node;
    lods[1].part_animation_count = 1;
    Threedi3di3 model{};
    model.lods = lods;
    model.lod_count = 2;
    model.part_animations = &model_node;
    model.part_animation_count = 1;
    ModelPanmCache cache;
    ControlRegisterValues controls{};
    const auto *pose = cache.evaluate(model, 0, 250, controls);
    TEST_EXPECT(pose && pose->revision() == 1 && pose->part_count() == 3);
    TEST_EXPECT(pose->changed_part(0, 0)->m[12] == 1);
    TEST_EXPECT(near(pose->changed_part(1, -10)->m[1], 1));
    TEST_EXPECT(pose->changed_part(2, 0)->m[14] == 3);
    TEST_EXPECT(!pose->changed_part(0, 1) && !pose->changed_part(999, 0));
    TEST_EXPECT(cache.evaluation_serial() == 1);
    cache.evaluate(model, 0, 250, controls);
    TEST_EXPECT(cache.evaluation_serial() == 1);
    pose = cache.evaluate(model, 1, 250, controls);
    TEST_EXPECT(near(pose->changed_part(2, 0)->m[1], 1));
    TEST_EXPECT(pose->changed_part(1, 0)->m[1] == 0);
    cache.evaluate(model, 0, 250, controls);
    TEST_EXPECT(cache.evaluation_serial() == 2); // Switching LODs does not evict either.
    pose = cache.evaluate(model, 0, 500, controls);
    TEST_EXPECT(pose->revision() == 2 && pose->changed_part(1, 1));
    TEST_EXPECT(!pose->changed_part(0, 1) && !pose->changed_part(2, 1));
    pose = cache.evaluate(model, 0, 750, controls);
    TEST_EXPECT(pose->revision() == 3 && near(pose->changed_part(1, 1)->m[1], -1));
    TEST_EXPECT(!pose->changed_part(1, 3)); // Stale callers get the current, not missed pose.
    const auto revision_zero = cache.evaluate(model, 0, 0, controls)->revision();
    TEST_EXPECT(cache.evaluate(model, 0, 0x100000000LL, controls)->revision() == revision_zero);
    TEST_EXPECT(cache.evaluate(model, 0, -1, controls)->revision() == revision_zero);
    const auto serial = cache.evaluation_serial();
    TEST_EXPECT(!cache.evaluate(model, -1, 0, controls));
    TEST_EXPECT(!cache.evaluate(model, 2, 0, controls));
    TEST_EXPECT(!cache.evaluate(Threedi3di3{}, 0, 0, controls));
    TEST_EXPECT(cache.evaluation_serial() == serial);
    cache.clear();
    TEST_EXPECT(cache.evaluation_serial() == serial);
    parts[0].abs[0] = 7;
    pose = cache.evaluate(model, 0, 0, controls);
    TEST_EXPECT(pose->revision() == 1 && pose->changed_part(0, 0)->m[12] == 7);

    // Register-controlled translation: same-time bus changes must evaluate;
    // a change in an unused bus slot evaluates but cannot mint a pose revision.
    ThreediControlRegister reg{};
    std::strcpy(reg.name, "VEHICLE_SPECIAL1");
    model.ctrl.registers = &reg;
    model.ctrl.count = 1;
    model_node = {};
    model_node.parent_subobject = 0xff;
    model_node.flags = static_cast<uint32_t>(THREEDI_TRANS_X) << 24;
    model_node.translation.control = 113;
    model_node.translation.end = 256;
    cache.clear();
    pose = cache.evaluate(model, 0, 0, controls);
    const float rest_x = pose->changed_part(0, 0)->m[12];
    controls[THREEDI_CTRL_VEHICLE_SPECIAL1] = 0x8000;
    pose = cache.evaluate(model, 0, 0, controls);
    TEST_EXPECT(pose->revision() == 2 && near(pose->changed_part(0, 1)->m[12], rest_x + 0.5f));
    controls[THREEDI_CTRL_VEHICLE_SPECIAL1] = -0x8000;
    pose = cache.evaluate(model, 0, 0, controls);
    TEST_EXPECT(pose->revision() == 3 && near(pose->changed_part(0, 2)->m[12], rest_x - 0.5f));
    const auto before_unused = cache.evaluation_serial();
    controls[THREEDI_CTRL_LOD_FRAC] = 42;
    pose = cache.evaluate(model, 0, 0, controls);
    TEST_EXPECT(cache.evaluation_serial() == before_unused + 1 && pose->revision() == 3);
    controls = {};
    pose = cache.evaluate(model, 0, 0, controls);
    TEST_EXPECT(pose->revision() == 4 && near(pose->changed_part(0, 3)->m[12], rest_x));

    // Even a noise sample that produces an identical pose counts as an
    // evaluation. Keep a zero-length track to make that outcome deterministic.
    model_node.translation.control = 0x36;
    model_node.translation.end = 0;
    cache.clear();
    pose = cache.evaluate(model, 0, 50, controls);
    const auto noise_serial = cache.evaluation_serial();
    TEST_EXPECT(pose->revision() == 1);
    pose = cache.evaluate(model, 0, 50, controls);
    TEST_EXPECT(cache.evaluation_serial() == noise_serial + 1 && pose->revision() == 1);
    TEST_EXPECT(!pose->changed_part(0, 1));
    return 0;
}
