// The host's remote player body (D-NET-NEW-LP-2, the host side): the C2S 0x0C
// read-apply stages a smooth target and writes the live position only for a hidden
// body, and the body's own update on the authority chases that target: a gap over
// 2 units dropped, a 0x2AAA deadband, a bucket of 10 with no vertical step and none
// at all past a vertical speed of 1670, the steps an idiv (truncation toward zero),
// the chase restarted by every uplink.
// [orig: NetPacket_SerializePlayerState @0x4C09C0 - the gates @0x4C2000..0x4C2028,
//  the staging @0x4C2042..0x4C205E, the hidden-body position stores
//  @0x4C207E..0x4C208E, progress @0x4C20A9; Entity_UpdateInfantryPlayerBody
//  @0x4B40E0 - the chase @0x4B4299..0x4B4445, @0x4B4459..0x4B4612, the 512 cap
//  @0x4B4431..0x4B4435 / @0x4B464F..0x4B465F]
// Every value below is derived from that arithmetic; core-only (no retail data).

#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include "channel_clip_source.h"

#include <cstdint>
#include <cstdio>
#include <memory>

using namespace opennova::world;
namespace rep = opennova::replication;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

test_world::ChannelClipSource channel_only;

// A remote player's body on the authority: a player-classified organic at the
// origin, its motor attached, holding its channel and playing nothing (no root
// motion), so the chase is the only thing that moves it on a terrain-less rig.
struct Rig {
    std::unique_ptr<World> world = std::make_unique<World>();
    EntityHandle body;
    uint32_t tick = 1;

    Rig() {
        world->registry.configure_pool(0, 4);
        Entity seed;
        seed.kind = EntityKind::Organic;
        seed.health = 100;
        seed.alive = true;
        seed.flags |= kEntityFlagPlayer;
        body = world->registry.spawn(0, seed);
        world->ai.root_motion = &channel_only;
        world->ai.is_authority = true;
        AiEntity *e = world->ai.at(world->ai.attach(body));
        e->inf.active = true;
        e->health = 100;
    }
    Entity &ent() { return *world->registry.get(body); }
    AiEntity &ai() { return *world->ai.for_handle(body); }
    bool uplink(int32_t x, int32_t y, int32_t z, int16_t heading = 0, int16_t pitch = 0,
                bool slot_in_game = true) {
        rep::PlayerIntent intent;
        intent.entity_handle = body.packed;
        intent.pos_x = x;
        intent.pos_y = y;
        intent.pos_z = z;
        intent.heading = heading;
        intent.pitch = pitch;
        return rep::apply_player_intent(*world, intent, slot_in_game);
    }
    void step(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) world->ai.update_organic(ai(), *world, tick++);
    }
};

// One uplink 0.5 units off along x and z: the chase steps x by
// ((10 >> 1) + 0x8000) / 10 = 3277 for ten ticks (progress 0..9), z not at all,
// and stops; the live position was never written by the read-apply.
void test_a_single_uplink_is_chased_in_ten_bucket_steps() {
    Rig r;
    CHECK(r.uplink(0x8000, 0, 0x8000));
    CHECK(r.ent().position.x.q16 == 0);           // a visible body: no snap
    CHECK(r.ai().net_smooth_target[0] == 0x8000); // the staged target
    CHECK(r.ai().net_interp_progress == 0);
    r.step();
    CHECK(r.ai().net_interp_steps == 10);
    CHECK(r.ai().net_smooth_target[0] == 3277);
    CHECK(r.ai().net_smooth_target[2] == 0);      // no vertical step on the authority
    CHECK(r.ent().position.x.q16 == 3277);
    r.step(9);
    CHECK(r.ent().position.x.q16 == 32770);
    CHECK(r.ent().position.z.q16 == 0);
    r.step(5);
    CHECK(r.ent().position.x.q16 == 32770);       // progress 10.. >= the bucket
    CHECK(r.ai().net_interp_progress == 15);
}

