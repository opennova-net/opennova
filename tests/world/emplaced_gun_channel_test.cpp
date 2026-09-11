// The emplaced gun channel's stored words (entity+0x322 yaw / +0x324 pitch):
// the immediate and IsTurret producers of Entity_UpdateChildAttachment
// @0x4409A0 (the recoil term on both paths, the 0x92CF34/tick traverse, the
// local +-90 deg and non-Player +-4 deg gunner-yaw tethers), the window
// clamp's occupant write-back in Entity_UpdateTransformAndTurret @0x440ca0
// (@0x441263 / @0x4412b3), and the publication of the words as stored.
// [orig: the ewep pair @0x4409A0 / @0x440ca0; Math_ClampAngleToBounds @0x540cc0]

#include <base/io/bam.h>
#include <formats/def/def.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <memory>

using namespace opennova::world;
using opennova::io::bam_add;
using opennova::io::bam_sar;
using opennova::io::bam_sub;

static int failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {

constexpr int32_t kBamPerDegree = 11930464;
// Mission yaw 0 is engine heading 90 deg: the gun's own frame in every rig.
constexpr int32_t kGunHeading = 0x40000000;

struct Rig {
    std::unique_ptr<World> wp = std::make_unique<World>();
    World &w = *wp;
    EntityHandle gun_h, gunner_h;
    AiEntity *body = nullptr;

    // A UseGun emplacement at mission yaw/pitch 0 with one organic gunner
    // attached through the ordinary attach chain (the UseGun claim sets
    // primary_weapon_owner). `local` makes the gunner the local player;
    // `player_bit` stamps the wire Player class (a remote human).
    Rig(bool local, bool player_bit, uint32_t attrib2 = 0) {
        w.registry.configure_pool(0, 4);
        w.registry.configure_pool(1, 4);

        Entity gun_seed;
        gun_seed.kind = EntityKind::Item;
        gun_seed.health = 100;
        gun_seed.alive = true;
        gun_seed.yaw = 0;
        gun_seed.pitch = 0;
        gun_seed.item_attrib2 = attrib2;
        Seat usegun;
        usegun.type = SeatType::Gunner;
        usegun.bone_index = 6;
        usegun.source_name = "UseGun";
        gun_seed.seats.push_back(usegun);
        gun_h = w.registry.spawn(1, gun_seed);

        Entity gunner;
        gunner.kind = EntityKind::Organic;
        gunner.player_class = 8;
        gunner.health = 100;
        gunner.alive = true;
        if (player_bit) gunner.engine_flags |= kEntityFlagPlayer;
        gunner_h = w.registry.spawn(0, gunner);
        if (local) w.cached.local_player = gunner_h;
        body = w.ai.at(w.ai.attach(gunner_h));
        body->health = 100;
        body->inf.active = true;
        body->inf.is_local_player = local;
        CHECK(w.vehicles.process_attach(gunner_h, gun_h, 6));
        CHECK(gun().primary_weapon_owner == gunner_h);
    }
    Entity &gun() { return *w.registry.get(gun_h); }
    Entity &gunner() { return *w.registry.get(gunner_h); }

    // Point the gunner's look (the local player's input-owned mirrors, or
    // the body's live look for everyone else).
    void look(int32_t heading, int32_t pitch) {
        body->heading = heading;
        body->pitch = pitch;
        body->inf.target_heading = heading;
        body->inf.look_pitch = pitch;
    }
    void tick_channel() {
        tick_emplaced_weapon_channel(w, gun(), gunner(), *body);
    }
};

// The per-seat addeweap arc: right/left +-30 deg, down 20 / up 10 deg (the
// mins stored negated, as the parser leaves them).
void author_arc(Entity &gun) {
    gun.emplacement_right_limit_bam = 30 * kBamPerDegree;
    gun.emplacement_left_limit_bam = -30 * kBamPerDegree;
    gun.emplacement_down_limit_bam = 20 * kBamPerDegree;
    gun.emplacement_up_limit_bam = -10 * kBamPerDegree;
}

// The immediate path (attrib2 IsTurret clear): word322 = (gun.Yaw - occ.Yaw)
// >> 16, word324 = (gun.Pitch - occ.recoilPitch - occ.Pitch) >> 16, and the
// publication is those words verbatim. [orig: @0x440b39..0x440b58]
void test_immediate_path_words_and_recoil_term() {
    Rig r(/*local=*/false, /*player_bit=*/false);
    const int32_t look_heading = bam_sub(kGunHeading, 0x10000000);
    constexpr int32_t kLookPitch = 0x02000000;
    constexpr int32_t kRecoil = 0x00300000;
    r.look(look_heading, kLookPitch);
    r.body->inf.recoil_pitch = kRecoil;

    r.tick_channel();
    CHECK(r.gun().emplaced_gun_yaw_word == static_cast<int16_t>(0x1000));
    CHECK(r.gun().emplaced_gun_pitch_word ==
            static_cast<int16_t>(bam_sar(bam_sub(bam_sub(0, kRecoil), kLookPitch), 16)));
    // The gunner's look is untouched without a window.
    CHECK(r.body->heading == look_heading);
    CHECK(r.body->pitch == kLookPitch);

    EmplacedWeaponControls out;
    CHECK(emplaced_weapon_controls_for(r.w, r.gun(), out));
    CHECK(out.valid);
    CHECK(out.gun_yaw == 0x1000);
    CHECK(out.gun_pitch == static_cast<uint16_t>(0xFDD0));

    // Same words through the gunner's own tick (pose_if_mounted runs the
    // channel at its head, in retail's pool-1-before-pool-0 order).
    r.gun().emplaced_gun_yaw_word = 0;
    r.gun().emplaced_gun_pitch_word = 0;
    CHECK(r.w.ai.pose_if_mounted(*r.body, r.w));
    CHECK(r.gun().emplaced_gun_yaw_word == static_cast<int16_t>(0x1000));
    CHECK(r.gun().emplaced_gun_pitch_word == static_cast<int16_t>(0xFDD0));
}

// The window clamp writes the OCCUPANT: a look past the per-seat arc pins the
// word at the bound and stores gun - bound into the gunner's live heading /
// pitch (the local player's input-owned mirrors included).
// [orig: @0x44123c..0x441277 (yaw), @0x44128c..0x4412b3 (pitch)]
void test_window_clamp_writes_occupant_look() {
    Rig r(/*local=*/true, /*player_bit=*/true);
    author_arc(r.gun());
    // 50 deg right of the gun (yaw delta = gun - look = +50 deg) and 25 deg
    // below level (pitch delta = 0 - (-25) = +25 deg): both past the arc.
    const int32_t look_heading = bam_sub(kGunHeading, 50 * kBamPerDegree);
    const int32_t look_pitch = -25 * kBamPerDegree;
    r.look(look_heading, look_pitch);

    CHECK(r.w.ai.pose_if_mounted(*r.body, r.w));
    const int32_t yaw_bound = 30 * kBamPerDegree;
    const int32_t pitch_bound = 20 * kBamPerDegree;
    CHECK(r.gun().emplaced_gun_yaw_word == static_cast<int16_t>(yaw_bound >> 16));
    CHECK(r.gun().emplaced_gun_pitch_word == static_cast<int16_t>(pitch_bound >> 16));
    CHECK(r.body->heading == bam_sub(kGunHeading, yaw_bound));
    CHECK(r.body->pitch == bam_sub(0, pitch_bound));
    CHECK(r.body->inf.target_heading == r.body->heading);
    CHECK(r.body->inf.look_pitch == r.body->pitch);

    // Inside the arc nothing is written back.
    const int32_t inside_heading = bam_sub(kGunHeading, 10 * kBamPerDegree);
    const int32_t inside_pitch = -5 * kBamPerDegree;
    r.look(inside_heading, inside_pitch);
    CHECK(r.w.ai.pose_if_mounted(*r.body, r.w));
    CHECK(r.body->heading == inside_heading);
    CHECK(r.body->pitch == inside_pitch);
    CHECK(r.body->inf.target_heading == inside_heading);
    CHECK(r.body->inf.look_pitch == inside_pitch);
    CHECK(r.gun().emplaced_gun_yaw_word ==
            static_cast<int16_t>((10 * kBamPerDegree) >> 16));
}

// IsTurret: the words slew toward the look at most 0x92CF34 per tick from
// the previous word (rounded by the 0x8000 half-step); a look within the
// tether leaves the gunner untouched. [orig: @0x440a43..0x440b37]
void test_isturret_slews_per_tick() {
    Rig r(/*local=*/true, /*player_bit=*/true,
            opennova::def::DEF_ITEM_ATTRIB2_ISTURRET);
    const int32_t look_heading = bam_sub(kGunHeading, 30 * kBamPerDegree);
    r.look(look_heading, 0);

    CHECK(r.w.ai.pose_if_mounted(*r.body, r.w));
    // (0x92CF34 + 0x8000) >> 16
    CHECK(r.gun().emplaced_gun_yaw_word == static_cast<int16_t>(0x93));
    // pitch: ecx = 0 - 0 - 0x8000 within the rate; (ecx + 0x8000) >> 16 = 0
    CHECK(r.gun().emplaced_gun_pitch_word == 0);
    CHECK(r.body->heading == look_heading);
    CHECK(r.body->inf.target_heading == look_heading);

    CHECK(r.w.ai.pose_if_mounted(*r.body, r.w));
    // (0x92CF34 + (0x93 << 16) + 0x8000) >> 16
    CHECK(r.gun().emplaced_gun_yaw_word == static_cast<int16_t>(0x126));
    CHECK(r.body->heading == look_heading);

    // Aligned look, nothing further to slew: the word settles at
    // (30 deg + rounding) once the remaining step fits the rate.
    for (int i = 0; i < 80; ++i) CHECK(r.w.ai.pose_if_mounted(*r.body, r.w));
    CHECK(r.gun().emplaced_gun_yaw_word ==
            static_cast<int16_t>((30 * kBamPerDegree) >> 16));
    EmplacedWeaponControls out;
    CHECK(emplaced_weapon_controls_for(r.w, r.gun(), out));
    CHECK(out.gun_yaw == static_cast<uint16_t>((30 * kBamPerDegree) >> 16));
}

// IsTurret, the local player: the look is pulled back to within +-0x3FFFFFC0
// of the turret (occ.Yaw = gun.Yaw - clamped - prevWord) and the input-owned
// look mirror follows (retail's g_LocalPlayerLookYaw store).
// [orig: @0x440a76..0x440aa8]
void test_isturret_local_gunner_yaw_tether() {
    Rig r(/*local=*/true, /*player_bit=*/true,
            opennova::def::DEF_ITEM_ATTRIB2_ISTURRET);
    const int32_t look_heading = bam_sub(kGunHeading, 170 * kBamPerDegree);
    r.look(look_heading, 0);

    CHECK(r.w.ai.pose_if_mounted(*r.body, r.w));
    const int32_t expected =
            bam_sub(bam_sub(kGunHeading, kEmplacedLocalGunnerYawTether), 0x8000);
    CHECK(r.body->heading == expected);
    CHECK(r.body->inf.target_heading == expected);
    CHECK(r.gun().emplaced_gun_yaw_word == static_cast<int16_t>(0x93));
}

// IsTurret, a non-Player occupant (an NPC gunner): the +-0x2D82D80 (4 deg)
// tether. [orig: `test [edx+24h],100h` @0x440ab4; @0x440abd..0x440ade]
void test_isturret_npc_gunner_yaw_tether() {
    Rig r(/*local=*/false, /*player_bit=*/false,
            opennova::def::DEF_ITEM_ATTRIB2_ISTURRET);
    const int32_t look_heading = bam_sub(kGunHeading, 10 * kBamPerDegree);
    r.look(look_heading, 0);

    r.tick_channel();
    CHECK(r.body->heading ==
            bam_sub(bam_sub(kGunHeading, kEmplacedNpcGunnerYawTether), 0x8000));
    CHECK(r.gun().emplaced_gun_yaw_word == static_cast<int16_t>(0x93));
}

// IsTurret, a remote Player occupant: neither tether — the look stays, the
// word still slews. [orig: the Player bit jnz @0x440abb skips the NPC clamp;
//  the local compare @0x440a76 fails for a remote human]
void test_isturret_remote_player_untethered() {
    Rig r(/*local=*/false, /*player_bit=*/true,
            opennova::def::DEF_ITEM_ATTRIB2_ISTURRET);
    r.body->net_is_remote_peer = true;
    const int32_t look_heading = bam_sub(kGunHeading, 170 * kBamPerDegree);
    r.look(look_heading, 0);

    r.tick_channel();
    CHECK(r.body->heading == look_heading);
    CHECK(r.gun().emplaced_gun_yaw_word == static_cast<int16_t>(0x93));
}

// IsTurret pitch: the recoil term is subtracted AFTER the rate clamp, so a
// recoil impulse larger than one tick's traverse lands whole in the word.
// [orig: @0x440afc..0x440b13 then `sub ecx,[edx+380h]` @0x440b2a]
void test_isturret_recoil_after_rate_clamp() {
    Rig r(/*local=*/false, /*player_bit=*/false,
            opennova::def::DEF_ITEM_ATTRIB2_ISTURRET);
    r.look(kGunHeading, 0);
    constexpr int32_t kRecoil = 0x04000000; // 22.5 deg, far beyond 0x92CF34
    r.body->inf.recoil_pitch = kRecoil;

    r.tick_channel();
    // ecx = 0 - 0 - 0x8000 (within the rate); word = (ecx - recoil + 0x8000) >> 16
    CHECK(r.gun().emplaced_gun_pitch_word ==
            static_cast<int16_t>(bam_sar(bam_sub(0, kRecoil), 16)));
    CHECK(r.body->pitch == 0);
}

// An IsTurret window clamp still writes the occupant from the slewed word,
// not from the raw look: a local gunner past the arc on a turret whose word
// is already at the bound is pinned to gun - bound.
void test_isturret_window_pins_after_slew() {
    Rig r(/*local=*/true, /*player_bit=*/true,
            opennova::def::DEF_ITEM_ATTRIB2_ISTURRET);
    author_arc(r.gun());
    const int32_t look_heading = bam_sub(kGunHeading, 60 * kBamPerDegree);
    r.look(look_heading, 0);
    for (int i = 0; i < 60; ++i) CHECK(r.w.ai.pose_if_mounted(*r.body, r.w));
    const int32_t yaw_bound = 30 * kBamPerDegree;
    CHECK(r.gun().emplaced_gun_yaw_word == static_cast<int16_t>(yaw_bound >> 16));
    CHECK(r.body->heading == bam_sub(kGunHeading, yaw_bound));
    CHECK(r.body->inf.target_heading == r.body->heading);
}

// An addeweap child of a brained parent: the parent (pool 1, its own
// AiEntity with the given profile type) and the child gun hung on it at
// userpoint 1 with the given anchor subobject.
struct ParentRig {
    Rig r;
    EntityHandle parent_h;
    AiEntity *parent_ai = nullptr;

    ParentRig(int32_t profile_type, int16_t anchor_subobject, uint32_t parent_attrib)
            : r(/*local=*/true, /*player_bit=*/true) {
        Entity parent;
        parent.kind = EntityKind::Item;
        parent.health = 100;
        parent.alive = true;
        parent.item_attrib = parent_attrib;
        parent_h = r.w.registry.spawn(1, parent);
        parent_ai = r.w.ai.at(r.w.ai.attach(parent_h));
        // The brain array may have reallocated on the second attach.
        r.body = r.w.ai.for_handle(r.gunner_h);
        parent_ai->profile.type = profile_type;
        Entity &gun = r.gun();
        gun.emplacement_parent = parent_h;
        gun.emplacement_bone = 1;
        gun.emplacement_anchor_subobject = anchor_subobject;
    }
};

// Profile type 2 (ground): the raw yaw word lands in the parent brain's live
// and staged turret yaw; pitch is untouched. [orig: @0x440f6b..0x440f8a]
void test_parent_publication_ground_raw_yaw() {
    ParentRig pr(/*profile_type=*/2, /*anchor_subobject=*/0, /*parent_attrib=*/0);
    const int32_t look_heading = bam_sub(kGunHeading, 20 * kBamPerDegree);
    pr.r.look(look_heading, -7 * kBamPerDegree);
    pr.parent_ai->brain.f[AiBrain::kActivePitch] = 0x11111111;

    CHECK(pr.r.w.ai.pose_if_mounted(*pr.r.body, pr.r.w));
    const int32_t yaw = emplaced_word_bam(pr.r.gun().emplaced_gun_yaw_word);
    CHECK(yaw == (bam_sub(kGunHeading, look_heading) & ~0xFFFF));
    CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == yaw);
    CHECK(pr.parent_ai->brain.f[AiBrain::kStagingBlock + 3] == yaw);
    CHECK(pr.parent_ai->brain.f[AiBrain::kActivePitch] == 0x11111111);
}

