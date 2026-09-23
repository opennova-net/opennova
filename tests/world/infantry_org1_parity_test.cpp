// The org1 (NPC) infantry motor legs against vectors produced by EXECUTING the
// retail motor. [orig: Entity_UpdateInfantryAI @0x4B9910]
//
// The vertical vectors below come from running the retail tail
// Entity_UpdateInfantryAI 0x4BF5CB..0x4BFC89 under Unicorn on the hash-pinned
// Jointops.exe (sha256 b9971c82...02fac), with the resolver
// Entity_MovementCollisionResolver @0x4B2BD0 stubbed to return
// `z - capsule_bottom - ground`, the same clearance the motor's no-collision
// fallback computes over the flat test field used here. Everything else in that
// range (the slide damp, the integrate, gravity, the landing/airborne edges, the
// ladder and water blocks, and the entity+0xAC quarter-step tail) ran as retail
// code.
#include <runtime/audio/sound_profile.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/world.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <vector>

using namespace opennova::world;
using opennova::terrain::TerrainHeightField;

namespace {

int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

constexpr int32_t fx(double units) { return static_cast<int32_t>(units * 65536.0); }

// A flat 512x512 height field at world height 0.
struct FlatField {
    static constexpr int kDim = 512;
    std::vector<uint16_t> heightmap = std::vector<uint16_t>(kDim * kDim, 0);
    std::vector<int> sector_grid = std::vector<int>(256, 1);
    TerrainHeightField field;
    FlatField() {
        field.heightmap = heightmap.data();
        field.dim = kDim;
        field.layout.sector_grid = sector_grid.data();
    }
};

// Idle-only source whose frames stand the capsule bottom 1.0 u under the origin
// and carry no root translation.
struct IdleSource : IRootMotionSource {
    std::set<int> clips{anim_state::kIdle};
    uint32_t events = 0; // the trigger bits every frame carries
    bool has_clip(int, int id) const override { return clips.count(id) != 0; }
    int32_t clip_length_ticks(int, int, int) const override { return -1; }
    bool advance(int, int id, int32_t &phase, RootMotionFrame &out) override {
        if (clips.count(id) == 0) return false;
        ++phase;
        out = RootMotionFrame{};
        out.capsule_bottom = fx(1);
        out.events = events;
        return true;
    }
};

// One org1 NPC over the flat field, registry-backed so the water block and the
// flag word run, driven through the public AiSystem tick.
struct Org1Rig {
    FlatField ground;
    IdleSource source;
    std::unique_ptr<World> w = std::make_unique<World>();
    EntityHandle handle;

