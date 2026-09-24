// The .bad construction seam (formats/bad/bad_build.h): the mission <-> clip
// frame maps, the derived bone table, the capsule extents, the validation, and
// the canonical .adm table. The retail leg (OPENNOVA_JO_ASSETS, SKIP-LEG) pins
// the two derivations against the shipped 19-bone BINOC.bad: every bone's
// rotation is the transpose of its first key, and the pivot relation inverts.
#include <formats/adm/adm.h>
#include <formats/bad/bad.h>
#include <formats/bad/bad_build.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"

using namespace opennova::bad;

namespace {

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

BadBuildQuat axis_quat(double ax, double ay, double az, double degrees) {
    const double half = degrees * 3.14159265358979323846 / 360.0;
    const double s = std::sin(half);
    const double len = std::sqrt(ax * ax + ay * ay + az * az);
    return BadBuildQuat{ax / len * s, ay / len * s, az / len * s, std::cos(half)};
}

// A two-bone clip: the root at the origin, a second bone one metre up (mission
// z), four frames of motion and one event per key.
BadBuildClip two_bone_clip(uint32_t flags) {
    BadBuildClip clip;
    clip.name = "authored";
    clip.frame_count = 3;
    clip.flags = flags;
    const size_t keys = clip.frame_count + 1;

    BadBuildBone root;
    root.name = "BN01 Root";
    root.parent = -1;
    root.length = 1.0;
    BadBuildBone tip;
    tip.name = "BN02 Tip";
    tip.parent = 0;
    tip.pivot = BadBuildVec3{0.0, 0.0, 1.0};
    tip.length = 0.5;
    for (size_t f = 0; f < keys; ++f) {
        // The root yaws about mission z (up), the tip holds its rest.
        root.keys.push_back(axis_quat(0.0, 0.0, 1.0, 30.0 * static_cast<double>(f)));
        tip.keys.push_back(BadBuildQuat{});
        if ((flags & BAD_FLAG_TRANSLATION) != 0) {
            root.translations.push_back(BadBuildVec3{});
            tip.translations.push_back(BadBuildVec3{0.0, 0.0, 0.25 * static_cast<double>(f)});
        }
    }
    clip.bones.push_back(root);
    clip.bones.push_back(tip);
    for (size_t f = 0; f < keys; ++f) {
        BadBuildEvent ev;
        ev.velocity = BadBuildVec3{0.05, 0.0, 0.0};
        ev.trigger = f == 1 ? 0x1 : 0;
        clip.events.push_back(ev);
    }
    return clip;
}

} // namespace