// An uplink every frame restarts the chase (progress 0 per 0x0C): each frame takes
// one step against a fresh gap, 3277, 2949, 2654, 2389, 2150, 1935, 1741, 1567,
// 1411, 1270, 1143, until the gap (10282) falls inside the 0x2AAA deadband.
void test_a_per_frame_uplink_steps_against_a_fresh_gap_into_the_deadband() {
    Rig r;
    const int32_t expected[] = {3277, 6226, 8880, 11269, 13419, 15354, 17095, 18662,
                                20073, 21343, 22486};
    for (int32_t want : expected) {
        CHECK(r.uplink(0x8000, 0, 0));
        r.step();
        CHECK(r.ent().position.x.q16 == want);
    }
    CHECK(0x8000 - r.ent().position.x.q16 == 10282); // < 0x2AAA
    for (int i = 0; i < 4; ++i) {
        CHECK(r.uplink(0x8000, 0, 0));
        r.step();
    }
    CHECK(r.ent().position.x.q16 == 22486);
}

// A copy 0.07 units off its uplink (4587 < 0x2AAA) never moves: the deadband.
void test_a_gap_inside_the_deadband_moves_nothing() {
    Rig r;
    r.ent().position.x.q16 = -0x11EB;
    for (int i = 0; i < 20; ++i) {
        CHECK(r.uplink(0, 0, 0));
        r.step();
    }
    CHECK(r.ent().position.x.q16 == -0x11EB);
    CHECK(r.ai().net_interp_steps == 0);
}

// A gap over 2 units is dropped on the authority: no snap, the target, bucket and
// angle steps cleared.
void test_a_gap_over_two_units_is_dropped_without_a_snap() {
    Rig r;
    CHECK(r.uplink(0x20001 + 0x10000, 0, 0));
    r.step();
    CHECK(r.ent().position.x.q16 == 0);
    CHECK(r.ai().net_smooth_target[0] == 0);
    CHECK(r.ai().net_interp_steps == 0);
    r.step(3);
    CHECK(r.ent().position.x.q16 == 0);
    // Exactly 2 units is not over: it is chased.
    Rig s;
    CHECK(s.uplink(0x20000, 0, 0));
    s.step();
    CHECK(s.ent().position.x.q16 == (5 + 0x20000) / 10);
}

// A negative delta steps by idiv: (5 - 0x8000) / 10 = -3276 (floor would be -3277).
void test_a_negative_gap_steps_by_truncation_toward_zero() {
    Rig r;
    CHECK(r.uplink(-0x8000, 0, 0));
    r.step();
    CHECK(r.ai().net_smooth_target[0] == -3276);
    CHECK(r.ent().position.x.q16 == -3276);
}

// While the body's vertical speed (+0xA0) exceeds 1670 the authority takes no
// horizontal step either; at exactly 1670 it does.
void test_a_fast_vertical_body_takes_no_horizontal_step() {
    Rig r;
    r.ai().inf.vel[2] = 1671;
    CHECK(r.uplink(0x8000, 0, 0));
    player_body_authority_chase(r.ai(), r.ent());
    CHECK(r.ai().net_smooth_target[0] == 0);
    CHECK(r.ent().position.x.q16 == 0);
    Rig s;
    s.ai().inf.vel[2] = -1670;
    CHECK(s.uplink(0x8000, 0, 0));
    player_body_authority_chase(s.ai(), s.ent());
    CHECK(s.ai().net_smooth_target[0] == 3277);
}

// The heading: the read-apply stores the live yaw with the target, so the chase's
// angle step is (target + 1 - yaw) / 2 = 0; a yaw moved since (the leg phase's
// clamp, a script) is pulled half the way back on each of the two divisor ticks.
void test_the_angle_steps_run_for_the_divisor_ticks() {
    Rig r;
    CHECK(r.uplink(0, 0, 0, 0x2000));
    CHECK(r.ai().heading == 0x20000000);
    r.ai().heading = 0x20000000 - 0x1000;
    player_body_authority_chase(r.ai(), r.ent());
    CHECK(r.ai().net_smooth_heading == (0x1000 + 1) / 2);
    CHECK(r.ai().heading == 0x20000000 - 0x1000 + 0x800);
    player_body_authority_chase(r.ai(), r.ent());
    CHECK(r.ai().heading == 0x20000000);
    player_body_authority_chase(r.ai(), r.ent());
    CHECK(r.ai().heading == 0x20000000); // progress 2: no more angle steps
}

