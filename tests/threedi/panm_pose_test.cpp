// threedi_panm_pose — the model-level PANM pose layer extracted from the
// shell adapter (S3, ADR 0028): LOD-effective node selection, liveness,
// register resolution, and the part-indexed pose array with its
// base-transform fallback, pinned over a hand-built Threedi3di3.
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <formats/threedi/threedi_panm.h>
#include <formats/threedi/threedi_panm_pose.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace opennova::threedi;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

bool near(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) < tol; }

ThreediPartAnimation spinner_node(uint8_t subobject) {
    // The quarter-cycle spinner the runtime test pins: rot_type 1, coefficient
    // 1.0f reinterpreted over rotation_y.control.
    ThreediPartAnimation node;
    std::memset(&node, 0, sizeof(node));
    node.flags = 1u << 8;
    node.parent_subobject = 0xff;
    node.subobject_index = subobject;
    const float coefficient = 1.0f;
    std::memcpy(&node.rotation_y.control, &coefficient, sizeof(coefficient));
    return node;
}

} // namespace

int main() {
    // ---- liveness classification ----
    ThreediPartAnimation inert;
    std::memset(&inert, 0, sizeof(inert));
    CHECK(!threedi_panm_animation_is_live(inert));
    CHECK(threedi_panm_animation_is_live(spinner_node(0))); // spinner: always
    ThreediPartAnimation euler;
    std::memset(&euler, 0, sizeof(euler));
    euler.flags = 2u << 8; // euler rotation family
    CHECK(!threedi_panm_animation_is_live(euler)); // no live track yet
    euler.rotation_x.control = 0x12; // sine source, high nibble set
    CHECK(threedi_panm_animation_is_live(euler));
    CHECK(!threedi_panm_animation_uses_noise(euler));
    euler.rotation_x.control = 0x16; // noise style (low nibble 6)
    CHECK(threedi_panm_animation_uses_noise(euler));

    // ---- the model: 2 LODs, 3 parts each, LOD0 has no local PANM ----
    ThreediRenderObject parts[3];
    std::memset(parts, 0, sizeof(parts));
    parts[0].abs[0] = 1.0f;
    parts[1].abs[1] = 2.0f;
    parts[2].abs[2] = 3.0f;
    ThreediLod lods[2];
    std::memset(lods, 0, sizeof(lods));
    lods[0].render_objects = parts;
    lods[0].render_object_count = 3;
    lods[1].render_objects = parts;
    lods[1].render_object_count = 3;
    ThreediPartAnimation model_nodes[1] = {spinner_node(1)};
    ThreediPartAnimation lod1_nodes[1] = {spinner_node(2)};
    lods[1].part_animations = lod1_nodes;
    lods[1].part_animation_count = 1;
    Threedi3di3 model;
    std::memset(&model, 0, sizeof(model));
    model.lods = lods;
    model.lod_count = 2;
    model.part_animations = model_nodes;
    model.part_animation_count = 1;

    // Selection: LOD0 inherits the model-level block; LOD1's local block wins.
    std::vector<ThreediPartAnimation> nodes;
    CHECK(threedi_panm_effective_for_lod(model, 0, nodes));
    CHECK(nodes.size() == 1 && nodes[0].subobject_index == 1);
    CHECK(threedi_panm_effective_for_lod(model, 1, nodes));
    CHECK(nodes.size() == 1 && nodes[0].subobject_index == 2);
    CHECK(!threedi_panm_effective_for_lod(model, 2, nodes)); // invalid lod
    CHECK(threedi_panm_lod_has_live(model, 0));
    CHECK(threedi_panm_lod_has_live(model, 1));
    CHECK(!threedi_panm_lod_has_live(model, 5));

    // Time clamp: retail's GetTickCount DWORD never goes negative.
    CHECK(threedi_panm_runtime_time_ms(-5) == 0u);
    CHECK(threedi_panm_runtime_time_ms(640) == 640u);

    // ---- pose_parts: animated part gets the node matrix, the rest keep
    // their base (translation-only abs pivot) ----
    std::vector<ThreediMatrix4x4> mats;
    std::vector<uint8_t> animated;
    CHECK(threedi_panm_pose_parts(model, 0, 250, nullptr, mats, &animated));
    CHECK(mats.size() == 3 && animated.size() == 3);
    CHECK(!animated[0] && animated[1] && !animated[2]);
    // Part 0: base transform — identity rotation, abs pivot translation.
    CHECK(near(mats[0].m[0], 1.0f) && near(mats[0].m[5], 1.0f));
    CHECK(near(mats[0].m[12], 1.0f) && near(mats[0].m[13], 0.0f));
    // Part 1: the quarter-cycle spinner at 250 ms (the runtime test's pin).
    CHECK(near(mats[1].m[0], 0.0f) && near(mats[1].m[1], 1.0f));
    CHECK(near(mats[1].m[4], -1.0f) && near(mats[1].m[5], 0.0f));
    // Part 2: base again.
    CHECK(near(mats[2].m[14], 3.0f));

    // ---- register resolution: a >0x70 style rewrites the file-local CTRL
    // index into the global loader ordinal ----
    ThreediControlRegister registers[2];
    std::memset(registers, 0, sizeof(registers));
    std::snprintf(registers[1].name, sizeof(registers[1].name), "%s",
            "VEHICLE_SPECIAL1");
    model.ctrl.registers = registers;
    model.ctrl.count = 2;
    std::vector<ThreediPartAnimation> resolve_nodes(1);
    std::memset(&resolve_nodes[0], 0, sizeof(resolve_nodes[0]));
    resolve_nodes[0].rotation_x.control = 113; // controlled style
    resolve_nodes[0].rotation_x.control_param = 1; // file-local index
    threedi_panm_resolve_registers(model, resolve_nodes);
    CHECK(resolve_nodes[0].rotation_x.control_param ==
            THREEDI_CTRL_VEHICLE_SPECIAL1);
    // An out-of-range local index aliases the zero ordinal (LOD_FRAC).
    resolve_nodes[0].rotation_y.control = 113;
    resolve_nodes[0].rotation_y.control_param = 9;
    threedi_panm_resolve_registers(model, resolve_nodes);
    CHECK(resolve_nodes[0].rotation_y.control_param == 0);

    // H50cal's barrel uses file PANM byte +6 to select MTRX row 1,
    // diag(-1, 1, -1). Its reverse 360->0 pitch track must raise +Z
    // for a negative gun-relative pitch word. This exercises the model-level
    // path used by the gun CAMERA userpoint, not just the scalar sampler.
    // [orig: GPM_LoadRenderModel @ 0x5B569C; Model_TransformBoneMatrices
    // @ 0x58E8AA / @ 0x58EAA7]
    ThreediMatrix4x4 frames[2];
    threedi_mat4_identity(&frames[0]);
    threedi_mat4_identity(&frames[1]);
    frames[1].m[0] = frames[1].m[10] = -1.0f;
    ThreediPartAnimation barrel{};
    barrel.flags = 2u << 8;
    barrel.parent_subobject = 0xff;
    barrel.matrix_index = 1;
    barrel.rotation_y.control = 113;
    barrel.rotation_y.start = 16384;
    std::strcpy(registers[0].name, "EWEAP_GUNPITCH");
    ThreediRenderObject barrel_part{};
    ThreediLod barrel_lod{};
    barrel_lod.render_objects = &barrel_part;
    barrel_lod.render_object_count = 1;
    barrel_lod.part_animations = &barrel;
    barrel_lod.part_animation_count = 1;
    Threedi3di3 gun{};
    gun.lods = &barrel_lod;
    gun.lod_count = 1;
    gun.ctrl = model.ctrl;
    gun.mtrx.count = 2;
    gun.mtrx.matrices = frames;
    int32_t bus[THREEDI_CTRL_REGISTER_COUNT]{};
    for (int pitch_word : {0xf000, 0x1000}) {
        bus[THREEDI_CTRL_EWEAP_GUNPITCH] = pitch_word;
        CHECK(threedi_panm_pose_parts(gun, 0, 0, bus, mats, nullptr));
        const float elevation = pitch_word == 0xf000 ? 0.38268343f : -0.38268343f;
        CHECK(near(mats[0].m[9], elevation));
        CHECK(near(mats[0].m[6], -elevation));
        CHECK(near(mats[0].m[10], 0.92387953f));
    }
    // Zero is a sentinel: even a nonidentity MTRX[0] is not applied.
    barrel.matrix_index = 0;
    frames[0] = frames[1];
    CHECK(threedi_panm_pose_parts(gun, 0, 0, bus, mats, nullptr));
    CHECK(near(mats[0].m[9], 0.38268343f));

    // An affine animation frame needs its actual inverse, including its
    // translation. At a quarter turn, (0,0,0) -> (10,20,30) -> (-20,10,30)
    // -> (-15,-10/3,0) through frame, rotation, inverse-frame respectively.
    barrel = spinner_node(0);
    barrel.matrix_index = 1;
    threedi_mat4_identity(&frames[1]);
    frames[1].m[0] = 2.0f; frames[1].m[5] = 3.0f; frames[1].m[10] = 4.0f;
    frames[1].m[12] = 10.0f; frames[1].m[13] = 20.0f; frames[1].m[14] = 30.0f;
    CHECK(threedi_panm_pose_parts(gun, 0, 250, bus, mats, nullptr));
    CHECK(near(mats[0].m[1], 2.0f / 3.0f) && near(mats[0].m[4], -1.5f));
    CHECK(near(mats[0].m[12], -15.0f) && near(mats[0].m[13], -10.0f / 3.0f));
    CHECK(near(mats[0].m[14], 0.0f));

    if (failures == 0) std::printf("threedi_panm_pose: OK\n");
    return failures == 0 ? 0 : 1;
}
