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
//   * gravity -416/2t to terminal -32768, landing snap + fall damage excess>>4 with the
//     injectable scale [orig: dword_C6EAE4],
//   * slope slide on steep ground: the exact 2048/8-tick downhill drift + body lean.
#include <cmath>
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
    motor->health = 1;
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
}

// Local-player body chase + leg-chain re-plant [orig: §3.3; consumed by the render
// overlay, Entity_BuildBoneTransformMatrices @0x4b1290 / world-wac-ai-re.md §14; the
// org2 chase source is unwitnessed — org1 math applied under D-INF-12]. Own function:
// main's frame is at its MSVC stack-probe limit (see test_remote_player_body_anim).

void test_player_body_chase_crosses_the_bam_seam() {
    // The chase near +/-180 deg: the wrapping (diff + 2) >> 2 quarter-step takes the
    // SHORT arc across the seam (io/bam.h wrap semantics — signed overflow would be
    // UB), so a body at ~+180 deg chasing a target just past -180 deg crosses the
    // seam instead of sweeping the long way around.
    World w;
    AiSystem ai;
    TestSource src;
    src.clips.insert(anim_state::kIdle);
    src.clips.insert(anim_state::kIdle2);
    ai.root_motion = &src;
    AiEntity *e = soldier(ai);
    e->inf.is_local_player = true;
    e->health = 100;

    const int32_t start = 0x7FFF0000;             // ~ +180 deg, just below the seam
    const int32_t target = (int32_t)0x80020000u;  // just past -180: +0x30000 the short way
    e->inf.body_heading = start;
    e->inf.target_heading = target;
    e->inf.leg_yaw[0] = e->inf.leg_yaw[1] = start;
    e->inf.leg_target[0] = e->inf.leg_target[1] = start;

    run_ticks(ai, w, 1, 2);
    // One quarter-step of the short arc, still on the positive side of the seam.
    CHECK(e->inf.body_heading == start + 0xC000);
    CHECK(e->heading == target);  // player aim yaw is instant

    run_ticks(ai, w, 2, 400);
    // Settled ACROSS the seam at the target (the chase stalls within one BAM unit),
    // legs re-planted and settled with it.
    CHECK(opennova::io::bam_abs(opennova::io::bam_sub(e->inf.body_heading, target)) <= 1);
    CHECK(opennova::io::bam_abs(opennova::io::bam_sub(e->inf.leg_yaw[0], e->inf.body_heading)) < 0x20000000);
}

