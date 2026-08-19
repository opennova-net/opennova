/* engine/runtime/simassets AdmSkeletalClips — the SECONDARY (upper-body weapon)
   channel's composition, pinned headless over the committed BINOC.bad rig (the
   real 19-tag BN01..BN19 body skeleton the weapon mask keys on):

     1. the mask splice: mask bones take the weapon channel's world rotation,
        unmasked bones keep the primary's, and the hierarchy re-localizes exactly
        [orig: mask @0x4b14db, second AnimChannel_ComputeBoneMatrices @0x4b16a7];
     2. the RESET backfill: a weapon key with NO clip plays RESET, it does not
        no-op — registration binds every absent anim_<name> to entry 0
        [orig: the unrolled backfill loops @0x40bc24 / @0x40bd2e; §14.8.1];
     3. the secondary cross-fade: at weight w the mask bone sits between the
        outgoing and target weapon clips (the shared AnimMap_UpdateEntity re-init
        blends the weapon layer exactly like the primary)
        [orig: AnimMap_UpdateDualChannels @0x40b8c0 -> @0x40b5f0;
         AnimChannel_BlendTwoChannels @0x410740].

   The healthy-export trap: with only real retail clips a naive test passes for
   the wrong reason (channel-at-reset == bind). So the second clip is SYNTHESIZED
   in-test from BINOC.bad by rotating ONE mask bone (BN06 R UpperArm) 90 deg and
   ONE unmasked bone (BN08 R Thigh) 90 deg, written with the from-scratch writer
   (ADR 0003) into the temp dir alongside a two-key .adm. */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "common/test_expect.h"
#include "common/test_paths.h"

#include <anim/aim_overlay.h>
#include <anim/anim_sample.h>
#include <anim/skeletal_pose.h>
#include <bad/bad.h>
#include <resource_index/resource_index.h>
#include <simassets/adm_skeletal_clips.h>

using opennova::anim::PoseBone;
using opennova::anim::Quat;
using opennova::simassets::AdmSkeletalClips;

namespace {

constexpr float kPi = 3.14159265358979f;

// Model bone index (BN## - 1) for the two probed bones.
constexpr int kMaskBone = 5;    // BN06 R UpperArm — in kWeaponChannelMaskBones
constexpr int kBodyBone = 7;    // BN08 R Thigh    — NOT in the mask

// World rotations by FK over parent-local rotations (the same walk the splice does).
std::vector<Quat> fk(const std::vector<int> &parents, const std::vector<PoseBone> &pose) {
    std::vector<Quat> world(pose.size());
    for (size_t i = 0; i < pose.size(); ++i) {
        const int p = parents[i];
        world[i] = (p >= 0 && static_cast<size_t>(p) < i)
                ? opennova::anim::quat_mul(world[static_cast<size_t>(p)], pose[i].rotation)
                : pose[i].rotation;
    }
    return world;
}

float angle_between(Quat a, Quat b) {
    a = opennova::anim::quat_normalize(a);
    b = opennova::anim::quat_normalize(b);
    float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < 0) d = -d;
    if (d > 1.0f) d = 1.0f;
    return 2.0f * std::acos(d);
}

// Rotate every keyframe of one channel by a fixed quaternion (about the bone's
// local Y), so the clip differs from reset on exactly that bone.
void twist_channel(BadFile &bf, int bone, float radians) {
    const float h = radians * 0.5f;
    const Quat rot = { std::cos(h), 0.0f, std::sin(h), 0.0f }; // w,x,y,z about Y
    BadChannel &ch = bf.channels[bone];
    for (uint32_t f = 0; f < ch.frame_count; ++f) {
        BadQuaternion &q = ch.rotations[f];
        const Quat cur = opennova::anim::bad_channel_quat(q.x, q.y, q.z, q.w);
        const Quat out = opennova::anim::quat_normalize(opennova::anim::quat_mul(cur, rot));
        q.x = out.x; q.y = out.y; q.z = out.z; q.w = out.w;
    }
}

bool write_text(const std::string &path, const std::string &body) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
    return static_cast<bool>(f);
}

} // namespace

