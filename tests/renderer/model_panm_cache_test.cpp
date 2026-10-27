#include <runtime/renderer/model_panm_cache.h>
#include <formats/threedi/threedi_panm.h>
#include <formats/threedi/threedi_panm_pose.h>
#include "common/test_expect.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace opennova::renderer;
using namespace opennova::threedi;

namespace {
bool near(float a, float b) { return std::fabs(a - b) < 0.0001f; }
// A row JO's loader keeps (rotation type 2): its rotation_z track the clock's
// sawtooth (style 4, rate 256, over 0..16384), a quarter turn about Z each
// 250 ms. A spinner (rotation type 1) stands still in JO (D-3DI-9).
ThreediPartAnimation turning(uint8_t part) {
    ThreediPartAnimation node{};
    node.flags = threedi_panm_pack_flags(0, 2, 0, 0);
    node.parent_subobject = 0xff;
    node.subobject_index = part;
    node.rotation_z.control = 0x14;
    node.rotation_z.rate = 256;
    node.rotation_z.end = 16384;
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
    auto model_node = turning(1);
    auto lod_node = turning(2);
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

    // H50cal's authored animation frame reverses the barrel's pitch axis.
    // The retained renderer must agree with the simulation's userpoint pose.
    ThreediMatrix4x4 frames[2];
    threedi_mat4_identity(&frames[0]);
    threedi_mat4_identity(&frames[1]);
    frames[1].m[0] = frames[1].m[10] = -1.0f;
    model.mtrx.count = 2;
    model.mtrx.matrices = frames;
    model_node = {};
    model_node.parent_subobject = 0xff;
    model_node.flags = 2u << 8;
    model_node.matrix_index = 1;
    model_node.rotation_y.control = 113;
    model_node.rotation_y.start = 16384;
    std::strcpy(reg.name, "EWEAP_GUNPITCH");
    controls = {};
    controls[THREEDI_CTRL_EWEAP_GUNPITCH] = 0xf000;
    cache.clear();
    pose = cache.evaluate(model, 0, 0, controls);
    TEST_EXPECT(near(pose->changed_part(0, 0)->m[9], 0.38268343f));
    controls[THREEDI_CTRL_EWEAP_GUNPITCH] = 0x1000;
    pose = cache.evaluate(model, 0, 0, controls);
    TEST_EXPECT(near(pose->changed_part(0, 0)->m[9], -0.38268343f));

    // Invalid authored frame references retain the same base-pose fallback
    // as the simulation evaluator, instead of publishing partial/zero matrices.
    model.mtrx.count = 1;
    cache.clear();
    pose = cache.evaluate(model, 0, 0, controls);
    TEST_EXPECT(near(pose->changed_part(0, 0)->m[12], parts[0].abs[0]));
    TEST_EXPECT(near(pose->changed_part(0, 0)->m[0], 1.0f));

    model_node = {};
    model_node.parent_subobject = 0xff;
    model_node.flags = static_cast<uint32_t>(THREEDI_TRANS_X) << 24;
    model_node.translation.end = 256;
    cache.clear();
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

    // D-COL-10: a view-moded part (rotation type 3) takes the drawing instance's view in its
    // model's frame. The render frame's camera reflects back into the native frame, its forward
    // row the reverse of its back axis.
    // [orig: Model_TransformBoneMatrices @0x58E390 - the type-3 branch @0x58EB48..0x58EC82;
    //  matA @0x27219C0]
    {
        const float right[3] = {1, 0, 0}, up[3] = {0, 1, 0}, back[3] = {0, 0, 1};
        const ThreediMatrix4x4 level = panm_view_in_model_frame(right, up, back);
        TEST_EXPECT(level.m[0] == -1 && level.m[5] == 1 && level.m[10] == -1);
        TEST_EXPECT(level.m[1] == 0 && level.m[4] == 0 && level.m[8] == 0 && level.m[15] == 1);
    }
    {
        ThreediRenderObject billboard_parts[1]{};
        billboard_parts[0].abs[0] = 2;
        billboard_parts[0].abs[1] = 3;
        billboard_parts[0].abs[2] = 4;
        ThreediLod billboard_lod{};
        billboard_lod.render_objects = billboard_parts;
        billboard_lod.render_object_count = 1;
        ThreediPartAnimation facing{};
        facing.flags = threedi_panm_pack_flags(0, 3, 0, 0);
        facing.parent_subobject = 0xff;
        facing.subobject_index = 0;
        Threedi3di3 billboard{};
        billboard.lods = &billboard_lod;
        billboard.lod_count = 1;
        billboard.part_animations = &facing;
        billboard.part_animation_count = 1;
        ModelPanmCache views;
        const ControlRegisterValues none{};
        // No view: identity. A rest frame keeps only its input's orientation, so the pivot
        // stays where the part's abs puts it and the part stands as authored.
        const auto *p = views.evaluate(billboard, 0, 0, none);
        const ThreediMatrix4x4 *rest = p != nullptr ? p->changed_part(0, 0) : nullptr;
        TEST_EXPECT(rest != nullptr && near(rest->m[0], 1) && near(rest->m[5], 1) &&
                near(rest->m[10], 1));
        TEST_EXPECT(rest != nullptr && near(rest->m[12], 0) && near(rest->m[13], 0) &&
                near(rest->m[14], 0));
        // A camera turned a quarter about the up axis: the part's rows are the view's, and its
        // pivot keeps its place.
        const float right[3] = {0, 0, -1}, up[3] = {0, 1, 0}, back[3] = {1, 0, 0};
        const ThreediMatrix4x4 turned = panm_view_in_model_frame(right, up, back);
        const uint64_t serial = views.evaluation_serial();
        p = views.evaluate(billboard, 0, 0, none, &turned);
        TEST_EXPECT(views.evaluation_serial() == serial + 1 && p->revision() == 2);
        const ThreediMatrix4x4 *changed = p->changed_part(0, 1);
        TEST_EXPECT(changed != nullptr);
        ThreediMatrix4x4 faced{};
        if (changed != nullptr) faced = *changed;
        for (int k : {0, 1, 2, 4, 5, 6, 8, 9, 10}) TEST_EXPECT(near(faced.m[k], turned.m[k]));
        const float pivot[3] = {2, 3, 4};
        float placed[3];
        threedi_mat4_apply_point(&faced, pivot, placed);
        TEST_EXPECT(near(placed[0], 2) && near(placed[1], 3) && near(placed[2], 4));
        // The same view keeps the pose; the level's pose_parts evaluation agrees with it.
        views.evaluate(billboard, 0, 0, none, &turned);
        TEST_EXPECT(views.evaluation_serial() == serial + 1);
        std::vector<ThreediMatrix4x4> parts_posed;
        TEST_EXPECT(threedi_panm_pose_parts(billboard, 0, 0, nullptr, parts_posed, nullptr,
                &turned));
        TEST_EXPECT(parts_posed.size() == 1 && near(parts_posed[0].m[12], faced.m[12]) &&
                near(parts_posed[0].m[14], faced.m[14]));
        // Back to no view evaluates again.
        views.evaluate(billboard, 0, 0, none);
        TEST_EXPECT(views.evaluation_serial() == serial + 2);
        // A level with no view-moded part ignores the view.
        model_node = turning(1);
        model.mtrx = {};
        model.ctrl = {};
        ModelPanmCache plain;
        plain.evaluate(model, 0, 250, none);
        const uint64_t plain_serial = plain.evaluation_serial();
        plain.evaluate(model, 0, 250, none, &turned);
        TEST_EXPECT(plain.evaluation_serial() == plain_serial);
    }
    return 0;
}
