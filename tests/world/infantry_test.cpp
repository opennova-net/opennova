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
#include <cstdint>
#include <cstdio>
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
constexpr int32_t kFloorStand = 0x50000;   // AiSystem::ground_stand_offset default

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

// Synthetic clip source: a configurable clip set; gait clips move `step` forward per
// tick, everything else plays without root motion.
struct TestSource : IRootMotionSource {
    std::set<int> clips;
    int32_t step = 0x4000;

    static bool gait(int id) {
        return id == anim_state::kWalkForward || id == anim_state::kRunForward ||
               id == anim_state::kJogForward || id == anim_state::kWoundedWalk ||
               id == anim_state::kWoundedRun;
    }
    bool has_clip(int id) const override { return clips.count(id) != 0; }
    bool advance(int id, int32_t &phase, RootMotionFrame &out) override {
        if (clips.count(id) == 0) return false;
        ++phase;
        out = RootMotionFrame{};
        if (gait(id)) out.dx = step;
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

        // Motion lags the order by one tick (each tick's root frame is fetched from the
        // current clip before think/select switch it): walking covers t=1..16 -> 2u, the
        // t=32 think arrives at node0 (pos 3.875u, dist 0.875u <= radius), and that
        // tick's already-fetched walk frame parks the soldier at exactly 4u.
        run_ticks(ai, w, 0, 33);
        CHECK(e->pos[0] == fx(4));
        CHECK(e->inf.wait_cooldown == 16);          // (248 + 8) >> 4
        CHECK(e->slot.f[38] == 1);                  // advanced past node0
        CHECK(ai.relmat_calls.size() == 2);         // SetBitB + SetBitA at the arrival
        if (!ai.relmat_calls.empty())
            CHECK(ai.relmat_calls[0].channel == 1 && ai.relmat_calls[0].node == 0);

        run_ticks(ai, w, 33, 200); // mid-hold: standing in idle, cooldown draining
        CHECK(e->pos[0] == fx(4));
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
        // threshold -> vel gains -2048 along the facing ((cos<<11)>>22), integrates once,
        // and the same tick's ground contact zeroes it. The lean chases the clamped slope
        // by an eighth-step: pitch = (656175520 + 4) >> 3.
        run_ticks(ai, w, 0, 8);
        CHECK(e->pos[0] == fx(100) - 2048);
        CHECK(e->pos[1] == fx(100));    // no roll component on an X-only ramp
        CHECK(e->pos[2] == fx(100) + kFloorStand); // landed back on the (cached) floor
        CHECK(e->inf.vel[0] == 0 && e->inf.vel[2] == 0);
        CHECK(e->pitch == (656175520 + 4) >> 3);
        CHECK(e->roll == 0);

        // Long run: once the soldier drifts, the 8-tick ground-cache lag lets the slide
        // velocity persist a few ticks before a landing zeroes it (a property of the
        // D-INF-3 resolver model), so the drift outpaces one impulse per pass — assert
        // the qualitative shape, not the trajectory.
        run_ticks(ai, w, 8, 80);
        CHECK(e->pos[0] < fx(100) - 8 * 2048);  // kept sliding downhill
        CHECK(e->pos[0] > fx(92));              // ...at a bounded rate
        CHECK(e->pos[1] == fx(100));
        CHECK(e->pitch > (656175520 + 4) >> 3); // lean keeps growing...
        CHECK(e->pitch <= 656175520);           // ...never past the clamped slope
        CHECK(e->pos[2] < fx(100) + kFloorStand);        // followed the ground down
        CHECK(e->pos[2] > fx(90) + kFloorStand);
    }

    if (failures == 0) std::printf("infantry_test: OK\n");
    else std::printf("infantry_test: %d FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
