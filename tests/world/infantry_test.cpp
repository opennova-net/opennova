// Infantry motor unit tests [orig: Entity_UpdateInfantryAI @0x4b9910] — the mechanics
// the promote end-to-end walk can't isolate, each against the constants witnessed in
// the binary (docs/world/world-wac-ai-re.md §3):
//   * body-heading quarter-step + the ±69273360/tick clamp,
//   * turn-in-place gates (>45 deg -> stop 147, 30..45 deg -> walk 1, overriding the gait),
//   * alerted-run via all three alert sources, wounded gaits at half health, and the
//     clip-availability fallback chains,
//   * final-node approach jog windows (dist/radius literals 139264/270336/73728),
//   * one-shot route lifecycle: arrival, movetimer hold + authored facing, end-of-path
//     stop (cooldown 20 re-arm) with exact resting position,
//   * state-commit rules straight off the real flag table (burn 111 = locked 0x004
//     queues; emote_1 115 = 0x020 yields only to movement-flagged targets),
//   * kJumpLoop forced forward delta 1024,
//   * per-tick gravity (org1 -416 + pos += 2*vel; org2 -208 + pos += vel) to terminal
//     -32768, landing snap + fall damage excess>>4 with the injectable scale
//     [orig: dword_C6EAE4], the player jump (cooldown 32 / no auto-repeat / prone gate),
//   * the slope pass: the conform selector (prone family / corpse / def attrib), the
//     org1 2048/8-tick slide + eighth-step body_pitch/roll chase, the org2 atan2
//     quarter-step leg, the non-conform decay — and the regression that a standing
//     local player's camera roll chain stays level on side slopes.
#include <cmath>
#include <memory>
#include <io/bam.h>
#include <cstdint>
#include <cstdio>
#include <array>
#include <map>
#include <set>
#include <vector>

#include "terrain/height_field.h"
#include "world/ai.h"
#include "world/world.h"

using namespace opennova::world;
using opennova::terrain::TerrainHeightField;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

constexpr int32_t fx(double units) { return static_cast<int32_t>(units * 65536.0); }

// Constants under test (mirrors of the cited values in infantry.cpp).
constexpr int32_t kClamp = 69273360;       // body turn clamp / tick
constexpr int32_t kTerminal = -32768;      // terminal fall velocity
constexpr int32_t kFloorStand = 0;         // infantry settles pos[2] to ground + the anim
                                           // frame's capsule_bottom; these tests use capsule 0,
                                           // so the floor is bare
                                           // ground (+0x50000 is the death-mover target, not the
                                           // on-foot floor — infantry.cpp / D-INF-6).

// A 512x512 height field with a caller-supplied raw16 column function (uniform in the
// second axis where not stated). Same wiring as tests/world/ground_height_test.cpp.
struct Field {
    static constexpr int kDim = 512;
    std::vector<uint16_t> heightmap;
    std::vector<int> sector_grid;
    TerrainHeightField field;

    template <typename Fn>
    explicit Field(Fn raw16_of_x) : heightmap(kDim * kDim), sector_grid(256, 1) {
        for (int z = 0; z < kDim; ++z)
            for (int x = 0; x < kDim; ++x)
                heightmap[z * kDim + x] = raw16_of_x(x);
        field.heightmap = heightmap.data();
        field.dim = kDim;
        field.layout.sector_grid = sector_grid.data();
        field.layout.origin_x = 0;
        field.layout.origin_y = 0;
    }
};

CollisionModel hurt_box_model() {
    CollisionModel model;
    auto plane = [&](int nx, int ny, int nz, double distance) {
        CollisionPlane p;
        p.nx = static_cast<int16_t>(nx);
        p.ny = static_cast<int16_t>(ny);
        p.nz = static_cast<int16_t>(nz);
        p.dist = fx(distance);
        model.planes.push_back(p);
    };
    plane(16384, 0, 0, -3.0);
    plane(-16384, 0, 0, -3.0);
    plane(0, 16384, 0, -3.0);
    plane(0, -16384, 0, -3.0);
    plane(0, 0, 16384, -3.0);
    plane(0, 0, -16384, 0.0);

    CollisionVolume hurt;
    hurt.type = 18;
    hurt.min_x = fx(-3.0);
    hurt.max_x = fx(3.0);
    hurt.min_y = fx(-3.0);
    hurt.max_y = fx(3.0);
    hurt.min_z = 0;
    hurt.max_z = fx(3.0);
    hurt.plane_count = 6;
    model.volumes.push_back(hurt);

    CollisionSection section;
    section.volume_count = 1;
    model.sections.push_back(section);
    return model;
}

// Test root source: a configurable clip set; gait clips move `step` forward per
// tick, everything else plays without root motion.
struct TestSource : IRootMotionSource {
    std::set<int> clips;
    int32_t step = 0x4000;
    int32_t capsule_bottom = 0; // absolute origin->feet the on-foot ground settle floors to

    static bool gait(int id) {
        return id == anim_state::kWalkForward || id == anim_state::kRunForward ||
               id == anim_state::kJogForward || id == anim_state::kWoundedWalk ||
               id == anim_state::kWoundedRun || id == anim_state::kWalkCrouchForward ||
               id == anim_state::kWalkProneForward;
    }
    bool has_clip(int /*adm_id*/, int id) const override { return clips.count(id) != 0; }
    // One-shot length per state when set: the weapon channel's clip-end promotion
    // [orig: AnimMap_UpdateEntity @0x40b77b] is exercised through this.
    std::map<int, int32_t> lengths;
    int32_t clip_length_ticks(int /*adm_id*/, int id) const override {
        auto it = lengths.find(id);
        return it == lengths.end() ? -1 : it->second;
    }
    bool advance(int /*adm_id*/, int id, int32_t &phase, RootMotionFrame &out) override {
        if (clips.count(id) == 0) return false;
        ++phase;
        out = RootMotionFrame{};
        if (gait(id)) out.dx = step;
        out.capsule_bottom = capsule_bottom;
        return true;
    }
};

struct BlendProbeSource : IRootMotionSource {
    std::map<int, RootMotionFrame> frames;

    bool has_clip(int, int id) const override { return frames.count(id) != 0; }
    int32_t clip_length_ticks(int, int) const override { return -1; }
    bool advance(int, int id, int32_t &phase, RootMotionFrame &out) override {
        auto it = frames.find(id);
        if (it == frames.end()) return false;
        ++phase;
        out = it->second;
        return true;
    }
};

AiEntity *soldier(AiSystem &ai) {
    int idx = ai.attach(EntityHandle::make(0, 0));
    AiEntity *e = ai.at(idx);
    e->inf.active = true;
    return e;
}

NavEntry node(int32_t x, int32_t y, int32_t radius, int32_t facing = 0, int32_t wait = 0) {
    NavEntry n{};
    n.f[0] = radius;
    n.f[1] = x;
    n.f[2] = y;
    n.f[3] = 0;
    n.f[4] = facing;
    n.wait_ticks = wait;
    return n;
}

void route(AiSystem &ai, AiEntity *e, const std::vector<NavEntry> &nodes, int loopflag) {
    ai.nav.channels.assign(2, NavChannel{});
    NavChannel &ch = ai.nav.channels[1];
    ch.loopflag = loopflag;
    ch.count = static_cast<int32_t>(nodes.size());
    ai.nav.nodes.clear();
    for (size_t i = 0; i < nodes.size(); ++i) {
        ch.entries[i] = static_cast<int32_t>(i);
        ai.nav.nodes.push_back(nodes[i]);
    }
    e->slot.f[35] = 1; // has-route [orig: slot+140]
    e->slot.f[37] = 1; // channel   [orig: slot+148]
    e->slot.f[38] = 0; // node      [orig: slot+152]
}

void run_ticks(AiSystem &ai, World &w, uint32_t from, uint32_t to_excl) {
    TickContext ctx;
    ctx.world = &w;
    ctx.is_authority = true;
    for (uint32_t t = from; t < to_excl; ++t) {
        ctx.logic_tick = t;
        ai.tick(w, ctx);
    }
}

// Run up to and INCLUDING the next 16-tick slow-pass boundary — the phase the original
// gates its weapon-channel selection on, so a test that wants a selection to happen must
// cross one. [orig: key `current_tick & 0xF` stored @0x4b4e79, tested @0x4b5d71]
uint32_t run_to_next_selection(AiSystem &ai, World &w, uint32_t from) {
    const uint32_t sel = ((from + 15u) / 16u) * 16u;
    run_ticks(ai, w, from, sel + 1u);
    return sel + 1u;
}

// Give a motor soldier the registry Entity and ADM table row the weapon channel reads its
// hold kind through. The original keeps no per-player hold-kind copy: it indexes AdmDefs
// by the posed entity's OWN equipped index every selection pass, which is exactly what
// lets a remote player's pose resolve from one replicated byte [orig: @0x4b5dba].
void give_held_weapon(World &w, AiEntity *e, uint8_t adm, int special_hold) {
    if (w.registry.get(e->handle) == nullptr) {
        w.registry.configure_pool(0, 4);
        Entity ent;
        ent.kind = EntityKind::Organic;
        w.registry.spawn(0, ent);
    }
    Entity *ent = w.registry.get(e->handle);
    ent->equipped_adm_index = adm;
    if (w.weapons.entries.size() <= adm) w.weapons.entries.resize(adm + 1u);
    w.weapons.entries[adm].valid = true;
    w.weapons.entries[adm].special_hold = special_hold;
}

void test_recoil_and_weapon_weight_kernels() {
    // Ammo recoil=1 in the standing slot produces 1<<18 before this body tick.
    // Pin the exact split, decay, pitch drift, and PRNG-selected yaw sign.
    // [orig: entity+0x380 body-update block]
    InfantryState recoil;
    recoil.recoil_pitch = 1 << 18;
    int32_t heading = 100;
    int32_t pitch = -50;
    infantry_recoil_tick(recoil, heading, pitch, 2); // even -> +half
    CHECK(recoil.recoil_pitch == 245760);
    CHECK(heading == 16484);
    CHECK(pitch == 4046);
    infantry_recoil_tick(recoil, heading, pitch, 3); // odd -> -half
    CHECK(recoil.recoil_pitch == 230400);
    CHECK(heading == 1124);
    CHECK(pitch == 7886);
    recoil.recoil_pitch = 0x301;
    infantry_recoil_tick(recoil, heading, pitch, 0);
    CHECK(recoil.recoil_pitch == 0); // signed <=0x300 snap after decay

    InfantryWeightSpreadInputs in;
    in.produce = true;
    in.weaponweight_fp16 = 98304; // 1.5
    in.clipweight_fp16 = 65536;   // 1.0; W=2.5

    InfantryState weight;
    infantry_weapon_weight_spread_tick(weight, in);
    CHECK(weight.weapon_weight_spread == 230400); // trunc(W*1.5), then 1/16 decay
    in.produce = false;
    infantry_weapon_weight_spread_tick(weight, in);
    CHECK(weight.weapon_weight_spread == 216000); // every body shares the decay

    weight.weapon_weight_spread = 0;
    in.produce = true;
    in.aimed_shot_available = true;
    infantry_weapon_weight_spread_tick(weight, in);
    CHECK(weight.weapon_weight_spread == 51200); // W/3, then decay

    weight.weapon_weight_spread = 0;
    in.aimed_shot_available = false;
    in.crouched = true;
    infantry_weapon_weight_spread_tick(weight, in);
    CHECK(weight.weapon_weight_spread == 102400); // trunc(double(W)*2/3), then decay

    weight.weapon_weight_spread = 0;
    in.crouched = false;
    in.airborne_rising = true;
    infantry_weapon_weight_spread_tick(weight, in);
    CHECK(weight.weapon_weight_spread == 15959040); // +0x01000000 precedes decay
}

void test_hurt_volume_updates_registry_health() {
    Field flat([](int) { return static_cast<uint16_t>(0); });
    World world;
    world.registry.configure_pool(0, 4);
    world.registry.configure_pool(2, 4);

    Entity hazard;
    hazard.kind = EntityKind::Building;
    hazard.position = {10.0f, 10.0f, 0.0f};
    hazard.yaw = 90;
    const EntityHandle hazard_handle = world.registry.spawn(2, hazard);

    Entity infantry;
    infantry.kind = EntityKind::Organic;
    infantry.position = {10.0f, 10.0f, 0.5f};
    infantry.health = 1;
    infantry.health_max = 1;
    const EntityHandle infantry_handle = world.registry.spawn(0, infantry);

    CollisionWorld collision;
    collision.terrain = &flat.field;
    const int32_t model_id = collision.add_model(hurt_box_model());
    collision.assign_entity(hazard_handle, model_id);

    AiSystem ai;
    ai.terrain = &flat.field;
    ai.collision = &collision;
    TestSource source;
    source.clips = {anim_state::kWalkForward};
    source.step = 0;
    ai.root_motion = &source;
    AiEntity *motor = ai.at(ai.attach(infantry_handle));
    motor->inf.active = true;
    motor->inf.is_local_player = true;
    motor->inf.player_moving = true;
    // Registry health is the canonical HUD/wire/script value. Leave the motor
    // deliberately stale to prove the tick hydrates it before applying damage.
    motor->health = 100;
    motor->pos[0] = fx(10.0);
    motor->pos[1] = fx(10.0);
    motor->pos[2] = fx(0.5);

    // Candidate slices mature on the seventeenth build. The type-18 contact then removes
    // the soldier's final HP through the normal authority tick.
    run_ticks(ai, world, 0, 17);

    const Entity *observed = world.registry.get(infantry_handle);
    CHECK(observed != nullptr);
    CHECK(observed->health == 0);
    CHECK(!observed->alive);
}

void test_registry_max_health_drives_wounded_gait() {
    World world;
    world.registry.configure_pool(0, 4);
    Entity infantry;
    infantry.kind = EntityKind::Organic;
    infantry.health = 75;
    infantry.health_max = 150;
    const EntityHandle handle = world.registry.spawn(0, infantry);

    AiSystem ai;
    TestSource source;
    source.clips = {anim_state::kWalkForward, anim_state::kWoundedWalk};
    ai.root_motion = &source;
    AiEntity *motor = ai.at(ai.attach(handle));
    motor->inf.active = true;
    motor->health = 100;
    motor->inf.max_health = 100;
    route(ai, motor, {node(fx(500), 0, fx(1))}, 0);

    run_ticks(ai, world, 0, 1);

    CHECK(motor->health == 75);
    CHECK(motor->inf.max_health == 150);
    CHECK(motor->inf.anim_state == anim_state::kWoundedWalk);
}

