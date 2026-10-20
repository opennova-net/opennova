// The .bad construction seam (formats/bad/bad_build.h): the mission <-> clip
// frame maps, the derived bone table, the events as the author states them,
// the validation, and the canonical .adm table. The two derivations are pinned
// on values witnessed in shipped clips: a bone's rotation is the transpose of
// its first key (BINOC.bad), and a clip's positions are the rig's pivots
// through its set's reset bind (US01's DT1PRONE.BAD against DT1RST.BAD).
#include <formats/adm/adm.h>
#include <formats/bad/bad.h>
#include <formats/bad/bad_build.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "common/test_expect.h"

using namespace opennova::bad;

namespace {

const BadTargetLimits kRetail = bad_retail_limits();

// A slot table for the set checks: `anim_<name>` names a slot when the name is
// one of a few (the runtime's 252 are anim::adm_slot_index's).
int test_slots(std::string_view key) {
    for (const char *slot : {"anim_reset", "anim_walk_forward", "anim_idle"})
        if (key == slot) return 1;
    return -1;
}

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
        TEST_EXPECT(bad_build_assemble(clip, nullptr, kRetail, built, &error));
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
        TEST_EXPECT(bad_build_mint(clip, nullptr, kRetail, bytes, &error));
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
        TEST_EXPECT(bad_build_assemble(clip, nullptr, kRetail, built, &error));
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
        TEST_EXPECT(bad_build_assemble(plain, &pitched, kRetail, built, &error));
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
        TEST_EXPECT(!bad_build_assemble(empty, nullptr, kRetail, built, &error) && !error.empty());

        BadBuildClip forward = clip;
        forward.bones[0].parent = 1;
        TEST_EXPECT(!bad_build_assemble(forward, nullptr, kRetail, built, &error));

        BadBuildClip sibling = clip;
        sibling.bones[1].parent = 1;
        TEST_EXPECT(!bad_build_assemble(sibling, nullptr, kRetail, built, &error));

        BadBuildClip short_keys = clip;
        short_keys.bones[1].keys.pop_back();
        TEST_EXPECT(!bad_build_assemble(short_keys, nullptr, kRetail, built, &error));

        BadBuildClip promised = clip;
        promised.flags = BAD_FLAG_TRANSLATION;
        TEST_EXPECT(!bad_build_assemble(promised, nullptr, kRetail, built, &error));

        BadBuildClip long_name = clip;
        long_name.bones[1].name = std::string(32, 'x');
        TEST_EXPECT(!bad_build_assemble(long_name, nullptr, kRetail, built, &error));

        BadBuildClip unit = clip;
        unit.bones[1].keys[2] = BadBuildQuat{0.0, 0.0, 0.0, 0.0};
        TEST_EXPECT(!bad_build_assemble(unit, nullptr, kRetail, built, &error));

        BadBuildClip zero_duration = clip;
        zero_duration.bones[1].durations.assign(4, 1);
        zero_duration.bones[1].durations[1] = 0;
        TEST_EXPECT(!bad_build_assemble(zero_duration, nullptr, kRetail, built, &error));

        BadBuildClip events = clip;
        events.events.pop_back();
        TEST_EXPECT(!bad_build_assemble(events, nullptr, kRetail, built, &error));

        // The loader knows event records of version 1 (with the trigger word)
        // and 0 (without): any other version is refused, and so is a trigger
        // on a version 0 event, which the writer would drop.
        BadBuildClip version2 = clip;
        version2.version = 2;
        TEST_EXPECT(!bad_build_assemble(version2, nullptr, kRetail, built, &error));
        BadBuildClip version0 = clip;
        version0.version = 0;
        TEST_EXPECT(!bad_build_assemble(version0, nullptr, kRetail, built, &error));
        for (BadBuildEvent &ev : version0.events) ev.trigger = 0;
        TEST_EXPECT(bad_build_assemble(version0, nullptr, kRetail, built, &error));

