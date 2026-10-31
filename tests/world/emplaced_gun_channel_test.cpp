// The emplaced gun channel's stored words (entity+0x322 yaw / +0x324 pitch):
// the immediate and IsTurret producers of Entity_UpdateChildAttachment
// @0x4409A0 (the recoil term on both paths, the 0x92CF34/tick traverse, the
// local +-90 deg and non-Player +-4 deg gunner-yaw tethers), the window
// clamp's occupant write-back in Entity_UpdateTransformAndTurret @0x440ca0
// (@0x441263 / @0x4412b3), and the publication of the words as stored.
// [orig: the ewep pair @0x4409A0 / @0x440ca0; Math_ClampAngleToBounds @0x540cc0]

#include <base/io/bam.h>
#include <formats/def/def.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/inmatch/client_replica_emplaced.h>
#include <runtime/mission/promote.h> // mission::ItemSeatSpec
#include <runtime/replication/client_state.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/entity.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/player_weapon.h>
#include <runtime/world/pose_provider.h>
#include <runtime/world/present_drains.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <utility>
#include "../common/title_fixture.h"

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
    bool wp_jo = test_titles::jo_world(*wp);
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
        gun_seed.has_item_def = true; // seats need the def (D-NET-422)
        gun_seed.item_type_index = 2; // a resolved def row: the walk skips row 0 (WeaponAction_ProcessAllEntities @0x542690, @0x5426F2)
        gun_seed.health = 100;
        gun_seed.alive = true;
        gun_seed.heading = placed_heading_bam(0);
        gun_seed.pitch_bam = 0;
        gun_seed.item_attrib2 = attrib2;
        // An 'ewep' render class: the writer that publishes the gun words.
        gun_seed.emplaced_ctrl_publisher = true;
        Seat usegun;
        usegun.type = SeatType::Gunner;
        usegun.bone_index = 6;
        usegun.source_name = "UseGun";
        gun_seed.seats.push_back(usegun);
        gun_h = w.registry.spawn(1, gun_seed);

        Entity gunner;
        gunner.kind = EntityKind::Organic;
        gunner.item_type_index = 3; // a resolved def row: the walk skips row 0 (WeaponAction_ProcessAllEntities @0x542690, @0x5426B9)
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

// Only the ewep class update composes an attached gun, behind its slot Def: an
// ewep row whose slot is bound to a Def with a window that spans every angle.
// [orig: Entity_UpdateTransformAndTurret @0x440CA0 - the slot Def exit
//  @0x440E8E..0x440EA0 ahead of the matrix @0x440F04..0x4411BC]
void make_composed_ewep(World &w, Entity &gun) {
    gun.emplaced_update = true;
    w.tables.weapons.entries.resize(1);
    WeaponTableEntry &weapon = w.tables.weapons.entries[0];
    weapon.valid = true;
    weapon.turret_yaw_range_deg = 180;
    weapon.turret_pitch_max_deg = 90;
    weapon.turret_pitch_min_deg = 90;
    gun.primary_weapon_slot_adm = 0;
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
    // The words the parent-brain publication read are the slew's, ahead of the window leg
    // (a joiner's replica publication of its own gun reads them).
    // [orig: Entity_UpdateTransformAndTurret @0x440CA0 - the publication @0x440F04..0x441020,
    //  the window leg @0x4411D1]
    CHECK(r.gun().emplaced_published_yaw_word == emplaced_bam_word(50 * kBamPerDegree));
    CHECK(r.gun().emplaced_published_pitch_word == emplaced_bam_word(25 * kBamPerDegree));

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

// Without an authored arc the weapon def's window clamps both axes every
// time, in the parser's integer BAM: a zero yawrange locks the traverse at
// the gun's own heading (the JOTAC WPN_ROCKTDFLT / WPN_C130_DOOR shape), and
// a gun with no weapon def has no window at all.
// [orig: Entity_GetWeaponTurretLimits fallback @0x540E2C..0x540E58;
//  Math_ClampAngleToBounds @0x44123C / @0x44128C, unconditional;
//  WeaponDefs_ParseLineCallback imul 0xB60B60 @0x5443EC]
void test_weapon_window_zero_yawrange_locks_traverse() {
    Rig r(/*local=*/false, /*player_bit=*/false);
    r.w.tables.weapons.entries.resize(1);
    WeaponTableEntry &weapon = r.w.tables.weapons.entries[0];
    weapon.valid = true;
    weapon.turret_yaw_range_deg = 0;
    weapon.turret_pitch_max_deg = 5;
    weapon.turret_pitch_min_deg = 5;
    r.gun().primary_weapon_slot_adm = 0;
    const int32_t look_heading = bam_sub(kGunHeading, 40 * kBamPerDegree);
    r.look(look_heading, -12 * kBamPerDegree);
    r.tick_channel();
    CHECK(r.gun().emplaced_gun_yaw_word == 0);
    CHECK(r.gun().emplaced_gun_pitch_word ==
            static_cast<int16_t>((5 * kBamPerDegree) >> 16));
    CHECK(r.body->heading == kGunHeading);
    CHECK(r.body->pitch == -5 * kBamPerDegree);
    // No weapon def: no window, the look stands.
    r.gun().primary_weapon_slot_adm = kAdmSlotNone;
    r.look(look_heading, -12 * kBamPerDegree);
    r.tick_channel();
    CHECK(r.gun().emplaced_gun_yaw_word == static_cast<int16_t>((40 * kBamPerDegree) >> 16));
    CHECK(r.body->heading == look_heading);
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

// D-NET-1152, the authority's channel: an NPC gunner's tick tethers its Yaw first (the
// gun's ai-fn in the pool-1 walk), then its seat branch chases the aim words: the Yaw a quarter
// of the way (the step clamped to +-0x2000000), the Pitch an eighth.
// [orig: Entity_UpdateChildAttachment @0x4409A0 - the NPC tether @0x440abd..0x440ade;
//  Entity_UpdateInfantryAI @0x4B9910 - the look @0x4BEF57..0x4BEF97; Entity_UpdateAllEntities
//  @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9 ahead of the pool-0 walk @0x4C2426..0x4C2472]
void test_isturret_npc_gunner_tether_then_look() {
    Rig r(/*local=*/false, /*player_bit=*/false,
            opennova::def::DEF_ITEM_ATTRIB2_ISTURRET);
    r.look(bam_sub(kGunHeading, 10 * kBamPerDegree), 0);
    r.body->inf.aim_heading = kGunHeading;
    r.body->inf.aim_pitch = 0x08000000;
    CHECK(r.w.ai.pose_if_mounted(*r.body, r.w));
    // The tether leaves 0x3D275280 (the gun's Yaw less 4 degrees and the word's half); the
    // chase adds (0x40000000 - 0x3D275280 + 2) >> 2 = 0xB62B60; the Pitch (0x08000000 + 4) >> 3.
    CHECK(r.body->heading == 0x3DDD7DE0);
    CHECK(r.gunner().heading == 0x3DDD7DE0);
    CHECK(r.body->pitch == 0x01000000);
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
        // The publication belongs to the ewep class update and runs behind
        // the gun's own weapon Def; this Def's window spans every angle.
        // [orig: Entity_UpdateTransformAndTurret Def gate @0x440E8C..0x440EA0]
        gun.emplaced_update = true;
        r.w.tables.weapons.entries.resize(1);
        WeaponTableEntry &gun_weapon = r.w.tables.weapons.entries[0];
        gun_weapon.valid = true;
        gun_weapon.name = "CHILD_GUN";
        gun_weapon.turret_yaw_range_deg = 180;
        gun_weapon.turret_pitch_max_deg = 90;
        gun_weapon.turret_pitch_min_deg = 90;
        gun.primary_weapon_slot_adm = 0;
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
        // A root-anchored child whose parent brain has no owner word publishes
        // nothing either. [orig: Entity_UpdateTransformAndTurret -- the
        //  publication's owner gate, after the subobject test @0x440f50]
        ParentRig pr(2, /*anchor_subobject=*/0, 0);
        pr.parent_ai->brain.f[AiBrain::kOwner] = 0;
        pr.r.gun().emplaced_gun_yaw_word = 0x1234;
        publish_emplaced_gun_words_to_parent(pr.r.w, pr.r.gun());
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
// authored primary-weapon window, land in the live and staged
// yaw AND pitch channels. [orig: @0x440f95..0x441020]
void test_parent_publication_helo_clamped_pair() {
    ParentRig pr(/*profile_type=*/1, 0, kItemAttribEweap);
    const int32_t look_heading = bam_sub(kGunHeading, 50 * kBamPerDegree);
    pr.r.look(look_heading, -25 * kBamPerDegree);
    // Aircraft +0x474 is bound from items.def primary_weapon by its class init at
    // its spawn, just as an ewep's +0x2B4 is; the clamp reads that bound Def.
    // [orig: Entity_InitAircraftWeaponBones @0x490173..0x49017b;
    // WeaponSlot_InitFromEntityDef @0x5466d8..0x54670a; slot-1 def [edi+494h] @0x440fbc]
    auto &table = pr.r.w.tables.weapons;
    table.entries.resize(2);
    WeaponTableEntry &slot1 = table.entries[1];
    slot1.valid = true;
    slot1.name = "PARENT_GUN";
    slot1.turret_yaw_range_deg = 30;
    slot1.turret_pitch_max_deg = 20;
    slot1.turret_pitch_min_deg = 10;
    pr.r.w.registry.get(pr.parent_h)->primary_weapon = slot1.name;
    // An unbound parent slot clamps nothing and publishes nothing.
    CHECK(pr.r.w.ai.pose_if_mounted(*pr.r.body, pr.r.w));
    CHECK(pr.parent_ai->brain.f[AiBrain::kActivePitch] == 0);
    CHECK(pr.r.w.vehicles.init_weapon_slot_from_entity_def(*pr.r.w.registry.get(pr.parent_h)));
    CHECK(pr.r.w.ai.pose_if_mounted(*pr.r.body, pr.r.w));
    CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == 30 * kBamPerDegree);
    CHECK(pr.parent_ai->brain.f[AiBrain::kStagingBlock + 3] == 30 * kBamPerDegree);
    CHECK(pr.parent_ai->brain.f[AiBrain::kActivePitch] == 20 * kBamPerDegree);
    CHECK(pr.parent_ai->brain.f[AiBrain::kStagingBlock + 4] == 20 * kBamPerDegree);
    // Unlike the child's optional window, the parent's two clamps always
    // run: a zero range locks that axis to zero.
    slot1.turret_yaw_range_deg = 0;
    slot1.turret_pitch_max_deg = 0;
    slot1.turret_pitch_min_deg = 0;
    CHECK(pr.r.w.ai.pose_if_mounted(*pr.r.body, pr.r.w));
    CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == 0);
    CHECK(pr.parent_ai->brain.f[AiBrain::kActivePitch] == 0);
    // Raw 180-degree bounds admit the complete signed angle domain; this
    // path must not reinterpret them as zero-width optional windows.
    slot1.turret_yaw_range_deg = 180;
    slot1.turret_pitch_max_deg = 180;
    slot1.turret_pitch_min_deg = 180;
    CHECK(pr.r.w.ai.pose_if_mounted(*pr.r.body, pr.r.w));
    CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == 0x238E0000);
    CHECK(pr.parent_ai->brain.f[AiBrain::kActivePitch] == 0x11C70000);
}