// D-NET-159 — AUTHORITY body-anim selection for a net-snapped REMOTE player. Runs in
// its own function: main's frame already unions a dozen scoped World locals and MSVC
// probes the whole frame at entry, so one more inline block overflowed the stack.
// The movement motor skips a wire-snapped peer, but the anim selection still runs from
// the REPLICATED MoveOrder byte (bits 0-2 dir, bit 3 moving) + the C2S 0x1D stance bits,
// and the selected state/phase mirror onto the world Entity the 0x0A record reads.
// [orig: Entity_UpdateInfantryPlayerBody @0x4b40e0 — authority gate @0x4b70a3-0x4b70b2,
//  4th-tick @0x4b70ce, bases @0x4b7183-0x4b7226, idles @0x4b7228-0x4b7293]
void test_remote_player_body_anim() {
    World w;
    w.registry.configure_pool(0, 4);
    Entity seed;
    seed.kind = EntityKind::Organic;
    seed.item_id = 0x14B9;
    seed.health = 100;
    const EntityHandle h = w.registry.spawn(0, seed);
    Entity *ent = w.registry.get(h);
    CHECK(ent != nullptr);

    AiSystem ai;
    TestSource src;
    for (int s = 1; s <= 8; ++s) src.clips.insert(s);          // stand walk block
    for (int s = 11; s <= 18; ++s) src.clips.insert(s);        // crouch walk block
    for (int s = 19; s <= 26; ++s) src.clips.insert(s);        // prone walk block
    src.clips.insert(anim_state::kIdle);
    src.clips.insert(anim_state::kIdle2);
    src.clips.insert(anim_state::kIdleCrouch);
    src.clips.insert(anim_state::kIdleProne);
    ai.root_motion = &src;
    AiEntity *e = soldier(ai); // attaches at handle (0,0) == h
    e->net_is_remote_peer = true;
    e->health = 100;

    // Walking forward: input bit3 (moving) + dir 0 -> stand-walk base 1.
    ent->net_move_input = 0x08;
    run_ticks(ai, w, 0, 8);
    CHECK(ent->net_anim_state == anim_state::kWalkForward);
    CHECK(ent->net_anim_phase > 0);            // the channel ratio sweeps
    CHECK(ent->net_move_input == 0x08);        // the uplinked byte is NOT overwritten
    // Crouch (the 0x1D stance-change model) lifts the walk to base 11.
    ent->net_stance_bits = 2;
    run_ticks(ai, w, 8, 16);
    CHECK(ent->net_anim_state == anim_state::kWalkCrouchForward);
    // Prone walking, diagonal dir 1 -> base 19 + the (8-d) offset 7 = 26.
    ent->net_stance_bits = 1;
    ent->net_move_input = 0x08 | 0x01;
    run_ticks(ai, w, 16, 24);
    CHECK(ent->net_anim_state == anim_state::kWalkProneForward + 7);
    // Stop prone -> prone idle 48; stand -> idle 43.
    ent->net_move_input = 0;
    run_ticks(ai, w, 24, 32);
    CHECK(ent->net_anim_state == anim_state::kIdleProne);
    ent->net_stance_bits = 0;
    run_ticks(ai, w, 32, 40);
    CHECK(ent->net_anim_state == anim_state::kIdle);
    // 62 idle selection passes (4-tick cadence) promote 43 -> 44 [orig: @0x4b727b].
    run_ticks(ai, w, 40, 40 + 62 * 4 + 4);
    CHECK(ent->net_anim_state == anim_state::kIdle2);

    // Prone lean = the roll pair, from the replicated MoveOrder lean bits 6/7
    // [orig: @0x4b731b-0x4b7354 — gated prone + alive + !airborne @0x4b7322;
    //  left @0x4b7335, right @0x4b734c]. A remote peer's rolls derive on the
    //  host exactly like the local player's — no ownership gate.
    src.clips.insert(anim_state::kRollLeft);
    src.clips.insert(anim_state::kRollRight);
    const int t_roll = 40 + 62 * 4 + 4;
    ent->net_stance_bits = 1;
    ent->net_move_input = 0x40; // lean-left, stationary, prone
    run_ticks(ai, w, t_roll, t_roll + 8);
    CHECK(ent->net_anim_state == anim_state::kRollLeft);
    // Both lean bits held: right wins the ladder [orig: bit-7 write @0x4b734c
    // lands after bit-6's]. 41 is transition-locked (flags 0x285 bit 0x4), so
    // the winner parks in the pending slot until the clip completes.
    ent->net_move_input = 0xC0;
    run_ticks(ai, w, t_roll + 8, t_roll + 16);
    CHECK(e->inf.anim_pending == anim_state::kRollRight);
}

// Local-player leg chase + body midpoint — the witnessed org2 model (D-INF-12
// closure): the LEGS chase the mouse yaw (quarter-step, rate clamp ±0x3000000,
// twist ±0x30000000 vs the yaw, per-leg staggered re-plant windows) and the body
// heading is written as their midpoint. Consumed by the render overlay,
// Entity_BuildBoneTransformMatrices @0x4b1290 / world-wac-ai-re.md §14.
// [orig: Entity_UpdateInfantryPlayerBody @0x4b4945-0x4b4ac1] Own function:
// main's frame is at its MSVC stack-probe limit (see test_remote_player_body_anim).

void test_player_body_chase_crosses_the_bam_seam() {
    // The leg chase near +/-180 deg: the wrapping (diff + 2) >> 2 quarter-step takes
    // the SHORT arc across the seam (io/bam.h wrap semantics — signed overflow would
    // be UB), so legs planted at ~+157 deg chasing a yaw at ~-157 deg cross the seam
    // instead of sweeping the long way around; the body midpoint crosses with them.
    World w;
    AiSystem ai;
    TestSource src;
    src.clips.insert(anim_state::kIdle);
    src.clips.insert(anim_state::kIdle2);
    ai.root_motion = &src;
    AiEntity *e = soldier(ai);
    e->inf.is_local_player = true;
    e->health = 100;

    const int32_t start = 0x70000000;             // +157.5 deg
    const int32_t target = (int32_t)0x90000000u;  // -157.5: +45 deg the short way
    e->inf.body_heading = start;
    e->inf.target_heading = target;
    e->inf.leg_yaw[0] = e->inf.leg_yaw[1] = start;
    e->inf.leg_target[0] = e->inf.leg_target[1] = start;

    run_ticks(ai, w, 1, 2);
    CHECK(e->heading == target); // player render/aim yaw is instant [orig: +0x10]
    // 45 deg > the 30-deg snap: both legs re-plant on the yaw and step the clamped
    // +0x3000000 along the SHORT arc — still on the positive side of the seam.
    CHECK(e->inf.leg_yaw[0] == start + 0x3000000);
    CHECK(e->inf.leg_yaw[1] == start + 0x3000000);
    CHECK(e->inf.body_heading == start + 0x3000000); // midpoint of equal legs

    run_ticks(ai, w, 2, 400);
    // Settled ACROSS the seam at the yaw (the chase stalls within one BAM unit);
    // the body midpoint crossed with the legs.
    CHECK(opennova::io::bam_abs(opennova::io::bam_sub(e->inf.leg_yaw[0], target)) <= 1);
    CHECK(opennova::io::bam_abs(opennova::io::bam_sub(e->inf.body_heading, target)) <= 1);
}

void test_player_body_chase_and_legs() {
    constexpr int32_t kLegClamp2 = 0x3000000;  // org2 rate clamp [orig: @0x4b49fb]
    constexpr int32_t kLegTwist2 = 0x30000000; // 67.5 deg vs the yaw [orig: @0x4b4a23]

    World w;
    AiSystem ai;
    TestSource src;
    src.clips.insert(anim_state::kIdle);
    src.clips.insert(anim_state::kIdle2);
    ai.root_motion = &src;
    AiEntity *e = soldier(ai);
    e->inf.is_local_player = true;
    e->health = 100;

    // Spawn-aligned: everything at heading 0; swing the aim 90 deg right.
    e->inf.target_heading = 0x40000000;
    run_ticks(ai, w, 1, 2);
    // Aim/render yaw is INSTANT for the player; 90 deg > the 30-deg snap re-plants
    // both legs at once, the chase steps the clamp, and the 67.5-deg twist limit
    // (measured vs the YAW) snaps them the rest of the way in the same tick.
    CHECK(e->heading == 0x40000000);
    CHECK(e->inf.leg_yaw[0] == 0x40000000 - kLegTwist2);
    CHECK(e->inf.leg_yaw[1] == 0x40000000 - kLegTwist2);
    // The body IS the leg midpoint — it moved with the legs, no chase of its own.
    CHECK(e->inf.body_heading == 0x40000000 - kLegTwist2);

    // Settle: the legs chase the clamped quarter-step onto the yaw (stalling within
    // one BAM unit), and the body midpoint lands with them.
    run_ticks(ai, w, 2, 160);
    CHECK(std::abs(e->inf.leg_yaw[0] - 0x40000000) <= 1);
    CHECK(std::abs(e->inf.leg_yaw[1] - 0x40000000) <= 1);
    CHECK(std::abs(e->inf.body_heading - 0x40000000) <= 1);
    (void)kLegClamp2;

    // Small look-around (4 deg < the 5-deg hysteresis floor): the legs never budge,
    // and because the body is their midpoint it does NOT follow the aim — the §14
    // torso twist absorbs small aim moves entirely. 4 deg = 47721856 BAM.
    const int32_t planted_r = e->inf.leg_yaw[0];
    const int32_t planted_l = e->inf.leg_yaw[1];
    e->inf.target_heading = 0x40000000 + 47721856;
    run_ticks(ai, w, 160, 260);
    CHECK(e->heading == e->inf.target_heading);
    CHECK(e->inf.leg_yaw[0] == planted_r);
    CHECK(e->inf.leg_yaw[1] == planted_l);
    CHECK(std::abs(e->inf.body_heading - 0x40000000) <= 1); // body held by the feet

    // A 10-deg move (> 5, < 30 deg): the re-plant waits for each leg's 64-tick
    // window, right first, LEFT 32 ticks later — the staggered shuffle. After both
    // windows pass, the feet have followed onto the yaw. 10 deg = 119304647 BAM.
    e->inf.target_heading = 0x40000000 + 119304640;
    run_ticks(ai, w, 260, 460);
    CHECK(std::abs(e->inf.leg_yaw[0] - e->inf.target_heading) <= 1);
    CHECK(std::abs(e->inf.leg_yaw[1] - e->inf.target_heading) <= 1);
    CHECK(std::abs(e->inf.body_heading - e->inf.target_heading) <= 1);
}

// An org1 (NPC) plain ledge fall keeps its clip: the fall edge's 47/31 stamp
// ladder is PARACHUTE-gated for the NPC motor (`test al,20h` @0x4bf8d8 — a live
// walker run-cycles off a roof in retail), so only airborne sets and any pending
// anim clears; the org2 player is the motor that stamps 31 straight.
// [orig: org1 @0x4bf8ae-0x4bf901; org2 @0x4b7e17-0x4b7e73] Own function: main's
// frame is at its MSVC stack-probe limit (see test_remote_player_body_anim).
void test_npc_ledge_fall_keeps_clip() {
    Field ground0([](int) { return static_cast<uint16_t>(0); }); // ground at 0
    World w;
    AiSystem ai;
    ai.terrain = &ground0.field;
    TestSource src;
    // kJumpLoop is AVAILABLE — proving the no-stamp is the parachute gate, not
    // clip availability.
    src.clips = {anim_state::kWalkForward, anim_state::kIdle, anim_state::kJumpLoop};
    ai.root_motion = &src;
    AiEntity *e = soldier(ai);
    e->health = 100;
    e->inf.is_local_player = false;
    route(ai, e, {node(fx(500), 0, fx(1))}, 0);
    e->pos[0] = fx(100); e->pos[1] = fx(100); e->pos[2] = fx(80); // 80u off the floor

    run_ticks(ai, w, 0, 1); // think commits the walk, then the fall edge fires
    CHECK(e->inf.airborne);
    CHECK(e->inf.anim_state == anim_state::kWalkForward); // NOT kJumpLoop
    CHECK(e->inf.anim_pending == 0);                      // [orig: @0x4bf901]
    CHECK(e->inf.vel[0] == 0 && e->inf.vel[1] == 0);      // no org2 momentum carry
}

// Unlike org1, org2 includes DEAD in the gate around the whole ledge-fall edge:
// a dead player that was grounded does not gain the airborne bit, clear pending,
// or stamp jump_loop merely because the resolver returns >0xF000.
// [orig: `test eax,10A002h; jnz` @0x4b7e22-0x4b7e2a, before the airborne write
//  @0x4b7e3c; compare org1's 0x10A000 gate + pre-dead-test write @0x4bf8b5-0x4bf8cf]
void test_dead_player_ledge_fall_edge_is_suppressed() {
    Field ground0([](int) { return static_cast<uint16_t>(0); });
    World w;
    AiSystem ai;
    ai.terrain = &ground0.field;
    TestSource src;
    src.clips = {anim_state::kDeathFire, anim_state::kJumpLoop};
    ai.root_motion = &src;
    AiEntity *e = soldier(ai);
    e->health = 0;
    e->inf.is_local_player = true;
    e->inf.anim_state = anim_state::kDeathFire; // already on the death-family path
    e->inf.anim_pending = 123;
    e->pos[0] = fx(100);
    e->pos[1] = fx(100);
    e->pos[2] = fx(80);

    run_ticks(ai, w, 0, 1);

    CHECK(!e->inf.airborne); // org2's dead gate owns the airborne-bit write
    CHECK(e->inf.anim_state == anim_state::kDeathFire);
    CHECK(e->inf.anim_pending == 123); // the whole fall edge was skipped
}