int main() {
    const char *root = test_paths_repo_root(__FILE__);
    const std::string binoc = std::string(root) + "/fixtures/bad/BINOC.bad";

    // ---- build the temp rig: reset.bad (verbatim BINOC), twist.bad (two bones turned) ----
    const std::string dir = std::string(test_paths_temp_dir()) + "/opennova_wpnch_test";
#ifdef _WIN32
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0777);
#endif
    BadFile reset_bf = {};
    TEST_EXPECT(bad_parse(binoc.c_str(), &reset_bf) == 0);
    TEST_EXPECT(reset_bf.num_bones >= 19);
    TEST_EXPECT(reset_bf.num_channels >= 19);
    BadWriteOptions opts;
    bad_write_options_default(&opts);
    TEST_EXPECT(bad_write((dir + "/reset.bad").c_str(), &reset_bf, &opts) == 0);

    BadFile twist_bf = {};
    TEST_EXPECT(bad_parse(binoc.c_str(), &twist_bf) == 0);
    twist_channel(twist_bf, kMaskBone, kPi * 0.5f);
    twist_channel(twist_bf, kBodyBone, kPi * 0.5f);
    TEST_EXPECT(bad_write((dir + "/twist.bad").c_str(), &twist_bf, &opts) == 0);
    bad_free(&twist_bf);
    bad_free(&reset_bf);

    // Two authored keys only. anim_reload is deliberately ABSENT (the backfill probe).
    TEST_EXPECT(write_text(dir + "/rig.adm",
            "\r\nanim_reset\t\t\t\t\"reset\"\r\n"
            "anim_idle\t\t\t\t\"reset\"\r\n"
            "anim_knife\t\t\t\t\"twist\"\r\n\r\n\r\n"));

    opennova::ResourceIndex index;
    TEST_EXPECT(index.scan(dir));

    // Model table = the .bad's own records (a body's rig; positions irrelevant here).
    BadFile probe = {};
    TEST_EXPECT(bad_parse(binoc.c_str(), &probe) == 0);
    std::vector<opennova::anim::Vec3> origins(probe.num_bones);
    std::vector<int> parents(probe.num_bones);
    for (size_t i = 0; i < probe.num_bones; ++i) {
        origins[i] = { probe.bones[i].position[0], probe.bones[i].position[1],
                       probe.bones[i].position[2] };
        parents[i] = probe.bones[i].parent_index;
    }
    bad_free(&probe);

    AdmSkeletalClips clips;
    TEST_EXPECT(clips.load_from_adm(&index, "rig.adm", origins, parents));
    TEST_EXPECT(clips.loaded());
    TEST_EXPECT(clips.has_clip("anim_knife"));
    TEST_EXPECT(!clips.has_clip("anim_reload"));
    TEST_EXPECT(clips.fk_valid());
    const std::vector<int> &rig_parents = clips.parents();
    TEST_EXPECT(rig_parents.size() > static_cast<size_t>(kBodyBone));

    std::vector<PoseBone> primary, weapon, composed;
    clips.eval_pose("anim_idle", 0.0, 0, primary);
    clips.eval_pose("anim_knife", 0.0, 0, weapon);
    TEST_EXPECT(primary.size() == weapon.size());
    const std::vector<Quat> primary_w = fk(rig_parents, primary);
    const std::vector<Quat> weapon_w = fk(rig_parents, weapon);
    // Sanity: the synthesized clip really differs from idle on both probed bones.
    TEST_EXPECT(angle_between(primary_w[kMaskBone], weapon_w[kMaskBone]) > 1.0f);
    TEST_EXPECT(angle_between(primary_w[kBodyBone], weapon_w[kBodyBone]) > 1.0f);

    // ---- 1. the mask splice (no overlay deltas: nullptr) ----
    TEST_EXPECT(clips.eval_composed_pose("anim_idle", 0.0, false, "", 0.0, 1.0f,
            nullptr, "anim_knife", 0.0, composed));
    {
        const std::vector<Quat> w = fk(rig_parents, composed);
        // BN06 (masked) takes the WEAPON world rotation exactly...
        TEST_EXPECT(angle_between(w[kMaskBone], weapon_w[kMaskBone]) < 1e-3f);
        // ...BN08 (unmasked leg) keeps the PRIMARY's, untouched by the weapon twist.
        TEST_EXPECT(angle_between(w[kBodyBone], primary_w[kBodyBone]) < 1e-3f);
        // Origins are the primary's — the shared pivots own translation.
        TEST_EXPECT(std::fabs(composed[kMaskBone].origin.x - primary[kMaskBone].origin.x) < 1e-6f);
    }

    // ---- 2. RESET backfill: an absent key is NOT a no-op ----
    // The idle clip here IS reset (same .bad), so to make the backfill observable use
    // the twisted clip as PRIMARY: a missing weapon key must pull the mask bones back
    // to RESET's rotation, i.e. AWAY from the twisted primary on the mask bone only.
    TEST_EXPECT(clips.eval_composed_pose("anim_knife", 0.0, false, "", 0.0, 1.0f,
            nullptr, "anim_reload", 0.0, composed));
    {
        std::vector<PoseBone> reset;
        clips.eval_pose("anim_reset", 0.0, 0, reset);
        const std::vector<Quat> reset_w = fk(rig_parents, reset);
        const std::vector<Quat> w = fk(rig_parents, composed);
        // masked BN06 -> RESET (the backfilled entry 0), not the twisted primary
        TEST_EXPECT(angle_between(w[kMaskBone], reset_w[kMaskBone]) < 1e-3f);
        TEST_EXPECT(angle_between(w[kMaskBone], weapon_w[kMaskBone]) > 1.0f);
        // unmasked BN08 keeps the twisted primary
        TEST_EXPECT(angle_between(w[kBodyBone], weapon_w[kBodyBone]) < 1e-3f);
    }
    // And the gate-off case (EMPTY key) really is a no-op — the primary passes through.
    TEST_EXPECT(clips.eval_composed_pose("anim_knife", 0.0, false, "", 0.0, 1.0f,
            nullptr, "", 0.0, composed));
    {
        const std::vector<Quat> w = fk(rig_parents, composed);
        TEST_EXPECT(angle_between(w[kMaskBone], weapon_w[kMaskBone]) < 1e-3f);
    }

    // ---- 3. the secondary cross-fade ----
    // prev = idle (reset pose), target = knife (twisted), weight 0.5 -> the masked
    // bone sits at ~45 deg between them; weight 1 == target; weight 0 == prev.
    const float full = angle_between(primary_w[kMaskBone], weapon_w[kMaskBone]);
    TEST_EXPECT(clips.eval_composed_pose("anim_idle", 0.0, false, "", 0.0, 1.0f,
            nullptr, "anim_knife", 0.0, composed, "anim_idle", 0.0, 0.5f));
    {
        const std::vector<Quat> w = fk(rig_parents, composed);
        const float to_prev = angle_between(w[kMaskBone], primary_w[kMaskBone]);
        const float to_tgt = angle_between(w[kMaskBone], weapon_w[kMaskBone]);
        std::printf("[wpnch] full=%.4f to_prev=%.4f to_tgt=%.4f\n", full, to_prev, to_tgt);
        TEST_EXPECT(std::fabs(to_prev - full * 0.5f) < 0.02f);
        TEST_EXPECT(std::fabs(to_tgt - full * 0.5f) < 0.02f);
        // the unmasked leg is untouched by the weapon blend
        TEST_EXPECT(angle_between(w[kBodyBone], primary_w[kBodyBone]) < 1e-3f);
    }
    TEST_EXPECT(clips.eval_composed_pose("anim_idle", 0.0, false, "", 0.0, 1.0f,
            nullptr, "anim_knife", 0.0, composed, "anim_idle", 0.0, 1.0f));
    {
        const std::vector<Quat> w = fk(rig_parents, composed);
        TEST_EXPECT(angle_between(w[kMaskBone], weapon_w[kMaskBone]) < 1e-3f);
    }
    TEST_EXPECT(clips.eval_composed_pose("anim_idle", 0.0, false, "", 0.0, 1.0f,
            nullptr, "anim_knife", 0.0, composed, "anim_idle", 0.0, 0.0f));
    {
        const std::vector<Quat> w = fk(rig_parents, composed);
        TEST_EXPECT(angle_between(w[kMaskBone], primary_w[kMaskBone]) < 1e-3f);
    }

    std::printf("simassets adm skeletal clips weapon channel: OK\n");
    return 0;
}