// A full world tick refreshes attached riders after the carrier pose. That
// second pose must not run the IsTurret producer a second time.
// [orig: Entity_UpdateChildAttachment @0x4409A0, once in the pool-1 walk @0x4b8e3c]
void test_attached_turret_slews_once_per_world_tick() {
    for (bool attached : {false, true}) {
        Rig r(/*local=*/true, /*player_bit=*/true,
                opennova::def::DEF_ITEM_ATTRIB2_ISTURRET);
        if (attached) {
            Entity carrier;
            carrier.kind = EntityKind::Item;
            carrier.health = 100;
            carrier.position = {8, 12, 4};
            const EntityHandle parent = r.w.registry.spawn(1, carrier);
            r.gun().emplacement_parent = parent;
            r.gun().emplacement_parent_spawn_id = r.w.registry.get(parent)->registry_spawn_id;
            r.gun().emplacement_pose_metadata_resolved = true;
            make_composed_ewep(r.w, r.gun());
        }
        const int32_t look = bam_sub(kGunHeading, 30 * kBamPerDegree);
        r.look(look, 0);
        r.w.run_logic_tick(true);
        CHECK(r.gun().emplaced_gun_yaw_word == 0x93);
        CHECK(r.body->heading == look);
        if (attached) {
            CHECK(r.body->self->position.x.q16 == to_fixed(8));
            CHECK(r.body->self->position.y.q16 == to_fixed(12));
            CHECK(r.body->self->position.z.q16 == to_fixed(4));
        }
        r.w.run_logic_tick(true);
        CHECK(r.gun().emplaced_gun_yaw_word == 0x126);
    }
}

// The barrel keeps its word when idle, wraps at 16 bits, and takes a full
// coast-down after its shared MountSlot kick byte reaches zero.
void test_barrel_spin_tail_and_class_gate() {
    Rig r(true, true);
    r.w.tables.weapons.entries.resize(1);
    r.w.tables.weapons.entries[0].valid = true;
    r.gun().primary_weapon_slot_adm = 0;
    r.gun().primary_weapon_slot.kick = 2;
    tick_emplaced_weapon_animation(r.w, r.gun());
    CHECK(r.gun().primary_weapon_slot.kick == 2); // a different class
    r.gun().emplaced_update = true;
    r.gun().primary_weapon_slot_adm = 0xFF;
    tick_emplaced_weapon_animation(r.w, r.gun());
    CHECK(r.gun().emplaced_spin_ticks == 0); // missing weapon definition
    CHECK(r.gun().primary_weapon_slot.kick == 2);
    r.gun().primary_weapon_slot_adm = 0;
    r.gun().emplaced_spin_phase = 65000;
    tick_emplaced_weapon_animation(r.w, r.gun());
    CHECK(r.gun().emplaced_spin_ticks == 59);
    CHECK(r.gun().emplaced_spin_phase == 1352);
    CHECK(r.gun().primary_weapon_slot.kick == 1);
    tick_emplaced_weapon_animation(r.w, r.gun());
    CHECK(r.gun().emplaced_spin_phase == 3240);
    CHECK(r.gun().primary_weapon_slot.kick == 0);
    r.w.vehicles.detach(r.gunner_h);
    r.w.rules.last_tick_of_batch = false;
    tick_emplaced_weapon_animation(r.w, r.gun());
    CHECK(r.gun().emplaced_spin_ticks == 58);
    CHECK(r.gun().emplaced_spin_phase == 5096); // no occupant/audio gate
    for (int tick = 0; tick < 58; ++tick)
        tick_emplaced_weapon_animation(r.w, r.gun());
    CHECK(r.gun().emplaced_spin_ticks == 0);
    CHECK(r.gun().emplaced_spin_phase == 57992);
    tick_emplaced_weapon_animation(r.w, r.gun());
    CHECK(r.gun().emplaced_spin_phase == 57992); // holds final angle
}

// The class tail consumes kick before the frame's weapon walk decays it again.
// A reversed order loses this spin step; a second entity update doubles it.
void test_barrel_spin_once_before_weapon_pump() {
    Rig r(true, true);
    r.w.tables.weapons.entries.resize(1);
    auto &weapon = r.w.tables.weapons.entries[0];
    weapon.valid = true;
    weapon.ammo_index = 0;
    r.gun().emplaced_update = true;
    r.gun().primary_weapon_slot_adm = 0;
    r.gun().primary_weapon_slot.kick = 2;
    r.w.run_logic_tick(true);
    r.w.pump_weapon_actions();
    CHECK(r.gun().primary_weapon_slot.kick == 0);
    CHECK(r.gun().emplaced_spin_ticks == 59);
    CHECK(r.gun().emplaced_spin_phase == 1888);
    EmplacedWeaponControls controls;
    CHECK(emplaced_weapon_controls_for(r.w, r.gun(), controls));
    CHECK(controls.spin == 1888);
    r.w.run_logic_tick(true);
    r.w.pump_weapon_actions();
    CHECK(r.gun().emplaced_spin_ticks == 58);
    CHECK(r.gun().emplaced_spin_phase == 3744);
}

// The spin tail follows the class update's matrix leg: the kick's last tick
// plays the trail-off at the child's composed Position, its carrier's of this
// tick. The carrier stands 1000 units from where the child stood: a play from
// that old frame would queue a distance-delayed slot instead of playing at
// the listener.
// [orig: Entity_UpdateTransformAndTurret @0x440CA0 - the root copy
//  @0x4410EA..0x4411BC and the OR @0x4411C2 ahead of the spin tail
//  @0x44139D..0x441447, Sound_PlayWithDistanceAttenuation at [child+4]
//  @0x44141E..0x441423]
void test_trail_off_plays_at_the_composed_position() {
    Rig r(/*local=*/true, /*player_bit=*/true);
    CHECK(r.w.vehicles.detach(r.gunner_h));
    r.w.tables.weapons.entries.resize(1);
    WeaponTableEntry &weapon = r.w.tables.weapons.entries[0];
    weapon.valid = true;
    std::strcpy(weapon.action_fsm.soundtrailoff, "GUN_TAIL");
    Entity carrier;
    carrier.kind = EntityKind::Item;
    carrier.health = 100;
    carrier.position = {1000.0f, 0.0f, 0.0f};
    const EntityHandle parent = r.w.registry.spawn(1, carrier);
    Entity &gun = r.gun();
    gun.emplaced_update = true;
    gun.primary_weapon_slot_adm = 0;
    gun.emplacement_parent = parent;
    gun.emplacement_parent_spawn_id = r.w.registry.get(parent)->registry_spawn_id;
    gun.emplacement_pose_metadata_resolved = true;
    gun.ground_target = parent;
    gun.primary_weapon_slot.kick = 1;
    r.w.out.fire_sounds.set_listener({1000.0f, 0.0f, 0.0f});
    r.w.run_logic_tick(true);
    CHECK(r.gun().position.x == 1000.0f); // the root copy of the carrier
    CHECK(r.gun().primary_weapon_slot.kick == 0);
    bool played = false;
    for (const ReadyFireSound &sound : r.w.out.fire_sounds.drain())
        if (sound.set_name == "GUN_TAIL")
            played = sound.pos.x == 1000.0f && sound.pos.y == 0.0f && sound.pos.z == 0.0f;
    CHECK(played);
    CHECK(r.w.out.fire_sounds.pending_count() == 0);
}