// The resolver's idle skip-throttle undo must match the CALLER's integrate per
// motor: x1 for the player body (org2 `pos += vel`, -208/tick) and x2 for the
// NPC (org1 `pos += 2*vel`). The x2-for-both undo (tuned to the pre-§22 2-tick
// player cadence) nets +208/tick of climb through the skip band, then the
// no-reset middle band accumulates the fall back to the floor snap — the
// standing player's visible rise-and-snap sawtooth (2026-07-17 regression).
// This drives the REAL motor against the REAL resolver across the throttle's
// 64-tick windows — the direct-resolver test hand-rolled the caller's cadence
// and stayed green while the pair diverged.
// [orig: the skip undo @0x4b2cd9-0x4b2ce9 — `test ecx,100h` picks x1 for the
//  Flags&0x100 PLAYER body (the "mounted" gloss was a kong misnomer; the kill
//  router and AI target filters key players on 0x100); gates @0x4b2c3d-0x4b2cba]
void test_player_idle_skip_throttle_no_bounce() {
    Field flat([](int) { return static_cast<uint16_t>(0); });
    World world;
    world.registry.configure_pool(0, 4);
    world.registry.configure_pool(2, 4);

    Entity far_building; // presence only: a live instance enables the resolver path
    far_building.kind = EntityKind::Building;
    far_building.position = {200.0f, 200.0f, 0.0f};
    const EntityHandle bh = world.registry.spawn(2, far_building);

    Entity player;
    player.kind = EntityKind::Organic;
    // Spawn ON the floor: the skip throttle is the thing under test, not the
    // drop landing (a >2u drop can outrun the witnessed 2u ground probe — the
    // quantize-up + 2u down-probe @0x4b3d6e — and tunnel in this bare rig).
    player.position = {10.0f, 10.0f, 0.05f};
    player.health = 100;
    player.health_max = 100;
    const EntityHandle ph = world.registry.spawn(0, player);

    CollisionWorld collision;
    collision.terrain = &flat.field;
    const int32_t model_id = collision.add_model(hurt_box_model());
    collision.assign_entity(bh, model_id);

    AiSystem ai;
    ai.terrain = &flat.field;
    ai.collision = &collision;
    TestSource source;
    source.clips = {anim_state::kIdle};
    source.step = 0;
    ai.root_motion = &source;
    AiEntity *e = ai.at(ai.attach(ph));
    e->inf.active = true;
    e->inf.is_local_player = true;
    e->health = 100;
    e->pos[0] = fx(10.0);
    e->pos[1] = fx(10.0);
    e->pos[2] = fx(0.05);

    run_ticks(ai, world, 0, 80); // pin onto the floor + pass the first 64-gate
    const int32_t settled = e->pos[2];
    int32_t min_z = settled;
    int32_t max_z = settled;
    for (uint32_t t = 80; t < 400; ++t) { // several 64-tick windows + skip bands
        run_ticks(ai, world, t, t + 1);
        if (e->pos[2] < min_z) min_z = e->pos[2];
        if (e->pos[2] > max_z) max_z = e->pos[2];
    }
    CHECK(max_z - min_z <= 208); // post-tick z pinned: no rise, no sawtooth
    CHECK(std::abs(e->pos[2] - settled) <= 208);
}

// The upper-body weapon channel (the entity's SECONDARY AnimMap channel), local-player
// slice: the rifle-mirror default, the 80-tick reload window -> state 65, the locked
// commit rule (65 = flag 0x84 defers exits to clip end), and the clip-end promotion.
// [orig: Entity_UpdateInfantryPlayerBody @0x4b5cab..0x4b5ea9 + AnimMap_UpdateDualChannels
//  @0x40b8c0 / AnimMap_UpdateEntity @0x40b77b; witness world-wac-ai-re.md §14.8]
void test_player_weapon_channel() {
    World w;
    AiSystem ai;
    TestSource src;
    src.clips.insert(anim_state::kWalkForward);
    src.clips.insert(anim_state::kIdle);
    src.clips.insert(anim_state::kIdle2);
    src.clips.insert(anim_state::kReload);
    src.lengths[anim_state::kReload] = 40; // one-shot reload clip, 40 phase ticks
    ai.root_motion = &src;
    AiEntity *e = soldier(ai);
    e->inf.is_local_player = true;
    e->health = 100;

    // Rifle default: the secondary channel MIRRORS the primary [orig: @0x4b5e46].
    // Selections land only on the 16-tick slow pass, so each step below crosses one.
    uint32_t t = 1;
    t = run_to_next_selection(ai, w, t);
    CHECK(e->inf.anim_state == anim_state::kIdle);
    CHECK(e->inf.wpn_state == anim_state::kIdle);
    e->inf.player_moving = true;
    t = run_to_next_selection(ai, w, t);
    CHECK(e->inf.anim_state == anim_state::kWalkForward);
    CHECK(e->inf.wpn_state == anim_state::kWalkForward);
    CHECK(e->inf.wpn_clip_phase > 0); // the secondary playhead advances on its own

    // The refill stamps the 80-tick window -> the channel wants 65 reload; idle/walk
    // currents are not locked, so the stamp lands on the next selection pass rather than
    // the next tick [orig: @0x4b5e67/@0x4b5e9d behind the @0x4b5d71 gate].
    e->inf.reload_anim_ticks = 80; // [orig: WeaponSlot_ReloadAmmo @0x54173c]
    t = run_to_next_selection(ai, w, t);
    CHECK(e->inf.wpn_state == anim_state::kReload);
    CHECK(e->inf.wpn_clip_phase == 1);          // fresh channel re-init + first advance
    CHECK(e->inf.anim_state == anim_state::kWalkForward); // the legs keep locomotion

    // The window itself still counts down EVERY tick — only the selection is gated.
    const int32_t before = e->inf.reload_anim_ticks;
    run_ticks(ai, w, t, t + 8);
    t += 8;
    CHECK(e->inf.reload_anim_ticks == before - 8);
    CHECK(e->inf.wpn_state == anim_state::kReload);

    // The clip ends (40 phase ticks) BEFORE the window does: 65 is locked (flag 0x84),
    // so the mirror desire defers, and the deferred state only lands once BOTH the
    // window has expired (desire leaves 65) and the clip end promotes it
    // [orig: defer @0x4b5e88; promote @0x40b77b].
    e->inf.reload_anim_ticks = 1;
    t = run_to_next_selection(ai, w, t);
    CHECK(e->inf.reload_anim_ticks == 0);
    CHECK(e->inf.wpn_state == anim_state::kReload);         // still locked in its clip
    CHECK(e->inf.wpn_deferred == anim_state::kWalkForward); // the exit is queued
    run_ticks(ai, w, t, t + 41); // the 40-tick clip completes -> promotion fires
    t += 41;
    CHECK(e->inf.wpn_state == anim_state::kWalkForward); // promoted back to the mirror
    CHECK(e->inf.wpn_deferred == 0);

    // Window expiring MID-CLIP: re-stamp, then cut it short — the locked reload keeps
    // playing to its own end, the mirror desire waits in the deferred slot.
    e->inf.reload_anim_ticks = 80;
    t = run_to_next_selection(ai, w, t);
    CHECK(e->inf.wpn_state == anim_state::kReload);
    e->inf.reload_anim_ticks = 2;
    t = run_to_next_selection(ai, w, t); // window over, clip still mid-play
    CHECK(e->inf.reload_anim_ticks == 0);
    CHECK(e->inf.wpn_state == anim_state::kReload);          // still locked in
    CHECK(e->inf.wpn_deferred == anim_state::kWalkForward);  // the exit is queued
    run_ticks(ai, w, t, t + 41); // ...until the clip's 40 phase ticks complete
    std::fprintf(stderr, "DBG wpn_state=%d deferred=%d phase=%d primary=%d\n",
                 e->inf.wpn_state, e->inf.wpn_deferred, e->inf.wpn_clip_phase,
                 e->inf.anim_state);
    CHECK(e->inf.wpn_state == anim_state::kWalkForward);
    CHECK(e->inf.wpn_deferred == 0);
}

// The hold-pose kind ladder (special_hold 1-8 -> states 50-61, the scoped +1 variants),
// the scoped rifle default (49 idle_3), and the override order — binoculars 64 beats the
// holds, the reload window beats binoculars, and the pistol kind selects 66 reload2.
// [orig: Entity_UpdateInfantryPlayerBody @0x4b5dc0..0x4b5e6f]
void test_player_weapon_hold_kinds() {
    World w;
    AiSystem ai;
    TestSource src;
    src.clips.insert(anim_state::kIdle);
    src.clips.insert(anim_state::kIdle2);
    for (int s = anim_state::kHoldKnife; s <= anim_state::kHoldJavelinScoped; ++s)
        src.clips.insert(s);
    src.clips.insert(anim_state::kBinoculars);
    src.clips.insert(anim_state::kReload2);
    src.lengths[anim_state::kReload2] = 30;
    ai.root_motion = &src;
    AiEntity *e = soldier(ai);
    e->inf.is_local_player = true;
    e->health = 100;

    // The hold kind now resolves through the ADM table by the entity's own equipped
    // index, the way every observer resolves it for every player body.
    give_held_weapon(w, e, /*adm=*/5, /*special_hold=*/0);
    uint32_t tick = 1;
    auto select = [&](int kind) {
        w.weapons.entries[5].special_hold = kind;
        tick = run_to_next_selection(ai, w, tick);
    };

    // Kinds 1-4: fixed holds 50-53; the scope flag is ignored [orig: lea eax,[ecx+31h]
    // @0x4b5dc5/0x4b5dd2/0x4b5ddc/0x4b5de6].
    static constexpr int kFixedHold[4] = {anim_state::kHoldKnife, anim_state::kHoldPistol,
                                          anim_state::kHoldGrenade, anim_state::kHoldStinger};
    for (int kind = 1; kind <= 4; ++kind) {
        e->inf.scope_raised = (kind & 1) != 0; // must not matter for 1-4
        select(kind);
        CHECK(e->inf.wpn_state == kFixedHold[kind - 1]);
    }
    // Kinds 5-8: 54/56/58/60, +1 scoped [orig: test Flags&0x10 @0x4b5df0..0x4b5e35].
    static constexpr int kScopedHold[4] = {anim_state::kHoldDesignator, anim_state::kHoldP90,
                                           anim_state::kHoldMP7, anim_state::kHoldJavelin};
    for (int kind = 5; kind <= 8; ++kind) {
        e->inf.scope_raised = false;
        select(kind);
        CHECK(e->inf.wpn_state == kScopedHold[kind - 5]);
        e->inf.scope_raised = true;
        select(kind);
        CHECK(e->inf.wpn_state == kScopedHold[kind - 5] + 1);
    }
    // Rifle (kind 0) + scope: the mirror default coerces to 49 idle_3; dropping the
    // scope returns the mirror [orig: @0x4b5e48..0x4b5e4e].
    e->inf.scope_raised = true;
    select(0);
    CHECK(e->inf.wpn_state == anim_state::kIdle3);
    e->inf.scope_raised = false;
    select(0);
    CHECK(e->inf.wpn_state == e->inf.anim_state);

    // An entity holding NOTHING (no ADM row) falls to the rifle mirror, which is what a
    // peer whose equipped index has not arrived yet must look like.
    w.registry.get(e->handle)->equipped_adm_index = 0xFF;
    select(7); // the table row is irrelevant now — the index resolves to no entry
    CHECK(e->inf.wpn_state == e->inf.anim_state);
    w.registry.get(e->handle)->equipped_adm_index = 5;

    // Binoculars override the hold pose [orig: @0x4b5e53]; the reload window overrides
    // binoculars, and the pistol kind (2) selects reload2 [orig: @0x4b5e5e..0x4b5e6f].
    e->inf.binoculars_raised = true;
    select(2);
    CHECK(e->inf.wpn_state == anim_state::kBinoculars);
    e->inf.reload_anim_ticks = 80;
    select(2);
    CHECK(e->inf.wpn_state == anim_state::kReload2);
}

// The fire-path attack stamps [orig: WeaponAction_Fire @0x542bbc..0x542bea]: knife kind
// 1 -> 62 / grenade kind 2 -> 63 stamped IMMEDIATELY, other kinds stamp nothing, a
// repeat stamp of the playing state does not restart the clip, and the locked (0x94)
// exit defers to clip end.
void test_player_weapon_attack_stamp() {
    World w;
    AiSystem ai;
    TestSource src;
    src.clips.insert(anim_state::kIdle);
    src.clips.insert(anim_state::kIdle2);
    src.clips.insert(anim_state::kHoldKnife);
    src.clips.insert(anim_state::kHoldGrenade);
    src.clips.insert(anim_state::kKnifeAttack);
    src.clips.insert(anim_state::kGrenadeAttack);
    src.lengths[anim_state::kKnifeAttack] = 24;
    src.lengths[anim_state::kGrenadeAttack] = 24;
    ai.root_motion = &src;
    AiEntity *e = soldier(ai);
    e->inf.is_local_player = true;
    e->health = 100;
    give_held_weapon(w, e, /*adm=*/5, /*special_hold=*/1); // knife family

    uint32_t t = run_to_next_selection(ai, w, 1);
    CHECK(e->inf.wpn_state == anim_state::kHoldKnife);

    // Rifle / unknown kinds: NO body stamp [orig: only the 1/2 compares].
    infantry_weapon_attack_stamp(e->inf, 0);
    CHECK(e->inf.wpn_state == anim_state::kHoldKnife);
    infantry_weapon_attack_stamp(e->inf, 3);
    CHECK(e->inf.wpn_state == anim_state::kHoldKnife);

    // The knife stamp lands immediately [orig: @0x542bcb]; the hold desire then defers
    // behind the locked attack until its 24-tick clip end.
    infantry_weapon_attack_stamp(e->inf, 1);
    CHECK(e->inf.wpn_state == anim_state::kKnifeAttack);
    CHECK(e->inf.wpn_deferred == 0);
    CHECK(e->inf.wpn_clip_phase == 0); // fresh clip on the target change
    t = run_to_next_selection(ai, w, t);
    CHECK(e->inf.wpn_state == anim_state::kKnifeAttack);
    CHECK(e->inf.wpn_deferred == anim_state::kHoldKnife); // the exit is queued

    // A repeat stamp mid-clip keeps the playhead — the channel re-inits only on a
    // target CHANGE [orig: AnimMap_UpdateEntity @0x40b5f0].
    const int32_t mid_phase = e->inf.wpn_clip_phase;
    CHECK(mid_phase > 0);
    infantry_weapon_attack_stamp(e->inf, 1);
    CHECK(e->inf.wpn_clip_phase == mid_phase);
    CHECK(e->inf.wpn_deferred == 0);

    // Clip end -> promotion back to the hold pose [orig: @0x40b77b].
    run_ticks(ai, w, t, t + 40);
    CHECK(e->inf.wpn_state == anim_state::kHoldKnife);

    // The grenade kind stamps 63 [orig: @0x542be0].
    infantry_weapon_attack_stamp(e->inf, 2);
    CHECK(e->inf.wpn_state == anim_state::kGrenadeAttack);
}