// The gates: a child on a non-root subobject, or with the subobject not
// stamped, publishes nothing. [orig: `cmp [ebx+18h],0` @0x440f50]
void test_parent_publication_gates() {
    {
        ParentRig pr(2, /*anchor_subobject=*/3, 0);
        pr.r.look(bam_sub(kGunHeading, 20 * kBamPerDegree), 0);
        CHECK(pr.r.w.ai.pose_if_mounted(*pr.r.body, pr.r.w));
        CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == 0);
        CHECK(pr.parent_ai->brain.f[AiBrain::kStagingBlock + 3] == 0);
    }
    {
        ParentRig pr(2, /*anchor_subobject=*/-1, 0);
        pr.r.look(bam_sub(kGunHeading, 20 * kBamPerDegree), 0);
        CHECK(pr.r.w.ai.pose_if_mounted(*pr.r.body, pr.r.w));
        CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == 0);
    }
    {
        // A HELO parent without EWeap publishes nothing either.
        ParentRig pr(1, 0, 0);
        pr.r.look(bam_sub(kGunHeading, 20 * kBamPerDegree), 0);
        CHECK(pr.r.w.ai.pose_if_mounted(*pr.r.body, pr.r.w));
        CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == 0);
        CHECK(pr.parent_ai->brain.f[AiBrain::kActivePitch] == 0);
    }
}