// The ewep class's CTRL writer has no occupant test: once the gunner leaves,
// the gun keeps publishing its held words, its spin word and its inline
// slot's heat, and only an 'ewep' render class publishes them at all.
// [orig: HUD_CacheWeaponSlotInfo @0x440930 (words @0x440934..0x440948, spin
//  @0x44094E..0x440955, heat @0x44095B..0x440991), def+0x144 of the 'ewep'
//  render-class row @0x82CFA0; the words' only writers @0x440B23/@0x440B58
//  (producer), @0x44125C/@0x4412A4 (window), @0x5470F9/@0x547100 (carrier
//  destruction)]
void test_unoccupied_gun_publishes_held_words_spin_and_heat() {
    Rig r(/*local=*/false, /*player_bit=*/false);
    r.w.tables.weapons.entries.resize(1);
    WeaponTableEntry &weapon = r.w.tables.weapons.entries[0];
    weapon.valid = true;
    weapon.turret_yaw_range_deg = 180;
    weapon.turret_pitch_max_deg = 90;
    weapon.turret_pitch_min_deg = 90;
    weapon.action_fsm.heat_per_shot = 100;
    weapon.action_fsm.heat_decay_per_tick = 7;
    r.gun().primary_weapon_slot_adm = 0;
    r.look(bam_sub(kGunHeading, 0x10000000), 0x02000000);
    r.tick_channel();
    const int16_t yaw = r.gun().emplaced_gun_yaw_word;
    const int16_t pitch = r.gun().emplaced_gun_pitch_word;
    CHECK(yaw != 0 && pitch != 0);
    r.gun().emplaced_spin_phase = 0x1234;
    r.w.logic_tick = 50;
    r.gun().primary_weapon_slot.heat_window_end_tick = 60;

    // A carrier of another render class publishes only through its UseGun
    // rider's seat call. [orig: Entity_AttachToBoneAndUpdateTransform
    //  @0x546517..0x546518]
    r.gun().emplaced_ctrl_publisher = false;
    EmplacedWeaponControls seat_call;
    CHECK(emplaced_weapon_controls_for(r.w, r.gun(), seat_call));
    CHECK(seat_call.gun_yaw == static_cast<uint16_t>(yaw));
    int32_t seat_heat = -1;
    CHECK(world_model_heat_glow_for(r.w, r.gun(), seat_heat));
    CHECK(seat_heat == 70);
    r.gun().emplaced_ctrl_publisher = true;

    CHECK(r.w.vehicles.detach(r.gunner_h));
    CHECK(!r.gunner().mounted);
    EmplacedWeaponControls held;
    CHECK(emplaced_weapon_controls_for(r.w, r.gun(), held));
    CHECK(held.valid);
    CHECK(held.gun_yaw == static_cast<uint16_t>(yaw));
    CHECK(held.gun_pitch == static_cast<uint16_t>(pitch));
    CHECK(held.spin == 0x1234);
    int32_t heat = -1;
    CHECK(world_model_heat_glow_for(r.w, r.gun(), heat));
    CHECK(heat == 70); // 10 ticks of window x 7 per tick
    r.w.logic_tick = 60;
    CHECK(world_model_heat_glow_for(r.w, r.gun(), heat));
    CHECK(heat == 0); // the cold leg still publishes literal zero

    // With the rider gone, another render class's callback writes none of
    // the three.
    r.gun().emplaced_ctrl_publisher = false;
    CHECK(!emplaced_weapon_controls_for(r.w, r.gun(), held));
    CHECK(!world_model_heat_glow_for(r.w, r.gun(), heat));
}

// The ewep class update runs every tick whether or not the gun is manned:
// an emptied turret keeps driving its hull's turret channel with the held
// word, so the hull turret does not drift back when the gunner leaves.
// [orig: Entity_UpdatePool1Slot @0x4B8E41..0x4B8E53 -> the publication
//  @0x440f04..0x441020, behind the Def gate @0x440E8C..0x440EA0]
void test_parent_publication_runs_unoccupied_every_tick() {
    ParentRig pr(/*profile_type=*/2, /*anchor_subobject=*/0, /*parent_attrib=*/0);
    pr.r.look(bam_sub(kGunHeading, 20 * kBamPerDegree), 0);
    CHECK(pr.r.w.ai.pose_if_mounted(*pr.r.body, pr.r.w));
    const int32_t yaw = emplaced_word_bam(pr.r.gun().emplaced_gun_yaw_word);
    CHECK(yaw != 0);
    CHECK(pr.r.w.vehicles.detach(pr.r.gunner_h));
    pr.parent_ai->brain.f[AiBrain::kActiveYaw] = 0x12340000;
    pr.parent_ai->brain.f[AiBrain::kStagingBlock + 3] = 0x12340000;
    tick_emplaced_weapon_class_update(pr.r.w, pr.r.gun());
    CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == yaw);
    CHECK(pr.parent_ai->brain.f[AiBrain::kStagingBlock + 3] == yaw);
    // The carried leg rebuilt the gun's matrix from its carrier: Flags 0x20000
    // on engine_flags [orig: Entity_UpdateTransformAndTurret @0x4411C2].
    CHECK((pr.r.gun().engine_flags & kEntityFlagMatrixBuilt) != 0);
    // The world tick's pool-1 walk runs that class update for the empty gun.
    pr.parent_ai->brain.f[AiBrain::kActiveYaw] = 0x12340000;
    pr.parent_ai->brain.f[AiBrain::kStagingBlock + 3] = 0x12340000;
    // A live parent generation keeps the child attached through the world
    // tick's orphan peel; the hull's own brain stays out of this tick.
    pr.r.gun().emplacement_parent_spawn_id =
            pr.r.w.registry.get(pr.parent_h)->registry_spawn_id;
    pr.r.w.registry.get(pr.parent_h)->spawn_phase = 1000;
    pr.r.w.run_logic_tick(true);
    CHECK(pr.r.w.registry.get(pr.r.gun_h) != nullptr);
    if (pr.r.w.registry.get(pr.r.gun_h) == nullptr) return;
    CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == yaw);
    // Without the gun's weapon Def the class update publishes nothing.
    pr.r.gun().primary_weapon_slot_adm = kAdmSlotNone;
    pr.parent_ai->brain.f[AiBrain::kActiveYaw] = 0x12340000;
    pr.r.gun().engine_flags &= ~kEntityFlagMatrixBuilt;
    tick_emplaced_weapon_class_update(pr.r.w, pr.r.gun());
    CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == 0x12340000);
    // ...and the slot gate's exit skips the matrix bit too [orig: @0x440EA0].
    CHECK((pr.r.gun().engine_flags & kEntityFlagMatrixBuilt) == 0);
    // The gate reads the slot the gun's class init bound, never the name its item
    // def carries: an unbound gun naming a weapon still publishes nothing until its
    // init binds the slot. [orig: WeaponSlot_InitFromEntityDef @0x5466E8..0x54670A,
    //  from Entity_InitBoneReferences @0x441482]
    pr.r.gun().primary_weapon = "CHILD_GUN";
    tick_emplaced_weapon_class_update(pr.r.w, pr.r.gun());
    CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == 0x12340000);
    CHECK(pr.r.w.vehicles.init_weapon_slot_from_entity_def(pr.r.gun()));
    CHECK(pr.r.gun().primary_weapon_slot_adm == 0);
    tick_emplaced_weapon_class_update(pr.r.w, pr.r.gun());
    CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == yaw);
}

// On a vehicle carrier (def type 1) the ewep class update clears the child's
// hide and kill bits (Flags & ~7, on both words) and copies the carrier's
// 16-bit Health word, every update. A dead PlayerControl carrier instead
// hides the child (Flags bit 0) and ends the update: no vehicle block, no
// publication, no matrix bit, no spin tail. Occupied, the producer still
// refreshes the words, but the class update's publication and window leg
// stop. A revived hull brings both back, and its vehicle block clears the
// hide. [orig: Entity_UpdateTransformAndTurret — the exit @0x440CBF..0x440CE1,
//  the vehicle block @0x440EA6..0x440EBE, the OR @0x4411C2, the window leg
//  @0x4411d1..0x4412b3]
void test_class_update_vehicle_block_and_dead_hull_exit() {
    ParentRig pr(/*profile_type=*/2, /*anchor_subobject=*/0, kItemAttribPlayerControl);
    World &w = pr.r.w;
    Entity &hull = *w.registry.get(pr.parent_h);
    hull.has_item_def = true;
    hull.item_type = 1;
    hull.health = 250;
    Entity &gun = pr.r.gun();
    author_arc(gun);
    gun.flags |= kEntityFlagCarried | kEntityFlagDead;
    gun.engine_flags |= kEntityFlagHusk;
    gun.health = 17;

    // A live hull: the vehicle block, the publication and the matrix bit.
    gun.emplaced_gun_yaw_word = 0x1234;
    tick_emplaced_weapon_class_update(w, gun);
    CHECK(((gun.flags | gun.engine_flags) &
                  (kEntityFlagCarried | kEntityFlagDead | kEntityFlagHusk)) == 0);
    CHECK(gun.health == 250);
    CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == 0x12340000);
    CHECK((gun.engine_flags & kEntityFlagMatrixBuilt) != 0);

    // The hull dies: the child hides and the update ends at once.
    hull.flags |= kEntityFlagDead | kEntityFlagHusk;
    hull.health = 0;
    gun.health = 17;
    gun.engine_flags &= ~kEntityFlagMatrixBuilt;
    gun.emplaced_gun_yaw_word = 0x2222;
    gun.primary_weapon_slot.kick = 2;
    tick_emplaced_weapon_class_update(w, gun);
    CHECK((gun.flags & kEntityFlagCarried) != 0);
    CHECK(gun.health == 17);
    CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == 0x12340000);
    CHECK((gun.engine_flags & kEntityFlagMatrixBuilt) == 0);
    CHECK(gun.primary_weapon_slot.kick == 2 && gun.emplaced_spin_ticks == 0);

    // Occupied on the dead hull: the word follows the look 50 deg past the
    // arc, unpinned, the look keeps its heading and the brain its word.
    const int32_t look_heading = bam_sub(kGunHeading, 50 * kBamPerDegree);
    pr.r.look(look_heading, 0);
    pr.r.tick_channel();
    const int16_t unpinned = static_cast<int16_t>((50 * kBamPerDegree) >> 16);
    CHECK(gun.emplaced_gun_yaw_word == unpinned);
    CHECK(pr.r.body->heading == look_heading);
    CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == 0x12340000);

    // Revived: the publication (before the pin) and the window pin return,
    // and the vehicle block clears the hide.
    hull.flags &= ~(kEntityFlagDead | kEntityFlagHusk);
    hull.health = 250;
    pr.r.look(look_heading, 0);
    pr.r.tick_channel();
    CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == emplaced_word_bam(unpinned));
    CHECK(gun.emplaced_gun_yaw_word == static_cast<int16_t>((30 * kBamPerDegree) >> 16));
    CHECK(pr.r.body->heading == bam_sub(kGunHeading, 30 * kBamPerDegree));
    tick_emplaced_weapon_class_update(w, gun);
    CHECK((gun.flags & kEntityFlagCarried) == 0);
    CHECK(gun.health == 250);
}