// The arms-dip feed [orig: @0x4b5cab..0x4b5ce7]: while the window runs the pitch-kick
// decay term drops 0x2800000/tick before the eighth-step ease, and the window
// decrements TWICE per tick — the 20-tick weapon-switch stamp dips for 10 ticks —
// then the ease brings the term back toward rest.
void test_player_arms_dip() {
    World w;
    AiSystem ai;
    TestSource src;
    src.clips.insert(anim_state::kIdle);
    src.clips.insert(anim_state::kIdle2);
    ai.root_motion = &src;
    AiEntity *e = soldier(ai);
    e->inf.is_local_player = true;
    e->health = 100;

    e->inf.arms_dip_ticks = 20; // [orig: the switch stamp @0x4b46f5]
    run_ticks(ai, w, 1, 2);
    // One tick: dip, then ease — HLD = d - (d+4)>>3 with d = -0x2800000 — and TWO
    // window decrements.
    const int32_t d = -0x2800000;
    const int32_t expected = d - opennova::io::bam_sar(opennova::io::bam_add(d, 4), 3);
    CHECK(e->inf.pitch_kick_accum == expected);
    CHECK(e->inf.arms_dip_ticks == 18);

    run_ticks(ai, w, 2, 11); // 9 more ticks: the window drains at 2/tick
    CHECK(e->inf.arms_dip_ticks == 0);
    CHECK(e->inf.pitch_kick_accum < d); // accumulated deeper than a single tick's dip

    run_ticks(ai, w, 11, 200); // the eighth-step ease settles back near rest
    CHECK(opennova::io::bam_abs(e->inf.pitch_kick_accum) <= 8);
}

// The dual-channel body update and arms/HLD block continue on dead local-player ticks.
// The death primary disables composition via its flags, but timers must not freeze a
// persistent arm-pitch offset on the corpse and the secondary playhead still advances.
void test_player_weapon_channel_ticks_while_dead() {
    World w;
    AiSystem ai;
    TestSource src;
    src.clips = {anim_state::kIdle, anim_state::kDeathFire, anim_state::kReload};
    src.lengths[anim_state::kReload] = 20;
    ai.root_motion = &src;
    AiEntity *e = soldier(ai);
    e->inf.is_local_player = true;
    e->health = 0;
    e->inf.anim_state = anim_state::kIdle;
    e->inf.wpn_state = anim_state::kReload;
    e->inf.wpn_clip_phase = 4;
    e->inf.reload_anim_ticks = 3;
    e->inf.arms_dip_ticks = 4;
    e->inf.pitch_kick_accum = -1000;

    run_ticks(ai, w, 1, 2);
    CHECK(e->inf.anim_state == anim_state::kDeathFire);
    CHECK(e->inf.wpn_state == anim_state::kReload);
    CHECK(e->inf.wpn_clip_phase == 5);
    CHECK(e->inf.reload_anim_ticks == 2);
    CHECK(e->inf.arms_dip_ticks == 2);
    CHECK(e->inf.pitch_kick_accum != -1000);
    CHECK(!infantry_weapon_channel_visible(e->inf, true, false));
}

void test_weapon_channel_consumer_gate_and_switch_identity() {
    InfantryState inf;
    inf.active = true;
    inf.anim_state = anim_state::kIdle; // flags 0x48: on-foot composition enabled
    inf.wpn_state = anim_state::kIdle;  // same state, but an independent playhead
    CHECK(infantry_weapon_channel_visible(inf, true, false));
    CHECK(!infantry_weapon_channel_visible(inf, false, false));
    CHECK(!infantry_weapon_channel_visible(inf, true, true));
    inf.anim_state = anim_state::kWalkProneForward; // flags 0x603: no 0x40
    CHECK(!infantry_weapon_channel_visible(inf, true, false));

    // The observed AnimMap serial is per entity: a repeated map does not restamp,
    // a changed map does, and a newly spawned entity sees the current map as new.
    InfantryState first;
    infantry_weapon_switch_stamp(first, 7);
    CHECK(first.wpn_anim_map_serial == 7);
    CHECK(first.arms_dip_ticks == 20);
    first.arms_dip_ticks = 5;
    infantry_weapon_switch_stamp(first, 7);
    CHECK(first.arms_dip_ticks == 5);
    infantry_weapon_switch_stamp(first, 8);
    CHECK(first.wpn_anim_map_serial == 8);
    CHECK(first.arms_dip_ticks == 20);

    InfantryState replacement;
    infantry_weapon_switch_stamp(replacement, 8);
    CHECK(replacement.wpn_anim_map_serial == 8);
    CHECK(replacement.arms_dip_ticks == 20);
    replacement.arms_dip_ticks = 3;
    infantry_weapon_switch_stamp(replacement, 0);
    CHECK(replacement.wpn_anim_map_serial == 8);
    CHECK(replacement.arms_dip_ticks == 3);
}

// The 0.25 u/u X-gradient ramp (~14 deg): raw16 = x*64 -> height = x*0.25u.
struct GentleRamp : Field {
    GentleRamp()
        : Field([](int x) {
              int v = x * 64;
              return static_cast<uint16_t>(v > 65535 ? 65535 : v);
          }) {}
};

// Regression (the slope-roll camera lean): a live STANDING local player across a
// side slope, no lean keys -> the conform selector routes to the DECAY leg, so the
// slope never reaches roll, torso_roll, or the composed FP camera roll.
// [orig: selector @0x4b6d95 -> decay @0x4b6dbd; fp_roll = torsoRoll + lean/4 @0x437fe6]
void test_slope_standing_camera_stays_level() {
    GentleRamp ramp;
    World w;
    AiSystem ai;
    ai.terrain = &ramp.field;
    TestSource src;
    src.clips = {anim_state::kIdle};
    ai.root_motion = &src;
    AiEntity *e = soldier(ai);
    e->inf.is_local_player = true;
    e->health = 100;
    // Facing +Y: the gradient is pure LATERAL (roll) slope. The motor chases
    // target_heading and rewrites e->heading, so drive all three.
    e->heading = 0x40000000;
    e->inf.body_heading = 0x40000000;
    e->inf.target_heading = 0x40000000;
    e->pos[0] = fx(100);
    e->pos[1] = fx(100);
    e->pos[2] = fx(25) + kFloorStand; // ground at x=100 on the 0.25 ramp

    run_ticks(ai, w, 0, 240);
    CHECK(e->roll == 0);
    CHECK(e->body_pitch == 0);
    CHECK(e->inf.torso_roll == 0);
    CHECK(e->inf.vel[0] == 0 && e->inf.vel[1] == 0); // no slide for a stander
    CHECK(e->pos[0] == fx(100));                     // ...so no drift either
}

// The org2 conform leg: the same slope, PRONE. The body roll quarter-chases the
// true atan2 slope angle every 2nd tick while prone idle 48 decays torso_roll, so
// the BODY conforms (~-14 deg) and the FP camera still stays level.
// [orig: probes/chase @0x4b6e41-0x4b6ff4; atan2 bases 45056/11264, scale 2^32/2pi;
//  torso decay @0x4b5d05-0x4b5d16]
void test_slope_prone_body_conforms_org2() {
    GentleRamp ramp;
    World w;
    AiSystem ai;
    ai.terrain = &ramp.field;
    TestSource src;
    src.clips = {anim_state::kIdle, anim_state::kIdleProne};
    ai.root_motion = &src;
    AiEntity *e = soldier(ai);
    e->inf.is_local_player = true;
    e->health = 100;
    e->inf.stance = InfantryState::Stance::kProne;
    e->inf.anim_state = anim_state::kIdleProne;
    e->heading = 0x40000000;
    e->inf.body_heading = 0x40000000;
    e->inf.target_heading = 0x40000000;
    e->pos[0] = fx(100);
    e->pos[1] = fx(100);
    e->pos[2] = fx(25) + kFloorStand;

    // First pass (t=0, even tick): lateral probes at +-0.0859375u see dh = -2816 ->
    // roll_slope = trunc(atan2(-2816, 11264) * 683565275.5764316) = -167458907
    // (-14.036 deg), first quarter-step (-167458905) >> 2 = -41864727.
    run_ticks(ai, w, 0, 1);
    CHECK(e->roll == -41864727);
    CHECK(e->body_pitch == 0); // no fore-aft gradient facing +Y

    // Converged: roll sits at the chase fixed point of the slope angle; the prone
    // idle decay keeps torso_roll level (the camera does NOT barrel with the body).
    run_ticks(ai, w, 1, 240);
    CHECK(e->roll >= -167458908 && e->roll <= -167458905);
    CHECK(e->inf.torso_roll > -0x100000 && e->inf.torso_roll <= 0);
    CHECK(e->inf.vel[0] == 0 && e->inf.vel[1] == 0); // 14 deg is under the 60-deg threshold
}

// The org1 leg + the selector, unit-driven through the pass itself (the NPC think/
// select churn would otherwise rewrite the anim state before the pass sees it).
// [orig: selector @0x4ba10f; chase @0x4ba320; decay @0x4ba133; slide @0x4ba24c]
void test_slope_pass_org1_selector_and_chase() {
    // Gradient 1 u/u (the steep 45-deg dune of the slide test).
    Field ramp([](int x) {
        int v = x * 256;
        return static_cast<uint16_t>(v > 65535 ? 65535 : v);
    });
    World w;
    AiSystem ai;
    ai.terrain = &ramp.field;
    AiEntity *e = soldier(ai);
    e->health = 100;
    e->pos[0] = fx(100);
    e->pos[1] = fx(100);
    e->pos[2] = fx(100) + kFloorStand;

    // Prone crawl (19, flags 0x603 bit 2): conform. Probes see the 0.6875u rise ->
    // pitch slope 45056<<14 clamped to 656175520, over the 0x22222200 threshold ->
    // slide back 2048; body_pitch chases an eighth-step; the X-only ramp has no roll.
    e->inf.anim_state = anim_state::kWalkProneForward;
    ai.infantry_slope_pass(*e, 0, 0);
    CHECK(e->body_pitch == (656175520 + 4) >> 3);
    CHECK(e->roll == 0);
    CHECK(e->inf.vel[0] == -2048 && e->inf.vel[1] == 0);

    // org1 cadence: off-phase key -> untouched.
    const int32_t held = e->body_pitch;
    ai.infantry_slope_pass(*e, 0, 3);
    CHECK(e->body_pitch == held);

    // Standing (43, flags 0x048): NOT conform -> both fields decay 1/16, no slide.
    e->inf.anim_state = anim_state::kIdle;
    e->inf.vel[0] = 0;
    e->roll = 0x01000000;
    ai.infantry_slope_pass(*e, 0, 0);
    CHECK(e->body_pitch == held - ((held + 8) >> 4));
    CHECK(e->roll == 0x01000000 - ((0x01000000 + 8) >> 4));
    CHECK(e->inf.vel[0] == 0);

    // A grounded corpse conforms regardless of state; dead + airborne is the
    // (unported) tumble branch -> the pass leaves everything alone.
    e->health = 0;
    e->body_pitch = 0;
    e->inf.vel[0] = 0;
    ai.infantry_slope_pass(*e, 0, 0);
    CHECK(e->body_pitch == (656175520 + 4) >> 3);
    CHECK(e->inf.vel[0] == -2048);
    const int32_t at_death = e->body_pitch;
    e->inf.airborne = true;
    ai.infantry_slope_pass(*e, 0, 0);
    CHECK(e->body_pitch == at_death);
    CHECK(e->inf.vel[0] == -2048);
}

} // namespace

