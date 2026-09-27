// The .bad construction seam (formats/bad/bad_build.h): the mission <-> clip
// frame maps, the derived bone table, the events as the author states them,
// the validation, and the canonical .adm table. The retail legs
// (OPENNOVA_JO_ASSETS, SKIP-LEG) pin the two derivations: every bone's rotation
// is the transpose of its first key (the shipped 19-bone BINOC.bad), and a
// clip's positions are the rig's pivots through its set's reset bind (US01's
// DT1RUNF against DT1RST).
#include <formats/adm/adm.h>
#include <formats/bad/bad.h>
#include <formats/bad/bad_build.h>

#include <algorithm>
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
        // The translation block holds a row per key, rows 0..frame_count.
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

int main(int argc, char **argv) {
    retail::configure_mixed(argc, argv);
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
        TEST_EXPECT(bad_build_assemble(clip, nullptr, built, &error));
        TEST_EXPECT(built.file.bone_count == 2 && built.file.frame_count == 3);
        // Channels, events and translation rows all carry frame_count + 1
        // entries, as the author gave them.
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
        TEST_EXPECT(bad_build_mint(clip, nullptr, bytes, &error));
        BadFile parsed = {};
        TEST_EXPECT(bad_parse_buffer(bytes.data(), bytes.size(), &parsed) == 0);
        TEST_EXPECT(parsed.bone_count == 2 && parsed.frame_count == 3 && parsed.fps == 30);
        TEST_EXPECT(parsed.num_events == 4 && parsed.num_channels == 2);
        TEST_EXPECT(std::strcmp(parsed.bones[1].name, "BN02 Tip") == 0);
        bad_free(&parsed);
        std::printf("two-bone clip: %zu bytes, derived bind and position\n", bytes.size());
    }

    // An event's bottom and top are the hips' and the head's height above the
    // ground, which a clip posed about its hips does not hold: the seam writes
    // every event's pair as the author states it, derives none, and never
    // measures one from the pose (docs/anim/adm-bad-format-re.md).
    {
        BadBuildClip clip = two_bone_clip(BAD_FLAG_TRANSLATION);
        for (size_t f = 0; f < clip.events.size(); ++f) {
            clip.events[f].bottom = 0.9 + 0.01 * static_cast<double>(f);
            clip.events[f].top = 1.6 + 0.02 * static_cast<double>(f);
        }
        // One event stands at the ground: zeros are values, not a request.
        clip.events[2].bottom = 0.0;
        clip.events[2].top = 0.0;
        BadAssembled built;
        std::string error;
        TEST_EXPECT(bad_build_assemble(clip, nullptr, built, &error));
        TEST_EXPECT(built.events.size() == 4);
        for (size_t f = 0; f < built.events.size(); ++f) {
            TEST_EXPECT(built.events[f].bottom == static_cast<float>(clip.events[f].bottom));
            TEST_EXPECT(built.events[f].top == static_cast<float>(clip.events[f].top));
        }

        // The set's reset clip binds the pose, never the heights: the same
        // events through a reset that pitches the rig write the same pair.
        BadBuildClip pitched = two_bone_clip(0);
        for (BadBuildQuat &key : pitched.bones[0].keys) key = axis_quat(0.0, 1.0, 0.0, 90.0);
        BadBuildClip plain = two_bone_clip(0);
        plain.events = clip.events;
        TEST_EXPECT(bad_build_assemble(plain, &pitched, built, &error));
        TEST_EXPECT(built.events[3].bottom == static_cast<float>(clip.events[3].bottom) &&
                    built.events[3].top == static_cast<float>(clip.events[3].top));
        std::printf("events: every bottom and top written as stated (tops %g..%g)\n",
                    built.events[0].top, built.events[3].top);
    }

    // The set names its reset clip by the slot-0 row's LAST variant.
    {
        const BadBuildClip reset = two_bone_clip(0);
        BadBuildClip pitched = two_bone_clip(0);
        pitched.name = "pitched";
        BadBuildSet set;
        set.rows.push_back(BadBuildRow{"anim_walk_forward", {"pitched"}});
        set.rows.push_back(BadBuildRow{"ANIM_RESET", {"pitched", "authored.bad"}});
        set.clips = {pitched, reset};
        const BadBuildClip *found = bad_build_reset_clip(set);
        TEST_EXPECT(found != nullptr && found->name == "authored");
        set.rows.pop_back();
        TEST_EXPECT(bad_build_reset_clip(set) == nullptr);
        std::printf("reset clip: the slot-0 row's last variant\n");
    }

    // What the format cannot hold.
    {
        BadAssembled built;
        std::string error;
        BadBuildClip clip = two_bone_clip(0);

        BadBuildClip empty;
        TEST_EXPECT(!bad_build_assemble(empty, nullptr, built, &error) && !error.empty());

        BadBuildClip forward = clip;
        forward.bones[0].parent = 1;
        TEST_EXPECT(!bad_build_assemble(forward, nullptr, built, &error));

        BadBuildClip sibling = clip;
        sibling.bones[1].parent = 1;
        TEST_EXPECT(!bad_build_assemble(sibling, nullptr, built, &error));

        BadBuildClip short_keys = clip;
        short_keys.bones[1].keys.pop_back();
        TEST_EXPECT(!bad_build_assemble(short_keys, nullptr, built, &error));

        BadBuildClip promised = clip;
        promised.flags = BAD_FLAG_TRANSLATION;
        TEST_EXPECT(!bad_build_assemble(promised, nullptr, built, &error));

        BadBuildClip long_name = clip;
        long_name.bones[1].name = std::string(32, 'x');
        TEST_EXPECT(!bad_build_assemble(long_name, nullptr, built, &error));

        BadBuildClip unit = clip;
        unit.bones[1].keys[2] = BadBuildQuat{0.0, 0.0, 0.0, 0.0};
        TEST_EXPECT(!bad_build_assemble(unit, nullptr, built, &error));

        BadBuildClip zero_duration = clip;
        zero_duration.bones[1].durations.assign(4, 1);
        zero_duration.bones[1].durations[1] = 0;
        TEST_EXPECT(!bad_build_assemble(zero_duration, nullptr, built, &error));

        BadBuildClip events = clip;
        events.events.pop_back();
        TEST_EXPECT(!bad_build_assemble(events, nullptr, built, &error));

        // The loader knows event records of version 1 (with the trigger word)
        // and 0 (without): any other version is refused, and so is a trigger
        // on a version 0 event, which the writer would drop.
        BadBuildClip version2 = clip;
        version2.version = 2;
        TEST_EXPECT(!bad_build_assemble(version2, nullptr, built, &error));
        BadBuildClip version0 = clip;
        version0.version = 0;
        TEST_EXPECT(!bad_build_assemble(version0, nullptr, built, &error));
        for (BadBuildEvent &ev : version0.events) ev.trigger = 0;
        TEST_EXPECT(bad_build_assemble(version0, nullptr, built, &error));

        // Translations the flags do not carry would be dropped: refused.
        BadBuildClip unflagged = two_bone_clip(BAD_FLAG_TRANSLATION);
        unflagged.flags = 0;
        TEST_EXPECT(!bad_build_assemble(unflagged, nullptr, built, &error));

        // The loader refuses a file over 500,000 bytes, so the mint does too.
        BadBuildClip huge = clip;
        huge.frame_count = 30000;
        huge.events.clear();
        for (BadBuildBone &bone : huge.bones) bone.keys.assign(30001, BadBuildQuat{});
        TEST_EXPECT(bad_build_assemble(huge, nullptr, built, &error));
        std::vector<uint8_t> huge_bytes;
        TEST_EXPECT(!bad_build_mint(huge, nullptr, huge_bytes, &error));
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

        // A key names its slot past its first five characters, whatever they
        // are, so one of five or fewer names none and is refused, and any other
        // prefix reads back as written.
        BadBuildSet outside = set;
        outside.rows[1].key = "walk";
        TEST_EXPECT(!bad_build_mint_table(outside, text, &error));
        BadBuildSet upper = set;
        upper.rows[0].key = "ANIM_RESET";
        upper.rows[1].key = "xxxx_walk_forward";
        TEST_EXPECT(bad_build_mint_table(upper, text, &error));
        TEST_EXPECT(text.find("ANIM_RESET\t\t\t\t\"authored\"") != std::string::npos);
        BadBuildSet bare = set;
        bare.rows[1].variants.clear();
        TEST_EXPECT(!bad_build_mint_table(bare, text, &error));
        // A table with no reset row binds nothing: retail faults loading one
        // [orig: AnimMap_LoadAdmFile @0x40cc40, @0x40ce11..0x40ce16].
        BadBuildSet unbound = set;
        unbound.rows[0].key = "anim_idle";
        TEST_EXPECT(!bad_build_mint_table(unbound, text, &error) &&
                    error.find("no reset row") != std::string::npos);
        BadBuildSet quoted = set;
        quoted.rows[1].variants[0] = "walk\"f";
        TEST_EXPECT(!bad_build_mint_table(quoted, text, &error));
        BadBuildSet wide = set;
        wide.rows[1].variants.assign(9, "walkf");
        TEST_EXPECT(!bad_build_mint_table(wide, text, &error));
        // A variant names a clip beside the table, never a path.
        BadBuildSet escape = set;
        escape.rows[1].variants[0] = "../walkf";
        TEST_EXPECT(!bad_build_mint_table(escape, text, &error));
        TEST_EXPECT(bad_build_bare_stem("walkf") && bad_build_bare_stem("a..b"));
        TEST_EXPECT(!bad_build_bare_stem("") && !bad_build_bare_stem("..") && !bad_build_bare_stem("a/b") &&
                    !bad_build_bare_stem("a\\b") && !bad_build_bare_stem("C:walk") &&
                    !bad_build_bare_stem("Armature|Walk"));
        std::printf("table: canonical rows, and four rows refused\n");
    }

    // The retail legs. The bind: every bone's rotation is the transpose of its
    // first key (the corpus sweep: 13517 of 13517, worst 5.0e-7), on the
    // shipped 19-bone BINOC.bad.
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
    }
    TEST_EXPECT(worst_bind <= 1e-5);
    std::printf("retail leg: BINOC.bad bind within %g\n", worst_bind);
    bad_free(&retail_file);

    // The position: a clip stores the rig's pivots turned through its SET's
    // reset bind, not its own (the corpus: 30358 of 32011 non-junk bones of
    // the tables' other clips, 365 through their own). US01's prone clip
    // DT1PRONE against its reset DT1RST: the pivots recovered from the reset
    // clip, derived through the reset's bind, land on 17 of its 18 child
    // bones (its neck is stale by 1.4 cm), and through its own bind on none.
    const std::string reset_path = retail::asset_file("DT1RST.BAD");
    const std::string run_path = retail::asset_file("DT1PRONE.BAD");
    if (reset_path.empty() || run_path.empty())
        return retail::skip_leg("OPENNOVA_JO_ASSETS/DT1RST.BAD and DT1PRONE.BAD (US01's reset and prone)");
    BadFile reset_file = {};
    BadFile run_file = {};
    TEST_EXPECT(bad_parse(reset_path.c_str(), &reset_file) == 0);
    TEST_EXPECT(bad_parse(run_path.c_str(), &run_file) == 0);
    TEST_EXPECT(reset_file.num_bones == run_file.num_bones && reset_file.num_bones > 1);
    const auto shape = [](const BadFile &file) {
        BadBuildClip clip;
        clip.bones.resize(file.num_bones);
        for (size_t i = 0; i < file.num_bones; ++i) {
            clip.bones[i].parent = file.bones[i].parent_index;
            const BadQuaternion &q = file.channels[i].rotations[0];
            clip.bones[i].keys.push_back(bad_mission_from_clip(BadBuildQuat{q.x, q.y, q.z, q.w}));
        }
        return clip;
    };
    const BadBuildClip reset_shape = shape(reset_file);
    BadBuildClip run_shape = shape(run_file);
    for (size_t i = 0; i < reset_file.num_bones; ++i) {
        const int parent = reset_file.bones[i].parent_index;
        BadBuildVec3 pivot = bad_mission_from_clip(BadBuildVec3{
                reset_file.bones[i].position[0], reset_file.bones[i].position[1],
                reset_file.bones[i].position[2]});
        if (parent >= 0) {
            float bind[9];
            bad_derive_bind_rows(reset_shape, nullptr, static_cast<size_t>(parent), bind);
            const BadBuildVec3 rel = bad_bone_rel(bind, reset_file.bones[i].position);
            const BadBuildVec3 &up = run_shape.bones[static_cast<size_t>(parent)].pivot;
            pivot = BadBuildVec3{up.x + rel.x, up.y + rel.y, up.z + rel.z};
        }
        run_shape.bones[i].pivot = pivot;
    }
    const auto fits = [&](const BadBuildClip *reset) {
        std::vector<BadBone> derived;
        bad_derive_bone_table(run_shape, reset, derived);
        int count = 0;
        for (size_t i = 1; i < run_file.num_bones; ++i) {
            double worst = 0.0;
            for (int k = 0; k < 3; ++k)
                worst = std::max(worst, std::fabs(static_cast<double>(derived[i].position[k]) -
                                                  run_file.bones[i].position[k]));
            if (worst <= 1e-4) ++count;
        }
        return count;
    };
    const int through_reset = fits(&reset_shape);
    const int through_own = fits(nullptr);
    TEST_EXPECT(through_reset == 17 && through_own == 0);
    std::printf("retail leg: DT1PRONE positions, %d bones through DT1RST's bind, %d through its own\n",
                through_reset, through_own);
    bad_free(&reset_file);
    bad_free(&run_file);
    return 0;
}