    Org1Rig() {
        w->registry.configure_pool(0, 4);
        Entity body;
        body.kind = EntityKind::Organic;
        body.health = 100;
        body.health_max = 100;
        body.eye_offset_z = 0x9000; // what the capsule restamp produces for these frames
        handle = w->registry.spawn(0, body);
        AiEntity &e = *w->ai.at(w->ai.attach(handle));
        e.inf.active = true;
        e.health = 100;
        e.pos[0] = fx(100);
        e.pos[1] = fx(100);
        w->ai.terrain = &ground.field;
        w->ai.root_motion = &source;
    }
    AiEntity &e() { return *w->ai.for_handle(handle); }
    Entity &entity() { return *w->registry.get(handle); }
    void airborne() {
        e().inf.airborne = true;
        entity().flags |= kEntityFlagInAir;
    }
    void tick(uint32_t t) {
        TickContext ctx;
        ctx.world = w.get();
        ctx.is_authority = true;
        ctx.logic_tick = t;
        w->logic_tick = t;
        w->update_all_entities(ctx);
    }
};

// R3-1: the org1 vertical quarter-step tail. An even key tick keeps a quarter
// of the Z change since the post-integrate save and stores it in +0xAC; the odd
// key tick skips gravity/resolve/edges and adds +0xAC again, so a falling NPC
// moves about vel_z per two ticks. Retail vectors: the body 8.0 u above its
// capsule floor, at rest, airborne. [orig: Entity_UpdateInfantryAI save
// @0x4BF6BA, odd skip @0x4BF6A5..0x4BF6B2, tail @0x4BFC65..0x4BFC86]
void test_org1_fall_takes_the_quarter_step_tail() {
    Org1Rig rig;
    rig.e().pos[2] = fx(1) + fx(8);
    rig.airborne();
    // retail z after ticks 2, 3, 4, 5 and the +0xAC each stored
    const int32_t z[] = {589616, 589408, 588992, 588576};
    const int32_t step[] = {-208, -208, -416, -416};
    const int32_t vel[] = {-416, -416, -832, -832};
    for (int i = 0; i < 4; ++i) {
        rig.tick(2u + static_cast<uint32_t>(i));
        CHECK(rig.e().pos[2] == z[i]);
        CHECK(rig.e().inf.z_quarter_step == step[i]);
        CHECK(rig.e().inf.vel[2] == vel[i]);
    }
    // After 60 ticks retail has fallen 2.95 u (the doubled integrate alone
    // would have dropped 5.9 u).
    for (uint32_t t = 6; t < 62; ++t) rig.tick(t);
    CHECK(rig.e().pos[2] == 396384);
    CHECK(rig.e().inf.vel[2] == -12480);
    CHECK(rig.e().inf.airborne);
}

// R3-1: the landing time and the fall damage follow the slower fall. At the
// default fallmps 13 a retail NPC dropping 4.0 u lands on tick 70 with 49 hp
// left; the doubled integrate landed it on tick 50 at -10400, under the
// -13741 threshold, unhurt. [orig: threshold `imul eax,0FFFFFBDFh`
// @0x4BF839, damage @0x4BF848..0x4BF864; landing re-save @0x4BF808]
void test_org1_fall_damage_follows_the_retail_fall() {
    Org1Rig rig;
    rig.w->script.wac_values.fallmps = 13;
    rig.e().pos[2] = fx(1) + fx(4);
    rig.airborne();
    uint32_t landed = 0;
    for (uint32_t t = 2; t < 400 && landed == 0; ++t) {
        rig.tick(t);
        if (!rig.e().inf.airborne) landed = t;
    }
    CHECK(landed == 70);
    CHECK(rig.e().health == 49);
    CHECK(rig.e().pos[2] == fx(1)); // the landing lands in full: +0xAC is 0 after it
    CHECK(rig.e().inf.z_quarter_step == 0);
    rig.tick(landed + 1);
    CHECK(rig.e().pos[2] == fx(1));
}

// R3-1 + R3-3: the org1 water block stores the float target and the shared tail
// settles it; a body that fell in keeps its stuck vel_z (gravity is skipped while
// afloat) without sinking under it. Retail vectors: water at 10.0 u, the body at
// 8.0 u with vel_z -8000 and the in-air bit, at (100, 100), eye height 0x9000.
// [orig: float target store @0x4BFB84; tail @0x4BFC65..0x4BFC86; odd skip of
// the water block @0x4BF6B2]
void test_org1_water_settles_through_the_tail() {
    Org1Rig rig;
    rig.w->env.water_z = fx(10);
    rig.e().pos[2] = fx(8);
    rig.e().inf.vel[2] = -8000;
    rig.airborne();
    struct Row { uint32_t tick; int32_t z; int32_t step; };
    const Row rows[] = {
            {2, 552421, 28133}, {3, 580554, 28133}, {4, 594631, 14077},
            {5, 608708, 14077}, {10, 631627, 1767}, {11, 633394, 1767},
            {23, 636740, 5},    {24, 636726, -14},  {41, 636002, -56},
    };
    size_t row = 0;
    for (uint32_t t = 2; t <= 41; ++t) {
        rig.tick(t);
        if (row < sizeof(rows) / sizeof(rows[0]) && rows[row].tick == t) {
            CHECK(rig.e().pos[2] == rows[row].z);
            CHECK(rig.e().inf.z_quarter_step == rows[row].step);
            ++row;
        }
    }
    CHECK(row == sizeof(rows) / sizeof(rows[0]));
    CHECK(rig.e().inf.vel[2] == -8416);
    CHECK((rig.entity().flags & kEntityFlagDrowning) != 0);
    CHECK(!rig.e().inf.airborne);
}

// R3-3: the org1 AI-climb chase (Flags 0x80) replaces gravity on EVEN key ticks
// only; an odd tick moves the body by the stored quarter step and leaves the
// chase velocity alone. [orig: climb @0x4BF6C1..0x4BF6EA inside the even-tick
// block; odd skip @0x4BF6A5..0x4BF6B2 -> @0x4BFC80]
void test_org1_climb_chase_runs_on_even_ticks_only() {
    Org1Rig rig;
    rig.e().pos[2] = fx(3);
    rig.e().inf.goal_z = fx(5); // the persisted +0x304 the chase reads @0x4BF6C7
    rig.entity().flags |= kEntityFlagAiClimb;
    rig.tick(2);
    // step = (5u - 3u + 8) >> 4 = 8192; Z += 2 * 8192, then the quarter tail
    CHECK(rig.e().inf.vel[2] == 8192);
    CHECK(rig.e().inf.z_quarter_step == (2 * 8192 + 2) >> 2);
    const int32_t z_even = rig.e().pos[2];
    CHECK(z_even == fx(3) + 4096);
    rig.tick(3);
    CHECK(rig.e().inf.vel[2] == 8192);           // no chase on the odd tick
    CHECK(rig.e().pos[2] == z_even + 4096);      // only the stored quarter step
}

// ---- the death edge (R3-2, R3-4, R3-5, R7-11) ----

constexpr char kDeathProfile[] = "begin \"SP_Org1\"\n     sounddeath     T_DEATH\nend\n";

int death_screams(const World &w) {
    int n = 0;
    for (const SoundSlotEvent &ev : w.out.slot_sounds)
        if (ev.slot == opennova::audio::kSlotDeath) ++n;
    return n;
}

void arm_death(Org1Rig &rig) {
    rig.w->tables.sound_profiles.parse(kDeathProfile, sizeof(kDeathProfile) - 1);
    rig.e().profile.sound_profile =
            static_cast<int16_t>(rig.w->tables.sound_profiles.index_of("SP_Org1"));
    rig.source.clips.insert({139, 174, 175, 184});
    rig.e().pos[2] = fx(1); // on its capsule floor
    rig.entity().health = 0;
    rig.entity().deathtime_ticks = 500;
}

// R3-2: the org1 edge fires once per life, keyed on the dead bit it latches.
// A corpse the medic drag re-poses as draggee (139, flags 0x002) must not take
// the edge again: one scream, the corpse timer keeps draining, the staged
// attacker survives. [orig: Entity_UpdateInfantryAI `cmp [esi+11Eh],bp; jg`
// @0x4B9C40, `test byte ptr [esi+24h],2; jnz` @0x4B9C4D; dead bit @0x4B9D18]
void test_org1_dragged_corpse_takes_one_death_edge() {
    Org1Rig rig;
    arm_death(rig);
    const EntityHandle shooter = EntityHandle::make(0, 3);
    rig.entity().last_attacker = shooter;
    rig.entity().death_anim_state = 184; // staged by the killing hit
    rig.tick(2);
    CHECK(death_screams(*rig.w) == 1);
    CHECK((rig.entity().flags & kEntityFlagDead) != 0);
    CHECK(rig.e().inf.anim_state == 184);
    CHECK(rig.entity().corpse_timer == 499);

    Entity medic;
    medic.kind = EntityKind::Organic;
    medic.item_id = 7;
    medic.health = 100;
    medic.alive = true;
    medic.position = {100.0f, 101.0f, 1.0f};
    const EntityHandle medic_h = rig.w->registry.spawn(0, medic);
    rig.entity().dragger = medic_h;
    rig.entity().dragger_spawn_id = rig.w->registry.get(medic_h)->registry_spawn_id;
    for (uint32_t t = 3; t < 13; ++t) rig.tick(t);
    CHECK(rig.e().inf.anim_state == 139);           // the draggee pose
    CHECK(death_screams(*rig.w) == 1);             // no second edge
    CHECK(rig.entity().corpse_timer == 489);        // drains, never re-seeded
    CHECK(rig.entity().last_attacker == shooter);   // never cleared again
}

// R3-4: the edge's own legs. [orig: Entity_UpdateInfantryAI drowning 175
// @0x4B9CF6..0x4B9D0E; unstaged hit callback @0x4B9CDB..0x4B9CF1 ->
// Entity_HandleDamageTrigger @0x4073C8..0x4073EA; death tick @0x4B9D24..0x4B9D2F;
// `and eax,0FFFFFF3Fh` @0x4B9D2A; Entity_CheckAndProcessDeath @0x4B9D4D]
void test_org1_death_edge_legs() {
    {   // an unstaged death afloat: death_drown, the hit callback's alert, the
        // death tick, the 0xC0 clear, and the authority's own transaction
        Org1Rig rig;
        arm_death(rig);
        rig.entity().group_id = 5;
        rig.entity().cause_flags = 0x900u;
        rig.entity().flags |= kEntityFlagDrowning | kEntityFlagAiClimb | kEntityFlagMounted;
        rig.tick(40);
        CHECK(rig.e().inf.anim_state == anim_state::kDeathDrown);
        CHECK(rig.e().slot.bytes()[AiSlot::kAlertByte] == 2);
        CHECK(rig.w->script.relations.group(5).alert == TriggerRelations::kAlertRed);
        CHECK(rig.entity().death_tick == 40);
        CHECK((rig.entity().flags & (kEntityFlagAiClimb | kEntityFlagMounted)) == 0);
        CHECK(!rig.entity().last_attacker.valid());
        CHECK(rig.w->round_sim.deaths.size() == 1);
        if (!rig.w->round_sim.deaths.empty()) {
            const RoundDeath &d = rig.w->round_sim.deaths.back();
            CHECK(d.motor_edge);
            CHECK(d.victim == rig.handle);
            CHECK(d.victim_handle == rig.handle.packed);
            CHECK(!d.killer.valid());
            CHECK(d.event_flags == 0x900u);
        }
        rig.tick(41);
        CHECK(rig.w->round_sim.deaths.size() == 1); // once per life
    }
    {   // a staged death: no alert, the shooter kept as the transaction's killer,
        // and no transaction off the authority
        Org1Rig rig;
        arm_death(rig);
        const EntityHandle shooter = EntityHandle::make(0, 3);
        rig.entity().last_attacker = shooter;
        rig.entity().death_anim_state = 184;
        TickContext ctx;
        ctx.world = rig.w.get();
        ctx.is_authority = false;
        ctx.logic_tick = 2;
        rig.w->update_all_entities(ctx);
        CHECK(rig.e().inf.anim_state == 184);
        CHECK(rig.e().slot.bytes()[AiSlot::kAlertByte] == 0);
        CHECK(rig.entity().last_attacker == shooter);
        CHECK(rig.w->round_sim.deaths.empty());
    }
}

// R3-5: a fatal landing credits the body itself and stages the generic
// selection, so the next edge keeps lastAttacker = self and raises the
// transaction with the body as its own killer. [orig: `mov [esi+178h],esi`
// @0x4BF86B, +0x2C0 @0x4BF879; the edge's staged path @0x4B9CCF]
void test_org1_fatal_fall_credits_itself() {
    Org1Rig rig;
    rig.w->script.wac_values.fallmps = 13;
    rig.entity().deathtime_ticks = 500; // keep the corpse row after the edge
    rig.e().pos[2] = fx(1) + fx(6);
    rig.airborne();
    uint32_t landed = 0;
    for (uint32_t t = 2; t < 400 && landed == 0; ++t) {
        rig.tick(t);
        if (!rig.e().inf.airborne) landed = t;
    }
    CHECK(landed != 0);
    CHECK(rig.entity().health <= 0);
    CHECK(rig.entity().last_attacker == rig.handle);
    CHECK(rig.entity().death_anim_state == anim_state::kDeathPungi);
    CHECK(rig.w->round_sim.deaths.empty()); // no transaction at the landing
    rig.tick(landed + 1);                   // the edge
    CHECK(rig.entity().last_attacker == rig.handle);
    CHECK(rig.w->round_sim.deaths.size() == 1);
    if (!rig.w->round_sim.deaths.empty()) {
        CHECK(rig.w->round_sim.deaths.back().motor_edge);
        CHECK(rig.w->round_sim.deaths.back().killer == rig.handle);
    }
}

// R7-11: a corpse stays dead on its dead bit when a script writes health back
// onto it: the corpse leg keeps draining its timer, the edge does not re-fire
// and the body is not alive. [orig: corpse leg `test al,2; jz`
// @0x4B9D55..0x4B9D5A; think gate `test byte ptr [esi+24h],2` @0x4BA98B]
void test_org1_corpse_stays_dead_on_a_health_write() {
    Org1Rig rig;
    arm_death(rig);
    rig.entity().death_anim_state = 184;
    rig.tick(2);
    CHECK(rig.entity().corpse_timer == 499);
    rig.entity().health = 50; // a script health write on the corpse
    for (uint32_t t = 3; t < 20; ++t) rig.tick(t);
    CHECK(rig.entity().corpse_timer == 499 - 17);
    CHECK(rig.e().inf.anim_state == 184);
    CHECK((rig.entity().flags & kEntityFlagDead) != 0);
    CHECK(!rig.entity().alive);
    CHECK(rig.w->round_sim.deaths.size() == 1); // the one edge transaction
}

// The player-body twins of the edge legs, local and wire-owned: a body dying
// afloat takes death_drown 175, and the edge stamps the death tick and drops
// Flags 0xC0; its death transaction stays the damage route's.
// [orig: Entity_UpdateInfantryPlayerBody @0x4B4C93..0x4B4CAB, @0x4B4CB5..0x4B4CDB]
void test_player_body_death_edge_legs() {
    Org1Rig local;
    local.e().inf.is_local_player = true;
    local.entity().flags |= kEntityFlagPlayer;
    arm_death(local);
    local.entity().death_anim_state = 184;
    local.entity().flags |= kEntityFlagDrowning | kEntityFlagMounted;
    local.tick(40);
    CHECK(local.e().inf.anim_state == anim_state::kDeathDrown);
    CHECK(local.entity().death_tick == 40);
    CHECK((local.entity().flags & kEntityFlagMounted) == 0);
    CHECK(local.w->round_sim.deaths.empty());

    Org1Rig remote;
    remote.e().net_is_remote_peer = true;
    remote.entity().flags |= kEntityFlagPlayer | kEntityFlagDrowning | kEntityFlagMounted;
    remote.source.clips.insert({175, 184});
    remote.entity().health = 0;
    remote.entity().death_anim_state = 184;
    remote.tick(40);
    CHECK(remote.e().inf.anim_state == anim_state::kDeathDrown);
    CHECK(remote.entity().death_tick == 40);
    CHECK((remote.entity().flags & kEntityFlagMounted) == 0);
}

// R3-23: an NPC rider's mounted leg scrubs Flags with 0xFF8F57DF (the chute,
// in-air, afloat, ladder and dive bits) and stops its vertical velocity every
// tick. [orig: Entity_UpdateInfantryAI @0x4BEC03..0x4BEC15]
void test_org1_rider_scrubs_its_fall_flags() {
    Org1Rig rig;
    rig.w->registry.configure_pool(1, 4);
    Entity hull;
    hull.has_item_def = true;
    hull.item_type = 1;
    hull.position = {100.0f, 100.0f, 0.0f};
    Seat seat;
    seat.type = SeatType::Passenger;
    hull.seats.push_back(seat);
    const EntityHandle hull_handle = rig.w->registry.spawn(1, hull);
    Entity &body = rig.entity();
    body.mounted = true;
    body.mount_target = hull_handle;
    body.mount_seat = 0;
    body.mount_type = SeatType::Passenger;
    const uint32_t scrubbed = 0x0070A820u; // ~0xFF8F57DF
    body.flags |= scrubbed;
    rig.e().inf.vel[2] = -5000;
    rig.e().inf.airborne = true;
    rig.tick(3);
    CHECK((rig.entity().flags & scrubbed) == 0);
    CHECK(rig.e().inf.vel[2] == 0);
    CHECK(!rig.e().inf.airborne);
    CHECK(rig.entity().mounted);
}

// R3-12: a mounted org1 rider takes its parent's deck ride too (the motor head
// points groundEntity at the parent), so its look and aim turn with the
// carrier before the seat pose; a turret parent (def+0x58 & 0x1000) keeps
// them. [orig: Entity_UpdateInfantryAI @0x4B9A0D..0x4B9A11, the ride
// @0x4BA45D..0x4BA891, the turret gate @0x4BA812..0x4BA82A]
void test_org1_rider_turns_with_its_carrier() {
    for (const bool turret : {false, true}) {
        Org1Rig rig;
        rig.w->registry.configure_pool(1, 4);
        Entity hull;
        hull.has_item_def = true;
        hull.item_type = 1;
        hull.position = {100.0f, 100.0f, 0.0f};
        hull.veh.yaw_seeded = true;
        hull.veh.yaw_bam = 0x01000000; // this tick's yaw; last tick's was 0
        hull.saved_live_valid = true;
        hull.saved_live_pos[0] = fx(100);
        hull.saved_live_pos[1] = fx(100);
        if (turret) hull.item_attrib2 = 0x1000u;
        Seat seat;
        seat.type = SeatType::Passenger;
        hull.seats.push_back(seat);
        const EntityHandle hull_handle = rig.w->registry.spawn(1, hull);
        Entity &body = rig.entity();
        body.mounted = true;
        body.mount_target = hull_handle;
        body.mount_seat = 0;
        body.mount_type = SeatType::Passenger;
        rig.tick(3);
        const int32_t turned = turret ? 0 : 0x01000000;
        CHECK(rig.e().inf.aim_heading == turned);
        CHECK(rig.e().heading == turned);
        CHECK(rig.entity().ground_target == hull_handle);
    }
}

// ---- the low-severity motor legs (R3-14, R3-15, R3-17, R3-18) ----

// R3-14: while the SP epilog screen is up (a lost round, from the tick after it
// ended) the NPC motor does nothing at all. [orig: Entity_UpdateInfantryAI
// @0x4B998C..0x4B99CD]
void test_org1_motor_holds_under_the_epilog() {
    Org1Rig rig;
    rig.e().pos[2] = fx(1) + fx(8);
    rig.airborne();
    rig.w->logic_tick = 2;
    rig.w->process_round_end(2);
    rig.tick(4); // an even key tick: gravity would run
    CHECK(rig.w->epilog_screen_active());
    CHECK(rig.e().pos[2] == fx(1) + fx(8));
    CHECK(rig.e().inf.vel[2] == 0);
}

// R3-15: a corpse afloat (or in the air, or on a ladder: Flags 0x10A000) takes
// the level decay, not the slope conform, unless its clip itself conforms (the
// death family's flag bit 2); here it holds the idle clip.
// [orig: Entity_UpdateInfantryAI anim test @0x4BA11E, dead leg
// `test eax,10A000h` @0x4BA12C]
void test_org1_afloat_corpse_decays() {
    Org1Rig rig;
    arm_death(rig);
    rig.entity().death_anim_state = 184;
    rig.tick(71);
    rig.e().inf.request_body_animation(anim_state::kIdle);
    rig.entity().flags |= kEntityFlagDrowning;
    rig.e().body_pitch = 0x01000000;
    rig.e().inf.aim_valid = true;
    rig.tick(72);
    CHECK(rig.e().body_pitch == 0x01000000 - ((0x01000000 + 8) >> 4));
    CHECK(rig.e().inf.aim_valid); // no conform pass, no dead slope aim
}

// R3-17: the org1 airborne edge stamps only a parachutist that is not carried:
// 47, else 31 when only that clip is authored, else nothing.
// [orig: Entity_UpdateInfantryAI @0x4BF8D4..0x4BF8F7]
void test_org1_airborne_edge_stamp() {
    {
        Org1Rig rig; // neither 47 nor 31 authored: the clip stays
        rig.e().pos[2] = fx(1) + fx(8);
        rig.entity().flags |= kEntityFlagParachute;
        rig.tick(2);
        CHECK(rig.e().inf.airborne);
        CHECK(rig.e().inf.anim_state == anim_state::kIdle);
    }
    {
        Org1Rig rig; // carried: no stamp
        rig.source.clips.insert(anim_state::kParachute);
        rig.e().pos[2] = fx(1) + fx(8);
        rig.entity().flags |= kEntityFlagParachute | kEntityFlagMounted;
        rig.tick(2);
        CHECK(rig.e().inf.anim_state == anim_state::kIdle);
    }
    {
        Org1Rig rig; // 31 only
        rig.source.clips.insert(anim_state::kJumpLoop);
        rig.e().pos[2] = fx(1) + fx(8);
        rig.entity().flags |= kEntityFlagParachute;
        rig.tick(2);
        CHECK(rig.e().inf.anim_state == anim_state::kJumpLoop);
    }
    {
        Org1Rig rig; // 47
        rig.source.clips.insert(anim_state::kParachute);
        rig.e().pos[2] = fx(1) + fx(8);
        rig.entity().flags |= kEntityFlagParachute;
        rig.tick(2);
        CHECK(rig.e().inf.anim_state == anim_state::kParachute);
    }
}

// R3-18: the org1 float target reads this tick's eye height (the motor's
// restamp, 0x9000 here), not last tick's registry mirror: the first row of the
// water vectors holds with a stale mirror. [orig: Entity_UpdateInfantryAI
// `mov ecx,[esi+74h]` @0x4BFB66]
void test_org1_float_reads_this_ticks_eye() {
    Org1Rig rig;
    rig.w->env.water_z = fx(10);
    rig.e().pos[2] = fx(8);
    rig.e().inf.vel[2] = -8000;
    rig.airborne();
    rig.entity().eye_offset_z = 0x20000; // stale
    rig.tick(2);
    CHECK(rig.e().pos[2] == 552421);
}

// R3-4 remainder: the org1 edge's unstaged hit callback reads the global hit
// record, so while it still holds a round the callback's round legs replace
// the generic clip with that round's (its section and approach quadrant)
// and, on a numbered section, cut the body mid-walk.
// [orig: Entity_UpdateInfantryAI @0x4B9CDB..0x4B9CF1; Entity_HandleDamageTrigger
// the round test @0x40740D, select @0x407483, the clone @0x40768A]
void test_org1_edge_reads_the_recorded_round() {
    CHECK(death_quadrant_from_round(bam_heading_from_mission_yaw_deg(90.0), 10.0f, 0.0f) == 2);
    for (const int32_t section : {0, 3}) {
        Org1Rig rig;
        arm_death(rig);
        rig.entity().yaw = 90; // engine heading 0: a +X round arrives from behind
        HitRecord &record = rig.w->round_sim.hit_record;
        record.has_round = true;
        record.round_vel_q16 = FixedVec3{10 << 16, 0, 0};
        record.round_ammo_index = -1; // no ammo row: no force leg
        record.section = section;
        rig.tick(40);
        CHECK(rig.e().inf.anim_state ==
              compute_death_anim_state(section, 2, death_cause::kBullet));
        CHECK(rig.w->round_sim.hits.size() == 1);
        int pieces = 0;
        rig.w->registry.for_each_in_pool(0, [&](const Entity &e) {
            if (e.dismemberment_piece) ++pieces;
        });
        CHECK(pieces == (section > 0 ? 1 : 0));
    }
}

// The plyr class callback's waypoint tail: a team 1/2 player whose AI slot
// carries a route channel marks every node inside the node's octagonal radius
// (the larger axis gap plus half the smaller) visited by its team and by its
// SSN, on the think event and on a script kill's event alike.
// [orig: Entity_HandleDamageAndTriggerZones @0x407B64..0x407C6B]
void test_player_waypoint_tail() {
    auto arm = [](Org1Rig &rig) {
        rig.e().inf.is_local_player = true;
        rig.entity().flags |= kEntityFlagPlayer;
        rig.entity().team = 1;
        rig.entity().net_id = 7;
        rig.e().slot.f[37] = 3;
        NavNodeTable &nav = rig.w->ai.nav;
        nav.channels.resize(4);
        nav.channels[3].count = 3;
        nav.channels[3].entries[0] = 0;
        nav.channels[3].entries[1] = 1;
        nav.channels[3].entries[2] = 2;
        nav.nodes.resize(3);
        // 0.75 u east, 0.5 u north of the body, radius 1 u: 0.75 + 0.25 is inside
        nav.nodes[0].f[0] = fx(1);
        nav.nodes[0].f[1] = fx(100.75);
        nav.nodes[0].f[2] = fx(100.5);
        // 0.8 u east, 0.5 u north: inside the square, outside the octagon
        nav.nodes[1].f[0] = fx(1);
        nav.nodes[1].f[1] = fx(100.8);
        nav.nodes[1].f[2] = fx(100.5);
        nav.nodes[2].f[0] = fx(1);
        nav.nodes[2].f[1] = fx(150);
        nav.nodes[2].f[2] = fx(100);
    };
    {
        Org1Rig rig; // the think event (spawn_phase starts at 0)
        arm(rig);
        rig.tick(2);
        const TriggerRelations &rel = rig.w->script.relations;
        CHECK(rel.single_visited(7, 3, 0) && rel.group_visited(1, 3, 0));
        CHECK(!rel.single_visited(7, 3, 1) && !rel.group_visited(1, 3, 1));
        CHECK(!rel.single_visited(7, 3, 2));
    }
    {
        Org1Rig rig; // the script kill's event
        arm(rig);
        rig.entity().item_id = 1;
        CHECK(rig.w->commands.kill_ssn(rig.handle));
        const TriggerRelations &rel = rig.w->script.relations;
        CHECK(rel.single_visited(7, 3, 0) && rel.group_visited(1, 3, 0));
        CHECK(!rel.single_visited(7, 3, 1));
    }
}

// ---- the org1 phase order (R3-8) ----

// R3-8: org1 chases its heading and look after the think and BEFORE its fire
// pass, so a round leaves along this tick's look, not last tick's.
// [orig: Entity_UpdateInfantryAI body chase @0x4BE8FD..0x4BE931 and look
// @0x4BEB18..0x4BEBE7, then the fire block @0x4BF15C..0x4BF4B0]
void test_org1_round_leaves_along_this_ticks_look() {
    Org1Rig rig;
    rig.w->tables.ammo.entries.resize(2);
    rig.w->tables.ammo.entries[1].valid = true;
    rig.w->tables.ammo.entries[1].velocity = 620;
    rig.w->tables.ammo.entries[1].max_age_ticks = 100;
    rig.e().pos[2] = fx(1); // on its capsule floor
    rig.e().profile.organic.ammo[1] = 1;
    rig.source.events = 0x8; // this odd tick's clip event latches the secondary fire
    rig.e().inf.target_heading = 0x10000000;
    rig.e().inf.aim_heading = 0x04000000; // the look chase then holds
    rig.tick(3);
    CHECK(rig.w->out.rounds.count == 1);
    if (rig.w->out.rounds.count == 1) {
        // (0x10000000 - 0 + 2) >> 2 = 0x04000000, inside the +-69273360 clamp
        CHECK(rig.w->out.rounds.records[0].dir_yaw == 0x04000000);
    }
    CHECK(rig.e().inf.body_heading == 0x04000000);
}

// ---- the corpse slope legs (R3-9) and the torso roll (R3-10) ----

// R3-9a: a dead org1 body in the air tumbles on its eight-tick phase. At key 72,
// a = (32 - 8) * 0xFFFFFF and b = (32 - (((72 >> 6) - 72) & 63)) * 0xFFFFFF =
// -25 * 0xFFFFFF. [orig: Entity_UpdateInfantryAI @0x4BA08D..0x4BA10A]
void test_org1_airborne_corpse_tumbles() {
    Org1Rig rig;
    arm_death(rig);
    rig.entity().death_anim_state = 184;
    rig.tick(71); // the edge latches the dead bit
    CHECK((rig.entity().flags & kEntityFlagDead) != 0);
    rig.e().pos[2] = fx(1) + fx(20);
    rig.airborne();
    rig.e().inf.target_heading = 0x1000;
    rig.e().inf.aim_valid = true;
    rig.e().body_pitch = 0;
    rig.e().roll = 0;
    rig.tick(72);
    const int32_t a = 24 * 0xFFFFFF;
    const int32_t b = -25 * 0xFFFFFF;
    CHECK(rig.e().inf.aim_pitch == a + b);
    CHECK(rig.e().inf.aim_heading == 0x1000);        // the target before the spin
    CHECK(rig.e().inf.target_heading == 0x1000 + (b >> 2));
    CHECK(rig.e().body_pitch == (a + 4) >> 3);
    CHECK(rig.e().roll == (b + 4) >> 3);
    CHECK(!rig.e().inf.aim_valid);
}

// R3-9b: a grounded org1 corpse aims along the slope on its conform pass: aim
// pitch = the pitch slope (level here), aim heading = the target heading, aim
// flag clear. [orig: Entity_UpdateInfantryAI @0x4BA301..0x4BA319]
void test_org1_corpse_aims_along_the_slope() {
    Org1Rig rig;
    arm_death(rig);
    rig.entity().death_anim_state = 184;
    rig.tick(71);
    rig.e().inf.target_heading = 0x2000;
    rig.e().inf.aim_heading = 0x777;
    rig.e().inf.aim_pitch = 0x123456;
    rig.e().inf.aim_valid = true;
    rig.tick(72);
    CHECK(rig.e().inf.aim_pitch == 0);
    CHECK(rig.e().inf.aim_heading == 0x2000);
    CHECK(!rig.e().inf.aim_valid);
}

// R3-10: org1 computes its own torso roll after the think: a sixteenth-step
// chase of the slope roll with the lag clamped to +-0x0E38E380, and a
// thirty-second-step decay in prone idle 48. [orig: Entity_UpdateInfantryAI
// @0x4BE897..0x4BE8EA]
void test_org1_torso_roll() {
    Org1Rig rig;
    rig.e().pos[2] = fx(1);
    rig.e().roll = 0x10000000;
    rig.tick(3); // off the slope phase: the roll holds
    // (0x10000000 + 8) >> 4 = 0x01000000 lags the roll by more than the clamp
    CHECK(rig.e().inf.torso_roll == 0x10000000 - 0x0E38E380);

    Org1Rig prone;
    prone.source.clips.insert(anim_state::kIdleProne);
    prone.e().pos[2] = fx(1);
    prone.e().inf.request_body_animation(anim_state::kIdleProne);
    prone.e().inf.torso_roll = 0x100000;
    prone.tick(5);
    CHECK(prone.e().inf.anim_state == anim_state::kIdleProne);
    CHECK(prone.e().inf.torso_roll == 0x100000 - ((0x100000 + 16) >> 5));
}

} // namespace