// Through a carrier's death and respawn the attachment stays: the world tick
// keeps it resident on the dead PlayerControl hull, which hides it, and the
// hull's respawn brings it back through the vehicle block (the hide, kill and
// husk bits cleared, the Health word copied, the intact model restored).
// [orig: Entity_UpdateTransformAndTurret @0x440CBF..0x440CE1, @0x440EB3..0x440EBE;
//  Entity_RespawnVehicle @0x45FF40]
void test_carrier_death_and_respawn_keep_the_child() {
    ParentRig pr(/*profile_type=*/2, /*anchor_subobject=*/0, kItemAttribPlayerControl);
    World &w = pr.r.w;
    Entity *hull = w.registry.get(pr.parent_h);
    hull->has_item_def = true;
    hull->item_type = 1;
    hull->health = 250;
    hull->health_max = 250;
    Entity &gun = pr.r.gun();
    gun.emplacement_parent_spawn_id = hull->registry_spawn_id;
    gun.ground_target = pr.parent_h;
    CHECK(w.vehicles.detach(pr.r.gunner_h));

    // The hull dies (the kill's |= 6 on both words) and the child with it.
    hull->health = 0;
    hull->alive = false;
    hull->flags |= kEntityFlagDead | kEntityFlagHusk;
    hull->engine_flags |= kEntityFlagDead | kEntityFlagHusk;
    hull->spawn_phase = 1000; // the hull's brain stays out of these ticks
    gun.health = 0;
    gun.alive = false;
    gun.engine_flags |= kEntityFlagDead | kEntityFlagHusk;
    w.run_logic_tick(true);
    const Entity *child = w.registry.get(pr.r.gun_h);
    CHECK(child != nullptr);
    if (child == nullptr) return;
    CHECK((child->flags & kEntityFlagCarried) != 0);
    CHECK(!child->alive);

    // The respawned hull's vehicle block revives it.
    w.vehicles.respawn(*w.registry.get(pr.parent_h));
    w.registry.get(pr.parent_h)->spawn_phase = 1000;
    w.run_logic_tick(true);
    child = w.registry.get(pr.r.gun_h);
    CHECK(child != nullptr);
    if (child == nullptr) return;
    CHECK(((child->flags | child->engine_flags) &
                  (kEntityFlagCarried | kEntityFlagDead | kEntityFlagHusk)) == 0);
    CHECK(child->alive && child->health == 250);
    bool restored = false;
    for (const HuskSwapEvent &swap : w.out.destruction.husk_swaps)
        restored |= swap.wire_handle == pr.r.gun_h.packed && swap.restore_intact;
    CHECK(restored);
}

// A carrier row destroyed under its child (a child outside the carrier's EWeap
// refNum group, which the destroy would take with it): retail keeps the child's
// pointer to the zeroed row, so the class update finds no def (no hide or
// vehicle block) and its root copy puts the child on the zeroed pose, the world
// origin with zero angles, keeping any earlier hide. The next entity allocated
// in that row becomes the child's carrier: a live vehicle there clears the hide.
// [orig: Entity_Destroy memset @0x43EA70, its refNum walk @0x43E9CD ->
//  EntityReference_DestroyEWeapGroup @0x546F30; Entity_UpdateTransformAndTurret def test
//  @0x440EA6, root copy @0x4410EA..0x4411BC, the vehicle block @0x440EB3]
void test_freed_carrier_row_keeps_the_child() {
    ParentRig pr(/*profile_type=*/2, /*anchor_subobject=*/0, kItemAttribPlayerControl);
    World &w = pr.r.w;
    Entity &gun = pr.r.gun();
    gun.position = {12.0f, 34.0f, 5.0f};
    gun.flags |= kEntityFlagCarried; // hidden by the hull's death
    w.registry.despawn(pr.parent_h);
    tick_emplaced_weapon_class_update(w, gun);
    CHECK(gun.position.x == 0.0f && gun.position.y == 0.0f && gun.position.z == 0.0f);
    CHECK(gun.heading == placed_heading_bam(90) && gun.pitch_bam == 0 && gun.roll_bam == 0);
    CHECK((gun.engine_flags & kEntityFlagMatrixBuilt) != 0);
    CHECK((gun.flags & kEntityFlagCarried) != 0);

    Entity next;
    next.kind = EntityKind::Item;
    next.has_item_def = true;
    next.item_type = 1;
    next.item_attrib = kItemAttribPlayerControl;
    next.health = 400;
    next.position = {7.0f, 8.0f, 9.0f};
    const EntityHandle next_h = w.registry.spawn_at(pr.parent_h, next);
    CHECK(next_h == pr.parent_h);
    tick_emplaced_weapon_class_update(w, pr.r.gun());
    CHECK((pr.r.gun().flags & kEntityFlagCarried) == 0);
    CHECK(pr.r.gun().health == 400);
}

// The destroyed carrier's refNum children return to rest: their held gun
// words are zeroed, and their ammo re-splits only through a resolved Def.
// [orig: Vehicle_ReleaseEWeapGroupOnDestruction @0x547040 (+0x324/+0x322 =
//  0 @0x5470f9..0x547100; WeaponSlot_SplitAmmoIntoClipAndReserve
//  @0x547107..0x54710e)]
void test_carrier_destruction_resets_child_words() {
    Rig r(/*local=*/false, /*player_bit=*/false);
    Entity carrier;
    carrier.kind = EntityKind::Item;
    carrier.has_item_def = true;
    carrier.item_type = 1;
    carrier.item_attrib = 0x40u;
    carrier.ref_num = 7;
    carrier.health = 0;
    const EntityHandle carrier_h = r.w.registry.spawn(1, carrier);
    Entity &gun = r.gun();
    gun.has_item_def = true;
    gun.item_attrib |= kItemAttribEweap;
    gun.ref_num = 7;
    gun.ref_group_member = true;
    gun.emplaced_gun_yaw_word = 0x1234;
    gun.emplaced_gun_pitch_word = -0x234;
    gun.primary_weapon_slot.clip = 3;
    gun.primary_weapon_slot.reserve = 9;
    r.w.vehicles.cleanup_destroyed_ref_group(*r.w.registry.get(carrier_h));
    CHECK(r.gun().emplaced_gun_yaw_word == 0);
    CHECK(r.gun().emplaced_gun_pitch_word == 0);
    // No weapon row: the split never runs and the ammo stays.
    CHECK(r.gun().primary_weapon_slot.clip == 3);
    CHECK(r.gun().primary_weapon_slot.reserve == 9);

    // Both death legs lead with that cleanup for a refNum carrier, ahead of
    // every husk gate. [orig: Entity_SpawnDeathPieces @0x493409..0x49344D;
    //  Entity_UpdateDeathTransforms @0x494669..0x494673]
    r.gun().emplaced_gun_yaw_word = 0x1234;
    r.gun().emplaced_gun_pitch_word = -0x234;
    spawn_death_pieces(r.w, *r.w.registry.get(carrier_h));
    CHECK(r.gun().emplaced_gun_yaw_word == 0);
    CHECK(r.gun().emplaced_gun_pitch_word == 0);
    r.gun().emplaced_gun_yaw_word = 0x1234;
    r.gun().emplaced_gun_pitch_word = -0x234;
    entity_update_death_transforms(r.w, *r.w.registry.get(carrier_h), /*silent=*/true);
    CHECK(r.gun().emplaced_gun_yaw_word == 0);
    CHECK(r.gun().emplaced_gun_pitch_word == 0);
}

// The dying vehicle's refNum group, by def: a vehicle without PlayerControl
// destroys the group list's EWeap members and gives up its refNum, so its
// wreck's destroy later walks nothing; a member without EWeap stays. With
// PlayerControl a row that carries the refNum outside the group list is left
// alone, and a non-vehicle def touches nothing.
// [orig: Vehicle_ReleaseEWeapGroupOnDestruction @0x547040 — the def type test
//  @0x54707C, the PlayerControl test @0x547086, the member tests
//  @0x5470B9..0x5470D3, the EntityReference_DestroyEWeapGroup call @0x547143;
//  EntityReference_DestroyEWeapGroup @0x546F30 (the list removal
//  @0x546FBC..0x547001, refNum = 0 @0x547017); Entity_UpdateDeathTransforms
//  @0x494669..0x494673]
void test_vehicle_death_releases_or_destroys_the_eweap_group() {
    auto wp = std::make_unique<World>();
    test_titles::jo_world(*wp);
    World &w = *wp;
    w.registry.configure_pool(1, 12);
    auto spawn = [&](uint8_t type, uint32_t attrib, uint8_t ref, bool member) {
        Entity e;
        e.kind = EntityKind::Item;
        e.has_item_def = true;
        e.item_type = type;
        e.item_attrib = attrib;
        e.ref_num = ref;
        e.ref_group_member = member;
        e.health = 10;
        e.alive = true;
        return w.registry.spawn(1, e);
    };
    const EntityHandle hull = spawn(1, 0, 7, true);
    const EntityHandle gun = spawn(6, kItemAttribEweap, 7, true);
    const EntityHandle crate = spawn(6, 0, 7, true);
    entity_update_death_transforms(w, *w.registry.get(hull), /*silent=*/true);
    CHECK(w.registry.get(gun) == nullptr);
    CHECK(w.registry.get(crate) != nullptr);
    const Entity *wreck = w.registry.get(hull);
    CHECK(wreck != nullptr && wreck->ref_num == 0 && !wreck->ref_group_member);
    w.out.entity_events.clear();
    CHECK(w.commands.remove_ssn(hull));
    CHECK(w.registry.get(crate) != nullptr);

    const EntityHandle tank = spawn(1, kItemAttribPlayerControl, 8, false);
    const EntityHandle turret = spawn(6, kItemAttribEweap, 8, true);
    const EntityHandle stray = spawn(6, kItemAttribEweap, 8, false);
    w.registry.get(turret)->emplaced_gun_yaw_word = 0x1234;
    w.registry.get(stray)->emplaced_gun_yaw_word = 0x1234;
    w.vehicles.cleanup_destroyed_ref_group(*w.registry.get(tank));
    CHECK(w.registry.get(turret) != nullptr && w.registry.get(turret)->emplaced_gun_yaw_word == 0);
    CHECK(w.registry.get(stray) != nullptr && w.registry.get(stray)->emplaced_gun_yaw_word == 0x1234);
    CHECK(w.registry.get(tank)->ref_num == 8);

    const EntityHandle bunker = spawn(6, 0, 9, true);
    const EntityHandle bunker_gun = spawn(6, kItemAttribEweap, 9, true);
    w.vehicles.cleanup_destroyed_ref_group(*w.registry.get(bunker));
    CHECK(w.registry.get(bunker_gun) != nullptr && w.registry.get(bunker)->ref_num == 9);
}