void test_player_body_chase_and_legs() {
    constexpr int32_t kClampL = 69273360;     // body turn clamp
    constexpr int32_t kLegTwist = 0x20000000; // 45 deg

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
    // Aim/render yaw is INSTANT for the player; the body lags at the clamped quarter-step.
    CHECK(e->heading == 0x40000000);
    CHECK(e->inf.body_heading == kClampL);
    // Legs: the body has only moved ~5.8 deg — under the ~30 deg snap and off the 64-tick
    // window, the re-plant holds and the legs stay planted at 0.
    CHECK(e->inf.leg_yaw[0] == 0 && e->inf.leg_yaw[1] == 0);

    // Keep turning: once the body outruns the 45-deg twist limit the legs clamp to
    // body - 45 deg even before a re-plant fires.
    run_ticks(ai, w, 2, 12);
    const int32_t body = e->inf.body_heading;
    CHECK(body > kLegTwist); // the body has swung past 45 deg by now
    CHECK(e->inf.leg_yaw[0] >= body - kLegTwist);
    CHECK(e->inf.leg_yaw[1] >= body - kLegTwist);

    // Long settle: the body reaches the aim (the (diff + 2) >> 2 chase stalls one BAM
    // unit short — step rounds to 0 at |diff| <= 1) and the legs re-plant + chase to
    // within the ~5 deg hysteresis FLOOR of the body: sub-floor drift never re-plants
    // (the witnessed rest state), so the legs settle near, not on, the body heading.
    run_ticks(ai, w, 12, 400);
    CHECK(std::abs(e->inf.body_heading - 0x40000000) <= 1);
    CHECK(std::abs(e->inf.leg_target[0] - e->inf.body_heading) < 59652320);
    CHECK(std::abs(e->inf.leg_target[1] - e->inf.body_heading) < 59652320);
    CHECK(std::abs(e->inf.leg_yaw[0] - e->inf.leg_target[0]) <= 1); // chase settled
    CHECK(std::abs(e->inf.leg_yaw[1] - e->inf.leg_target[1]) <= 1);

    // Small look-around (< 5 deg drift once the body follows): the legs never budge —
    // the re-plant hysteresis floor. 4 deg = 47721856 BAM.
    const int32_t planted_r = e->inf.leg_yaw[0];
    const int32_t planted_l = e->inf.leg_yaw[1];
    e->inf.target_heading = 0x40000000 + 47721856;
    run_ticks(ai, w, 400, 500);
    CHECK(std::abs(e->inf.body_heading - e->inf.target_heading) <= 1);
    CHECK(e->inf.leg_yaw[0] == planted_r);
    CHECK(e->inf.leg_yaw[1] == planted_l);
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
    run_ticks(ai, w, 1, 4);
    CHECK(e->inf.anim_state == anim_state::kIdle);
    CHECK(e->inf.wpn_state == anim_state::kIdle);
    e->inf.player_moving = true;
    run_ticks(ai, w, 4, 8);
    CHECK(e->inf.anim_state == anim_state::kWalkForward);
    CHECK(e->inf.wpn_state == anim_state::kWalkForward);
    CHECK(e->inf.wpn_clip_phase > 0); // the secondary playhead advances on its own

    // The refill stamps the 80-tick window -> the channel wants 65 reload; idle/walk
    // currents are not locked, so the stamp lands immediately [orig: @0x4b5e67/@0x4b5e9d].
    e->inf.reload_anim_ticks = 80; // [orig: WeaponSlot_ReloadAmmo @0x54173c]
    run_ticks(ai, w, 8, 9);
    CHECK(e->inf.wpn_state == anim_state::kReload);
    CHECK(e->inf.wpn_clip_phase == 1);          // fresh channel re-init + first advance
    CHECK(e->inf.anim_state == anim_state::kWalkForward); // the legs keep locomotion

    // While the window runs, the desire holds; the primary is untouched.
    run_ticks(ai, w, 9, 40);
    CHECK(e->inf.wpn_state == anim_state::kReload);
    CHECK(e->inf.reload_anim_ticks == 80 - 32);

    // The clip ends (40 phase ticks) BEFORE the window does: 65 is locked (flag 0x84),
    // so the mirror desire defers, and the deferred state only lands once BOTH the
    // window has expired (desire leaves 65) and the clip end promotes it
    // [orig: defer @0x4b5e88; promote @0x40b77b].
    run_ticks(ai, w, 40, 88);
    CHECK(e->inf.reload_anim_ticks == 0);
    CHECK(e->inf.wpn_state == anim_state::kWalkForward); // promoted back to the mirror
    CHECK(e->inf.wpn_deferred == 0);

    // Window expiring MID-CLIP: re-stamp, then cut it short after 10 ticks — the locked
    // reload keeps playing to its own end, the mirror desire waits in the deferred slot.
    e->inf.reload_anim_ticks = 80;
    run_ticks(ai, w, 88, 89);
    CHECK(e->inf.wpn_state == anim_state::kReload);
    e->inf.reload_anim_ticks = 10;
    run_ticks(ai, w, 89, 99); // window over, clip at ~11/40
    CHECK(e->inf.reload_anim_ticks == 0);
    CHECK(e->inf.wpn_state == anim_state::kReload);          // still locked in
    CHECK(e->inf.wpn_deferred == anim_state::kWalkForward);  // the exit is queued
    run_ticks(ai, w, 99, 89 + 41); // ...until the clip's 40 phase ticks complete
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

    uint32_t tick = 1;
    auto step = [&](int n) { run_ticks(ai, w, tick, tick + n); tick += n; };

    // Kinds 1-4: fixed holds 50-53; the scope flag is ignored [orig: lea eax,[ecx+31h]
    // @0x4b5dc5/0x4b5dd2/0x4b5ddc/0x4b5de6].
    static constexpr int kFixedHold[4] = {anim_state::kHoldKnife, anim_state::kHoldPistol,
                                          anim_state::kHoldGrenade, anim_state::kHoldStinger};
    for (int kind = 1; kind <= 4; ++kind) {
        e->inf.wpn_hold_kind = kind;
        e->inf.scope_raised = (kind & 1) != 0; // must not matter for 1-4
        step(1);
        CHECK(e->inf.wpn_state == kFixedHold[kind - 1]);
    }
    // Kinds 5-8: 54/56/58/60, +1 scoped [orig: test Flags&0x10 @0x4b5df0..0x4b5e35].
    static constexpr int kScopedHold[4] = {anim_state::kHoldDesignator, anim_state::kHoldP90,
                                           anim_state::kHoldMP7, anim_state::kHoldJavelin};
    for (int kind = 5; kind <= 8; ++kind) {
        e->inf.wpn_hold_kind = kind;
        e->inf.scope_raised = false;
        step(1);
        CHECK(e->inf.wpn_state == kScopedHold[kind - 5]);
        e->inf.scope_raised = true;
        step(1);
        CHECK(e->inf.wpn_state == kScopedHold[kind - 5] + 1);
    }
    // Rifle (kind 0) + scope: the mirror default coerces to 49 idle_3; dropping the
    // scope returns the mirror [orig: @0x4b5e48..0x4b5e4e].
    e->inf.wpn_hold_kind = 0;
    e->inf.scope_raised = true;
    step(1);
    CHECK(e->inf.wpn_state == anim_state::kIdle3);
    e->inf.scope_raised = false;
    step(1);
    CHECK(e->inf.wpn_state == e->inf.anim_state);

    // Binoculars override the hold pose [orig: @0x4b5e53]; the reload window overrides
    // binoculars, and the pistol kind (2) selects reload2 [orig: @0x4b5e5e..0x4b5e6f].
    e->inf.wpn_hold_kind = 2;
    e->inf.binoculars_raised = true;
    step(1);
    CHECK(e->inf.wpn_state == anim_state::kBinoculars);
    e->inf.reload_anim_ticks = 80;
    step(1);
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
    e->inf.wpn_hold_kind = 1; // knife family

    run_ticks(ai, w, 1, 3);
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
    run_ticks(ai, w, 3, 4);
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
    run_ticks(ai, w, 4, 40);
    CHECK(e->inf.wpn_state == anim_state::kHoldKnife);

    // The grenade kind stamps 63 [orig: @0x542be0].
    e->inf.wpn_hold_kind = 3;
    infantry_weapon_attack_stamp(e->inf, 2);
    CHECK(e->inf.wpn_state == anim_state::kGrenadeAttack);
}