// T7: the secondary-fire latch is a frame local of the org1 motor. A latch left
// from before this pass is gone at the motor head, so an even tick whose clip
// carries no fire event fires nothing. [orig: Entity_UpdateInfantryAI
// `mov [esp+var_108C],ebp` @0x4B99B8; the latch test @0x4BF406]
void test_org1_fire_latch_is_a_pass_local() {
    Org1Rig rig;
    rig.w->tables.ammo.entries.resize(2);
    rig.w->tables.ammo.entries[1].valid = true;
    rig.w->tables.ammo.entries[1].velocity = 620;
    rig.w->tables.ammo.entries[1].max_age_ticks = 100;
    rig.e().pos[2] = fx(1);
    rig.e().profile.organic.ammo[1] = 1;
    rig.e().inf.fire_secondary_latch = true;
    rig.tick(2);
    CHECK(rig.w->out.rounds.count == 0);
    CHECK(!rig.e().inf.fire_secondary_latch);
}

// R4-18: on a ladder the combat tail drops the move and the selector tail still
// runs: +0x36A takes the dropped move mode, the flinch consumes wasHit and the
// zero-distance leg clears the path state. [orig: Entity_UpdateInfantryAI the
// ladder drop @0x4BD187..0x4BD194, the zero-distance leg @0x4BD2DE..0x4BD2E9,
// +0x36A @0x4BD356, wasHit @0x4BD6EE]
void test_org1_ladder_runs_the_selector_tail() {
    Org1Rig rig;
    rig.e().pos[2] = fx(1);
    rig.entity().flags |= kEntityFlagLadderContact;
    rig.e().inf.was_hit = true;
    rig.e().inf.prev_move_mode = 3;
    rig.e().inf.path_state = 1;
    rig.tick(16);
    CHECK(!rig.e().inf.was_hit);
    CHECK(rig.e().inf.prev_move_mode == 0);
    CHECK(rig.e().inf.path_state == 0);
}