// P1c death presentation (world-wac-ai-re §19). In its own function: a new block
// inside main() overflows the 1 MB stack on entry (__chkstk touches the whole frame).
void test_death_presentation() {
    // ---- the death-anim selector ----
    // [orig: Entity_ComputeAnimSlotIndex @0x43a690 — cause routing + the 32-entry
    // bone->group table; world-wac-ai-re §19]
    {
        using namespace anim_state;
        CHECK(compute_death_anim_state(0, 0, death_cause::kBullet) == 180);  // hip fwd
        CHECK(compute_death_anim_state(1, 2, death_cause::kBullet) == 186);  // torso back
        CHECK(compute_death_anim_state(14, 1, death_cause::kBullet) == 189); // head right
        CHECK(compute_death_anim_state(16, 3, death_cause::kBullet) == 211); // rhand left
        CHECK(compute_death_anim_state(18, 0, death_cause::kBullet) == 236); // lfoot fwd
        CHECK(compute_death_anim_state(40, 5, death_cause::kBullet) == 180); // clamps -> hip fwd
        CHECK(compute_death_anim_state(0, 2, death_cause::kExplosive) == 178); // grenade back
        CHECK(compute_death_anim_state(0, 0, death_cause::kFire) == kDeathFire);
        CHECK(compute_death_anim_state(0, 0, death_cause::kDrown) == kDeathDrown);
        CHECK(compute_death_anim_state(0, 0, death_cause::kGeneric) == kDeathPungi);
        // Quadrant: victim facing +X (heading BAM 0 = mission yaw 90); a round flying
        // +X (from behind) -> bearing 0 -> (0 - 0 - 0x60000000) >> 30 = 2 (back).
        CHECK(death_quadrant_from_round(0, 1.0f, 0.0f) == 2);
        CHECK(death_quadrant_from_round(0, -1.0f, 0.0f) == 0); // head-on -> forward
    }

    // ---- death edge consumes the damage-time selection (+0x2C0) ----
    // [orig: @0x4b9cc9 — the staged deathAnimStateId wins over the generic; consumed]
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        w.registry.configure_pool(0, 4);
        Entity seed;
        seed.health = 0;
        seed.deathtime_ticks = 100;
        const EntityHandle h = w.registry.spawn(0, seed);
        Entity *ent = w.registry.get(h);
        ent->death_anim_state = 189; // death_bullet_head_right, staged by the kill
        auto ai_heap = std::make_unique<AiSystem>();
        AiSystem &ai = *ai_heap;
        TestSource src;
        src.clips = {189};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        run_ticks(ai, w, 1, 2);
        CHECK(e->inf.anim_state == 189);
        CHECK(ent->death_anim_state == 0);   // consumed [orig: +0x2C0 = 0 @0x4b9d38]
        CHECK(ent->corpse_timer == 100 - 1); // seeded from deathtime, then 1 dead tick
    }

    // ---- corpse persistence: countdown -> despawn (no local player = no watcher) ----
    // [orig: @0x4b9e6a decrement / Entity_Destroy @0x4b9f93; our despawn = hidden]
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        w.registry.configure_pool(0, 4);
        Entity seed;
        seed.health = 0;
        seed.deathtime_ticks = 5;
        const EntityHandle h = w.registry.spawn(0, seed);
        auto ai_heap = std::make_unique<AiSystem>();
        AiSystem &ai = *ai_heap;
        AiEntity *e = soldier(ai);
        run_ticks(ai, w, 1, 3); // edge (timer=5, -1) + one more dead tick
        Entity *ent = w.registry.get(h);
        CHECK(ent->corpse_timer == 3);
        CHECK(!ent->hidden);
        run_ticks(ai, w, 3, 8); // drain to 0 -> despawn
        CHECK(ent->hidden);
        CHECK(e->inf.anim_state == anim_state::kDeathPungi); // the corpse pose held
    }

    // ---- LeaveCorpse (attrib 0x400000): the corpse never expires ----
    // [orig: the @0x4b9e54 skip]
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        w.registry.configure_pool(0, 4);
        Entity seed;
        seed.health = 0;
        seed.deathtime_ticks = 2;
        seed.leave_corpse = true;
        const EntityHandle h = w.registry.spawn(0, seed);
        auto ai_heap = std::make_unique<AiSystem>();
        AiSystem &ai = *ai_heap;
        soldier(ai);
        run_ticks(ai, w, 1, 40);
        Entity *ent = w.registry.get(h);
        CHECK(!ent->hidden);
        CHECK(ent->corpse_timer == 2); // the timer never even decrements
    }

    // ---- the local-player watch: a seen corpse holds at 62-tick retries ----
    // [orig: @0x4b9f77 Physics_RaycastTerrainAndSectors(corpse, player) CLEAR ->
    // +0x148 = 62 @0x4b9f83; no terrain/collision wired = clear]
    {
        auto w_heap = std::make_unique<World>();
        World &w = *w_heap;
        w.registry.configure_pool(0, 4);
        Entity seed;
        seed.health = 0;
        seed.deathtime_ticks = 3;
        const EntityHandle h = w.registry.spawn(0, seed);
        Entity player;
        player.health = 100;
        player.position = Vec3{10.0f, 0.0f, 0.0f};
        const EntityHandle ph = w.registry.spawn(0, player);
        w.cached.local_player = ph;
        auto ai_heap = std::make_unique<AiSystem>();
        AiSystem &ai = *ai_heap;
        soldier(ai);
        run_ticks(ai, w, 1, 30);
        Entity *ent = w.registry.get(h);
        CHECK(!ent->hidden);           // watched: never despawns
        CHECK(ent->corpse_timer > 0);  // parked on the 62-tick retry clock
        CHECK(ent->corpse_timer <= 62);
    }
}

void test_primary_body_blend_windows_keep_independent_playheads() {
    constexpr std::array<float, 10> kNormalWeights = {
        0.10000000149011612f, 0.20000000298023224f, 0.30000001192092896f,
        0.40000000596046448f, 0.5f, 0.60000002384185791f,
        0.70000004768371582f, 0.80000007152557373f, 0.90000009536743164f,
        1.0f,
    };
    constexpr std::array<float, 15> kLongWeights = {
        0.06666667014360428f, 0.13333334028720856f, 0.20000001788139343f,
        0.26666668057441711f, 0.33333334326744080f, 0.40000000596046448f,
        0.46666666865348816f, 0.53333336114883423f, 0.60000002384185791f,
        0.66666668653488159f, 0.73333334922790527f, 0.80000001192092896f,
        0.86666667461395264f, 0.93333333730697632f, 1.0f,
    };

    auto exercise = [](int target_state, const auto &expected_weights) {
        auto w = std::make_unique<World>();
        auto ai = std::make_unique<AiSystem>();
        TestSource src;
        src.clips = {anim_state::kIdle, target_state};
        ai->root_motion = &src;

        AiEntity *e = soldier(*ai);
        e->inf.clip_phase = 7;
        e->inf.begin_body_transition(target_state);

        CHECK(e->inf.anim_state == target_state);
        CHECK(e->inf.anim_prev == anim_state::kIdle);
        CHECK(e->inf.clip_phase == 0);
        CHECK(e->inf.anim_prev_clip_phase == 7);
        CHECK(e->inf.anim_blend_weight == 0.0f);

        TickContext ctx{};
        ctx.world = w.get();
        ctx.is_authority = false; // advance the body without an NPC selection pass
        for (int tick = 1; tick <= static_cast<int>(expected_weights.size()); ++tick) {
            ctx.logic_tick = static_cast<uint32_t>(tick);
            ai->tick(*w, ctx);
            CHECK(e->inf.clip_phase == tick);
            CHECK(e->inf.anim_prev_clip_phase == 7 + tick);
            CHECK(e->inf.anim_blend_weight == expected_weights[static_cast<size_t>(tick - 1)]);
        }

        // Once weight reaches 1, only the target playhead continues.
        const int blend_ticks = static_cast<int>(expected_weights.size());
        ctx.logic_tick = static_cast<uint32_t>(blend_ticks + 1);
        ai->tick(*w, ctx);
        CHECK(e->inf.clip_phase == blend_ticks + 1);
        CHECK(e->inf.anim_prev_clip_phase == 7 + blend_ticks);
        CHECK(e->inf.anim_blend_weight == 1.0f);
        CHECK(e->inf.anim_blend_step == 0.0f);
    };

    CHECK((infantry_anim_flags(anim_state::kIdle2) & 0x400u) == 0);
    exercise(anim_state::kIdle2, kNormalWeights);

    CHECK((infantry_anim_flags(anim_state::kWalkProneForward) & 0x400u) != 0);
    exercise(anim_state::kWalkProneForward, kLongWeights);
}

void test_primary_body_mid_blend_retarget_keeps_original_primary() {
    constexpr int kPrimary = anim_state::kIdle;
    constexpr int kFirstTarget = anim_state::kIdle2;
    constexpr int kReplacement = anim_state::kSit;

    auto w = std::make_unique<World>();
    auto ai = std::make_unique<AiSystem>();
    BlendProbeSource src;
    src.frames[kPrimary].dx = 100;
    src.frames[kPrimary].events = 0x4u;
    src.frames[kFirstTarget].dx = 200;
    src.frames[kFirstTarget].events = 0u;
    src.frames[kReplacement].dx = 300;
    src.frames[kReplacement].events = 0x8u;
    ai->root_motion = &src;

    AiEntity *e = soldier(*ai);
    e->inf.begin_body_transition(kFirstTarget);

    TickContext ctx{};
    ctx.world = w.get();
    ctx.is_authority = false;
    for (uint32_t tick = 1; tick <= 4; ++tick) {
        ctx.logic_tick = tick;
        ai->tick(*w, ctx);
    }
    CHECK(e->inf.anim_blend_weight == 0.40000000596046448f);
    CHECK(e->inf.anim_prev == kPrimary);
    CHECK(e->inf.anim_prev_clip_phase == 4);
    CHECK(e->inf.clip_phase == 4);
    CHECK(e->inf.last_events == 0u); // primary-only triggers are not inherited

    e->inf.begin_body_transition(kReplacement);
    CHECK(e->inf.anim_prev == kPrimary);
    CHECK(e->inf.anim_prev_clip_phase == 4);
    CHECK(e->inf.anim_state == kReplacement);
    CHECK(e->inf.clip_phase == 0);
    CHECK(e->inf.anim_blend_weight == 0.0f);

    const int32_t x_before = e->pos[0];
    ctx.logic_tick = 5;
    ai->tick(*w, ctx);
    CHECK(e->pos[0] - x_before == 120); // .9*A(100) + .1*C(300), not .9*B + .1*C
    CHECK(e->inf.anim_prev_clip_phase == 5);
    CHECK(e->inf.clip_phase == 1);
    CHECK(e->inf.anim_blend_weight == 0.1f);
    CHECK(e->inf.last_events == 0x8u); // target-only triggers survive unchanged
}

void test_death_during_blend_finishes_old_tuple_then_retargets() {
    constexpr int kPrimary = anim_state::kIdle;
    constexpr int kFirstTarget = anim_state::kIdle2;
    constexpr int kDeath = anim_state::kDeathBulletBase + 4;

    auto w = std::make_unique<World>();
    w->registry.configure_pool(0, 4);
    Entity seed;
    seed.health = 100;
    seed.health_max = 100;
    const EntityHandle handle = w->registry.spawn(0, seed);

    auto ai = std::make_unique<AiSystem>();
    BlendProbeSource src;
    src.frames[kPrimary].dx = 100;
    src.frames[kPrimary].events = 0x1u;
    src.frames[kFirstTarget].dx = 200;
    src.frames[kFirstTarget].events = 0x2u;
    src.frames[kDeath].dx = 300;
    src.frames[kDeath].events = 0x4u;
    ai->root_motion = &src;

    AiEntity *e = ai->at(ai->attach(handle));
    e->inf.active = true;
    e->health = 100;
    e->inf.begin_body_transition(kFirstTarget);

    TickContext ctx{};
    ctx.world = w.get();
    ctx.is_authority = false;
    for (uint32_t tick = 1; tick <= 4; ++tick) {
        ctx.logic_tick = tick;
        ai->tick(*w, ctx);
    }

    Entity *ent = w->registry.get(handle);
    ent->health = 0;
    ent->death_anim_state = kDeath;
    e->health = 0;
    const int32_t kill_x = e->pos[0];
    ctx.logic_tick = 5;
    ai->tick(*w, ctx);

    CHECK(e->pos[0] - kill_x == 150); // the existing A/B blend advances from .4 to .5
    CHECK(e->inf.last_events == 0x2u);
    CHECK(e->inf.anim_prev == kPrimary);
    CHECK(e->inf.anim_prev_clip_phase == 5);
    CHECK(e->inf.anim_state == kDeath);
    CHECK(e->inf.clip_phase == 0);
    CHECK(e->inf.anim_blend_weight == 0.0f);

    const int32_t blend_x = e->pos[0];
    ctx.logic_tick = 6;
    ai->tick(*w, ctx);
    CHECK(e->pos[0] - blend_x == 120); // the replacement starts as .9*A + .1*death
    CHECK(e->inf.last_events == 0x4u);
    CHECK(e->inf.anim_prev_clip_phase == 6);
    CHECK(e->inf.clip_phase == 1);
    CHECK(e->inf.anim_blend_weight == 0.1f);
}