// Profile type 1 (helo) with EWeap: both words, clamped by the parent's
// slot-1 weapon window when one is supplied, land in the live and staged
// yaw AND pitch channels. [orig: @0x440f95..0x441020]
void test_parent_publication_helo_clamped_pair() {
    ParentRig pr(/*profile_type=*/1, 0, kItemAttribEweap);
    const int32_t look_heading = bam_sub(kGunHeading, 50 * kBamPerDegree);
    pr.r.look(look_heading, -25 * kBamPerDegree);
    // Through the tick: no slot-1 row is carried, so the pair publishes
    // unclamped.
    CHECK(pr.r.w.ai.pose_if_mounted(*pr.r.body, pr.r.w));
    const int32_t yaw = emplaced_word_bam(pr.r.gun().emplaced_gun_yaw_word);
    const int32_t pitch = emplaced_word_bam(pr.r.gun().emplaced_gun_pitch_word);
    CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == yaw);
    CHECK(pr.parent_ai->brain.f[AiBrain::kStagingBlock + 3] == yaw);
    CHECK(pr.parent_ai->brain.f[AiBrain::kActivePitch] == pitch);
    CHECK(pr.parent_ai->brain.f[AiBrain::kStagingBlock + 4] == pitch);
    // The clamp leg itself, with a slot-1 row: yaw +-30, pitch [-10, +20].
    WeaponTableEntry slot1;
    slot1.turret_yaw_range_deg = 30;
    slot1.turret_pitch_max_deg = 20;
    slot1.turret_pitch_min_deg = 10;
    publish_emplaced_gun_words_to_parent(pr.r.w, pr.r.gun(), &slot1);
    CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == turret_window_limit_bam(30));
    CHECK(pr.parent_ai->brain.f[AiBrain::kStagingBlock + 3] == turret_window_limit_bam(30));
    CHECK(pr.parent_ai->brain.f[AiBrain::kActivePitch] == turret_window_limit_bam(20));
    CHECK(pr.parent_ai->brain.f[AiBrain::kStagingBlock + 4] == turret_window_limit_bam(20));
}

} // namespace

int main() {
    test_immediate_path_words_and_recoil_term();
    test_window_clamp_writes_occupant_look();
    test_isturret_slews_per_tick();
    test_isturret_local_gunner_yaw_tether();
    test_isturret_npc_gunner_yaw_tether();
    test_isturret_remote_player_untethered();
    test_isturret_recoil_after_rate_clamp();
    test_isturret_window_pins_after_slew();
    test_parent_publication_ground_raw_yaw();
    test_parent_publication_gates();
    test_parent_publication_helo_clamped_pair();
    if (failures == 0) std::puts("emplaced_gun_channel: OK");
    return failures == 0 ? 0 : 1;
}