// The arms-dip feed [orig: @0x4b5cab..0x4b5ce7]: while the window runs the head-look
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
    CHECK(e->inf.head_look_decay == expected);
    CHECK(e->inf.arms_dip_ticks == 18);

    run_ticks(ai, w, 2, 11); // 9 more ticks: the window drains at 2/tick
    CHECK(e->inf.arms_dip_ticks == 0);
    CHECK(e->inf.head_look_decay < d); // accumulated deeper than a single tick's dip

    run_ticks(ai, w, 11, 200); // the eighth-step ease settles back near rest
    CHECK(opennova::io::bam_abs(e->inf.head_look_decay) <= 8);
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
    e->inf.head_look_decay = -1000;

    run_ticks(ai, w, 1, 2);
    CHECK(e->inf.anim_state == anim_state::kDeathFire);
    CHECK(e->inf.wpn_state == anim_state::kReload);
    CHECK(e->inf.wpn_clip_phase == 5);
    CHECK(e->inf.reload_anim_ticks == 2);
    CHECK(e->inf.arms_dip_ticks == 2);
    CHECK(e->inf.head_look_decay != -1000);
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

} // namespace

int main() {
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
        // node0 at 3u holds 4s (248 ticks -> 16 thinks); node1 at 6u ends the path.
        route(ai, e,
              {node(fx(3), 0, fx(1), /*facing=*/0, /*wait=*/248), node(fx(6), 0, fx(1))}, 1);

        // Selection happens before root evaluation: walking covers t=0..15 -> 2u, and
        // the t=16 think arrives because the authored radius is 1u around the 3u marker.
        run_ticks(ai, w, 0, 33);
        CHECK(e->pos[0] == fx(2));
        CHECK(e->inf.wait_cooldown == 15);          // (248 + 8) >> 4, decremented at t=32
        CHECK(e->slot.f[38] == 1);                  // advanced past node0
        CHECK(ai.relmat_calls.size() == 2);         // SetBitB + SetBitA at the arrival
        if (!ai.relmat_calls.empty())
            CHECK(ai.relmat_calls[0].channel == 1 && ai.relmat_calls[0].node == 0);

        run_ticks(ai, w, 33, 200); // mid-hold: standing in idle, cooldown draining
        CHECK(e->pos[0] == fx(2));
        CHECK(e->inf.anim_state == anim_state::kIdle);
        CHECK(e->inf.wait_cooldown > 0);

        run_ticks(ai, w, 200, 320); // hold expires at t=288; walks the remaining 2u by t=304
        CHECK(e->pos[0] == fx(6));                  // resting exactly on node1
        CHECK(e->slot.f[38] == 1);                  // one-shot end pins the last node
        CHECK(e->inf.anim_state == anim_state::kIdle);
        CHECK(e->inf.wait_cooldown == 20);          // end-of-path cooldown

        const size_t marks = ai.relmat_calls.size();
        run_ticks(ai, w, 320, 1200); // parked: cooldown re-arms, never moves again
        CHECK(e->pos[0] == fx(6));
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
    // [orig: dump 751-913 — death family flags 0x82 gate the re-trigger]
    {
        World w;
        AiSystem ai; // no source: falls back to kDeathFire
        AiEntity *e = soldier(ai);
        e->health = 0;
        run_ticks(ai, w, 1, 2);
        CHECK(e->inf.anim_state == anim_state::kDeathFire);
        CHECK(e->inf.anim_prev == anim_state::kIdle);
        run_ticks(ai, w, 2, 34); // stable: 0x82 flags block a second death pick
        CHECK(e->inf.anim_state == anim_state::kDeathFire);
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

    // ---- gravity cadence: the NPC (org1) falls EVERY tick; the player (org2) keeps the 2-tick
    //      discretization. [orig: NPC -416/tick @0x4bf7bf; player -416 every 2 ticks; D-INF-10]
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

        run_ticks(ai, w, 0, 2); // ticks 0 (even) and 1 (odd); both stay airborne (100u up)
        CHECK(ai.at(0)->inf.vel[2] == -2 * 416); // NPC: two gravity steps -> per-tick fall
        CHECK(ai.at(1)->inf.vel[2] == -416);     // player: one gravity step -> 2-tick discretization
    }

    // ---- slope slide: steep ground drifts the soldier downhill + leans the body ----
    // [orig: dump 930-1000 — probes ±22528-dir; pitch slope <<14 vs threshold 0x22222200;
    //  slide (cos|sin)<<11>>22 = 2048 per 8-tick pass at heading 0; lean eighth-step]
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

        // First pass (t=0), pinned exactly: probes at ±0.34375u see a 0.6875u rise ->
        // pitch slope 45056<<14 = 738197504, clamped to 656175520, over the 0x22222200
        // threshold -> vel gains -2048 along the facing ((cos<<11)>>22), then the
        // NPC horizontal decay from 0x4b9910 damps it before integration.
        // The lean chases the clamped slope by an eighth-step: pitch = (656175520 + 4) >> 3.
        run_ticks(ai, w, 0, 8);
        CHECK(e->pos[0] < fx(100) - 2048);
        CHECK(e->pos[1] == fx(100));    // no roll component on an X-only ramp
        CHECK(e->pos[2] == fx(100) + kFloorStand); // landed back on the (cached) floor
        CHECK(e->inf.vel[0] < 0 && e->inf.vel[2] == 0);
        CHECK(e->pitch == (656175520 + 4) >> 3);
        CHECK(e->roll == 0);

        // Long run: slide velocity persists through the IDA decay path, so assert the
        // qualitative shape rather than a hand-derived trajectory.
        run_ticks(ai, w, 8, 80);
        CHECK(e->pos[0] < fx(100) - 8 * 2048);  // kept sliding downhill
        CHECK(e->pos[0] > fx(92));              // ...at a bounded rate
        CHECK(e->pos[1] == fx(100));
        CHECK(e->pitch > (656175520 + 4) >> 3); // lean keeps growing...
        CHECK(e->pitch <= 656175520);           // ...never past the clamped slope
        CHECK(e->pos[2] < fx(100) + kFloorStand);        // followed the ground down
        CHECK(e->pos[2] > fx(90) + kFloorStand);
    }

    // ---- ground settle floors pos[2] to ground + the frame's capsule_bottom (origin->feet),
    //      NOT the death-fall mover's +0x50000 [orig: Entity_ProcessCollisionAndPlatformPhysics
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

    // ---- stance: crouch/prone remap the gait + idle to the stance clips (player) ----
    // [orig: Entity_UpdateInfantryPlayerBody @0x4b40e0 — moving base 1/11/19, idle 43/45/48;
    //  player body tests prone bit 0x100 before crouch bit 0x200]
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
        run_ticks(ai, w, 0, 1);
        CHECK(e->inf.anim_state == anim_state::kWalkCrouchForward); // 11

        e->inf.stance = InfantryState::Stance::kProne;
        run_ticks(ai, w, 1, 2);
        CHECK(e->inf.anim_state == anim_state::kWalkProneForward); // 19

        e->inf.alert_timer = 16; // alerted -> run gait, but crouch has no run clip...
        e->inf.stance = InfantryState::Stance::kCrouch;
        run_ticks(ai, w, 2, 3);
        CHECK(e->inf.anim_state == anim_state::kWalkCrouchForward); // ...still crouch WALK (11)
        e->inf.alert_timer = 0;

        e->inf.player_moving = false; // stationary
        e->inf.stance = InfantryState::Stance::kCrouch;
        run_ticks(ai, w, 3, 4);
        CHECK(e->inf.anim_state == anim_state::kIdleCrouch); // 45

        e->inf.stance = InfantryState::Stance::kProne;
        run_ticks(ai, w, 4, 5);
        CHECK(e->inf.anim_state == anim_state::kIdleProne); // 48

        e->inf.stance = InfantryState::Stance::kStand;
        run_ticks(ai, w, 5, 6);
        CHECK(e->inf.anim_state == anim_state::kIdle); // 43
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
        run_ticks(ai, w, 0, 1);
        CHECK(e->inf.anim_state == anim_state::kWalkForward); // crouch-walk -> stand walk
        e->inf.stance = InfantryState::Stance::kProne;
        run_ticks(ai, w, 1, 2);
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

    // ---- player jump: a grounded jump request launches the vel_z impulse + jump-loop clip,
    //      then gravity brings it back to the floor. [orig: @0x4b7ee5 vel_z=0x1600 + in-air;
    //      jump_loop 31; gravity -416/2t] ----
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

        run_ticks(ai, w, 0, 2);               // settle on the ground first
        CHECK(!e->inf.airborne);
        CHECK(e->pos[2] == floor_z);

        e->inf.jump_requested = true;
        run_ticks(ai, w, 2, 3);               // the jump tick
        CHECK(e->inf.airborne);
        CHECK(e->inf.vel[2] == 0x1600 - 416); // launch impulse minus one gravity step
        CHECK(e->inf.anim_state == anim_state::kJumpLoop);
        CHECK(e->pos[2] > floor_z);           // rose off the ground

        run_ticks(ai, w, 3, 400);             // ...arcs up and lands
        CHECK(!e->inf.airborne);
        CHECK(e->pos[2] == floor_z);
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

        e->inf.player_moving = true; // walk forward
        run_ticks(ai, w, 8, 14);
        CHECK(e->inf.anim_state == anim_state::kWalkForward);
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
    test_hurt_volume_updates_registry_health();
    test_player_body_chase_and_legs();
    test_player_body_chase_crosses_the_bam_seam();
    test_player_weapon_channel();
    test_player_weapon_hold_kinds();
    test_player_weapon_attack_stamp();
    test_player_arms_dip();
    test_player_weapon_channel_ticks_while_dead();
    test_weapon_channel_consumer_gate_and_switch_identity();

    if (failures == 0) std::printf("infantry_test: OK\n");
    else std::printf("infantry_test: %d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