// R4-1: an airborne org1 body without the 0x80 climb order leaves the think at
// its head: no selection (the flinch keeps wasHit, +0x36A keeps its move mode)
// and no attention (no ride link to the deck's occupant). With the climb order
// the same body thinks. [orig: Entity_UpdateInfantryAI @0x4BAA57..0x4BAA66 ->
// loc_4BE7FD; +0x36A @0x4BD356, wasHit @0x4BD6EE, the ride link
// @0x4BD87E..0x4BD905]
void test_org1_airborne_body_skips_the_think() {
    for (const bool climb : {false, true}) {
        Org1Rig rig;
        rig.w->registry.configure_pool(1, 4);
        Entity mate;
        mate.kind = EntityKind::Organic;
        mate.health = 100;
        const EntityHandle mate_handle = rig.w->registry.spawn(0, mate);
        Entity deck;
        deck.kind = EntityKind::Item;
        deck.health = 100;
        deck.primary_occupant = mate_handle;
        const EntityHandle deck_handle = rig.w->registry.spawn(1, deck);
        rig.entity().ground_target = deck_handle;
        rig.e().pos[2] = fx(1) + fx(8);
        rig.airborne();
        if (climb) rig.entity().flags |= kEntityFlagAiClimb;
        rig.e().inf.was_hit = true;
        rig.e().inf.prev_move_mode = 3;
        rig.tick(16);
        CHECK(rig.e().inf.was_hit == !climb);
        CHECK(rig.e().inf.prev_move_mode == (climb ? 0 : 3));
        CHECK((rig.entity().primary_occupant == mate_handle) == climb);
    }
}