// The overheat emitter's anchor fake: every userpoint of `gun` resolves at
// (index, 2, 3) facing +Y; it records the record's model and index.
struct HeatPoints : IPoseProvider {
    EntityHandle gun;
    int index = 0;
    const opennova::threedi::Threedi3di3 *model = nullptr;
    bool resolve_userpoint_frame(World &, EntityHandle h, const opennova::threedi::Threedi3di3 *m,
            int point, int32_t out[6], int32_t dir[3]) override {
        if (h != gun) return false;
        index = point;
        model = m;
        out[0] = point << 16;
        out[1] = 2 << 16;
        out[2] = 3 << 16;
        out[3] = out[4] = out[5] = 0;
        if (dir != nullptr) {
            dir[0] = 0;
            dir[1] = 1 << 16;
            dir[2] = 0;
        }
        return true;
    }
};

// The emplaced .50's heat model (heat_values 0.35,0.3 parsed as the existing FSM
// rig's 1310 / 42; heat_effect HEAT, .5) with an OVERHEATED row naming its effect.
void configure_heat_gun(WeaponTableEntry &e) {
    e.name = "WPN_EMPLCD50";
    e.valid = true;
    e.clipsize = -1;
    e.action_fsm.clip_capacity = -1;
    for (int action = 0; action < weapon_action::kCount; ++action)
        e.action_fsm.actions[action].id = action;
    e.action_fsm.heat_per_shot = 1310;
    e.action_fsm.heat_decay_per_tick = 42;
    e.action_fsm.heat_glow_threshold = 32768;
    std::strcpy(e.action_fsm.actions[weapon_action::kOverheated].particle, "FX_OVERHEAT1");
}

// An unoccupied hot gun walked as itself drives the overheat emitter on its own heat
// userpoint: the fraction of the heat past the threshold, the gun as the tag, re-aimed
// each hot tick; a zero gate byte takes no anchor; a gfx3 def anchors OVERHEATED's gfx3
// point behind the launch byte; heat under the threshold releases it.
// [orig: WeaponAction_ProcessAllEntities @0x5426E9..0x542716; WeaponAction_ProcessFrame
//  @0x541052..0x54125C (the EWeap branch @0x5410B4..0x5410DF); Entity_GetWeaponBoneByte @0x545A00;
//  Entity_ComputeUserpointWorldTransform @0x545C60 (the gate @0x545D06..0x545D68)]
void test_unoccupied_hot_gun_drives_its_overheat_emitter() {
    auto wp = std::make_unique<World>();
    test_titles::jo_world(*wp);
    World &w = *wp;
    w.registry.configure_pool(0, 4);
    w.registry.configure_pool(1, 4);
    w.tables.weapons.entries.resize(3);
    configure_heat_gun(w.tables.weapons.entries[1]);
    configure_heat_gun(w.tables.weapons.entries[2]);
    WeaponTableEntry &turret_def = w.tables.weapons.entries[2];
    turret_def.name = "WPN_T80_DshKTURRET";
    turret_def.third_person_model_asset = std::make_shared<const opennova::threedi::Threedi3di3>();
    turret_def.launch_userpoint = 3;
    turret_def.action_fsm.actions[weapon_action::kOverheated].particle_userpoint_3p = 4;
    HeatPoints points;
    w.pose_provider = &points;
    Entity seed;
    seed.kind = EntityKind::Item;
    seed.has_item_def = true;
    seed.item_type_index = 2;
    seed.item_attrib = kItemAttribEweap;
    seed.position = {0.0f, 0.0f, 10.0f};
    seed.primary_weapon_slot_adm = 1;
    seed.heat_userpoint_byte = 5;
    seed.weapon_userpoint_bytes[0][0] = 2; // the barrel-0 fire origin: the gate
    const EntityHandle gun = w.registry.spawn(1, seed);
    points.gun = gun;
    Entity &g = *w.registry.get(gun);
    // The frame's tick is 10: 1000 ticks of window are 42000 of heat.
    g.primary_weapon_slot.heat_window_end_tick = 10 + 1000;
    w.logic_tick = 11;
    w.pump_weapon_actions();
    const WeaponFsmDef &fsm = w.tables.weapons.entries[1].action_fsm;
    const int32_t fraction = weapon_heat_glow_fraction(fsm, 42000);
    CHECK(fraction == 18464);
    CHECK(g.primary_weapon_slot.heat_glow.live);
    CHECK(g.primary_weapon_slot.heat_glow.fraction == fraction);
    CHECK(g.primary_weapon_slot.heat_glow.tag == gun.packed);
    CHECK(points.index == 5 && points.model == nullptr);
    CHECK(g.primary_weapon_slot.heat_glow.pos[0] == (5 << 16));
    CHECK(g.primary_weapon_slot.heat_glow.dir[1] == (1 << 16));
    std::vector<WeaponHeatGlowRow> rows;
    fill_weapon_heat_glow_rows(w, rows);
    CHECK(rows.size() == 1);
    if (rows.size() == 1) {
        CHECK(rows[0].handle_packed == gun.packed && rows[0].slot == 0);
        CHECK(rows[0].effect == "FX_OVERHEAT1");
        CHECK(rows[0].pos.x == 5.0f && rows[0].pos.y == 2.0f && rows[0].pos.z == 3.0f);
        CHECK(std::fabs(rows[0].control - float(18464 * 0.000015259022)) < 1e-6f);
    }
    // Hotter on the next tick: re-aimed and re-intensified.
    g.primary_weapon_slot.heat_window_end_tick = 11 + 1500;
    w.logic_tick = 12;
    w.pump_weapon_actions();
    CHECK(g.primary_weapon_slot.heat_glow.fraction == weapon_heat_glow_fraction(fsm, 63000));
    // A zero gate byte takes no anchor: the live emitter keeps its last state.
    g.weapon_userpoint_bytes[0][0] = 0;
    const int32_t held = g.primary_weapon_slot.heat_glow.fraction;
    g.primary_weapon_slot.heat_window_end_tick = 12 + 1000;
    w.logic_tick = 13;
    w.pump_weapon_actions();
    CHECK(g.primary_weapon_slot.heat_glow.live && g.primary_weapon_slot.heat_glow.fraction == held);
    // Under the threshold: released, and no row.
    g.primary_weapon_slot.heat_window_end_tick = 13 + 500;
    w.logic_tick = 14;
    w.pump_weapon_actions();
    CHECK(!g.primary_weapon_slot.heat_glow.live);
    fill_weapon_heat_glow_rows(w, rows);
    CHECK(rows.empty());
    // A gfx3 def: OVERHEATED's gfx3 point on the gfx3 model, behind the launch byte.
    g.primary_weapon_slot_adm = 2;
    g.primary_weapon_slot.heat_window_end_tick = 14 + 1000;
    w.logic_tick = 15;
    w.pump_weapon_actions();
    CHECK(g.primary_weapon_slot.heat_glow.live);
    CHECK(points.index == 4 && points.model == turret_def.third_person_model_asset.get());
    g.primary_weapon_slot.heat_glow = {};
    turret_def.launch_userpoint = 0;
    g.primary_weapon_slot.heat_window_end_tick = 15 + 1000;
    w.logic_tick = 16;
    w.pump_weapon_actions();
    CHECK(!g.primary_weapon_slot.heat_glow.live);
}

// The anchor by the walked body: a body riding the gun anchors the gun's heat point
// unless the def has gfx3 (the camera-view leg, not ported) or the body is on foot; the
// local player in first person anchors it only with no gfx1 and from a UseGun seat.
// [orig: WeaponAction_ProcessFrame @0x541113..0x5411B9 (the local first-person branch
//  @0x541113..0x541164, the other-body branch @0x54118A..0x5411B9, the carrier leg
//  @0x54119E..0x5411B2)]
void test_overheat_emitter_anchor_by_walked_body() {
    auto wp = std::make_unique<World>();
    test_titles::jo_world(*wp);
    World &w = *wp;
    w.registry.configure_pool(1, 4);
    w.tables.weapons.entries.resize(2);
    WeaponTableEntry &def = w.tables.weapons.entries[1];
    configure_heat_gun(def);
    HeatPoints points;
    w.pose_provider = &points;
    Entity seed;
    seed.kind = EntityKind::Item;
    seed.has_item_def = true;
    seed.item_attrib = kItemAttribEweap;
    seed.heat_userpoint_byte = 6;
    seed.weapon_userpoint_bytes[0][0] = 2;
    const EntityHandle gun = w.registry.spawn(1, seed);
    points.gun = gun;
    const Entity &g = *w.registry.get(gun);
    WeaponFsmEvents hot;
    hot.heat_glow_fraction = 1000;
    const auto anchored = [&](const HeatGlowWalk &walk) {
        WeaponSlotState slot;
        points.index = 0;
        weapon_heat_glow_update(w, walk, def, slot, hot);
        return slot.heat_glow.live && points.index == 6 && slot.heat_glow.tag == gun.packed;
    };
    HeatGlowWalk body;
    body.carrier = &g;
    CHECK(anchored(body));   // a remote or AI gunner
    HeatGlowWalk on_foot;
    CHECK(!anchored(on_foot));
    HeatGlowWalk local = body;
    local.local = true;
    local.first_person = true;
    local.usegun_seat = true;
    CHECK(anchored(local));  // no gfx1: the UseGun seat's carrier point
    local.usegun_seat = false;
    CHECK(!anchored(local)); // another seat: no anchor
    local.usegun_seat = true;
    def.has_first_person_model_reference = true;
    CHECK(!anchored(local)); // gfx1: the first-person camera leg
    local.first_person = false;
    CHECK(anchored(local));  // the local third person takes the carrier point
    def.third_person_model_asset = std::make_shared<const opennova::threedi::Threedi3di3>();
    def.launch_userpoint = 3;
    CHECK(!anchored(body));  // gfx3: the body's camera-view leg
    // A release drops a live emitter whatever the walk.
    WeaponSlotState slot;
    slot.heat_glow.live = true;
    WeaponFsmEvents release;
    release.heat_glow_release = true;
    weapon_heat_glow_update(w, on_foot, def, slot, release);
    CHECK(!slot.heat_glow.live);
}

