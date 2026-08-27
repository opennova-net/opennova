// The fire-sound legs on the logic clock (world/fire_sound.h): the witnessed
// distance gate + delay formula, the pending-slot pool semantics, the arm
// split's sound routing, and the listener/local-player gates — all through
// RoundSim::spawn and the World drain, the public seam.
#include <cstdio>
#include <cstring>
#include <vector>

#include <runtime/world/fire_sound.h>
#include <runtime/world/round_sim.h>
#include <runtime/world/weapon_fsm.h>
#include <runtime/world/world.h>

using namespace opennova::world;

namespace {

int failures = 0;
#define CHECK(c)                                                               \
    do {                                                                       \
        if (!(c)) {                                                            \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);           \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

struct Rig {
    World world;
    EntityHandle shooter;

    Rig() {
        world.registry.configure_pool(0, 8);
        Entity s;
        s.kind = EntityKind::Organic;
        s.item_type = 3;
        s.bms_id = 41;
        s.position = {0.0f, 0.0f, 0.0f};
        s.health = 100;
        shooter = world.registry.spawn(0, s);

        world.ammo.entries.resize(1);
        AmmoTableEntry &ammo = world.ammo.entries[0];
        ammo.name = "BALL";
        ammo.valid = true;
        ammo.velocity = 620;
        ammo.max_age_ticks = 20;
        ammo.weight_in_grains = 875;
        ammo.ai_launch_set = "GS_AK47_2";

        // adm 1: fire row authors begin+end sets, recoil row an end set — the
        // JOX shape (gunshots ride soundsetend).
        world.weapons.entries.resize(2);
        WeaponTableEntry &def = world.weapons.entries[1];
        def.valid = true;
        def.name = "WPN_TEST";
        std::strcpy(def.action_fsm.actions[weapon_action::kFire].soundset,
                "GS_BEGIN");
        std::strcpy(def.action_fsm.actions[weapon_action::kFire].soundsetend,
                "GS_END");
        std::strcpy(def.action_fsm.actions[weapon_action::kRecoil].soundsetend,
                "GS_RECOIL");
    }

    int fire(const Vec3 &origin, uint8_t wire_flags = 0,
             bool with_owner = true,
             RoundConsequenceMode mode = RoundConsequenceMode::Authoritative) {
        RoundSpawnParams p;
        if (with_owner) {
            p.owner = shooter;
            p.shooter_handle = shooter.packed;
        }
        p.origin = origin;
        p.ammo_index = 0;
        p.adm_index = 1;
        p.wire_round_flags = wire_flags;
        return world.round_sim.spawn(world, p, mode);
    }

    std::vector<ReadyFireSound> drain() { return world.fire_sounds.drain(); }
};

// Without a stamped listener (a dedicated host) no sound leg runs at all
// [orig: the is_mp_session_peer gate @ 0x528e57].
void test_no_listener_no_sounds() {
    Rig r;
    CHECK(r.fire({10.0f, 0.0f, 0.9f}) >= 0);
    CHECK(r.drain().empty());
    CHECK(r.world.fire_sounds.pending_count() == 0);
}

// Under 30 integer units the ammo-arm sound plays this present, carrying the
// shooter's occlusion identity [orig: the else leg @ 0x528f07].
void test_near_fire_plays_immediately() {
    Rig r;
    r.world.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    CHECK(r.fire({10.0f, 0.0f, 0.0f}) >= 0);
    const std::vector<ReadyFireSound> out = r.drain();
    CHECK(out.size() == 1);
    if (out.size() == 1) {
        CHECK(out[0].set_name == "GS_AK47_2");
        CHECK(out[0].pos.x == 10.0f);
        CHECK(out[0].source_bms_id == 41);
    }
}

// At 330 units the witnessed delay is (62 * 330 / 330) >> 2 = 15 ticks, and
// the play fires at the RECORDED fire-time position [orig: @ 0x528ef2,
// @ 0x52937b].
void test_far_fire_counts_down_on_the_logic_clock() {
    Rig r;
    r.world.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    CHECK(r.fire({330.0f, 0.0f, 0.0f}) >= 0);
    CHECK(r.drain().empty());
    CHECK(r.world.fire_sounds.pending_count() == 1);
    for (int i = 0; i < 14; ++i) {
        r.world.fire_sounds.tick();
        CHECK(r.drain().empty());
    }
    r.world.fire_sounds.tick();
    const std::vector<ReadyFireSound> out = r.drain();
    CHECK(out.size() == 1);
    if (out.size() == 1) {
        CHECK(out[0].set_name == "GS_AK47_2");
        CHECK(out[0].pos.x == 330.0f);
        CHECK(out[0].source_bms_id == 41);
    }
    CHECK(r.world.fire_sounds.pending_count() == 0);
}

// The >= 30 gate is on the TRUNCATED integer distance: 29.9 plays now, 30
// queues one tick ((62 * 30 / 330) >> 2 = 1) [orig: @ 0x528ed4].
void test_distance_gate_truncates_to_units() {
    Rig r;
    r.world.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    CHECK(r.fire({29.9f, 0.0f, 0.0f}) >= 0);
    CHECK(r.drain().size() == 1);
    CHECK(r.fire({30.0f, 0.0f, 0.0f}) >= 0);
    CHECK(r.drain().empty());
    CHECK(r.world.fire_sounds.pending_count() == 1);
    r.world.fire_sounds.tick();
    CHECK(r.drain().size() == 1);
}

// The local player's own fire keeps its action-slot presentation — no seed.
void test_local_player_fire_is_filtered() {
    Rig r;
    r.world.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    r.world.cached.local_player = r.shooter;
    CHECK(r.fire({10.0f, 0.0f, 0.0f}) >= 0);
    CHECK(r.drain().empty());
    CHECK(r.world.fire_sounds.pending_count() == 0);
}

// The adm-indexed arm plays every authored action-row set (fire + recoil,
// begin + end) IMMEDIATELY at the shooter's position — never the fire origin,
// never delay-gated [orig: ActionSlot_ExecuteAction @ 0x4020ef + the one-shot
// end shim @ 0x401100, both at entity+4; rows @ 0x42f777/@ 0x42f785].
void test_adm_arm_plays_action_rows_at_the_shooter() {
    Rig r;
    // Listener 500 u away: an ammo-arm sound would queue; the action rows may not.
    r.world.fire_sounds.set_listener({500.0f, 0.0f, 0.0f});
    CHECK(r.fire({0.0f, 0.0f, 0.9f}, round_event_flag::kAdmIndexed) >= 0);
    const std::vector<ReadyFireSound> out = r.drain();
    CHECK(out.size() == 3);
    CHECK(r.world.fire_sounds.pending_count() == 0);
    if (out.size() == 3) {
        CHECK(out[0].set_name == "GS_BEGIN");
        CHECK(out[1].set_name == "GS_END");
        CHECK(out[2].set_name == "GS_RECOIL");
        for (const ReadyFireSound &s : out) {
            CHECK(s.pos.x == 0.0f);
            CHECK(s.pos.z == 0.0f); // the entity position, not the 0.9 eye origin
            CHECK(s.source_bms_id == 41);
        }
    }
}

// Bit 0 is tested first: set means the ammo-def arm even when bit 1 rides
// along [orig: @ 0x42f521 before @ 0x42f6ce].
void test_alt_fire_bit_selects_the_ammo_arm() {
    Rig r;
    r.world.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    CHECK(r.fire({10.0f, 0.0f, 0.0f},
                  round_event_flag::kAltFire | round_event_flag::kAdmIndexed) >= 0);
    const std::vector<ReadyFireSound> out = r.drain();
    CHECK(out.size() == 1);
    if (out.size() == 1) CHECK(out[0].set_name == "GS_AK47_2");
}

// An adm-arm round with no local shooter entity (a joiner's decoded remote
// fire) plays at the wire row's position when supplied, else at the origin.
void test_entityless_adm_arm_uses_the_supplied_shooter_pos() {
    Rig r;
    r.world.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    RoundSpawnParams p;
    p.origin = {5.0f, 0.0f, 1.7f};
    p.ammo_index = 0;
    p.adm_index = 1;
    p.wire_round_flags = round_event_flag::kAdmIndexed;
    p.shooter_pos = {5.0f, 0.0f, 0.2f};
    p.shooter_pos_valid = true;
    CHECK(r.world.round_sim.spawn(r.world, p,
                  RoundConsequenceMode::VisualOnly) >= 0);
    std::vector<ReadyFireSound> out = r.drain();
    CHECK(out.size() == 3);
    if (!out.empty()) CHECK(out[0].pos.z == 0.2f);

    p.shooter_pos_valid = false;
    CHECK(r.world.round_sim.spawn(r.world, p,
                  RoundConsequenceMode::VisualOnly) >= 0);
    out = r.drain();
    CHECK(out.size() == 3);
    if (!out.empty()) CHECK(out[0].pos.z == 1.7f);
}

// A full 128-slot pool drops further delayed sounds, exactly like the retail
// allocator's failed scan [orig: @ 0x527c47].
void test_full_pending_pool_drops() {
    Rig r;
    r.world.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    for (int i = 0; i < FireSoundQueue::kSlotCount + 5; ++i)
        r.world.fire_sounds.play_with_distance_delay(
                "GS_FAR", {330.0f, 0.0f, 0.0f}, 0);
    CHECK(r.world.fire_sounds.pending_count() == FireSoundQueue::kSlotCount);
    for (int i = 0; i < 15; ++i) r.world.fire_sounds.tick();
    CHECK(r.drain().size() == FireSoundQueue::kSlotCount);
}

// run_logic_tick advances the countdown once per tick from the World seam.
void test_world_tick_advances_the_countdown() {
    Rig r;
    r.world.fire_sounds.set_listener({0.0f, 0.0f, 0.0f});
    CHECK(r.fire({330.0f, 0.0f, 0.0f}) >= 0);
    for (int i = 0; i < 15; ++i) r.world.run_logic_tick();
    CHECK(r.drain().size() == 1);
}

} // namespace

int main() {
    test_no_listener_no_sounds();
    test_near_fire_plays_immediately();
    test_far_fire_counts_down_on_the_logic_clock();
    test_distance_gate_truncates_to_units();
    test_local_player_fire_is_filtered();
    test_adm_arm_plays_action_rows_at_the_shooter();
    test_alt_fire_bit_selects_the_ammo_arm();
    test_entityless_adm_arm_uses_the_supplied_shooter_pos();
    test_full_pending_pool_drops();
    test_world_tick_advances_the_countdown();
    if (failures == 0) std::printf("fire_sound_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
