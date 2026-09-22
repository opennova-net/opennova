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
    // Aircraft +0x474 is initialized from items.def primary_weapon, just
    // as an ewep's +0x2B4 is. Resolve it through the production channel tick.
    // [orig: Entity_InitInfantryBoneData @0x490173..0x49017b;
    // WeaponSlot_InitFromEntityDef @0x5466d8..0x54670a]
    auto &table = pr.r.w.tables.weapons;
    table.entries.resize(2);
    WeaponTableEntry &slot1 = table.entries[1];
    slot1.valid = true;
    slot1.name = "PARENT_GUN";
    slot1.turret_yaw_range_deg = 30;
    slot1.turret_pitch_max_deg = 20;
    slot1.turret_pitch_min_deg = 10;
    pr.r.w.registry.get(pr.parent_h)->primary_weapon = slot1.name;
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
        }
        const int32_t look = bam_sub(kGunHeading, 30 * kBamPerDegree);
        r.look(look, 0);
        r.w.add_system(&r.w.ai);
        r.w.run_logic_tick(true);
        CHECK(r.gun().emplaced_gun_yaw_word == 0x93);
        CHECK(r.body->heading == look);
        if (attached) {
            CHECK(r.body->pos[0] == to_fixed(8));
            CHECK(r.body->pos[1] == to_fixed(12));
            CHECK(r.body->pos[2] == to_fixed(4));
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

// The class tail consumes kick before the global weapon pump decays it again.
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
    r.w.add_system(&r.w.ai);
    r.w.run_logic_tick(true);
    CHECK(r.gun().primary_weapon_slot.kick == 0);
    CHECK(r.gun().emplaced_spin_ticks == 59);
    CHECK(r.gun().emplaced_spin_phase == 1888);
    EmplacedWeaponControls controls;
    CHECK(emplaced_weapon_controls_for(r.w, r.gun(), controls));
    CHECK(controls.spin == 1888);
    r.w.run_logic_tick(true);
    CHECK(r.gun().emplaced_spin_ticks == 58);
    CHECK(r.gun().emplaced_spin_phase == 3744);
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
    // The world tick's pool-1 walk runs that class update for the empty gun.
    pr.parent_ai->brain.f[AiBrain::kActiveYaw] = 0x12340000;
    pr.parent_ai->brain.f[AiBrain::kStagingBlock + 3] = 0x12340000;
    // A live parent generation keeps the child attached through the world
    // tick's orphan peel; the hull's own brain stays out of this tick.
    pr.r.gun().emplacement_parent_spawn_id =
            pr.r.w.registry.get(pr.parent_h)->registry_spawn_id;
    pr.r.w.registry.get(pr.parent_h)->spawn_phase = 1000;
    pr.r.w.add_system(&pr.r.w.ai);
    pr.r.w.run_logic_tick(true);
    CHECK(pr.r.w.registry.get(pr.r.gun_h) != nullptr);
    if (pr.r.w.registry.get(pr.r.gun_h) == nullptr) return;
    CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == yaw);
    // Without the gun's weapon Def the class update publishes nothing.
    pr.r.gun().primary_weapon_slot_adm = kAdmSlotNone;
    pr.parent_ai->brain.f[AiBrain::kActiveYaw] = 0x12340000;
    tick_emplaced_weapon_class_update(pr.r.w, pr.r.gun());
    CHECK(pr.parent_ai->brain.f[AiBrain::kActiveYaw] == 0x12340000);
}

// The destroyed carrier's refNum children return to rest: their held gun
// words are zeroed, and their ammo re-splits only through a resolved Def.
// [orig: Vehicle_CleanupTeamEntitiesOnDestruction @0x547040 (+0x324/+0x322 =
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

} // namespace

int main() {
    test_weapon_window_zero_yawrange_locks_traverse();
    test_unoccupied_gun_publishes_held_words_spin_and_heat();
    test_parent_publication_runs_unoccupied_every_tick();
    test_carrier_destruction_resets_child_words();
    test_barrel_spin_once_before_weapon_pump();
    test_barrel_spin_tail_and_class_gate();
    test_attached_turret_slews_once_per_world_tick();
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