// DFX2-26: dfx2.exe's IsTurret traverse holds the step's planar length to the rate, both axes
// scaled by rate / length and truncated, where Jointops.exe's clamps each axis. A 30 deg / 20 deg
// step from rest: Jointops.exe's words 0x93 / 0x93 (each axis at 0x92CF34), dfx2.exe's 122 / 81
// at 0x92CF34 and 49 / 33 at the solo rate 0x3AB948.
// [orig: Entity_UpdateChildAttachment @0x4407A0 (dfx2.exe) - @0x440949..0x4409AB]
void test_dfx2_planar_rate() {
    const auto words = [](int32_t rate) {
        EmplacedGunChannel channel;
        EmplacedGunnerLook look{bam_sub(kGunHeading, 30 * kBamPerDegree), -20 * kBamPerDegree, 0,
                /*is_local_player=*/false, /*is_player=*/true};
        advance_emplaced_gun_channel(channel, look, kGunHeading, 0, /*is_turret=*/true, rate);
        return std::pair<int, int>{channel.yaw, channel.pitch};
    };
    CHECK((words(0) == std::pair<int, int>{0x93, 0x93}));
    CHECK((words(kEmplacedTurretSlewPerTick) == std::pair<int, int>{122, 81}));
    CHECK((words(opennova::world::title_rules("dfx2").emplaced_solo_planar_slew) ==
            std::pair<int, int>{49, 33}));
}

// The rate's pick: none for Jointops.exe; dfx2.exe's slower one for the local gunner of a gun
// whose carrier counts one occupied seat at most (any rider dead, def-less or model-less makes
// the count 0), the full one otherwise and for a gun with no carrier.
// [orig: Entity_UpdateChildAttachment @0x4407A0 (dfx2.exe) - @0x440875..0x4408A7;
//  Entity_CountOccupiedSeats @0x434E70 (dfx2.exe)]
void test_dfx2_rate_pick_and_seat_count() {
    auto wp = std::make_unique<World>();
    test_titles::jo_world(*wp);
    World &w = *wp;
    w.registry.configure_pool(1, 4);
    Entity carrier_seed;
    carrier_seed.kind = EntityKind::Item;
    carrier_seed.has_item_def = true;
    carrier_seed.has_graphic_model = true;
    carrier_seed.graphic_model_userpoints = 4;
    Seat driver;
    driver.type = SeatType::Driver;
    Seat passenger;
    passenger.type = SeatType::Passenger;
    carrier_seed.seats = {driver, passenger};
    const EntityHandle carrier_h = w.registry.spawn(1, carrier_seed);
    Entity gun_seed;
    gun_seed.kind = EntityKind::Item;
    gun_seed.has_item_def = true;
    gun_seed.emplacement_parent = carrier_h;
    const EntityHandle gun_h = w.registry.spawn(1, gun_seed);
    Entity &carrier = *w.registry.get(carrier_h);
    Entity &gun = *w.registry.get(gun_h);
    w.rules.title_rules = &opennova::world::title_rules("jo");
    CHECK(emplaced_turret_planar_rate(w, gun, true) == 0); // Jointops.exe's
    w.rules.title_rules = &opennova::world::title_rules("dfx2");
    const int32_t solo = w.rules.title().emplaced_solo_planar_slew;
    CHECK(emplaced_carrier_occupied_seats(w, carrier) == 0);
    CHECK(emplaced_turret_planar_rate(w, gun, true) == solo);
    CHECK(emplaced_turret_planar_rate(w, gun, false) == kEmplacedTurretSlewPerTick);
    carrier.seats[0].occupant = EntityHandle::make(0, 1);
    CHECK(emplaced_carrier_occupied_seats(w, carrier) == 1);
    CHECK(emplaced_turret_planar_rate(w, gun, true) == solo);
    carrier.seats[1].occupant = EntityHandle::make(0, 2);
    CHECK(emplaced_carrier_occupied_seats(w, carrier) == 2);
    CHECK(emplaced_turret_planar_rate(w, gun, true) == kEmplacedTurretSlewPerTick);
    carrier.graphic_model_userpoints = 0; // a model with no userpoint counts 0
    CHECK(emplaced_carrier_occupied_seats(w, carrier) == 0);
    CHECK(emplaced_turret_planar_rate(w, gun, true) == solo);
    gun.emplacement_parent = EntityHandle{};
    CHECK(emplaced_turret_planar_rate(w, gun, true) == kEmplacedTurretSlewPerTick);
}

// DFX2-25: an IsTurret child's words turn against its frame's turn: a 1/256 turn of the heading
// adds 0x100 to the yaw word and leaves the pitch word; no turn leaves both.
// [orig: Entity_UpdateTransformAndTurret @0x440B20 (dfx2.exe) - @0x440FA1..0x440FFE]
void test_dfx2_counter_rotation() {
    EmplacedGunChannel channel{0x0123, 0x0040};
    const int32_t before[3] = {kGunHeading, 0, 0};
    const int32_t after[3] = {bam_add(kGunHeading, 0x01000000), 0, 0};
    counter_rotate_emplaced_gun_channel(channel, before, after);
    CHECK(channel.yaw == 0x0223);
    CHECK(channel.pitch == 0x0040);
    counter_rotate_emplaced_gun_channel(channel, after, after);
    CHECK(channel.yaw == 0x0223 && channel.pitch == 0x0040);
}

// The counter-rotation's gate: dfx2.exe's, an IsTurret child resolved on a carrier userpoint with
// an occupant, not over a dead PlayerControl hull.
// [orig: Entity_UpdateTransformAndTurret @0x440B20 (dfx2.exe) - @0x440B44..0x440B61,
//  @0x440DAE..0x440DDB, @0x440F87, @0x440F94]
void test_dfx2_counter_rotation_gate() {
    auto wp = std::make_unique<World>();
    test_titles::jo_world(*wp);
    World &w = *wp;
    w.registry.configure_pool(1, 4);
    Entity carrier_seed;
    carrier_seed.kind = EntityKind::Item;
    carrier_seed.has_item_def = true;
    carrier_seed.has_graphic_model = true;
    carrier_seed.graphic_model_userpoints = 4;
    const EntityHandle carrier_h = w.registry.spawn(1, carrier_seed);
    Entity gun_seed;
    gun_seed.kind = EntityKind::Item;
    gun_seed.has_item_def = true;
    gun_seed.item_attrib2 = opennova::def::DEF_ITEM_ATTRIB2_ISTURRET;
    gun_seed.emplacement_parent = carrier_h;
    gun_seed.emplacement_bone = 2;
    gun_seed.emplacement_pose_metadata_resolved = true;
    gun_seed.primary_occupant = EntityHandle::make(0, 1);
    gun_seed.emplaced_update = true;
    gun_seed.primary_weapon_slot_adm = 0;
    w.tables.weapons.entries.resize(1);
    w.tables.weapons.entries[0].valid = true;
    const EntityHandle gun_h = w.registry.spawn(1, gun_seed);
    Entity &gun = *w.registry.get(gun_h);
    w.rules.title_rules = &opennova::world::title_rules("jo");
    CHECK(!emplaced_counter_rotates(w, gun)); // Jointops.exe's
    w.rules.title_rules = &opennova::world::title_rules("dfx2");
    CHECK(emplaced_counter_rotates(w, gun));
    gun.emplaced_update = false; // the ewep class update's alone
    CHECK(!emplaced_counter_rotates(w, gun));
    gun.emplaced_update = true;
    gun.emplacement_bone = 5; // past the carrier's userpoints: the root copy
    CHECK(!emplaced_counter_rotates(w, gun));
    gun.emplacement_bone = 2;
    gun.primary_occupant = EntityHandle{};
    CHECK(!emplaced_counter_rotates(w, gun) && emplaced_counter_rotates_frame(w, gun));
}