int main() {
    test_slope_standing_camera_stays_level();
    test_slope_prone_body_conforms_org2();
    test_slope_pass_org1_selector_and_chase();
    // ---- body heading: quarter-step toward the target, clamped ±69273360/tick ----
    // [orig: 0x4b9910 dump 4600-4611 — step = (diff + 2) >> 2, clamp]
    {
        World w;
        AiSystem ai;
        AiEntity *e = soldier(ai);
        e->inf.target_heading = 0x40000000; // 90 deg: quarter-step would be 268435456 -> clamped
        run_ticks(ai, w, 1, 2);
        CHECK(e->inf.body_heading == kClamp);
        CHECK(e->heading == kClamp); // render yaw moves with the body
        run_ticks(ai, w, 2, 3);
        CHECK(e->inf.body_heading == 2 * kClamp);

        e->inf.target_heading = e->inf.body_heading + 1000; // small diff: exact quarter-step
        run_ticks(ai, w, 3, 4);
        CHECK(e->inf.target_heading - e->inf.body_heading == 1000 - 250);
    }
    {
        World w;
        AiSystem ai;
        AiEntity *e = soldier(ai);
        e->inf.target_heading = -0x40000000; // negative side clamps symmetrically
        run_ticks(ai, w, 1, 2);
        CHECK(e->inf.body_heading == -kClamp);
    }

    // ---- turn-in-place gates override the gait ----
    // [orig: dump 2940-2952 — err > 536870880 (45 deg) -> 147; > 357913920 (30 deg) -> 1]
    {
        World w;
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kWalkForward, anim_state::kRunForward, anim_state::kIdle,
                     anim_state::kStop};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        route(ai, e, {node(fx(500), 0, fx(1))}, 0);
        e->inf.alert_timer = 1;          // alerted: the gait would be run...
        e->inf.body_heading = 600000000; // ...but err > 45 deg forces stop
        e->heading = 600000000;
        run_ticks(ai, w, 0, 1);
        CHECK(e->inf.anim_state == anim_state::kStop);
    }
    {
        World w;
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kWalkForward, anim_state::kRunForward, anim_state::kIdle,
                     anim_state::kStop};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        route(ai, e, {node(fx(500), 0, fx(1))}, 0);
        e->inf.alert_timer = 1;          // alerted run...
        e->inf.body_heading = 400000000; // ...but 30 deg < err < 45 deg -> walk turn
        e->heading = 400000000;
        run_ticks(ai, w, 0, 1);
        CHECK(e->inf.anim_state == anim_state::kWalkForward);
    }

    // ---- gait selection: alert sources, wounded gaits, availability fallbacks ----
    // [orig: dump 2898-2906 alert; 3090-3151 wounded at def+380 >> 1; fallbacks 3010-3025]
    {
        World w;
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kWalkForward, anim_state::kRunForward, anim_state::kIdle,
                     anim_state::kStop, anim_state::kWoundedWalk, anim_state::kWoundedRun};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        route(ai, e, {node(fx(500), 0, fx(1))}, 0);

        run_ticks(ai, w, 0, 1); // think+select at t=0
        CHECK(e->inf.anim_state == anim_state::kWalkForward); // unalerted patrol walks

        e->inf.alert_timer = 1; // alert source 1: entity[190]
        run_ticks(ai, w, 1, 17);
        CHECK(e->inf.anim_state == anim_state::kRunForward);

        e->inf.alert_timer = 0;
        e->slot.bytes()[AiSlot::kMoveFlagByte] = 1; // alert source 2: slot byte +136
        run_ticks(ai, w, 17, 33);
        CHECK(e->inf.anim_state == anim_state::kRunForward);

        e->slot.bytes()[AiSlot::kMoveFlagByte] = 0;
        e->inf.combat_reaction = true; // alert source 3: byte entity+875
        run_ticks(ai, w, 33, 49);
        CHECK(e->inf.anim_state == anim_state::kRunForward);

        e->inf.combat_reaction = false;
        e->health = 50; // == max_health/2 -> wounded
        run_ticks(ai, w, 49, 65);
        CHECK(e->inf.anim_state == anim_state::kWoundedWalk);

        e->inf.alert_timer = 1; // wounded + alerted
        run_ticks(ai, w, 65, 81);
        CHECK(e->inf.anim_state == anim_state::kWoundedRun);

        src.clips.erase(anim_state::kWoundedRun); // availability fallback: 146 -> 149
        src.clips.erase(anim_state::kWoundedWalk);
        run_ticks(ai, w, 81, 97);
        CHECK(e->inf.anim_state == anim_state::kRunForward);

        e->inf.alert_timer = 0; // wounded walk falls back to the base gait
        run_ticks(ai, w, 97, 113);
        CHECK(e->inf.anim_state == anim_state::kWalkForward);
    }

    // ---- final-node approach jog windows ----
    // [orig: dump 2907-2924 — one-shot last node; dist gates 139264/270336, radius 139264]
    {
        World w;
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kWalkForward, anim_state::kRunForward, anim_state::kJogForward,
                     anim_state::kIdle};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        route(ai, e, {node(fx(3), 0, fx(1))}, 1); // dist 3u in (2.125u, 4.125u], radius 1u < 2.125u
        run_ticks(ai, w, 0, 1);
        CHECK(e->inf.anim_state == anim_state::kJogForward);
    }
    {
        World w; // jog clip missing -> falls back to run
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kWalkForward, anim_state::kRunForward, anim_state::kIdle};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        route(ai, e, {node(fx(3), 0, fx(1))}, 1);
        run_ticks(ai, w, 0, 1);
        CHECK(e->inf.anim_state == anim_state::kRunForward);
    }
    {
        World w; // far approach (dist > 4.125u) skips the jog entirely
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kWalkForward, anim_state::kRunForward, anim_state::kJogForward,
                     anim_state::kIdle};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        route(ai, e, {node(fx(5), 0, fx(1))}, 1);
        run_ticks(ai, w, 0, 1);
        CHECK(e->inf.anim_state == anim_state::kWalkForward);
    }
    {
        World w; // wide arrival radius (>= 2.125u) doesn't jog
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kWalkForward, anim_state::kRunForward, anim_state::kJogForward,
                     anim_state::kIdle};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        route(ai, e, {node(fx(4), 0, fx(3))}, 1);
        run_ticks(ai, w, 0, 1);
        CHECK(e->inf.anim_state == anim_state::kWalkForward);
    }

    // ---- one-shot route lifecycle: arrival, movetimer hold, end stop (exact) ----
    // [orig: dump 1464-1532 — relmat marks, hold (wait+8)>>4, one-shot end cooldown 20]
    {
        World w;
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kWalkForward, anim_state::kRunForward, anim_state::kJogForward,
                     anim_state::kIdle, anim_state::kStop};
        src.step = 0x2000; // 0.125u/tick: a think window (16 ticks) covers 2u exactly
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        // Preserve this lifecycle test's think phase: 36 * 8 is divisible by 16.
        e->net_id = 8;
        e->relmat_id = 4;
        // node0 at 3u holds 4s (248 ticks -> 16 thinks); node1 at 6u ends the path.
        route(ai, e,
              {node(fx(3), 0, fx(1), /*facing=*/0, /*wait=*/248), node(fx(6), 0, fx(1))}, 1);

        // The 15-tick idle->walk blend reaches the first marker on the t=32 think.
        // That tick starts the 10-tick walk->idle blend, whose retained primary root
        // carries the body a bounded distance beyond the radius edge.
        run_ticks(ai, w, 0, 33);
        CHECK(e->pos[0] == 212165);                 // first idle-blend sample included
        CHECK(e->inf.wait_cooldown == 16);          // (248 + 8) >> 4, stamped at t=32
        CHECK(e->slot.f[38] == 1);                  // advanced past node0
        CHECK(ai.relmat_calls.size() == 2);         // SetBitB + SetBitA at the arrival
        if (!ai.relmat_calls.empty())
            CHECK(ai.relmat_calls[0].channel == 1 && ai.relmat_calls[0].node == 0);
        CHECK(w.relations.group_visited(4, 1, 0));
        CHECK(w.relations.single_visited(8, 1, 0));

        run_ticks(ai, w, 33, 200); // mid-hold: standing in idle, cooldown draining
        CHECK(e->pos[0] == 241653);                 // walk->idle blend has settled
        CHECK(e->inf.anim_state == anim_state::kIdle);
        CHECK(e->inf.wait_cooldown == 6);

        run_ticks(ai, w, 200, 320); // hold expires, then blended walk/idle reaches node1
        CHECK(e->pos[0] == 372717);                 // bounded stop inside node1 radius
        CHECK(e->slot.f[38] == 1);                  // one-shot end pins the last node
        CHECK(e->inf.anim_state == anim_state::kIdle);
        CHECK(e->inf.wait_cooldown == 20);          // end-of-path cooldown
        CHECK(w.relations.group_visited(4, 1, 1));
        CHECK(w.relations.single_visited(8, 1, 1));

        const size_t marks = ai.relmat_calls.size();
        run_ticks(ai, w, 320, 1200); // parked: cooldown re-arms, never moves again
        CHECK(e->pos[0] == 372717);
        CHECK(e->pos[1] == 0);
        CHECK(ai.relmat_calls.size() > marks); // re-arrivals keep marking the matrix
    }

    // ---- movetimer facing: arrival turns the body to the marker's authored heading ----
    // [orig: dump 1468-1477 — entity[106] = marker+16 on a wait-marker arrival]
    {
        World w;
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kWalkForward, anim_state::kIdle, anim_state::kStop};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        route(ai, e, {node(fx(1), 0, fx(1), /*facing=*/0x20000000, /*wait=*/248)}, 1);
        // Arrives immediately (dist == radius), then turns in place. The quarter-step
        // covers 90 deg in ~4 clamped + ~65 shrinking ticks, settling within 2 BAM
        // (diff=1 -> step (1+2)>>2 = 0 is the converged fixed point).
        run_ticks(ai, w, 0, 120);
        CHECK(e->inf.target_heading == 0x20000000);
        CHECK(std::abs(e->heading - 0x20000000) <= 2);
        CHECK(e->pos[0] == 0 && e->pos[1] == 0);       // never walked
    }

    // ---- commit rules against the real flag table ----
    // [orig: dump 3693-3710 — flag&4 locked queues in entity[174]; flag&0x20 emote yields
    //  only to movement-flagged (bit0) targets. burn=111 (0x004), emote_1=115 (0x020).]
    {
        World w;
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kWalkForward, anim_state::kIdle};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        e->inf.anim_state = 111; // burn: locked
        run_ticks(ai, w, 0, 33); // two think rounds: the request stays queued
        CHECK(e->inf.anim_state == 111);
        CHECK(e->inf.anim_pending == anim_state::kIdle);
    }
    {
        World w;
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kWalkForward, anim_state::kIdle};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        e->inf.anim_state = 115; // emote_1
        run_ticks(ai, w, 0, 1);  // idle (no movement bit) cannot interrupt the emote
        CHECK(e->inf.anim_state == 115);
        CHECK(e->inf.anim_pending == anim_state::kIdle);

        route(ai, e, {node(fx(500), 0, fx(1))}, 0); // a movement target CAN interrupt
        run_ticks(ai, w, 1, 17);
        CHECK(e->inf.anim_state == anim_state::kWalkForward);
        CHECK(e->inf.anim_pending == 0);
        CHECK(e->inf.anim_prev == 115);
    }

    // ---- death edge: one-shot pose pick + freeze ----
    // [orig: Entity_UpdateInfantryAI @0x4b9c40 — death family flags 0x82 gate the
    // re-trigger; no staged +0x2C0 selection -> the generic 174 death_pungi]
    {
        World w;
        AiSystem ai; // no source: the witnessed generic selection stands
        AiEntity *e = soldier(ai);
        e->health = 0;
        run_ticks(ai, w, 1, 2);
        CHECK(e->inf.anim_state == anim_state::kDeathPungi);
        CHECK(e->inf.anim_prev == anim_state::kIdle);
        run_ticks(ai, w, 2, 34); // stable: 0x82 flags block a second death pick
        CHECK(e->inf.anim_state == anim_state::kDeathPungi);
    }
    {
        World w;
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kDeathBulletBase + 4}; // death_bullet_torso_forward
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        e->health = 0;
        run_ticks(ai, w, 1, 2);
        CHECK(e->inf.anim_state == anim_state::kDeathBulletBase + 4);
    }

    // ---- kJumpLoop forces forward delta 1024 ----  [orig: dump 4756]
    {
        World w;
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kJumpLoop};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        e->inf.anim_state = anim_state::kJumpLoop;
        run_ticks(ai, w, 1, 16); // 15 non-think ticks at heading 0
        CHECK(e->pos[0] == 15 * 1024);
        CHECK(e->pos[1] == 0);
    }

    // ---- no clip source: the selector holds and nothing moves ----
    {
        World w;
        AiSystem ai;
        AiEntity *e = soldier(ai);
        route(ai, e, {node(fx(500), 0, fx(1))}, 0);
        run_ticks(ai, w, 0, 50);
        CHECK(e->inf.anim_state == anim_state::kIdle);
        CHECK(e->pos[0] == 0);
    }

    // ---- gravity: -416 every 2 ticks to terminal -32768; landing + fall damage ----
    // [orig: dump 5088-5173 — pos.z += 2*vel_z; damage when vel_z <= -1057*scale,
    //  health -= excess >> 4 (dword_C6EAE4 scale, injectable)]
    {
        Field flat([](int) { return static_cast<uint16_t>(50 * 256); }); // 50u everywhere
        const int32_t floor_z = fx(50) + kFloorStand;

        World w;
        AiSystem ai;
        ai.terrain = &flat.field;
        ai.fall_damage_scale = 1;
        AiEntity *e = soldier(ai);
        e->pos[0] = fx(100);
        e->pos[1] = fx(100);
        e->pos[2] = fx(200); // 145u above the floor: reaches terminal velocity
        e->health = 30000;

        int32_t min_vel = 0;
        TickContext ctx;
        ctx.world = &w;
        ctx.is_authority = true;
        for (uint32_t t = 0; t < 600; ++t) {
            ctx.logic_tick = t;
            ai.tick(w, ctx);
            if (e->inf.vel[2] < min_vel) min_vel = e->inf.vel[2];
        }
        CHECK(min_vel == kTerminal);
        CHECK(e->pos[2] == floor_z);
        CHECK(e->inf.vel[0] == 0 && e->inf.vel[1] == 0 && e->inf.vel[2] == 0);
        CHECK(e->health == 30000 - ((-1057 - kTerminal) >> 4)); // 30000 - 1981
    }
    {
        Field flat([](int) { return static_cast<uint16_t>(50 * 256); });
        const int32_t floor_z = fx(50) + kFloorStand;

        World w; // a hop (2 gravity steps, vel -832 > -1057) lands without damage
        AiSystem ai;
        ai.terrain = &flat.field;
        ai.fall_damage_scale = 1;
        AiEntity *e = soldier(ai);
        e->pos[0] = fx(100);
        e->pos[1] = fx(100);
        e->pos[2] = floor_z + 2000;
        run_ticks(ai, w, 0, 10);
        CHECK(e->pos[2] == floor_z);
        CHECK(e->health == 100);
    }
    {
        Field flat([](int) { return static_cast<uint16_t>(50 * 256); });
        const int32_t floor_z = fx(50) + kFloorStand;

        World w; // scale 0 (the image default) disables fall damage entirely
        AiSystem ai;
        ai.terrain = &flat.field;
        ai.fall_damage_scale = 0;
        AiEntity *e = soldier(ai);
        e->pos[0] = fx(100);
        e->pos[1] = fx(100);
        e->pos[2] = fx(200);
        e->health = 100;
        run_ticks(ai, w, 0, 600);
        CHECK(e->pos[2] == floor_z);
        CHECK(e->health == 100);
    }

    // ---- gravity cadence: BOTH motors fall EVERY tick, asymmetric steps — the NPC
    //      (org1) at -416 with pos.z += 2*vel, the player (org2) at -208 with
    //      pos.z += vel. [orig: NPC @0x4bf7bf/@0x4bf7ec; player @0x4b7acf/@0x4b7cef;
    //      D-INF-10 closed for both legs]
    {
        Field ground0([](int) { return static_cast<uint16_t>(0); }); // ground at 0
        World w;
        AiSystem ai;
        ai.terrain = &ground0.field;

        soldier(ai); // index 0: NPC (is_local_player defaults false)
        soldier(ai); // index 1: player (attach may realloc the AI vector — set fields AFTER both)
        ai.at(0)->health = 100;
        ai.at(0)->pos[0] = fx(100); ai.at(0)->pos[1] = fx(100); ai.at(0)->pos[2] = fx(100); // high up
        ai.at(1)->health = 100;
        ai.at(1)->inf.is_local_player = true;
        ai.at(1)->pos[0] = fx(120); ai.at(1)->pos[1] = fx(120); ai.at(1)->pos[2] = fx(100);

        run_ticks(ai, w, 0, 1); // both stay airborne (100u up)
        CHECK(ai.at(0)->inf.vel[2] == -416); // NPC: one per-tick step
        CHECK(ai.at(1)->inf.vel[2] == -208); // player: the org2 half-step, same tick
        const int32_t npc_z = ai.at(0)->pos[2];
        const int32_t ply_z = ai.at(1)->pos[2];
        run_ticks(ai, w, 1, 2);
        CHECK(ai.at(0)->inf.vel[2] == -2 * 416);
        CHECK(ai.at(1)->inf.vel[2] == -2 * 208);
        CHECK(ai.at(0)->pos[2] == npc_z + 2 * (-2 * 416)); // pos.z += 2*vel (org1)
        CHECK(ai.at(1)->pos[2] == ply_z + (-2 * 208));     // pos.z += vel (org2)
    }

    // ---- slope pass through the motor: a live STANDING soldier holds steep ground —
    //      the conform selector routes him to the decay leg, so no slide impulse and
    //      no body lean ever build (the slide/lean live inside the conform branch;
    //      the conform legs themselves are pinned in the test_slope_* functions).
    //      [orig: selector @0x4ba10f -> decay @0x4ba133]
    {
        // Gradient 1 u/u along X (raw16 = x*256, capped); facing +X means uphill ahead.
        Field ramp([](int x) {
            int v = x * 256;
            return static_cast<uint16_t>(v > 65535 ? 65535 : v);
        });
        World w;
        AiSystem ai;
        ai.terrain = &ramp.field;
        AiEntity *e = soldier(ai);
        e->pos[0] = fx(100);
        e->pos[1] = fx(100);
        e->pos[2] = fx(100) + kFloorStand; // standing on the slope

        run_ticks(ai, w, 0, 80);
        CHECK(e->pos[0] == fx(100));               // no downhill drift
        CHECK(e->pos[1] == fx(100));
        CHECK(e->pos[2] == fx(100) + kFloorStand); // stays on the floor
        CHECK(e->inf.vel[0] == 0 && e->inf.vel[1] == 0);
        CHECK(e->pitch == 0);
        CHECK(e->body_pitch == 0);
        CHECK(e->roll == 0);
    }

    // ---- ground settle floors pos[2] to ground + the frame's capsule_bottom (origin->feet),
    //      NOT the death-fall mover's +0x50000 [orig: movement collision resolver
    //      @0x4b2bd0 settles entity[3]=entityRadius+ground @0x4b3da3; entityRadius = the .bad
    //      capsule_bottom*65536 via AnimMap_UpdateEntity @0x40b82f; D-INF-6]. ----
    {
        Field flat([](int) { return static_cast<uint16_t>(50 * 256); }); // 50u everywhere
        TestSource src;
        src.clips = {anim_state::kIdle};
        src.capsule_bottom = fx(1); // 1u origin->feet for the held idle pose

        World w;
        AiSystem ai;
        ai.terrain = &flat.field;
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        e->health = 100;
        e->inf.anim_state = anim_state::kIdle;
        e->pos[0] = fx(100);
        e->pos[1] = fx(100);
        e->pos[2] = fx(200); // dropped well above the floor

        // is_authority=false so think/select never retargets the held idle clip; the ground
        // clamp itself has no authority gate, so the soldier still settles.
        TickContext ctx;
        ctx.world = &w;
        ctx.is_authority = false;
        for (uint32_t t = 0; t < 600; ++t) {
            ctx.logic_tick = t;
            ai.tick(w, ctx);
        }

        CHECK(e->pos[2] == fx(50) + fx(1)); // ground + capsule_bottom, not ground + 0x50000
    }
    {
        Field flat([](int) { return static_cast<uint16_t>(50 * 256); });
        TestSource src;
        src.clips = {anim_state::kIdle};
        src.capsule_bottom = fx(1);

        World w;
        AiSystem ai;
        ai.terrain = &flat.field;
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        e->health = 100;
        e->inf.anim_state = anim_state::kIdle;
        e->pos[0] = fx(100);
        e->pos[1] = fx(100);
        e->pos[2] = fx(50) + fx(1) + 0x8000; // positive foot gap, but <= 0xF000

        run_ticks(ai, w, 1, 2);

        // Per-tick NPC gravity (D-INF-10) steps pos.z down one step, but the small positive
        // foot clearance (<= 0xF000) is otherwise left alone — NOT snapped to the floor, NOT airborne.
        CHECK(e->pos[2] == fx(50) + fx(1) + 0x8000 - 2 * 416);
        CHECK(e->pos[2] > fx(50) + fx(1)); // still above the floor (clearance not snapped)
        CHECK(!e->inf.airborne);
    }
    {
        Field flat([](int) { return static_cast<uint16_t>(50 * 256); });
        TestSource src;
        src.clips = {anim_state::kIdle};
        src.capsule_bottom = fx(1);

        World w;
        AiSystem ai;
        ai.terrain = &flat.field;
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        e->health = 100;
        e->inf.anim_state = anim_state::kIdle;
        e->pos[0] = fx(100);
        e->pos[1] = fx(100);
        e->pos[2] = fx(50) + fx(1) - 0x1000; // foot penetrates the ground

        run_ticks(ai, w, 1, 2);

        CHECK(e->pos[2] == fx(50) + fx(1)); // only negative/zero clearance lifts
    }
    {
        Field flat([](int) { return static_cast<uint16_t>(50 * 256); });
        TestSource src;
        src.clips = {anim_state::kIdle};
        src.capsule_bottom = fx(1);

        World w;
        AiSystem ai;
        ai.terrain = &flat.field;
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        e->health = 100;
        e->inf.anim_state = anim_state::kIdle;
        e->pos[0] = fx(100);
        e->pos[1] = fx(100);
        e->pos[2] = fx(50) + fx(1);
        e->inf.vel[0] = 64;
        e->inf.vel[1] = -64;

        run_ticks(ai, w, 1, 2);

        CHECK(e->inf.vel[0] != 0); // ground contact does not clear horizontal slide
        CHECK(e->inf.vel[1] != 0);
    }

    // ---- player slide damp: a GROUNDED player's horizontal slide decays by (63*v)>>6 each tick
    //      (org2 block A, no deadzone), so a slope-slide impulse settles instead of drifting
    //      forever — the player's slide was previously never damped. [orig: @0x4b7949;
    //      D-INF-9]
    {
        Field flat([](int) { return static_cast<uint16_t>(50 * 256); });
        TestSource src;
        src.clips = {anim_state::kIdle};
        src.capsule_bottom = fx(1);
        World w;
        AiSystem ai;
        ai.terrain = &flat.field;
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        e->inf.is_local_player = true;
        e->health = 100;
        e->inf.anim_state = anim_state::kIdle;
        e->pos[0] = fx(100); e->pos[1] = fx(100); e->pos[2] = fx(50) + fx(1);

        run_ticks(ai, w, 0, 1); // settle grounded
        CHECK(!e->inf.airborne);

        e->inf.vel[0] = 6400;
        e->inf.vel[1] = -6400;
        run_ticks(ai, w, 1, 2); // one grounded tick: org2 (63*v)>>6, NOT the NPC (7v+4)>>3 (=5600)
        CHECK(e->inf.vel[0] == (63 * 6400) >> 6);  // 6300
        CHECK(e->inf.vel[1] == (63 * -6400) >> 6); // -6300

        run_ticks(ai, w, 2, 300); // ...and it keeps decaying (the drift bug is fixed)
        CHECK(e->inf.vel[0] >= 0 && e->inf.vel[0] < 100);
        CHECK(e->inf.vel[1] <= 0 && e->inf.vel[1] > -100);
    }

    // ---- stance: crouch/prone select the stance gait + idle clips (player). The
    //      selection runs every 4th tick, so each stance flip advances a full 4-tick
    //      window. [orig: Entity_UpdateInfantryPlayerBody @0x4b40e0 — moving base
    //      1/11/19, idle 43/45/48; 4th-tick gate @0x4b70ce; the player body tests
    //      prone bit 0x100 before crouch bit 0x200]
    {
        World w;
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kWalkForward, anim_state::kRunForward, anim_state::kIdle,
                     anim_state::kStop, anim_state::kWalkCrouchForward, anim_state::kWalkProneForward,
                     anim_state::kIdleCrouch, anim_state::kIdleProne};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        e->inf.is_local_player = true;
        e->health = 100;

        // Local-player motion is driven by the raw-input player_moving flag, not the
        // NPC move_mode/target_dist (which apply_player_body_input clears). [a61a7e04]
        e->inf.player_moving = true; // moving forward
        e->inf.stance = InfantryState::Stance::kCrouch;
        run_ticks(ai, w, 0, 4);
        CHECK(e->inf.anim_state == anim_state::kWalkCrouchForward); // 11

        e->inf.stance = InfantryState::Stance::kProne;
        run_ticks(ai, w, 4, 8);
        CHECK(e->inf.anim_state == anim_state::kWalkProneForward); // 19

        e->inf.alert_timer = 16; // an org1 alert flag: the player selection ignores it
        e->inf.stance = InfantryState::Stance::kCrouch;
        run_ticks(ai, w, 8, 12);
        CHECK(e->inf.anim_state == anim_state::kWalkCrouchForward); // still crouch WALK (11)
        e->inf.alert_timer = 0;

        e->inf.player_moving = false; // stationary
        e->inf.stance = InfantryState::Stance::kCrouch;
        run_ticks(ai, w, 12, 16);
        CHECK(e->inf.anim_state == anim_state::kIdleCrouch); // 45

        e->inf.stance = InfantryState::Stance::kProne;
        run_ticks(ai, w, 16, 20);
        CHECK(e->inf.anim_state == anim_state::kIdleProne); // 48

        e->inf.stance = InfantryState::Stance::kStand;
        run_ticks(ai, w, 20, 24);
        CHECK(e->inf.anim_state == anim_state::kIdle); // 43
    }

    // ---- the run promotion: pure-forward standing walk promotes to run_3 (run_2 at
    //      tier 1), suppressed while scoped; strafe/back/stance gaits never promote.
    //      [orig: @0x4b729d-0x4b731b — tier = 2 (the dead +0x37C pitch term) +
    //      run_anim; scope Flags&0x10 @0x4b72e2; run_2 fallback @0x4b7311]
    {
        World w;
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kWalkForward, anim_state::kWalkForward + 4,
                     anim_state::kIdle, anim_state::kRun2, anim_state::kRun3,
                     anim_state::kWalkCrouchForward};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        e->inf.is_local_player = true;
        e->health = 100;

        e->inf.player_moving = true; // pure forward, standing
        run_ticks(ai, w, 0, 4);
        CHECK(e->inf.anim_state == anim_state::kRun3); // tier 2 -> run_3

        e->inf.scope_raised = true; // scope suppresses the promotion
        run_ticks(ai, w, 4, 8);
        CHECK(e->inf.anim_state == anim_state::kWalkForward);
        e->inf.scope_raised = false;

        e->inf.wpn_run_anim = -1; // tier 1 -> run_2 [orig: the tier==1 leg]
        run_ticks(ai, w, 8, 12);
        CHECK(e->inf.anim_state == anim_state::kRun2);
        e->inf.wpn_run_anim = 0;

        e->inf.player_move_dir_index = 4; // moving BACK: never promotes
        run_ticks(ai, w, 12, 16);
        CHECK(e->inf.anim_state == anim_state::kWalkForward + 4);
        e->inf.player_move_dir_index = 0;

        e->inf.stance = InfantryState::Stance::kCrouch; // crouch gait: never promotes
        run_ticks(ai, w, 16, 20);
        CHECK(e->inf.anim_state == anim_state::kWalkCrouchForward);
    }

    // ---- the lean chain: the angle ramps -/+0x3000000 per held key against the
    //      1/16-step decay; prone + lean selects the roll anims 41/42 (right wins).
    //      [orig: decay @0x4b5c97; ramp @0x4b7dbf/@0x4b7dd6; anims @0x4b731b-0x4b7354]
    {
        World w;
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kWalkForward, anim_state::kIdle, anim_state::kIdleProne,
                     anim_state::kRollLeft, anim_state::kRollRight};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        e->inf.is_local_player = true;
        e->health = 100;

        e->inf.lean_left = true; // standing lean: angle ramps negative
        run_ticks(ai, w, 0, 1);
        CHECK(e->inf.lean_angle == -0x3000000); // decay of 0, then one ramp step
        run_ticks(ai, w, 1, 2);
        // decay pulls 1/16 back before the second ramp step
        CHECK(e->inf.lean_angle < -0x3000000 && e->inf.lean_angle > -0x6000000);

        e->inf.lean_left = false; // released: decays toward 0
        for (int t = 2; t < 200; ++t) run_ticks(ai, w, t, t + 1);
        CHECK(e->inf.lean_angle > -0x100000 && e->inf.lean_angle <= 0);

        e->inf.stance = InfantryState::Stance::kProne; // prone: the ramp is gated off...
        e->inf.lean_right = true;
        const int32_t before = e->inf.lean_angle;
        run_ticks(ai, w, 200, 201);
        // ...so the angle only decays toward 0 (never ramps right).
        CHECK(e->inf.lean_angle >= before && e->inf.lean_angle <= 0);
        run_ticks(ai, w, 201, 205); // ...and the selection picks roll_right 42
        CHECK(e->inf.anim_state == anim_state::kRollRight);

        e->inf.lean_left = true; // both held: right wins [orig: 42 written last]
        run_ticks(ai, w, 205, 209);
        CHECK(e->inf.anim_state == anim_state::kRollRight);

        // The rolls are LOCKED states (flags 0x285, bit 0x4): a new target parks in
        // pending and lands on the clip-end promotion [orig: the @0x40b77b end-flag
        // path; commit arbitration @0x4b7356]. Give roll_right a finite length so
        // the pending roll_left promotes when it runs out.
        e->inf.lean_right = false; // left only -> roll_left 41 queued behind 42
        src.lengths[anim_state::kRollRight] = 30;
        run_ticks(ai, w, 209, 213);
        CHECK(e->inf.anim_pending == anim_state::kRollLeft);
        CHECK(e->inf.anim_state == anim_state::kRollRight);
        run_ticks(ai, w, 213, 245); // the playing roll reaches its end and promotes
        CHECK(e->inf.anim_state == anim_state::kRollLeft);
    }

    // ---- the torso roll: chases the slope roll (entity+0x18) a sixteenth-step per
    //      tick clamped to roll +-0x0E38E380 (20 deg); the combat rolls 41/42 freeze
    //      it; prone idle 48 decays it toward level.
    //      [orig: Entity_UpdateInfantryPlayerBody @0x4b5cff-0x4b5d6d]
    {
        World w;
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kIdle, anim_state::kIdleProne, anim_state::kRollLeft};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        e->inf.is_local_player = true;
        e->health = 100;

        e->roll = 0x04000000; // a slope roll inside the +-20 deg window: pure chase
        run_ticks(ai, w, 0, 1);
        CHECK(e->inf.torso_roll == 0x400000); // one sixteenth-step
        for (int t = 1; t < 200; ++t) run_ticks(ai, w, t, t + 1);
        CHECK(e->inf.torso_roll > 0x3F00000 && e->inf.torso_roll <= 0x4000000);

        // A slope jump far past the window: the clamp is on the LAG — torsoRoll
        // snaps to within 20 deg of roll immediately and eases the rest
        // [orig: delta = newTorso - roll vs +-0x0E38E380 @0x4b5d47/@0x4b5d5a].
        e->roll = -0x30000000;
        run_ticks(ai, w, 200, 201);
        CHECK(e->inf.torso_roll == -0x30000000 + 0x0E38E380);

        // The combat roll RAMPS torsoRoll -0x4000000 (5.625 deg) per tick — the FP
        // barrel-roll view [orig: @0x4b700e; the chase block skips 41/42
        // @0x4b5d20-0x4b5d28] (41 is a LOCKED state, so the selection parks its
        // own target in pending and the state holds).
        e->inf.anim_state = anim_state::kRollLeft;
        const int32_t at_roll_start = e->inf.torso_roll;
        run_ticks(ai, w, 201, 205);
        CHECK(e->inf.torso_roll == at_roll_start - 4 * 0x4000000);

        // Prone idle decays toward level regardless of the slope
        // [orig: @0x4b5d0a-0x4b5d16].
        e->inf.anim_state = anim_state::kIdleProne;
        e->inf.stance = InfantryState::Stance::kProne;
        for (int t = 205; t < 400; ++t) run_ticks(ai, w, t, t + 1);
        CHECK(e->inf.torso_roll > -0x100000 && e->inf.torso_roll <= 0);
    }

    // ---- stance availability fallback: a model with no crouch/prone clips uses stand siblings ----
    {
        World w;
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kWalkForward, anim_state::kIdle}; // no crouch/prone clips
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        e->inf.is_local_player = true;
        e->health = 100;
        e->inf.player_moving = true;
        e->inf.stance = InfantryState::Stance::kCrouch;
        run_ticks(ai, w, 0, 4);
        CHECK(e->inf.anim_state == anim_state::kWalkForward); // crouch-walk -> stand walk
        e->inf.stance = InfantryState::Stance::kProne;
        run_ticks(ai, w, 4, 8);
        CHECK(e->inf.anim_state == anim_state::kWalkForward); // prone-walk -> crouch -> stand walk
    }
    {
        World w;
        AiSystem ai;
        TestSource src;
        src.clips = {anim_state::kWalkForward, anim_state::kWalkCrouchForward,
                     anim_state::kIdle, anim_state::kIdleCrouch};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        e->health = 100;
        e->inf.is_local_player = false;
        e->inf.stance = InfantryState::Stance::kCrouch;
        route(ai, e, {node(fx(500), 0, fx(1))}, 0);

        run_ticks(ai, w, 0, 1);

        CHECK(e->inf.anim_state == anim_state::kWalkForward); // NPC stance is not player input
    }

    // ---- player jump: witnessed org2 — the jump block runs AFTER the vertical
    //      resolve, stamps jump_start 30 with jump_loop 31 queued (31 straight when
    //      the model has no 30), reloads the 32-tick cooldown, and holding the key
    //      never auto-repeats (the cooldown parks at 1 until release). [orig: gates
    //      @0x4b7e8c-0x4b7ebd; impulse @0x4b7ec3-0x4b7f06; cooldown @0x4b7de0-0x4b7e15]
    {
        Field flat([](int) { return static_cast<uint16_t>(50 * 256); });
        const int32_t floor_z = fx(50);
        World w;
        AiSystem ai;
        ai.terrain = &flat.field;
        TestSource src;
        src.clips = {anim_state::kWalkForward, anim_state::kIdle, anim_state::kJumpLoop};
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        e->inf.is_local_player = true;
        e->health = 100;
        e->pos[0] = fx(100); e->pos[1] = fx(100); e->pos[2] = floor_z;

        e->inf.player_moving = true;           // running jump: root step carries 3/4
        run_ticks(ai, w, 0, 2);               // settle on the ground first
        CHECK(!e->inf.airborne);
        CHECK(e->pos[2] == floor_z);

        e->inf.jump_requested = true;
        run_ticks(ai, w, 2, 3);               // the jump tick: impulse after the resolve
        CHECK(e->inf.airborne);
        CHECK(e->inf.vel[2] == 0x1600);       // the raw impulse; gravity bites next tick
        CHECK(e->inf.jump_cooldown == 32);    // reloaded [orig: @0x4b7f06]
        CHECK(e->inf.anim_state == anim_state::kJumpLoop); // stamped at the jump (no 30 clip)
        // Third 15-tick blend sample: trunc(0x4000 * 0.2000000179f) = 3276;
        // the jump carries three quarters of that current root step.
        CHECK(e->inf.vel[0] == 2457);
        run_ticks(ai, w, 3, 4);
        CHECK(e->inf.vel[2] == 0x1600 - 208); // org2 per-tick gravity
        CHECK(e->pos[2] > floor_z);           // rising

        // Model a genuinely HELD key: the host reapplies the level input before every
        // motor tick. The cooldown parks at 1 and the grounded player never auto-repeats.
        for (uint32_t t = 4; t < 100; ++t) {
            e->inf.jump_requested = true;
            run_ticks(ai, w, t, t + 1);
        }
        CHECK(!e->inf.airborne);
        CHECK(e->pos[2] == floor_z);
        CHECK(e->inf.jump_cooldown == 1);     // held-at-1 latch: no auto-repeat

        run_ticks(ai, w, 100, 101);           // release edge
        CHECK(e->inf.jump_cooldown == 0);

        // Prone bodies never jump. [orig: the var_10AC gate @0x4b7e99]
        e->inf.stance = InfantryState::Stance::kProne;
        e->inf.jump_requested = true;
        run_ticks(ai, w, 101, 102);
        CHECK(!e->inf.airborne);
        e->inf.stance = InfantryState::Stance::kStand;
    }

    // ---- idle root output is still entity root motion: the movement flag gates state commits,
    //      not position integration. [orig: AnimMap_UpdateEntity @0x40b82f produces root output;
    //      Entity_UpdateInfantryAI @0x4BF684 integrates it on the authoritative path without a
    //      g_animStateFlagsTable movement-bit gate.] ----
    {
        World w;
        AiSystem ai;
        // A source whose clip carries a forward step for EVERY state: the motor consumes it
        // even while the selected state is idle, matching the original's unconditional root
        // integration. Real idle clips may author zero mean root velocity; the gate is data.
        struct SwaySource : IRootMotionSource {
            bool has_clip(int, int) const override { return true; }
            int32_t clip_length_ticks(int, int) const override { return -1; }
            bool advance(int, int, int32_t &phase, RootMotionFrame &out) override {
                ++phase;
                out = RootMotionFrame{};
                out.dx = 0x2000;
                return true;
            }
        } src;
        ai.root_motion = &src;
        AiEntity *e = soldier(ai);
        e->inf.is_local_player = true;
        e->health = 100;
        e->pos[0] = fx(10);

        e->inf.move_mode = 0; e->inf.target_dist = 0; // idle: no move order
        run_ticks(ai, w, 0, 8);
        CHECK(e->inf.anim_state == anim_state::kIdle);
        CHECK(e->pos[0] == fx(10) + 8 * 0x2000);

        e->inf.player_moving = true; // forward: the every-clip source promotes to run_3
        run_ticks(ai, w, 8, 14);
        CHECK(e->inf.anim_state == anim_state::kRun3);
        CHECK(e->pos[0] > fx(10)); // a movement state DOES translate the same clip step
    }

    // ---- per-ADM root motion: each soldier uses its own clip set for both movement and
    //      capsule_bottom grounding. [orig: AnimMap_UpdateEntity @0x40b5f0 evaluates each
    //      entity's own anim map; the out-transform bottom becomes the on-foot floor] ----
    {
        Field flat([](int) { return static_cast<uint16_t>(50 * 256); });
        struct PerAdmSource : IRootMotionSource {
            std::array<std::set<int>, 2> clips;
            std::array<int32_t, 2> step = {0x1000, 0x3000};
            std::array<int32_t, 2> capsule = {fx(1), fx(3)};

            bool has_clip(int adm_id, int id) const override {
                if (adm_id < 0 || adm_id >= static_cast<int>(clips.size())) return false;
                return clips[static_cast<size_t>(adm_id)].count(id) != 0;
            }
            int32_t clip_length_ticks(int, int) const override { return -1; }
            bool advance(int adm_id, int id, int32_t &phase, RootMotionFrame &out) override {
                if (!has_clip(adm_id, id)) return false;
                ++phase;
                out = RootMotionFrame{};
                out.dx = step[static_cast<size_t>(adm_id)];
                out.capsule_bottom = capsule[static_cast<size_t>(adm_id)];
                return true;
            }
        } src;
        src.clips[0] = {anim_state::kWalkForward, anim_state::kIdle};
        src.clips[1] = {anim_state::kWalkCrouchForward, anim_state::kIdleCrouch};

        World w;
        AiSystem ai;
        ai.terrain = &flat.field;
        ai.root_motion = &src;
        soldier(ai);
        soldier(ai);
        AiEntity *stand = ai.at(0);
        AiEntity *crouch = ai.at(1);
        stand->handle = EntityHandle::make(0, 0);
        crouch->handle = EntityHandle::make(0, 1);

        stand->inf.is_local_player = true;
        stand->health = 100;
        stand->pos[0] = fx(100); stand->pos[1] = fx(100); stand->pos[2] = fx(50);
        stand->inf.adm_id = 0;
        stand->inf.player_moving = true;
        stand->inf.anim_state = anim_state::kWalkForward;

        crouch->inf.is_local_player = true;
        crouch->health = 100;
        crouch->pos[0] = fx(100); crouch->pos[1] = fx(100); crouch->pos[2] = fx(50);
        crouch->inf.adm_id = 1;
        crouch->inf.stance = InfantryState::Stance::kCrouch;
        crouch->inf.player_moving = true;
        crouch->inf.anim_state = anim_state::kWalkCrouchForward;

        run_ticks(ai, w, 0, 3);

        CHECK(stand->inf.anim_state == anim_state::kWalkForward);
        CHECK(crouch->inf.anim_state == anim_state::kWalkCrouchForward);
        CHECK(stand->pos[0] == fx(100) + 3 * src.step[0]);
        CHECK(crouch->pos[0] == fx(100) + 3 * src.step[1]);
        CHECK(stand->pos[2] == fx(50) + src.capsule[0]);
        CHECK(crouch->pos[2] == fx(50) + src.capsule[1]);
    }

    // ---- body-anim slot mapping: the motor's clip state -> present-pass BodyAnim slot ----
    {
        CHECK(body_anim_slot_from_state(anim_state::kIdle) == kBodyAnimIdle);
        CHECK(body_anim_slot_from_state(anim_state::kWalkForward) == kBodyAnimWalkForward);
        CHECK(body_anim_slot_from_state(anim_state::kJogForward) == kBodyAnimJogForward);
        CHECK(body_anim_slot_from_state(anim_state::kRunForward) == kBodyAnimRunForward);
        CHECK(body_anim_slot_from_state(anim_state::kWoundedRun) == kBodyAnimRunForward);
        CHECK(body_anim_slot_from_state(anim_state::kDeathFire) == kBodyAnimIdle); // unmapped -> idle
    }

    // ---- D-INF-4 closed: the witnessed direction-table generator ----
    // [orig: Math_BuildSinTable @ 0x613050] builds ONE 1281-entry sin table at
    // 2^22 by ACCUMULATING the step dbl_7DF578 = 0.006135923151542565 per entry
    // (ftol2_sse truncation); cos reads the same table +256 entries
    // (off_849934 = outMillis + 0x400). Pin: the accumulated build is
    // integer-identical to the closed form trunc(sin(i*2pi/1024)*2^22) for
    // every entry, and the landmark values hold (a wrong step, scale, count,
    // or a rounding "fix" flips this red).
    {
        double angle = 0.0;
        constexpr double kStep = 0.006135923151542565; // [orig: dbl_7DF578]
        bool all_equal = true;
        int32_t landmark_0 = 0, landmark_256 = 0, landmark_512 = 0, landmark_768 = 0;
        for (int i = 0; i < 1281; ++i) {
            const int32_t acc = static_cast<int32_t>(std::sin(angle) * 4194304.0);
            const double closed = static_cast<double>(i) * (6.283185307179586476925 / 1024.0);
            const int32_t mul = static_cast<int32_t>(std::sin(closed) * 4194304.0);
            if (acc != mul) all_equal = false;
            if (i == 0) landmark_0 = acc;
            if (i == 256) landmark_256 = acc;
            if (i == 512) landmark_512 = acc;
            if (i == 768) landmark_768 = acc;
            angle += kStep;
        }
        CHECK(all_equal); // accumulated == closed form at integer truncation, every entry
        CHECK(landmark_0 == 0);          // sin(0)
        CHECK(landmark_256 == 4194304);  // sin(pi/2) rounds to exactly 1.0 in double
        CHECK(landmark_512 == 0);        // sin(pi) truncates toward zero
        CHECK(landmark_768 == -4194304); // sin(3pi/2)
    }

    // Was defined but never invoked (a silently-dead test) — called since the leg-chase
    // change landed alongside it.
    test_remote_player_body_anim();
    test_recoil_and_weapon_weight_kernels();
    test_hurt_volume_updates_registry_health();
    test_registry_max_health_drives_wounded_gait();
    test_player_body_chase_and_legs();
    test_player_body_chase_crosses_the_bam_seam();
    test_npc_ledge_fall_keeps_clip();
    test_dead_player_ledge_fall_edge_is_suppressed();
    test_player_idle_skip_throttle_no_bounce();
    test_player_weapon_channel();
    test_player_weapon_hold_kinds();
    test_player_weapon_attack_stamp();
    test_player_arms_dip();
    test_player_weapon_channel_ticks_while_dead();
    test_weapon_channel_consumer_gate_and_switch_identity();
    test_death_presentation();
    test_primary_body_blend_windows_keep_independent_playheads();
    test_primary_body_mid_blend_retarget_keeps_original_primary();
    test_death_during_blend_finishes_old_tuple_then_retargets();

    if (failures == 0) std::printf("infantry_test: OK\n");
    else std::printf("infantry_test: %d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