int main() {
    test_org1_fall_takes_the_quarter_step_tail();
    test_org1_fall_damage_follows_the_retail_fall();
    test_org1_water_settles_through_the_tail();
    test_org1_climb_chase_runs_on_even_ticks_only();
    test_org1_dragged_corpse_takes_one_death_edge();
    test_org1_death_edge_legs();
    test_org1_fatal_fall_credits_itself();
    test_org1_corpse_stays_dead_on_a_health_write();
    test_player_body_death_edge_legs();
    test_org1_rider_scrubs_its_fall_flags();
    test_org1_rider_turns_with_its_carrier();
    test_org1_motor_holds_under_the_epilog();
    test_org1_afloat_corpse_decays();
    test_org1_airborne_edge_stamp();
    test_org1_float_reads_this_ticks_eye();
    test_player_waypoint_tail();
    test_org1_edge_reads_the_recorded_round();
    test_org1_round_leaves_along_this_ticks_look();
    test_org1_fire_latch_is_a_pass_local();
    test_org1_airborne_body_skips_the_think();
    test_org1_ladder_runs_the_selector_tail();
    test_org1_airborne_corpse_tumbles();
    test_org1_corpse_aims_along_the_slope();
    test_org1_torso_roll();
    if (failures) {
        std::printf("infantry_org1_parity: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("infantry_org1_parity: all passed\n");
    return 0;
}