// DFX2-25 in the channel: the child posed ahead of its gunner's channel slews against its frame
// before the pose (savedLivePose), publishes the slewed words to its carrier's brain, then turns
// by the pose's turn and takes its window. A gunner on the old aim leaves the words 0 by the slew;
// the GROUND brain takes that 0, and the 1/256 turn adds 0x100 to the stored word. Jointops.exe's
// slews against the same frame and keeps its words 0: it has no counter-rotation.
// [orig: Entity_UpdateChildAttachment @0x4407A0 then Entity_UpdateTransformAndTurret @0x440B20
//  (dfx2.exe) - the slew's +0x10 @0x44084E, the publication @0x440E01, the counter-rotation
//  @0x440FF1 / @0x440FFE, the window @0x4410EA]
void test_dfx2_channel_publishes_then_counter_rotates() {
    for (const bool dfx2 : {false, true}) {
        ParentRig pr(/*profile_type=*/2, /*anchor_subobject=*/0, /*parent_attrib=*/0);
        Rig &r = pr.r;
        r.w.rules.title_rules = &opennova::world::title_rules(dfx2 ? "dfx2" : "jo");
        Entity &parent = *r.w.registry.get(pr.parent_h);
        parent.has_item_def = true;
        parent.has_graphic_model = true;
        parent.graphic_model_userpoints = 4;
        Entity &gun = r.gun();
        gun.item_attrib2 = opennova::def::DEF_ITEM_ATTRIB2_ISTURRET;
        gun.emplacement_pose_metadata_resolved = true;
        gun.primary_occupant = r.gunner_h;
        gun.saved_live_yaw = kGunHeading; // the frame before this tick's pose
        gun.saved_live_valid = true;
        gun.heading = bam_add(kGunHeading, 0x01000000); // the posed frame
        r.look(kGunHeading, 0);
        r.tick_channel();
        const int32_t brain_yaw = pr.parent_ai->brain.f[AiBrain::kActiveYaw];
        if (dfx2) {
            CHECK(r.gun().emplaced_gun_yaw_word == 0x100);
            CHECK(brain_yaw == 0); // the slewed word, ahead of the counter-rotation
        } else {
            CHECK(r.gun().emplaced_gun_yaw_word == 0);
            CHECK(brain_yaw == 0);
        }
        CHECK(r.gun().emplaced_gun_pitch_word == 0);
    }
}

// The slew reads the frame ahead of this tick's compose, both titles: the gun's ai-fn runs in its
// pool-1 visit ahead of its class update. A child anchored on a carrier userpoint, its gunner
// holding the gun's aim while the carrier turns 1/256 of a turn: the IsTurret words stay 0 on the
// turning tick (the frame before the turn), and take one rate step (0x93) the tick after. A slew
// against the composed frame took the step a tick early (0x93, then 0x100).
// [orig: Entity_UpdatePool1Slot @0x4B8DD0 - @0x4B8E3C ahead of @0x4B8E53;
//  Entity_UpdateChildAttachment @0x4409A0 - @0x440A51 / @0x440A58]
void test_slew_reads_the_frame_before_the_compose() {
    Rig r(/*local=*/true, /*player_bit=*/true, opennova::def::DEF_ITEM_ATTRIB2_ISTURRET);
    Entity carrier;
    carrier.kind = EntityKind::Item;
    carrier.health = 100;
    carrier.heading = kGunHeading;
    const EntityHandle parent = r.w.registry.spawn(1, carrier);
    r.gun().emplacement_parent = parent;
    r.gun().emplacement_parent_spawn_id = r.w.registry.get(parent)->registry_spawn_id;
    r.gun().emplacement_pose_metadata_resolved = true;
    r.gun().emplacement_bone = 1;
    make_composed_ewep(r.w, r.gun());
    r.look(kGunHeading, 0);
    r.w.run_logic_tick(true);
    CHECK(r.gun().emplaced_gun_yaw_word == 0);
    const int32_t turned = bam_add(kGunHeading, 0x01000000);
    r.w.registry.get(parent)->heading = turned;
    r.w.run_logic_tick(true);
    CHECK(emplaced_gun_frame_heading(r.gun()) == turned); // composed on the turned carrier
    CHECK(r.gun().saved_live_yaw == kGunHeading);
    CHECK(r.gun().emplaced_gun_yaw_word == 0);
    r.w.run_logic_tick(true);
    CHECK(r.gun().emplaced_gun_yaw_word == static_cast<int16_t>(0x93));
    CHECK(r.body->heading == kGunHeading);

    // The immediate path reads the same frame: (frame - look) >> 16 on each axis.
    Rig i(/*local=*/false, /*player_bit=*/true);
    Entity hull;
    hull.kind = EntityKind::Item;
    hull.health = 100;
    const EntityHandle hull_h = i.w.registry.spawn(1, hull);
    Entity &gun = i.gun();
    gun.emplacement_parent = hull_h;
    gun.emplacement_bone = 1;
    gun.emplacement_pose_metadata_resolved = true;
    make_composed_ewep(i.w, gun);
    gun.saved_live_yaw = kGunHeading;
    gun.saved_live_pitch = 0x00400000;
    gun.saved_live_valid = true;
    gun.heading = bam_add(kGunHeading, 0x01000000);
    gun.pitch_bam = 0;
    i.look(bam_sub(kGunHeading, 0x01000000), 0);
    i.tick_channel();
    CHECK(i.gun().emplaced_gun_yaw_word == 0x0100);
    CHECK(i.gun().emplaced_gun_pitch_word == 0x0040);
    // A child no compose stamped (here unresolved) reads the frame it stands in.
    i.gun().emplacement_pose_metadata_resolved = false;
    i.tick_channel();
    CHECK(i.gun().emplaced_gun_yaw_word == 0x0200);
    CHECK(i.gun().emplaced_gun_pitch_word == 0);
    // So does a child whose class update stops ahead of the compose (no slot Def).
    i.gun().emplacement_pose_metadata_resolved = true;
    i.gun().emplaced_gun_yaw_word = 0x0100;
    i.gun().emplaced_gun_pitch_word = 0x0040;
    i.gun().primary_weapon_slot_adm = kAdmSlotNone;
    i.tick_channel();
    CHECK(i.gun().emplaced_gun_yaw_word == 0x0200);
    CHECK(i.gun().emplaced_gun_pitch_word == 0);
}

// A gunner on an attached child keeps the seat frame of its last update through the
// projectiles and the proximity build, and its own body update in the pool-0 walk poses it
// on the child's new frame: no rider pass runs between the walks (D-SIM-56).
// [orig: Entity_UpdateAllEntities @0x4C2100 - the pool-1 walk @0x4C2158..0x4C21E9,
//  Weapon_UpdateAllProjectiles @0x4C223A, the proximity build @0x4C240A, the pool-0
//  +0x1C4 call @0x4C2472]
void test_gunner_takes_the_child_frame_in_its_own_update() {
    Rig r(/*local=*/true, /*player_bit=*/true);
    Entity carrier;
    carrier.kind = EntityKind::Item;
    carrier.health = 100;
    carrier.heading = kGunHeading;
    const EntityHandle parent = r.w.registry.spawn(1, carrier);
    r.gun().emplacement_parent = parent;
    r.gun().emplacement_parent_spawn_id = r.w.registry.get(parent)->registry_spawn_id;
    r.gun().emplacement_pose_metadata_resolved = true;
    r.gun().emplacement_bone = 1;
    make_composed_ewep(r.w, r.gun());
    r.look(kGunHeading, 0);
    r.w.run_logic_tick(true);
    const EntityPosition before = r.gunner().position;
    // The carrier moves; the child's compose in its own pool-1 visit follows it.
    r.w.registry.get(parent)->position.x.q16 += 8 * 65536;
    EntityPosition at_build;
    bool seen = false;
    r.w.before_proximity_build = [&] {
        at_build = r.gunner().position;
        seen = true;
    };
    r.w.run_logic_tick(true);
    r.w.before_proximity_build = nullptr;
    CHECK(seen);
    // Between the walks the gunner still stands where its last update put it.
    CHECK(at_build.x.q16 == before.x.q16 && at_build.y.q16 == before.y.q16 &&
            at_build.z.q16 == before.z.q16);
    // Past the pool-0 walk it stands on the child's final frame.
    Seat seat;
    CHECK(r.w.vehicles.rider_seat(r.gunner(), r.gun(), seat));
    Entity probe = r.gunner();
    r.w.vehicles.pose_mounted_occupant(probe, r.gun(), seat);
    CHECK(r.gunner().position.x.q16 != before.x.q16);
    CHECK(r.gunner().position.x.q16 == probe.position.x.q16 &&
            r.gunner().position.y.q16 == probe.position.y.q16 &&
            r.gunner().position.z.q16 == probe.position.z.q16);
}

// Only the ewep class update composes an attached child, past its gates: a
// child whose move function is not ewep, one with no slot Def and one whose
// carrier is a dead PlayerControl hull all hold their frames on a moved
// carrier (the hull's child hidden); the ewep child with its slot Def rides.
// [orig: Entity_UpdatePool1Slot @0x4B8E53 (the row's own +0x1C4);
//  Entity_UpdateTransformAndTurret @0x440CA0 - the hide and return
//  @0x440CD7..0x440CE1, the slot Def exit @0x440E8E..0x440EA0, the root copy
//  @0x4410EA..0x4411BC]
void test_only_the_ewep_update_composes_a_child() {
    auto wp = std::make_unique<World>();
    test_titles::jo_world(*wp);
    World &w = *wp;
    w.registry.configure_pool(1, 8);
    Entity carrier_seed;
    carrier_seed.kind = EntityKind::Item;
    carrier_seed.health = 100;
    carrier_seed.alive = true;
    carrier_seed.position = {8.0f, 12.0f, 4.0f};
    const EntityHandle carrier = w.registry.spawn(1, carrier_seed);
    Entity hull_seed = carrier_seed;
    hull_seed.has_item_def = true;
    hull_seed.item_attrib = kItemAttribPlayerControl;
    hull_seed.flags = kEntityFlagDead;
    hull_seed.alive = false;
    const EntityHandle hull = w.registry.spawn(1, hull_seed);
    const auto child_on = [&](EntityHandle parent, bool ewep, bool slot_def) {
        Entity seed;
        seed.kind = EntityKind::Item;
        seed.item_type_index = 2; // a resolved def row: the walk visits it
        seed.health = 100;
        seed.alive = true;
        seed.emplacement_parent = parent;
        seed.emplacement_parent_spawn_id = w.registry.get(parent)->registry_spawn_id;
        seed.ground_target = parent;
        seed.emplacement_pose_metadata_resolved = true;
        const EntityHandle h = w.registry.spawn(1, seed);
        Entity &child = *w.registry.get(h);
        make_composed_ewep(w, child);
        child.emplaced_update = ewep;
        if (!slot_def) child.primary_weapon_slot_adm = kAdmSlotNone;
        return h;
    };
    const EntityHandle ridden = child_on(carrier, true, true);
    const EntityHandle other_class = child_on(carrier, false, true);
    const EntityHandle no_def = child_on(carrier, true, false);
    const EntityHandle on_wreck = child_on(hull, true, true);
    w.run_logic_tick();
    const auto at = [&](EntityHandle h, float x, float y, float z) {
        const Entity *e = w.registry.get(h);
        return e != nullptr && e->position.x == x && e->position.y == y && e->position.z == z;
    };
    CHECK(at(ridden, 8.0f, 12.0f, 4.0f));
    CHECK(at(other_class, 0.0f, 0.0f, 0.0f));
    CHECK(at(no_def, 0.0f, 0.0f, 0.0f));
    CHECK(at(on_wreck, 0.0f, 0.0f, 0.0f));
    CHECK((w.registry.get(on_wreck)->flags & kEntityFlagCarried) != 0);
    CHECK((w.registry.get(on_wreck)->engine_flags & kEntityFlagMatrixBuilt) == 0);
}