// Held at progress 512 without an uplink, a movement clip falls back to idle 43.
void test_progress_caps_at_512_and_a_movement_clip_falls_back_to_idle() {
    Rig r;
    r.ai().net_interp_progress = 511;
    r.ai().inf.store_body_animation(anim_state::kWalkForward);
    player_body_authority_chase(r.ai(), r.ent());
    CHECK(r.ai().net_interp_progress == 512);
    CHECK(r.ai().inf.anim_state == anim_state::kWalkForward);
    player_body_authority_chase(r.ai(), r.ent());
    CHECK(r.ai().net_interp_progress == 512);
    CHECK(r.ai().inf.anim_state == anim_state::kIdle);
}

// A hidden body (Flags bit 0) takes the uplinked position at the read-apply, and
// its update returns at the head, so it never chases.
void test_a_hidden_body_snaps_at_the_read_apply() {
    Rig r;
    r.ent().flags |= kEntityFlagCarried;
    CHECK(r.uplink(0x50000, 0x30000, 0x8000));
    CHECK(r.ent().position.x.q16 == 0x50000);
    CHECK(r.ent().position.y.q16 == 0x30000);
    CHECK(r.ent().position.z.q16 == 0x8000);
    r.step(3);
    CHECK(r.ent().position.x.q16 == 0x50000);
    CHECK(r.ai().net_interp_progress == 0);
}

// The gates: the movement-input byte, the state-flag bits 2-4 and the analog axes
// land whatever the state; a dead body, a finished round, a slot not in game and
// the pre-round countdown each refuse the pose (no staging, no progress reset).
void test_the_read_apply_gates_hold_the_pose_not_the_inputs() {
    {
        Rig r;
        r.ent().flags |= kEntityFlagDead;
        rep::PlayerIntent intent;
        intent.entity_handle = r.body.packed;
        intent.pos_x = 0x8000;
        intent.move_input = 0x09;
        intent.state_flags = 0x10;
        intent.analog_x = -3;
        r.ai().net_interp_progress = 7;
        CHECK(!rep::apply_player_intent(*r.world, intent, true));
        CHECK(r.ent().net_move_input == 0x09);
        CHECK((r.ent().flags & 0x1Cu) == 0x10u);
        CHECK(r.ent().net_analog_x == -3);
        CHECK(r.ai().net_smooth_target[0] == 0);
        CHECK(r.ai().net_interp_progress == 7);
    }
    {
        Rig r;
        r.ai().net_interp_progress = 7;
        CHECK(!r.uplink(0x8000, 0, 0, 0, 0, /*slot_in_game=*/false));
        CHECK(r.ai().net_interp_progress == 7);
    }
    {
        Rig r;
        r.world->preround_delay_seconds = 3;
        CHECK(!r.uplink(0x8000, 0, 0));
        CHECK(r.ai().net_smooth_target[0] == 0);
    }
    {
        Rig r;
        r.world->match.latch_round_over();
        CHECK(!r.uplink(0x8000, 0, 0));
        CHECK(r.ai().net_smooth_target[0] == 0);
    }
}

} // namespace

int main() {
    test_a_single_uplink_is_chased_in_ten_bucket_steps();
    test_a_per_frame_uplink_steps_against_a_fresh_gap_into_the_deadband();
    test_a_gap_inside_the_deadband_moves_nothing();
    test_a_gap_over_two_units_is_dropped_without_a_snap();
    test_a_negative_gap_steps_by_truncation_toward_zero();
    test_a_fast_vertical_body_takes_no_horizontal_step();
    test_the_angle_steps_run_for_the_divisor_ticks();
    test_progress_caps_at_512_and_a_movement_clip_falls_back_to_idle();
    test_a_hidden_body_snaps_at_the_read_apply();
    test_the_read_apply_gates_hold_the_pose_not_the_inputs();
    if (failures == 0) std::printf("player_body_chase: all passed\n");
    return failures == 0 ? 0 : 1;
}