        // Translations the flags do not carry would be dropped: refused.
        BadBuildClip unflagged = two_bone_clip(BAD_FLAG_TRANSLATION);
        unflagged.flags = 0;
        TEST_EXPECT(!bad_build_assemble(unflagged, nullptr, kRetail, built, &error));

        // The loader refuses a file over 500,000 bytes, so the mint does too.
        BadBuildClip huge = clip;
        huge.frame_count = 30000;
        huge.events.clear();
        for (BadBuildBone &bone : huge.bones) bone.keys.assign(30001, BadBuildQuat{});
        TEST_EXPECT(bad_build_assemble(huge, nullptr, kRetail, built, &error));
        std::vector<uint8_t> huge_bytes;
        TEST_EXPECT(!bad_build_mint(huge, nullptr, kRetail, huge_bytes, &error));

        // The game's bone arrays hold 64 [orig: BoneSystem_Init @0x410170].
        BadBuildClip wide_rig = clip;
        while (wide_rig.bones.size() < kRetail.clip_bones) {
            BadBuildBone extra = clip.bones[1];
            extra.name = "BN" + std::to_string(wide_rig.bones.size() + 1);
            wide_rig.bones.push_back(extra);
        }
        TEST_EXPECT(bad_build_assemble(wide_rig, nullptr, kRetail, built, &error));
        wide_rig.bones.push_back(clip.bones[1]);
        TEST_EXPECT(!bad_build_assemble(wide_rig, nullptr, kRetail, built, &error) &&
                    error.find("65 bones") != std::string::npos);