// The joiner's remote-gunner leg over a gun its world composes: the slew reads the row's frame
// (its world row's before this tick's compose, which the pool-1 walk mirrored), the window and
// its occupant write-back the world row's composed frame. An immediate gun 1/32 of a turn behind
// its composed frame, a +-45 degree window: the gunner held past it is pinned at 45 degrees off
// the composed frame; a row the world does not compose keeps its own frame for both.
// [orig: Entity_UpdateChildAttachment @0x4409A0 - the immediate path @0x440B39..0x440B58;
//  Entity_UpdateTransformAndTurret @0x440CA0 - Math_FixedPointMatrixToEulerAngles @0x4410DD
//  ahead of the window leg @0x4411D1..0x4412B3]
void test_replica_gun_window_reads_the_composed_frame() {
    using opennova::replication::ClientEntityState;
    for (const bool composed : {false, true}) {
        auto wp = std::make_unique<World>();
        test_titles::jo_world(*wp);
        World &w = *wp;
        w.registry.configure_pool(1, 4);
        Entity twin_seed;
        twin_seed.kind = EntityKind::Item;
        twin_seed.item_id = 123;
        // The twin's slot Def, its class init's bind: the fallback window reads it.
        w.tables.weapons.entries.resize(1);
        w.tables.weapons.entries[0].valid = true;
        twin_seed.primary_weapon_slot_adm = 0;
        const EntityHandle twin_h = w.registry.spawn(1, twin_seed);
        const int32_t row_frame = kGunHeading;
        const int32_t composed_frame = bam_add(kGunHeading, 0x08000000);
        Entity &twin = *w.registry.get(twin_h);
        twin.veh.yaw_seeded = true;
        twin.veh.yaw_bam = composed_frame;
        std::vector<opennova::mission::ItemSeatSpec> specs(1);
        specs[0].type_id = 123;
        Seat seat;
        seat.type = SeatType::Gunner;
        seat.bone_index = 1;
        specs[0].seats.push_back(seat);
        specs[0].turret_limits_valid = true;
        specs[0].turret_yaw_range_bam = 0x20000000;
        specs[0].turret_pitch_max_bam = 0x40000000;
        specs[0].turret_pitch_min_bam = 0x40000000;
        opennova::replication::ClientState state;
        ClientEntityState &mount = state.upsert(twin_h.packed);
        mount.type_id = 123;
        mount.cls = opennova::EntityClass::NoNetworkCallback;
        mount.heading_bam = row_frame;
        if (composed) mount.world_class_update = ClientEntityState::WorldClassUpdate::Ewep;
        ClientEntityState &gunner = state.upsert(2);
        gunner.cls = opennova::EntityClass::Player;
        gunner.carrier_handle = twin_h.packed;
        gunner.mount_bone = 1;
        // Within the window: the word is the row's frame less the look.
        gunner.heading_bam = bam_sub(row_frame, 0x01000000);
        opennova::inmatch::tick_replica_emplaced_channels(state, specs, w, 0xFFFF, nullptr,
                opennova::inmatch::ReplicaGunLeg::Pool1Walk);
        CHECK(state.find(twin_h.packed)->emplaced_gun_yaw_word == 0x0100);
        CHECK(state.find(2)->heading_bam == bam_sub(row_frame, 0x01000000));
        // Past it: pinned, and the look written back off the frame the window read.
        state.find(2)->heading_bam = bam_sub(row_frame, 0x30000000);
        opennova::inmatch::tick_replica_emplaced_channels(state, specs, w, 0xFFFF, nullptr,
                opennova::inmatch::ReplicaGunLeg::Pool1Walk);
        const int16_t pinned = state.find(twin_h.packed)->emplaced_gun_yaw_word;
        CHECK(pinned == 0x2000);
        CHECK(state.find(2)->heading_bam ==
                bam_sub(composed ? composed_frame : row_frame, emplaced_word_bam(pinned)));
    }
}

// The joiner's fallback window reads the gun's slot Def, as the World's window does: a twin no
// class init bound (a gun neither ewep nor an EWeap aircraft) takes no window from the weapon its
// def names, whose limits its spec carries, so a gunner past the +-45 degree yaw range keeps its
// look, as on the host; with the Def bound the same look pins at the window.
// [orig: Entity_GetWeaponTurretLimits @0x540D70 - the fallback off the slot's def
//  @0x540E35..0x540E58; WeaponSlot_InitFromEntityDef @0x5466C0, called only by
//  Entity_InitBoneReferences @0x441482 and Entity_InitAircraftWeaponBones @0x49017B]
void test_replica_fallback_window_reads_the_twins_slot_def() {
    using opennova::replication::ClientEntityState;
    for (const bool bound : {false, true}) {
        auto wp = std::make_unique<World>();
        test_titles::jo_world(*wp);
        World &w = *wp;
        w.registry.configure_pool(1, 4);
        w.tables.weapons.entries.resize(1);
        w.tables.weapons.entries[0].valid = true;
        Entity twin_seed;
        twin_seed.kind = EntityKind::Item;
        twin_seed.item_id = 123;
        if (bound) twin_seed.primary_weapon_slot_adm = 0;
        const EntityHandle twin_h = w.registry.spawn(1, twin_seed);
        std::vector<opennova::mission::ItemSeatSpec> specs(1);
        specs[0].type_id = 123;
        Seat seat;
        seat.type = SeatType::Gunner;
        seat.bone_index = 1;
        specs[0].seats.push_back(seat);
        specs[0].turret_limits_valid = true;
        specs[0].turret_yaw_range_bam = 0x20000000;
        specs[0].turret_pitch_max_bam = 0x40000000;
        specs[0].turret_pitch_min_bam = 0x40000000;
        opennova::replication::ClientState state;
        ClientEntityState &mount = state.upsert(twin_h.packed);
        mount.type_id = 123;
        mount.cls = opennova::EntityClass::NoNetworkCallback;
        mount.heading_bam = kGunHeading;
        ClientEntityState &gunner = state.upsert(2);
        gunner.cls = opennova::EntityClass::Player;
        gunner.carrier_handle = twin_h.packed;
        gunner.mount_bone = 1;
        gunner.heading_bam = bam_sub(kGunHeading, 0x30000000);
        opennova::inmatch::tick_replica_emplaced_channels(state, specs, w, 0xFFFF, nullptr,
                opennova::inmatch::ReplicaGunLeg::Pool1Walk);
        CHECK(state.find(twin_h.packed)->emplaced_gun_yaw_word == (bound ? 0x2000 : 0x3000));
        CHECK(state.find(2)->heading_bam ==
                bam_sub(kGunHeading, bound ? 0x20000000 : 0x30000000));
    }
}

} // namespace

int main() {
    test_dfx2_planar_rate();
    test_dfx2_rate_pick_and_seat_count();
    test_dfx2_counter_rotation();
    test_dfx2_counter_rotation_gate();
    test_dfx2_channel_publishes_then_counter_rotates();
    test_slew_reads_the_frame_before_the_compose();
    test_replica_gun_window_reads_the_composed_frame();
    test_replica_fallback_window_reads_the_twins_slot_def();
    test_weapon_window_zero_yawrange_locks_traverse();
    test_unoccupied_gun_publishes_held_words_spin_and_heat();
    test_parent_publication_runs_unoccupied_every_tick();
    test_class_update_vehicle_block_and_dead_hull_exit();
    test_carrier_death_and_respawn_keep_the_child();
    test_freed_carrier_row_keeps_the_child();
    test_carrier_destruction_resets_child_words();
    test_vehicle_death_releases_or_destroys_the_eweap_group();
    test_barrel_spin_once_before_weapon_pump();
    test_trail_off_plays_at_the_composed_position();
    test_only_the_ewep_update_composes_a_child();
    test_gunner_takes_the_child_frame_in_its_own_update();
    test_barrel_spin_tail_and_class_gate();
    test_attached_turret_slews_once_per_world_tick();
    test_immediate_path_words_and_recoil_term();
    test_window_clamp_writes_occupant_look();
    test_isturret_slews_per_tick();
    test_isturret_local_gunner_yaw_tether();
    test_isturret_npc_gunner_yaw_tether();
    test_isturret_npc_gunner_tether_then_look();
    test_isturret_remote_player_untethered();
    test_isturret_recoil_after_rate_clamp();
    test_isturret_window_pins_after_slew();
    test_parent_publication_ground_raw_yaw();
    test_parent_publication_gates();
    test_parent_publication_helo_clamped_pair();
    test_unoccupied_hot_gun_drives_its_overheat_emitter();
    test_overheat_emitter_anchor_by_walked_body();
    if (failures == 0) std::puts("emplaced_gun_channel: OK");
    return failures == 0 ? 0 : 1;
}