int main() {
    // The frame maps: mission x forward / y left / z up becomes clip
    // x side / y up / z forward, and both directions invert.
    {
        const BadBuildVec3 m{1.0, 2.0, 3.0};
        const BadBuildVec3 c = bad_clip_from_mission(m);
        TEST_EXPECT(near(c.x, 2.0) && near(c.y, 3.0) && near(c.z, 1.0));
        const BadBuildVec3 back = bad_mission_from_clip(c);
        TEST_EXPECT(near(back.x, m.x) && near(back.y, m.y) && near(back.z, m.z));

        // A yaw about mission up turns about the clip's up axis, which is y.
        const BadBuildQuat yaw = bad_clip_from_mission(axis_quat(0.0, 0.0, 1.0, 90.0));
        TEST_EXPECT(near(yaw.x, 0.0, 1e-9) && near(yaw.z, 0.0, 1e-9));
        TEST_EXPECT(near(std::fabs(yaw.y), std::sin(3.14159265358979323846 / 4.0)));
        const BadBuildQuat roundtrip = bad_mission_from_clip(yaw);
        TEST_EXPECT(near(std::fabs(roundtrip.z), std::sin(3.14159265358979323846 / 4.0)));
        std::printf("frame maps: mission(1,2,3) -> clip(%g,%g,%g), yaw about clip y\n", c.x, c.y, c.z);
    }

    // The derived bone table, and the clip the writer serializes.
    {
        const BadBuildClip clip = two_bone_clip(BAD_FLAG_LOOP | BAD_FLAG_TRANSLATION);
        BadAssembled built;
        std::string error;
        TEST_EXPECT(bad_build_assemble(clip, built, &error));
        TEST_EXPECT(built.file.bone_count == 2 && built.file.frame_count == 3);
        // Channels, events and translations all carry the terminal duplicate.
        TEST_EXPECT(built.channels[0].frame_count == 4 && built.file.num_events == 4);
        TEST_EXPECT(built.file.num_translations == 8);
        TEST_EXPECT(built.durations[0][0] == 1 && built.durations[1][3] == 1);
        TEST_EXPECT(built.bones[0].parent_index == -1 && built.bones[1].parent_index == 0);

        // rotation[9] is the transpose of the stored first key.
        for (size_t i = 0; i < 2; ++i) {
            const BadQuaternion &k0 = built.rotations[i][0];
            float rows[9];
            bad_quat_to_rows(BadBuildQuat{k0.x, k0.y, k0.z, k0.w}, rows);
            float want[9];
            bad_rows_transpose(rows, want);
            for (int k = 0; k < 9; ++k) TEST_EXPECT(near(built.bones[i].rotation[k], want[k], 1e-6));
        }

        // position[3] follows the parent's bind and the pivot difference: the
        // tip sits one metre up, which is the clip frame's y.
        const BadBuildVec3 rel = bad_bone_rel(built.bones[0].rotation, built.bones[1].position);
        TEST_EXPECT(near(rel.x, 0.0, 1e-5) && near(rel.y, 0.0, 1e-5) && near(rel.z, 1.0, 1e-5));

        // The event velocity converts with the same map; the trigger does not.
        TEST_EXPECT(near(built.events[0].velocity[1], 0.0, 1e-6));
        TEST_EXPECT(near(built.events[0].velocity[2], 0.05, 1e-6));
        TEST_EXPECT(built.events[1].trigger == 0x1);

        // A mint reads back through the parser.
        std::vector<uint8_t> bytes;
        TEST_EXPECT(bad_build_mint(clip, bytes, &error));
        BadFile parsed = {};
        TEST_EXPECT(bad_parse_buffer(bytes.data(), bytes.size(), &parsed) == 0);
        TEST_EXPECT(parsed.bone_count == 2 && parsed.frame_count == 3 && parsed.fps == 30);
        TEST_EXPECT(parsed.num_events == 4 && parsed.num_channels == 2);
        TEST_EXPECT(std::strcmp(parsed.bones[1].name, "BN02 Tip") == 0);
        bad_free(&parsed);
        std::printf("two-bone clip: %zu bytes, derived bind and position\n", bytes.size());
    }

    // The capsule extents: bone 0 is the reference, so the tip one metre up
    // gives no drop and a metre of height; its translation lifts it further.
    {
        const BadBuildClip clip = two_bone_clip(BAD_FLAG_TRANSLATION);
        std::vector<double> bottom;
        std::vector<double> top;
        bad_clip_extents(clip, bottom, top);
        TEST_EXPECT(bottom.size() == 4 && top.size() == 4);
        TEST_EXPECT(near(bottom[0], 0.0) && near(top[0], 1.0, 1e-5));
        TEST_EXPECT(near(top[3], 1.75, 1e-5));

        // With no author-given pair, the events take the derived one.
        BadAssembled built;
        std::string error;
        TEST_EXPECT(bad_build_assemble(clip, built, &error));
        TEST_EXPECT(near(built.events[3].top, 1.75, 1e-5));

        // A constant pair wins over the derivation, the shape retail's
        // viewmodel clips carry.
        BadBuildClip constant = clip;
        constant.capsule_given = true;
        constant.capsule_bottom = 0.0;
        constant.capsule_top = 0.6;
        TEST_EXPECT(bad_build_assemble(constant, built, &error));
        for (size_t f = 0; f < 4; ++f) TEST_EXPECT(near(built.events[f].top, 0.6, 1e-6));

        // An explicit pair wins over both.
        BadBuildClip explicit_pair = constant;
        explicit_pair.events[2].extents_given = true;
        explicit_pair.events[2].bottom = 0.25;
        explicit_pair.events[2].top = 1.5;
        TEST_EXPECT(bad_build_assemble(explicit_pair, built, &error));
        TEST_EXPECT(near(built.events[2].bottom, 0.25, 1e-6) && near(built.events[2].top, 1.5, 1e-6));
        std::printf("capsule extents: derived %g/%g, constant and explicit override\n", bottom[3], top[3]);
    }

    // What the format cannot hold.
    {
        BadAssembled built;
        std::string error;
        BadBuildClip clip = two_bone_clip(0);

        BadBuildClip empty;
        TEST_EXPECT(!bad_build_assemble(empty, built, &error) && !error.empty());

        BadBuildClip forward = clip;
        forward.bones[0].parent = 1;
        TEST_EXPECT(!bad_build_assemble(forward, built, &error));

        BadBuildClip sibling = clip;
        sibling.bones[1].parent = 1;
        TEST_EXPECT(!bad_build_assemble(sibling, built, &error));

        BadBuildClip short_keys = clip;
        short_keys.bones[1].keys.pop_back();
        TEST_EXPECT(!bad_build_assemble(short_keys, built, &error));

        BadBuildClip promised = clip;
        promised.flags = BAD_FLAG_TRANSLATION;
        TEST_EXPECT(!bad_build_assemble(promised, built, &error));

        BadBuildClip long_name = clip;
        long_name.bones[1].name = std::string(32, 'x');
        TEST_EXPECT(!bad_build_assemble(long_name, built, &error));

        BadBuildClip unit = clip;
        unit.bones[1].keys[2] = BadBuildQuat{0.0, 0.0, 0.0, 0.0};
        TEST_EXPECT(!bad_build_assemble(unit, built, &error));

        BadBuildClip zero_duration = clip;
        zero_duration.bones[1].durations.assign(4, 1);
        zero_duration.bones[1].durations[1] = 0;
        TEST_EXPECT(!bad_build_assemble(zero_duration, built, &error));

        BadBuildClip events = clip;
        events.events.pop_back();
        TEST_EXPECT(!bad_build_assemble(events, built, &error));
        std::printf("validation: every malformed clip is refused by name\n");
    }

    // The table: the canonical row shape, and the rows the parser could not
    // read back.
    {
        BadBuildSet set;
        set.adm_name = "authored.adm";
        set.rows.push_back(BadBuildRow{"anim_reset", {"authored"}});
        set.rows.push_back(BadBuildRow{"anim_walk_forward", {"walkf", "walkf2"}});
        std::string text;
        std::string error;
        TEST_EXPECT(bad_build_mint_table(set, text, &error));
        TEST_EXPECT(text.rfind("\r\n", 0) == 0);
        TEST_EXPECT(text.find("anim_reset\t\t\t\t\"authored\"") != std::string::npos);
        TEST_EXPECT(text.find("\"walkf\" \"walkf2\"") != std::string::npos);
        TEST_EXPECT(text.size() > 6 && text.back() == '\0');

        // The table reads back as the rows that went in.
        opennova::adm::AdmFile table = {};
        TEST_EXPECT(opennova::adm::adm_parse_buffer(text.data(), text.size(), &table) == 0);
        TEST_EXPECT(table.count == 2 && table.entries[1].variant_count == 2);
        TEST_EXPECT(std::strcmp(table.entries[1].variants[1], "walkf2") == 0);
        opennova::adm::adm_free(&table);

        BadBuildSet outside = set;
        outside.rows[0].key = "walk_forward";
        TEST_EXPECT(!bad_build_mint_table(outside, text, &error));
        BadBuildSet bare = set;
        bare.rows[1].variants.clear();
        TEST_EXPECT(!bad_build_mint_table(bare, text, &error));
        BadBuildSet quoted = set;
        quoted.rows[1].variants[0] = "walk\"f";
        TEST_EXPECT(!bad_build_mint_table(quoted, text, &error));
        BadBuildSet wide = set;
        wide.rows[1].variants.assign(9, "walkf");
        TEST_EXPECT(!bad_build_mint_table(wide, text, &error));
        std::printf("table: canonical rows, and four rows refused\n");
    }

    // The retail leg: the two derivations against the shipped 19-bone rig.
    const std::string fixture = retail::reference_fixture("bad/BINOC.bad");
    if (fixture.empty()) {
        return retail::skip_leg("OPENNOVA_JO_ASSETS/fixtures/bad/BINOC.bad (the shipped 19-bone rig)");
    }
    std::vector<uint8_t> retail_bytes;
    TEST_EXPECT(test_io::read_file(fixture, retail_bytes) && !retail_bytes.empty());
    BadFile retail_file = {};
    TEST_EXPECT(bad_parse_buffer(retail_bytes.data(), retail_bytes.size(), &retail_file) == 0);
    TEST_EXPECT(retail_file.bone_count == 19);
    double worst_bind = 0.0;
    double worst_position = 0.0;
    for (size_t i = 0; i < retail_file.num_bones; ++i) {
        const BadBone &bone = retail_file.bones[i];
        const BadQuaternion &k0 = retail_file.channels[i].rotations[0];
        float rows[9];
        bad_quat_to_rows(BadBuildQuat{k0.x, k0.y, k0.z, k0.w}, rows);
        float want[9];
        bad_rows_transpose(rows, want);
        for (int k = 0; k < 9; ++k) {
            const double d = std::fabs(static_cast<double>(bone.rotation[k]) - want[k]);
            if (d > worst_bind) worst_bind = d;
        }
        if (bone.parent_index < 0) continue;
        const BadBone &up = retail_file.bones[static_cast<size_t>(bone.parent_index)];
        const BadBuildVec3 rel = bad_bone_rel(up.rotation, bone.position);
        const BadBuildVec3 again = bad_bone_position(up.rotation, rel);
        const double d[3] = {std::fabs(again.x - bone.position[0]), std::fabs(again.y - bone.position[1]),
                             std::fabs(again.z - bone.position[2])};
        for (int k = 0; k < 3; ++k)
            if (d[k] > worst_position) worst_position = d[k];
    }
    // The bind is the transpose of the first key in every retail bone (the
    // corpus sweep: 13517 of 13517, worst 5.0e-7), and the pivot relation
    // inverts back onto the stored position.
    TEST_EXPECT(worst_bind <= 1e-5);
    TEST_EXPECT(worst_position <= 1e-5);
    std::printf("retail leg: BINOC.bad bind within %g, position inverse within %g\n", worst_bind,
                worst_position);
    bad_free(&retail_file);
    return 0;
}