        // A loop stepping a whole cycle a tick (fps >= 62 * frames) never
        // plays; a one-shot at that rate just ends [orig: AnimChannel_InitFromData
        // @0x4105BA; AnimChannel_AdvancePlayback @0x40B199].
        BadBuildClip fast = clip;
        fast.fps = 62 * fast.frame_count - 1;
        fast.flags = BAD_FLAG_LOOP;
        TEST_EXPECT(bad_build_assemble(fast, nullptr, kRetail, built, &error));
        fast.fps = 62 * fast.frame_count;
        TEST_EXPECT(!bad_build_assemble(fast, nullptr, kRetail, built, &error) &&
                    error.find("whole cycle") != std::string::npos);
        fast.flags = 0;
        TEST_EXPECT(bad_build_assemble(fast, nullptr, kRetail, built, &error));
        std::printf("validation: every malformed clip is refused by name\n");
    }

    // The set: every clip file packs (15 bytes at most, the extension
    // included, ASCII), and a clip carries translations only over a translated
    // reset clip [orig: PFF_FindEntry @0x7685D0; AnimChannel_ComputeBoneMatrices
    // @0x410DE7].
    {
        TEST_EXPECT(bad_build_packable_name("avenger_025.bad", kRetail) && !bad_build_packable_name("avenger_0255.bad", kRetail));
        TEST_EXPECT(!bad_build_packable_name("", kRetail) && !bad_build_packable_name("w\xC3\xA4lk.bad", kRetail));
        BadBuildSet set;
        set.rows.push_back(BadBuildRow{"anim_reset", {"rst"}});
        set.rows.push_back(BadBuildRow{"anim_walk_forward", {"walk"}});
        BadBuildClip reset = two_bone_clip(0);
        reset.name = "rst";
        BadBuildClip walk = two_bone_clip(BAD_FLAG_TRANSLATION);
        walk.name = "walk";
        set.clips = {reset, walk};
        std::vector<BadBuildProblem> problems;
        TEST_EXPECT(!bad_build_check_set(set, kRetail, test_slots, problems) && problems.size() == 1 &&
                    problems[0].what.find("clip 'walk' carries translations") != std::string::npos &&
                    problems[0].clip == 1);
        // The other way round moves nothing either, and retail ships it (3 clips).
        problems.clear();
        set.clips[0] = two_bone_clip(BAD_FLAG_TRANSLATION);
        set.clips[0].name = "rst";
        set.clips[1] = two_bone_clip(0);
        set.clips[1].name = "walk";
        TEST_EXPECT(bad_build_check_set(set, kRetail, test_slots, problems) && problems.empty());
        set.clips[1].name = "walk_forward1";
        set.rows[1].variants[0] = "walk_forward1";
        TEST_EXPECT(!bad_build_check_set(set, kRetail, test_slots, problems) && problems.size() == 1 &&
                    problems[0].what.find("17 bytes") != std::string::npos);
        // A lone clip is named by its output.
        BadBuildSet lone;
        lone.clips = {set.clips[1]};
        problems.clear();
        TEST_EXPECT(bad_build_check_set(lone, kRetail, test_slots, problems));

        // Every problem at once, each naming its clip, bone or row: a key
        // naming no slot, a row of thirty clips, a bone name of 32 characters, a
        // key held past the u16 duration word (the channel slerps across a
        // key's whole duration, so no run of keys stands in for it), a clip of
        // 65 bones.
        BadBuildSet many;
        BadBuildClip rst = two_bone_clip(0);
        rst.name = "rst";
        BadBuildClip held = two_bone_clip(0);
        held.name = "held";
        held.bones[1].name = std::string(32, 'b');
        held.bones[1].durations = {1, 70000, 1, 1};
        BadBuildClip wide = two_bone_clip(0);
        wide.name = "wide";
        while (wide.bones.size() <= kRetail.clip_bones) wide.bones.push_back(wide.bones[1]);
        many.clips = {rst, held, wide};
        many.rows.push_back(BadBuildRow{"anim_reset", {"rst"}});
        many.rows.push_back(BadBuildRow{"anim_nowhere", {"held"}});
        many.rows.push_back(BadBuildRow{"anim_idle", std::vector<std::string>(30, "wide")});
        problems.clear();
        TEST_EXPECT(!bad_build_check_set(many, kRetail, test_slots, problems) && problems.size() == 5);
        const auto found = [&problems](const std::string &words, int clip, int bone, int row) {
            for (const BadBuildProblem &p : problems)
                if (p.what.find(words) != std::string::npos && p.clip == clip && p.bone == bone && p.row == row)
                    return true;
            return false;
        };
        TEST_EXPECT(found("row 'anim_nowhere' names no anim slot", -1, -1, 1));
        TEST_EXPECT(found("names 30 clips; a table row holds 29", -1, -1, 2));
        TEST_EXPECT(found("is 32 characters; a bone name holds 31", 1, 1, -1));
        TEST_EXPECT(found("key 1 holds 70000 frames; a key's duration word holds 65535", 1, 1, -1));
        TEST_EXPECT(found("holds 65 bones", 2, -1, -1));
        // The mint checks first and stops there, every problem said.
        std::vector<BadMintedFile> files;
        problems.clear();
        TEST_EXPECT(!bad_build_mint_set(many, "many.adm", kRetail, test_slots, files, problems) &&
                    problems.size() == 5 && files.empty());
        // A 29th clip the table writes bare, its line's 30th token: one with a space does not read back, refused
        // with the writer's rule (adm::adm_bare_token), naming the clip; a bare one passes.
        {
            BadBuildSet tail;
            tail.clips = {rst};
            tail.rows.push_back(BadBuildRow{"anim_reset", {"rst"}});
            std::vector<std::string> clips(28, "rst");
            clips.push_back("rst two");
            tail.rows.push_back(BadBuildRow{"anim_idle", clips});
            problems.clear();
            bad_build_check_set(tail, kRetail, test_slots, problems);
            TEST_EXPECT(found("names 'rst two' as its 29th clip", -1, -1, 1));
            tail.rows[1].variants.back() = "rst";
            problems.clear();
            bad_build_check_set(tail, kRetail, test_slots, problems);
            TEST_EXPECT(!found("as its 29th clip", -1, -1, 1));
        }
        // The assembly refuses the long hold itself, naming bone and key.
        BadAssembled built;
        std::string error;
        held.bones[1].name = "BN02";
        TEST_EXPECT(!bad_build_assemble(held, nullptr, kRetail, built, &error) &&
                    error.find("bone 1 key 1 holds 70000 frames") != std::string::npos);
        held.bones[1].durations[1] = 65535;
        TEST_EXPECT(bad_build_assemble(held, nullptr, kRetail, built, &error) && built.durations[1][1] == 65535);
        std::printf("set: every problem the target finds, named by clip, bone and row\n");
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
        TEST_EXPECT(bad_build_mint_table(set, kRetail, text, &error));
        TEST_EXPECT(text.rfind("\r\n", 0) == 0);
        TEST_EXPECT(text.find("anim_reset\t\t\t\t\"authored\"") != std::string::npos);
        TEST_EXPECT(text.find("\"walkf\" \"walkf2\"") != std::string::npos);
        TEST_EXPECT(text.size() > 6 && text.back() == '\0');

        // The table reads back as the rows that went in.
        opennova::adm::AdmFile table = {};
        TEST_EXPECT(opennova::adm::adm_parse_buffer(text.data(), text.size(), &table) == 0);
        TEST_EXPECT(table.count == 2 && table.entries[1].variants.size() == 2);
        TEST_EXPECT(std::strcmp(table.entries[1].variants[1], "walkf2") == 0);
        opennova::adm::adm_free(&table);

        // A key names its slot past its first five characters, whatever they
        // are, so one of five or fewer names none and is refused, and any other
        // prefix reads back as written.
        BadBuildSet outside = set;
        outside.rows[1].key = "walk";
        TEST_EXPECT(!bad_build_mint_table(outside, kRetail, text, &error));
        BadBuildSet upper = set;
        upper.rows[0].key = "ANIM_RESET";
        upper.rows[1].key = "xxxx_walk_forward";
        TEST_EXPECT(bad_build_mint_table(upper, kRetail, text, &error));
        TEST_EXPECT(text.find("ANIM_RESET\t\t\t\t\"authored\"") != std::string::npos);
        BadBuildSet bare = set;
        bare.rows[1].variants.clear();
        TEST_EXPECT(!bad_build_mint_table(bare, kRetail, text, &error));
        // A table with no reset row binds nothing: retail faults loading one
        // [orig: AnimMap_LoadAdmFile @0x40cc40, @0x40ce11..0x40ce16].
        BadBuildSet unbound = set;
        unbound.rows[0].key = "anim_idle";
        TEST_EXPECT(!bad_build_mint_table(unbound, kRetail, text, &error) &&
                    error.find("no reset row") != std::string::npos);
        BadBuildSet quoted = set;
        quoted.rows[1].variants[0] = "walk\"f";
        TEST_EXPECT(!bad_build_mint_table(quoted, kRetail, text, &error));
        BadBuildSet wide = set;
        wide.rows[1].variants.assign(30, "walkf");
        TEST_EXPECT(!bad_build_mint_table(wide, kRetail, text, &error));
        // A variant names a clip beside the table, never a path.
        BadBuildSet escape = set;
        escape.rows[1].variants[0] = "../walkf";
        TEST_EXPECT(!bad_build_mint_table(escape, kRetail, text, &error));
        TEST_EXPECT(bad_build_bare_stem("walkf") && bad_build_bare_stem("a..b"));
        TEST_EXPECT(!bad_build_bare_stem("") && !bad_build_bare_stem("..") && !bad_build_bare_stem("a/b") &&
                    !bad_build_bare_stem("a\\b") && !bad_build_bare_stem("C:walk") &&
                    !bad_build_bare_stem("Armature|Walk"));
        std::printf("table: canonical rows, and four rows refused\n");
    }

    // The bind: every bone's rotation is the transpose of its first key (the
    // corpus sweep: 13517 of 13517, worst 5.0e-7). One witnessed bone, BINOC.bad
    // bone 3 (BN04 R Clavicle): its first key (x, y, z, w) and the rotation rows
    // the file stores (the sweep's last run put the 19 bones within 1.5e-7).
    {
        const BadBuildQuat key{-0.471554816, -0.530660629, 0.448825985, 0.542761922};
        const float stored[9] = {0.0339089558f, 0.987682521f, 0.152752668f, 0.0132598262f, 0.152382508f,
                                 -0.988232613f, -0.999336958f, 0.0355354063f, -0.00792932883f};
        float rows[9];
        bad_quat_to_rows(key, rows);
        float want[9];
        bad_rows_transpose(rows, want);
        double worst_bind = 0.0;
        for (int k = 0; k < 9; ++k)
            worst_bind = std::max(worst_bind, std::fabs(static_cast<double>(stored[k]) - want[k]));
        TEST_EXPECT(worst_bind <= 1e-5);
        // The rows untransposed are not the stored ones: the transpose is the rule.
        double worst_untransposed = 0.0;
        for (int k = 0; k < 9; ++k)
            worst_untransposed = std::max(worst_untransposed, std::fabs(static_cast<double>(stored[k]) - rows[k]));
        TEST_EXPECT(worst_untransposed > 0.1);
        std::printf("witness: BINOC.bad bone 3's bind within %g of its first key's transpose\n", worst_bind);
    }

    // The position: a clip stores the rig's pivots turned through its SET's
    // reset bind, not its own (the corpus: 30358 of 32011 non-junk bones of
    // the tables' other clips, 365 through their own). US01's prone clip
    // DT1PRONE.BAD against its reset DT1RST.BAD: the pivots recovered from the
    // reset clip, derived through the reset's bind, landed on 17 of its 18 child
    // bones (its neck stale by 1.4 cm) and through its own bind on none. One
    // witnessed bone, 3 (BN04 R Clavicle, parent 2): the parent's first key in
    // each clip and the bone's stored position in each.
    {
        const BadQuaternion reset_parent{-4.95343556e-07f, 0.706825197f, 0.707388282f, 4.96510381e-07f};
        const float reset_position[3] = {0.0545783751f, 1.50633795e-07f, 0.280747235f};
        const BadQuaternion prone_parent{-0.0126676662f, 0.0676731095f, 0.997074723f, 0.033195056f};
        const float prone_position[3] = {0.0545783713f, 1.5491436e-07f, 0.280747265f};
        const auto shape = [](const BadQuaternion &parent) {
            BadBuildClip clip;
            clip.bones.resize(2);
            clip.bones[0].keys.push_back(bad_mission_from_clip(BadBuildQuat{parent.x, parent.y, parent.z, parent.w}));
            clip.bones[1].parent = 0;
            clip.bones[1].keys.push_back(BadBuildQuat{});
            return clip;
        };
        const BadBuildClip reset_shape = shape(reset_parent);
        BadBuildClip prone_shape = shape(prone_parent);
        float bind[9];
        bad_derive_bind_rows(reset_shape, nullptr, 0, bind);
        prone_shape.bones[1].pivot = bad_bone_rel(bind, reset_position);
        const auto gap = [&](const BadBuildClip *reset) {
            std::vector<BadBone> derived;
            bad_derive_bone_table(prone_shape, reset, derived);
            double worst = 0.0;
            for (int k = 0; k < 3; ++k)
                worst = std::max(worst, std::fabs(static_cast<double>(derived[1].position[k]) - prone_position[k]));
            return worst;
        };
        const double through_reset = gap(&reset_shape);
        const double through_own = gap(nullptr);
        TEST_EXPECT(through_reset <= 1e-4 && through_own > 1e-4);
        std::printf("witness: DT1PRONE.BAD bone 3 sits %g from the derivation through DT1RST's bind, %g through "
                    "its own\n",
                    through_reset, through_own);
    }
    return 0;
}
