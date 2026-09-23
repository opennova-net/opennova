// The org1 think block against the retail instructions [orig:
// Entity_UpdateInfantryAI @0x4B9910, the think @0x4BA970..0x4BE7FD]: the
// combat legs (the held, possibly dead, target and post_attack as the chain's
// last link, the no-target search walk, the aim target point and eye, the lead
// distance, the hold-timer tail, the reload on every path, the retaliation LOS
// and the persistent aimFlag), the guard family and the holdSSN hold at the
// combat tail, the think-entry heading restore, the board walk's arrival,
// S stage, E-point claim and UseGun ring, the post-commit ride link and idle
// facing fan, the airborne skip, the route's raw node read, and the selector's
// swim and run_attack substitutions. Synthetic bodies and clips; no retail data.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <base/io/bam.h>
#include <runtime/terrain_query/height_field.h>
#include <runtime/world/ai.h>
#include <runtime/world/collision.h>
#include <runtime/world/infantry.h>
#include <runtime/world/pose_provider.h>
#include <runtime/world/world.h>

using namespace opennova::world;
namespace io = opennova::io;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)

namespace {

constexpr int32_t fx(double units) { return static_cast<int32_t>(units * 65536.0); }
constexpr double kBamPerRadian = 683565275.5764316; // [orig: dbl_7C19D8]

int32_t chop(double value) {
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<int64_t>(value)));
}

struct Clips : IRootMotionSource {
    std::set<int> states;
    bool has_clip(int, int state) const override { return states.count(state) != 0; }
    int32_t clip_length_ticks(int, int, int) const override { return -1; }
    bool advance(int, int state, int32_t &phase, RootMotionFrame &out) override {
        if (!states.count(state)) return false;
        ++phase;
        out = {};
        return true;
    }
};

// A launch point a fixed offset from the body, the posed +0x366 muzzle stand-in.
struct Muzzle : IPoseProvider {
    EntityHandle body;
    int32_t offset[3] = {};
    const AiSystem *ai = nullptr;
    bool resolve_organic_attachment(World &, EntityHandle h, uint8_t, int32_t out[3]) override {
        if (h != body || ai == nullptr) return false;
        const AiEntity *e = ai->for_handle(h);
        for (int axis = 0; axis < 3; ++axis) out[axis] = e->pos[axis] + offset[axis];
        return true;
    }
};

// Authored named points (E/G/S/UseGun) on one carrier.
struct Points : IPoseProvider {
    EntityHandle carrier;
    std::vector<std::pair<std::string, std::vector<int32_t>>> points;
    bool resolve_named_transform(World &, EntityHandle h, const char *name,
                                 int32_t out[6]) override {
        if (h != carrier) return false;
        for (const auto &point : points) {
            if (point.first != name) continue;
            std::copy(point.second.begin(), point.second.end(), out);
            return true;
        }
        return false;
    }
};

// Red (team 2, the target) and blue (team 1, at the origin); blue holds red.
struct Rig {
    // Off the perception cadence (key & 0x1F != 0) and off the alert decay
    // (key & 0x3F != 0); heading phase (key>>2)&63 = 4, pitch phase 7.
    static constexpr uint32_t kKey = 16 + 3 * 512;
    std::unique_ptr<World> storage = std::make_unique<World>();
    World &w = *storage;
    Clips clips;
    EntityHandle red_h, blue_h;
    Rig(int32_t rx, int32_t ry, int32_t rz, std::set<int> states = {43, 44, 49, 151, 155}) {
        w.registry.configure_pool(0, 16);
        w.registry.configure_pool(1, 16);
        w.registry.configure_pool(2, 8);
        clips.states = std::move(states);
        w.ai.root_motion = &clips;
        red_h = make(0, 2, rx, ry, rz);
        blue_h = make(1, 1, 0, 0, 0);
        blue().inf.combat_target = red_h;
        blue().slot.f[3] = static_cast<int32_t>(red_h.packed) + 1;
    }
    EntityHandle make(int idx, uint8_t team, int32_t x, int32_t y, int32_t z) {
        Entity body{};
        body.alive = true;
        body.item_id = 1001;
        body.item_type = 3;
        body.has_item_def = true;
        body.kind = EntityKind::Organic;
        body.health = body.health_max = 150;
        body.team = team;
        body.net_id = uint16_t(700 + idx);
        body.position = {float(x) / 65536.0f, float(y) / 65536.0f, float(z) / 65536.0f};
        const EntityHandle h = w.registry.spawn(0, body);
        AiEntity *e = w.ai.at(w.ai.attach(h));
        e->inf.active = true;
        e->net_id = body.net_id;
        e->team = team;
        e->health = 150;
        e->inf.max_health = 150;
        e->inf.adm_id = 1;
        e->pos[0] = x;
        e->pos[1] = y;
        e->pos[2] = z;
        e->slot.f[15] = 8 * 65536;  // attack range
        e->slot.f[16] = 32768;      // min engagement
        e->slot.f[17] = 40 * 65536; // sight
        e->slot.f[22] = 0x400;
        return h;
    }
    AiEntity &blue() { return *w.ai.for_handle(blue_h); }
    AiEntity &red() { return *w.ai.for_handle(red_h); }
    Entity &red_entity() { return *w.registry.get(red_h); }
    int think(uint32_t key = kKey) { return w.ai.infantry_combat_think(blue(), w, key); }
    // One whole AI tick at blue's staggered key (key = tick + 36 * net_id).
    void tick(uint32_t key) {
        TickContext ctx;
        ctx.world = &w;
        ctx.logic_tick = key - 36u * static_cast<uint32_t>(blue().net_id);
        ctx.is_authority = true;
        w.logic_tick = ctx.logic_tick;
        w.update_all_entities(ctx);
    }
    // An alerted body with no target and no scan idles in idle_2 (44).
    void alerted_idle() {
        blue().inf.combat_target = {};
        blue().slot.f[3] = 0;
        blue().slot.f[1] |= 1; // blind: no perception scan
        blue().inf.damage_timer = 40;
    }
    void kill_red() {
        red_entity().health = 0;
        red_entity().flags |= kEntityFlagDead;
        red().health = 0;
    }
    Entity &blue_entity() { return *w.registry.get(blue_h); }
    void guard() {
        blue_entity().flags |= kEntityFlagMounted;
        blue_entity().engine_flags |= kEntityFlagMounted;
    }
    bool guarding() {
        return ((blue_entity().flags | blue_entity().engine_flags) & kEntityFlagMounted) != 0;
    }
    // A pool-1 item for the board walk, at (x, y, 0).
    EntityHandle item(uint16_t net_id, uint8_t item_type, uint32_t attrib, int32_t x, int32_t y) {
        Entity item{};
        item.alive = true;
        item.item_id = 2001;
        item.item_type = item_type;
        item.has_item_def = true;
        item.item_attrib = attrib;
        item.kind = EntityKind::Item;
        item.health = item.health_max = 100;
        item.net_id = net_id;
        item.bound_radius = 1.0f;
        item.position = {float(x) / 65536.0f, float(y) / 65536.0f, 0.0f};
        return w.registry.spawn(1, item);
    }
    // Blue ordered to board SSN `net_id` (command 125) [orig: slot+148 / +152].
    void board(uint16_t net_id) {
        blue().slot.f[37] = 125;
        blue().slot.f[38] = net_id;
        blue().inf.combat_target = {};
        blue().slot.f[3] = 0;
    }
    // A one-node route on channel 1 [orig: slot+140 has-route, slot+148
    // channel, slot+152 node].
    void route(int32_t x, int32_t y, int32_t wait_ticks = 0, int32_t facing = 0) {
        w.ai.nav.channels.resize(2);
        w.ai.nav.channels[1].count = 1;
        w.ai.nav.channels[1].entries[0] = 0;
        NavEntry node;
        node.f[0] = 0x8000;
        node.f[1] = x;
        node.f[2] = y;
        node.f[4] = facing;
        node.wait_ticks = wait_ticks;
        w.ai.nav.nodes.assign(1, node);
        blue().slot.f[35] = 1;
        blue().slot.f[37] = 1;
        blue().slot.f[38] = 0;
    }
};

// A dead held target is not dropped by the think: inside attack range with the
// hold timer run out the reaction chain keeps working the corpse, and
// post_attack is only its last link (within 3 u, clearing the focus).
// [orig: the chain @0x4BC158..0x4BC297 (151 @0x4BC269..0x4BC297); the held
//  re-commit @0x4BBEE4..0x4BBEED; moveTimer stamp @0x4BC2A2..0x4BC2B4]
static void test_dead_target_stays_held_and_attacked() {
    {
        Rig r(fx(5), 0, 0);
        r.kill_red();
        r.blue().inf.ai_focus = r.red_h;
        const int state = r.think();
        CHECK(state == anim_state::kAttack);
        CHECK(r.blue().inf.combat_target == r.red_h);
        CHECK(r.blue().slot.f[3] != 0);
        CHECK(r.blue().inf.move_mode == 7 && r.blue().inf.target_dist == 0);
        CHECK(r.blue().inf.combat_move_timer == (0x400 >> 4) - 2); // stamp, tail, 10 u extra
        CHECK(r.blue().inf.ai_focus == r.red_h);
    }
    {
        Rig r(fx(2), 0, 0);
        r.kill_red();
        r.blue().inf.ai_focus = r.red_h;
        CHECK(r.think() == anim_state::kPostAttack);
        CHECK(!r.blue().inf.ai_focus.valid());
        CHECK(r.blue().inf.combat_target == r.red_h);
    }
}

// post_attack needs the reaction chain: with the hold timer still up the corpse
// takes the approach arm instead. [orig: `cmp [esi+148h],0; jnz` @0x4BC14B ->
// the approach @0x4BC2C2..0x4BC316]
static void test_post_attack_needs_the_reaction_chain() {
    Rig r(fx(2), 0, 0);
    r.kill_red();
    r.blue().inf.combat_move_timer = 5;
    const int state = r.think();
    CHECK(state != anim_state::kPostAttack);
    CHECK(r.blue().inf.move_mode == 1);
    CHECK(r.blue().inf.target_dist == fx(2));
    CHECK(r.blue().inf.combat_target == r.red_h);
}

// The retaliation fallback has no health test: a dead attacker of another team
// with a clear LOS is committed. [orig: @0x4BBEEF..0x4BBF24]
static void test_retaliation_admits_a_dead_attacker() {
    Rig r(fx(10), 0, 0);
    r.kill_red();
    r.blue().inf.combat_target = {};
    r.blue().slot.f[3] = 0;
    r.blue().slot.f[17] = fx(1); // the scan itself misses
    r.blue().inf.last_attacker = r.red_h;
    r.think(128);                 // phase 0 of the 32-tick perception
    CHECK(r.blue().inf.combat_target == r.red_h);
    CHECK(!r.blue().inf.last_attacker.valid());
}

// The retaliation LOS runs origin to origin through the entity LOS at height 0:
// a body standing under a terrain lip cannot retaliate even though eye-level
// rays clear it. [orig: pushes @0x4BBF0A..0x4BBF16 — allTypes 0, heightOffset 0,
// &attacker.Position, &self.Position; Entity_CheckLineOfSightTerrainAndEntities
// @0x53B130]
static void test_retaliation_los_runs_origin_to_origin() {
    Rig r(fx(10), 0, 0);
    std::vector<uint16_t> heights(512 * 512, 128); // 0.5 u everywhere
    std::vector<int> grid(256, 1);
    opennova::terrain::TerrainHeightField terrain;
    terrain.heightmap = heights.data();
    terrain.dim = 512;
    terrain.layout.sector_grid = grid.data();
    r.w.ai.terrain = &terrain;
    r.blue().inf.eye_offset_z = fx(1.5);
    r.red().inf.eye_offset_z = fx(1.5);
    r.blue().inf.combat_target = {};
    r.blue().slot.f[3] = 0;
    r.blue().slot.f[17] = fx(1);
    r.blue().inf.last_attacker = r.red_h;
    const int32_t eyes_a[3] = {0, 0, fx(1.5)}, eyes_b[3] = {fx(10), 0, fx(1.5)};
    CHECK(r.w.ai.line_of_sight_clear(r.w, eyes_a, eyes_b, r.blue_h, r.red_h));
    r.think(128);
    CHECK(!r.blue().inf.combat_target.valid());
}

// The aim target is the led raw Position: the target's CameraOffset never
// enters, because the ComputeWeaponFireOrigin result both blocks compute is
// never read. [orig: block 1 @0x4BC697..0x4BC798 (the dead call @0x4BC720);
// the delta @0x4BC825..0x4BC84E]
static void test_aim_targets_the_led_position() {
    Rig r(fx(20), 0, 0);
    r.red().inf.eye_offset_z = fx(1.5);
    r.blue().inf.anim_state = anim_state::kIdle; // flag 0x8: block 1
    r.blue().inf.combat_move_timer = 1;          // no reaction: the approach arm
    r.think();
    CHECK(r.blue().inf.aim_valid);
    CHECK(r.blue().inf.aim_heading == 0);
    CHECK(r.blue().inf.aim_pitch == 0);
}

// Within 3 u (planar, led point to body) the eye moves only a quarter of the way
// from the body origin to the launch point on X/Y and takes its Z; farther it is
// the launch point. [orig: block 1 @0x4BC7B1..0x4BC825]
static void test_aim_eye_mixes_toward_the_launch_point_up_close() {
    const auto solve = [](int32_t red_x, int32_t &heading, int32_t &pitch) {
        Rig r(red_x, 0, 0);
        Muzzle muzzle;
        muzzle.body = r.blue_h;
        muzzle.ai = &r.w.ai;
        muzzle.offset[1] = fx(1);
        muzzle.offset[2] = fx(1.25);
        r.w.pose_provider = &muzzle;
        r.blue().inf.anim_state = anim_state::kIdle;
        r.blue().inf.combat_move_timer = 1;
        r.think();
        CHECK(r.blue().inf.aim_valid);
        heading = r.blue().inf.aim_heading;
        pitch = r.blue().inf.aim_pitch;
        r.w.pose_provider = nullptr;
    };
    const auto expect = [](int32_t dx, int32_t dy, int32_t dz, int32_t &heading,
                           int32_t &pitch) {
        heading = chop(std::atan2(double(dy), double(dx)) * kBamPerRadian);
        const int32_t h = chop(std::sqrt(double(dx) * dx + double(dy) * dy));
        pitch = chop(std::atan2(double(dz), double(h)) * kBamPerRadian);
    };
    int32_t heading = 0, pitch = 0, want_heading = 0, want_pitch = 0;
    solve(fx(2), heading, pitch);
    expect(fx(2), -fx(1) / 4, -fx(1.25), want_heading, want_pitch);
    CHECK(heading == want_heading && pitch == want_pitch);
    solve(fx(5), heading, pitch);
    expect(fx(5), -fx(1), -fx(1.25), want_heading, want_pitch);
    CHECK(heading == want_heading && pitch == want_pitch);
}

// The lead distance is the FULL 3-D distance, chopped, over 528500: a target 7.8 u
// out and 3 u up leads by 2 ticks although its (dz sar 1) distance would give 1.
// [orig: @0x4BC697..0x4BC71D — no dz halving; `mov eax,7EFAD919h; mul ecx;
//  shr edx,12h; add 1`]
static void test_lead_uses_the_full_three_d_distance() {
    Rig r(0, fx(7.8), fx(3));
    Entity &red = r.red_entity();
    red.saved_live_pos[0] = 0;
    red.saved_live_pos[1] = fx(7.8) - fx(1); // moved +1 u north over its last tick
    red.saved_live_pos[2] = fx(3);
    red.saved_live_valid = true;
    r.blue().inf.anim_state = anim_state::kIdle;
    r.blue().inf.body_heading = 0x40000000;
    r.blue().inf.combat_move_timer = 1;
    r.think();
    CHECK(r.blue().inf.aim_point[1] == fx(7.8) + 2 * fx(1));
    CHECK(r.blue().inf.aim_point[2] == fx(3));
}

// With no target, an alerted body with a focus walks to the retained aim point
// (arrival 2 u), and near it scans in place with idle_2 and moveMode 8.
// [orig: @0x4BC34B..0x4BC4BE — moveMode 2 @0x4BC448..0x4BC469, 44/8
//  @0x4BC46F..0x4BC4B4]
static void test_lost_target_search_walks_to_the_aim_point() {
    Rig r(fx(30), 0, 0);
    r.blue().inf.combat_target = {};
    r.blue().slot.f[3] = 0;
    r.blue().inf.damage_timer = 5;
    r.blue().inf.ai_focus = r.red_h;
    r.blue().inf.aim_point[0] = fx(10);
    r.think();
    CHECK(r.blue().inf.move_mode == 2);
    CHECK(r.blue().inf.target_dist == fx(10));
    CHECK(r.blue().inf.arrival_radius == 0x20000);
    CHECK(r.blue().inf.move_target[0] == fx(10) && r.blue().inf.move_target[1] == 0);
    CHECK(r.blue().inf.ai_focus == r.red_h);

    r.blue().inf.move_mode = 0;
    r.blue().inf.aim_point[0] = fx(1.5);
    CHECK(r.think() == anim_state::kIdle2);
    CHECK(r.blue().inf.move_mode == 8 && r.blue().inf.target_dist == 0);
}

// A dead focus re-seats the aim point on the corpse when post_attack exists and
// is watched with it on arrival; without an alert the focus is dropped.
// [orig: @0x4BC37E..0x4BC3A8, @0x4BC46F..0x4BC4A5, @0x4BC4BE]
static void test_search_watches_a_dead_focus_and_needs_the_alert() {
    {
        Rig r(fx(1.5), 0, 0);
        r.kill_red();
        r.blue().inf.combat_target = {};
        r.blue().slot.f[3] = 0;
        r.blue().inf.damage_timer = 5;
        r.blue().inf.ai_focus = r.red_h;
        r.blue().inf.aim_point[0] = fx(30);
        CHECK(r.think() == anim_state::kPostAttack);
        CHECK(r.blue().inf.aim_point[0] == fx(1.5));
        CHECK(!r.blue().inf.ai_focus.valid());
        CHECK(r.blue().inf.move_mode == 8);
    }
    {
        Rig r(fx(30), 0, 0);
        r.blue().inf.combat_target = {};
        r.blue().slot.f[3] = 0;
        r.blue().inf.damage_timer = 0;
        r.blue().inf.ai_focus = r.red_h;
        r.blue().inf.aim_point[0] = fx(10);
        r.think();
        CHECK(!r.blue().inf.ai_focus.valid());
        CHECK(r.blue().inf.move_mode == 0);
    }
}

// The hold-timer tail runs on every path; a non-coward body in idle_3 loses one
// more tick. [orig: @0x4BC4C4..0x4BC537]
static void test_hold_timer_tail_runs_without_a_target() {
    Rig r(fx(30), 0, 0);
    r.blue().inf.combat_target = {};
    r.blue().slot.f[3] = 0;
    r.blue().inf.anim_state = anim_state::kIdle3;
    r.blue().inf.combat_move_timer = 10;
    r.think();
    CHECK(r.blue().inf.combat_move_timer == 8);
}

// aimFlag is only ever set by the aim blocks: a think without an aim write leaves
// it up. [orig: sets @0x4BC894 / @0x4BCFB1 only]
static void test_aim_flag_is_not_cleared_by_the_combat_think() {
    Rig r(fx(30), 0, 0);
    r.blue().inf.combat_target = {};
    r.blue().slot.f[3] = 0;
    r.blue().inf.anim_state = anim_state::kIdle;
    r.blue().inf.aim_valid = true;
    r.think();
    CHECK(r.blue().inf.aim_valid);
}

// The reload runs on every path after the aim blocks, and a current reload
// re-selects itself while refilling. [orig: @0x4BD132..0x4BD17D]
static void test_reload_runs_without_a_target() {
    {
        Rig r(fx(30), 0, 0, {43, 44, 49, 65});
        r.blue().inf.combat_target = {};
        r.blue().slot.f[3] = 0;
        r.blue().profile.clip_size = 30;
        r.blue().inf.magazine = 0;
        CHECK(r.think() == anim_state::kReload);
        CHECK(r.blue().inf.move_mode == 0 && r.blue().inf.magazine == 0);
    }
    {
        Rig r(fx(30), 0, 0, {43, 44, 49, 65});
        r.blue().inf.combat_target = {};
        r.blue().slot.f[3] = 0;
        r.blue().profile.clip_size = 30;
        r.blue().inf.magazine = 0;
        r.blue().inf.anim_state = anim_state::kReload;
        CHECK(r.think() == anim_state::kReload);
        CHECK(r.blue().inf.magazine == 30);
    }
}

// A Flags 0x40 (guard) body holds in place in guard: the combat approach's move
// is dropped and 140 proposed, and the selection never persists the cancelled
// goal Z. [orig: @0x4BD196..0x4BD1BB; the +0x304 write @0x4BD3F7 on a moving
// selection only]
static void test_guard_holds_in_place() {
    Rig r(fx(20), 0, fx(2), {43, 44, 49, 140, 155});
    r.guard();
    r.blue().inf.combat_move_timer = 1; // no reaction: the approach arm runs
    const int state = r.think();
    CHECK(state == anim_state::kGuard);
    CHECK(r.blue().inf.move_mode == 0 && r.blue().inf.target_dist == 0);
    r.w.ai.infantry_select(r.blue(), r.w, state);
    CHECK(r.blue().inf.goal_z == 0);
    CHECK(r.blue().inf.anim_state == anim_state::kGuard);
    CHECK(r.guarding());
}

// A hit takes guard_cover (143); a chosen reaction takes guard_attack (142)
// with moveMode 7; without the guard clip the flag drops from both mirrors.
// [orig: @0x4BD1BE..0x4BD1F8; the clear @0x4BD1B8..0x4BD1BB]
static void test_guard_cover_attack_and_the_missing_clip() {
    {
        Rig r(fx(20), 0, 0, {43, 44, 49, 140, 143, 155});
        r.guard();
        r.blue().inf.combat_move_timer = 1;
        r.blue().inf.was_hit = true;
        CHECK(r.think() == anim_state::kGuardCover);
        CHECK(r.blue().inf.move_mode == 0);
    }
    {
        Rig r(fx(5), 0, 0, {43, 44, 49, 140, 142, 155});
        r.guard();
        CHECK(r.think() == anim_state::kGuardAttack);
        CHECK(r.blue().inf.move_mode == 7 && r.blue().inf.target_dist == 0);
    }
    {
        Rig r(fx(20), 0, 0, {43, 44, 49, 155});
        r.guard();
        r.blue().inf.combat_move_timer = 1;
        CHECK(r.think() != anim_state::kGuard);
        CHECK(r.blue().inf.move_mode == 0);
        CHECK((r.blue_entity().flags & kEntityFlagMounted) == 0);
        CHECK((r.blue_entity().engine_flags & kEntityFlagMounted) == 0);
    }
}

// Off guard, a current guard state leaves through guard_leave (144) with no
// move. [orig: @0x4BD1FA..0x4BD231]
static void test_leaving_guard_plays_guard_leave() {
    Rig r(fx(20), 0, 0, {43, 44, 49, 140, 144, 155});
    r.blue().inf.combat_move_timer = 1;
    r.blue().inf.anim_state = anim_state::kGuard;
    CHECK(r.think() == anim_state::kGuardLeave);
    CHECK(r.blue().inf.move_mode == 0 && r.blue().inf.target_dist == 0);
}

// The think's legs end in the common move tail: a guard keeps the think-entry
// target heading, and a marker wait rewrites that local along with the target
// and aim headings. The route leg itself writes only the goal; a moving
// selection's detour publishes the bearing. [orig: the entry local
// @0x4BA9B4..0x4BA9BA; the restore @0x4BBE11..0x4BBE1E; the marker wait
// @0x4BAD1E..0x4BAD48; the detour's +0x1A8 write ai_find_cover_position
// @0x4AFF2C]
static void test_guard_keeps_the_think_entry_heading() {
    {
        Rig r(fx(60), 0, 0);
        r.route(0, fx(20));
        r.w.ai.infantry_think(r.blue(), r.w);
        CHECK(r.blue().inf.move_mode == 3);
        CHECK(r.blue().inf.target_heading == 0);
        r.w.ai.infantry_select(r.blue(), r.w, 0);
        CHECK(r.blue().inf.target_heading == 0x40000000);
    }
    {
        Rig r(fx(60), 0, 0);
        r.route(0, fx(20));
        r.guard();
        r.w.ai.infantry_think(r.blue(), r.w);
        CHECK(r.blue().inf.move_mode == 3);
        CHECK(r.blue().inf.target_heading == 0);
    }
    {
        Rig r(fx(60), 0, 0);
        r.route(0, 0, 32, 0x20000000);
        r.guard();
        r.w.ai.infantry_think(r.blue(), r.w);
        CHECK(r.blue().inf.target_heading == 0x20000000);
        CHECK(r.blue().inf.aim_heading == 0x20000000);
        CHECK(r.blue().inf.wait_cooldown == (32 + 8) >> 4);
    }
}

// The WAC holdSSN bit parks the body in moveMode 12 after the guard legs; a
// chosen reaction still wins with 7. [orig: @0x4BD235..0x4BD251]
static void test_hold_parks_the_body() {
    {
        Rig r(fx(20), 0, 0);
        r.blue_entity().cause_flags |= 0x2000u;
        r.blue().inf.combat_move_timer = 1;
        r.think();
        CHECK(r.blue().inf.move_mode == 12 && r.blue().inf.target_dist == 0);
    }
    {
        Rig r(fx(5), 0, 0);
        r.blue_entity().cause_flags |= 0x2000u;
        CHECK(r.think() == anim_state::kAttack);
        CHECK(r.blue().inf.move_mode == 7);
    }
}

// Without the has-route flag, or while the cooldown holds, the route leg clears
// the entry stage; channel 0 drops the has-route flag; the cooldown steps down
// while nonzero, a negative one included. [orig: @0x4BAA7B..0x4BAAB1;
// @0x4BABBD..0x4BABE5 -> @0x4BAE75 / @0x4BAE80..0x4BAE88]
static void test_route_gate_clears_the_entry_stage() {
    {
        Rig r(fx(60), 0, 0);
        r.route(0, fx(20));
        r.blue().slot.f[35] = 0;
        r.blue().inf.board_entry_stage = 3;
        r.w.ai.infantry_think(r.blue(), r.w);
        CHECK(r.blue().inf.board_entry_stage == 0);
    }
    {
        Rig r(fx(60), 0, 0);
        r.route(0, fx(20));
        r.blue().inf.wait_cooldown = -3;
        r.blue().inf.board_entry_stage = 3;
        r.w.ai.infantry_think(r.blue(), r.w);
        CHECK(r.blue().inf.wait_cooldown == -4);
        CHECK(r.blue().inf.move_mode == 0);
        CHECK(r.blue().inf.board_entry_stage == 0);
    }
    {
        Rig r(fx(60), 0, 0);
        r.route(0, fx(20));
        r.blue().slot.f[37] = 0;
        r.w.ai.infantry_think(r.blue(), r.w);
        CHECK(r.blue().slot.f[35] == 0);
    }
}

// The route reads its nodes through the flat record block with no bound and an
// unchecked pool-3 read: a start node past the count reads the raw slot word
// there, and a slot no marker filled is a zeroed marker the body walks to (the
// route is kept), for the current node and the next one alike.
// [orig: Entity_UpdateInfantryAI @0x4BABFF..0x4BAC19 and @0x4BADCB..0x4BADDF;
//  Pool_GetEntryUnchecked @0x441FC0]
static void test_route_reads_the_raw_node_word() {
    {
        Rig r(fx(60), 0, 0);
        r.route(0, fx(20));
        NavEntry past;
        past.f[0] = 0x8000;
        past.f[1] = fx(-30);
        past.f[2] = fx(5);
        r.w.ai.nav.nodes.push_back(past);      // pool-3 slot 1
        r.w.ai.nav.channels[1].entries[2] = 1; // the raw word past the count
        r.blue().slot.f[38] = 2;               // a start node past the count
        r.w.ai.infantry_think(r.blue(), r.w);
        CHECK(r.blue().inf.move_mode == 3);
        CHECK(r.blue().inf.move_target[0] == fx(-30) && r.blue().inf.move_target[1] == fx(5));
    }
    {
        Rig r(fx(60), 0, 0);
        r.route(0, fx(20));
        r.w.ai.nav.channels[1].entries[0] = 7; // no marker filled pool-3 slot 7
        r.blue().pos[0] = fx(10);
        r.w.ai.infantry_think(r.blue(), r.w);
        CHECK(r.blue().slot.f[35] == 1);
        CHECK(r.blue().inf.move_mode == 3 && r.blue().inf.target_dist == fx(10));
        CHECK(r.blue().inf.move_target[0] == 0 && r.blue().inf.move_target[1] == 0 &&
              r.blue().inf.move_target[2] == 0x4000);
    }
    {
        Rig r(fx(60), 0, 0);
        r.route(fx(10), 0); // blue arrives on node 0 ...
        r.blue().pos[0] = fx(10);
        r.w.ai.nav.channels[1].count = 2;
        r.w.ai.nav.channels[1].entries[1] = 7; // ... and node 1 names an unfilled slot
        r.w.ai.infantry_think(r.blue(), r.w);
        CHECK(r.blue().slot.f[38] == 1);
        CHECK(r.blue().inf.move_mode == 4 && r.blue().inf.target_dist == fx(10));
        CHECK(r.blue().inf.move_target[0] == 0 && r.blue().inf.move_target[1] == 0);
    }
}

// The board arrival clears Flags 0x40 and the parent slot only when the attach
// it tried left the body unparented: an arrival that cannot attach keeps a
// guard. [orig: gate @0x4BBDA6..0x4BBDC8; clear @0x4BBDFA..0x4BBE07]
static void test_board_arrival_clears_the_guard_only_after_an_attach() {
    {
        Rig r(fx(60), 0, 0);
        r.item(900, 3, 0, fx(1), 0);
        r.board(900);
        r.guard();
        r.w.ai.infantry_think(r.blue(), r.w);
        CHECK(r.blue().inf.move_mode == 0);
        CHECK((r.blue_entity().flags & kEntityFlagMounted) != 0);
    }
    {
        Rig r(fx(60), 0, 0);
        r.item(900, 3, kItemAttribPlayerControl, fx(1), 0);
        r.board(900);
        r.guard();
        r.blue_entity().mount_type = SeatType::Passenger;
        r.w.ai.infantry_think(r.blue(), r.w);
        CHECK(!r.blue_entity().mounted);
        CHECK((r.blue_entity().flags & kEntityFlagMounted) == 0);
        CHECK((r.blue_entity().engine_flags & kEntityFlagMounted) == 0);
        CHECK(r.blue_entity().mount_type == SeatType::None);
    }
    {
        // A live carrier that cannot be entered: the goal is the body itself in a
        // 125 u ring, so the arrival runs with the attach gated off.
        // [orig: @0x4BB2CE..0x4BB2E8]
        Rig r(fx(60), 0, 0);
        r.item(900, 1, kItemAttribPlayerControl, fx(30), 0);
        r.board(900);
        r.guard();
        r.blue().inf.board_entry_stage = 2;
        r.w.ai.infantry_think(r.blue(), r.w);
        CHECK(r.blue().inf.move_mode == 0);
        CHECK(r.blue().inf.board_entry_stage == 3);
        CHECK((r.blue_entity().flags & kEntityFlagMounted) != 0);
    }
}

// The S stage writes the S yaw into the target heading and the think-entry
// local, never the body yaw; with the guard clip it stores 140 raw and proposes
// it. [orig: @0x4BB7CA / @0x4BB7D0; Flags |= 0x40 @0x4BB81C; +0x2BC / the selection
// @0x4BB82F..0x4BB835]
static void test_s_stage_sets_the_heading_locals_and_the_guard() {
    Rig r(fx(60), 0, 0, {43, 44, 49, 140, 147});
    const EntityHandle carrier = r.item(900, 6, 0, fx(3), 0);
    Points points;
    points.carrier = carrier;
    points.points.push_back({"S1", {fx(1), fx(1), 0, 0x10000000, 0, 0}});
    r.w.pose_provider = &points;
    r.board(900);
    r.blue().inf.board_entry_stage = 4;
    r.blue().heading = 0x7000;
    r.w.ai.infantry_think(r.blue(), r.w);
    CHECK(r.blue().inf.target_heading == 0x10000000);
    CHECK(r.blue().heading == 0x7000);
    CHECK(r.guarding());
    CHECK((r.blue_entity().engine_flags & kEntityFlagMounted) != 0);
    CHECK(r.blue().inf.anim_state == anim_state::kGuard);
    CHECK(r.blue().inf.goal_z == 0);
    CHECK(r.think() == anim_state::kGuard);
    r.w.pose_provider = nullptr;
}

// The command legs' proposal is the combat think's starting selection.
// [orig: the G stage's 147 @0x4BB72A carried into the combat legs]
static void test_board_proposal_reaches_the_selection() {
    Rig r(fx(60), 0, 0, {43, 44, 49, 147});
    const EntityHandle carrier = r.item(900, 6, 0, fx(20), 0);
    Points points;
    points.carrier = carrier;
    points.points.push_back({"G1", {fx(10), 0, 0, 0, 0, 0}});
    r.w.pose_provider = &points;
    r.board(900);
    r.blue().inf.board_entry_stage = 2;
    r.w.ai.infantry_think(r.blue(), r.w);
    CHECK(r.blue().inf.move_mode == 3);
    CHECK(r.think() == anim_state::kStop);
    r.w.pose_provider = nullptr;
}

// The E-point claim runs only for a target resolved outside pool 0.
// [orig: `jnz loc_4BB187` @0x4BAEF1; the claim @0x4BAF9B..0x4BB185]
static void test_claim_skips_a_pool_zero_target() {
    {
        Rig r(fx(60), 0, 0);
        r.board(r.red_entity().net_id);
        r.blue().inf.board_entry_slot = 5;
        r.w.ai.infantry_think(r.blue(), r.w);
        CHECK(r.blue().inf.board_entry_slot == 5);
    }
    {
        Rig r(fx(60), 0, 0);
        r.item(900, 3, 0, fx(20), 0);
        r.board(900);
        r.blue().inf.board_entry_slot = 5;
        r.w.ai.infantry_think(r.blue(), r.w);
        CHECK(r.blue().inf.board_entry_slot == 1);
    }
}

// The UseGun ring widens to 3 u while the gun holds its +0x170 occupant, not
// its weapon owner. [orig: `cmp dword ptr [edx+170h],0` @0x4BB39C]
static void test_use_gun_ring_reads_the_occupant() {
    const auto ring = [](bool occupant, bool owner) {
        Rig r(fx(60), 0, 0);
        const EntityHandle gun = r.item(900, 6, 0, fx(20), 0);
        Points points;
        points.carrier = gun;
        points.points.push_back({"UseGun", {fx(20), 0, 0, 0, 0, 0}});
        r.w.pose_provider = &points;
        if (occupant) r.w.registry.get(gun)->primary_occupant = r.red_h;
        if (owner) r.w.registry.get(gun)->primary_weapon_owner = r.red_h;
        r.board(900);
        r.w.ai.infantry_think(r.blue(), r.w);
        const int32_t radius = r.blue().inf.arrival_radius;
        r.w.pose_provider = nullptr;
        return radius;
    };
    CHECK(ring(true, false) == 0x30000);
    CHECK(ring(false, true) == 0x10000);
}

// The ride link: the same-team occupant of the carrier under the body, else of
// that carrier's own ground entity, held by +0x174 (+4 per think up to 0xF0,
// one tick spent per think without it, then cleared); an enemy or the body
// itself never links. [orig: Entity_UpdateInfantryAI @0x4BD87E..0x4BD905]
static void test_ride_link_holds_the_same_team_rider() {
    Rig r(fx(60), 0, 0);
    const EntityHandle hull = r.item(900, 1, kItemAttribPlayerControl, 0, 0);
    const EntityHandle turret = r.item(901, 6, 0, 0, 0);
    const EntityHandle driver = r.make(2, 1, fx(1), 0, 0);
    const EntityHandle enemy = r.make(3, 2, fx(2), 0, 0);
    r.w.registry.get(hull)->primary_occupant = driver;
    r.blue_entity().ground_target = hull;
    infantry_ride_link(r.w, r.blue_entity());
    CHECK(r.blue_entity().primary_occupant == driver);
    CHECK(r.blue_entity().ride_link_hold == 4);
    for (int i = 0; i < 70; ++i) infantry_ride_link(r.w, r.blue_entity());
    CHECK(r.blue_entity().ride_link_hold == 0xF0);
    // Off the carrier the hold is spent a tick per think before the link goes.
    r.blue_entity().ground_target = {};
    infantry_ride_link(r.w, r.blue_entity());
    CHECK(r.blue_entity().ride_link_hold == 0xEF);
    CHECK(r.blue_entity().primary_occupant == driver);
    r.blue_entity().ride_link_hold = 1;
    infantry_ride_link(r.w, r.blue_entity());
    CHECK(r.blue_entity().ride_link_hold == 0 && r.blue_entity().primary_occupant == driver);
    infantry_ride_link(r.w, r.blue_entity());
    CHECK(!r.blue_entity().primary_occupant.valid());
    // One level up: a turret with no occupant standing on the hull.
    r.w.registry.get(turret)->ground_target = hull;
    r.blue_entity().ground_target = turret;
    infantry_ride_link(r.w, r.blue_entity());
    CHECK(r.blue_entity().primary_occupant == driver);
    // An enemy occupant, or the body itself, is no link.
    r.blue_entity().ride_link_hold = 0;
    r.blue_entity().primary_occupant = {};
    r.w.registry.get(hull)->primary_occupant = enemy;
    infantry_ride_link(r.w, r.blue_entity());
    CHECK(!r.blue_entity().primary_occupant.valid());
    r.w.registry.get(hull)->primary_occupant = r.blue_h;
    infantry_ride_link(r.w, r.blue_entity());
    CHECK(!r.blue_entity().primary_occupant.valid());
}

// The org1 think refreshes the link after its selection commit.
// [orig: Entity_UpdateInfantryAI @0x4BD87E, after the arbitration @0x4BD837]
static void test_think_refreshes_the_ride_link() {
    Rig r(fx(200), 0, 0, {1, 43, 44, 49});
    const EntityHandle hull = r.item(900, 1, kItemAttribPlayerControl, fx(40), 0);
    const EntityHandle driver = r.make(2, 1, fx(40), fx(5), 0);
    r.w.registry.get(hull)->primary_occupant = driver;
    r.alerted_idle();
    r.blue_entity().ground_target = hull;
    r.tick(16);
    CHECK(r.blue_entity().primary_occupant == driver);
    CHECK(r.blue_entity().ride_link_hold == 4);
}

// The org2 twin rides the player body's 16-tick slow pass, right after the
// weapon-hold commit. [orig: Entity_UpdateInfantryPlayerBody @0x4B5EA9..0x4B5F2C]
static void test_player_slow_pass_refreshes_the_ride_link() {
    Rig r(fx(200), 0, 0);
    const EntityHandle hull = r.item(900, 1, kItemAttribPlayerControl, 0, 0);
    const EntityHandle driver = r.make(2, 1, fx(1), 0, 0);
    r.w.registry.get(hull)->primary_occupant = driver;
    r.blue_entity().ground_target = hull;
    r.w.ai.infantry_weapon_channel(r.blue(), r.w, 15);
    CHECK(!r.blue_entity().primary_occupant.valid());
    r.w.ai.infantry_weapon_channel(r.blue(), r.w, 16);
    CHECK(r.blue_entity().primary_occupant == driver);
}

// The idle facing fan turns an alerted idle body away from a teammate it sees
// close by: the bearing away from the one teammate east is due west.
// [orig: Entity_UpdateInfantryAI @0x4BD905..0x4BE07A — the walk
//  @0x4BD990..0x4BDA9D, the clear first probe @0x4BDB8E, the store @0x4BE07A]
static void test_idle_facing_turns_away_from_a_teammate() {
    {
        Rig r(fx(200), 0, 0, {1, 43, 44, 49});
        r.make(2, 1, fx(1), 0, 0);
        r.alerted_idle();
        r.tick(16);
        CHECK(r.blue().inf.anim_state == anim_state::kIdle2);
        CHECK(r.blue().inf.target_heading == INT32_MIN);
    }
    {
        // Farther than 3 u planar: no turn.
        Rig r(fx(200), 0, 0, {1, 43, 44, 49});
        r.make(2, 1, fx(3.2), 0, 0);
        r.alerted_idle();
        r.tick(16);
        CHECK(r.blue().inf.target_heading == 0);
    }
}

// A blocked first probe (3 u along the mean) turns the mean half a turn; the
// fan's clear probes leave it there. [orig: @0x4BDB75..0x4BDBC6]
static void test_idle_facing_probes_turn_away_from_a_wall() {
    Rig r(fx(200), 0, 0, {1, 43, 44, 49});
    CollisionModel box;
    const auto plane = [&](int nx, int ny, int nz, double d) {
        CollisionPlane p;
        p.nx = static_cast<int16_t>(nx);
        p.ny = static_cast<int16_t>(ny);
        p.nz = static_cast<int16_t>(nz);
        p.dist = fx(d);
        box.planes.push_back(p);
    };
    plane(16384, 0, 0, -0.5);
    plane(-16384, 0, 0, -0.5);
    plane(0, 16384, 0, -0.5);
    plane(0, -16384, 0, -0.5);
    plane(0, 0, 16384, -3.0);
    plane(0, 0, -16384, 0.0);
    CollisionVolume volume;
    volume.type = 1;
    volume.min_x = volume.min_y = fx(-0.5);
    volume.max_x = volume.max_y = fx(0.5);
    volume.max_z = fx(3.0);
    volume.plane_count = 6;
    box.volumes.push_back(volume);
    CollisionSection section;
    section.volume_count = 1;
    box.sections.push_back(section);
    Entity wall{};
    wall.kind = EntityKind::Building;
    wall.alive = true;
    wall.yaw = 90;
    wall.position = {1.5f, 0.0f, 0.0f};
    const EntityHandle wall_h = r.w.registry.spawn(2, wall);
    auto collision = std::make_unique<CollisionWorld>();
    collision->assign_entity(wall_h, collision->add_model(std::move(box)));
    for (int i = 0; i < 17; ++i) collision->build_tick_tables(r.w);
    r.w.ai.collision = collision.get();
    r.alerted_idle();
    r.tick(16);
    CHECK(r.blue().inf.anim_state == anim_state::kIdle2);
    CHECK(r.blue().inf.target_heading == 0x7FFFFF80);
    r.w.ai.collision = nullptr;
}

// Airborne without the 0x80 climb order, the think seeds aimPitch from the SSN
// and skips the rest: no cooldown step, no route leg. The climb order exempts
// the body. [orig: Entity_UpdateInfantryAI @0x4BAA4B..0x4BAA66]
static void test_airborne_think_skips_after_the_pitch_seed() {
    Rig r(fx(60), 0, 0);
    r.route(0, fx(20));
    r.blue_entity().engine_flags |= kEntityFlagInAir;
    CHECK(!r.w.ai.infantry_think(r.blue(), r.w));
    CHECK(r.blue().inf.aim_pitch == io::bam_sar(static_cast<int32_t>(701u << 27), 4));
    CHECK(r.blue().inf.move_mode == 0);
    r.blue().inf.wait_cooldown = 3;
    r.w.ai.infantry_think(r.blue(), r.w);
    CHECK(r.blue().inf.wait_cooldown == 3);
    r.blue().inf.wait_cooldown = 0;
    r.blue_entity().engine_flags |= kEntityFlagAiClimb;
    CHECK(r.w.ai.infantry_think(r.blue(), r.w));
    CHECK(r.blue().inf.move_mode == 3);
}

// Swimming (Flags 0x8000) swaps the gaits, the idles and the close attacks for
// the swim clips. [orig: Entity_UpdateInfantryAI @0x4BD635..0x4BD6A3]
static void test_swimming_selects_the_swim_clips() {
    const auto select = [](bool moving, int proposal) {
        Rig r(fx(200), 0, 0, {1, 36, 37, 43, 149, 154, 155});
        r.blue_entity().engine_flags |= kEntityFlagDrowning;
        if (moving) {
            r.blue().inf.damage_timer = 5; // alerted: the run gait
            r.blue().inf.move_mode = 3;
            r.blue().inf.target_dist = fx(10);
            r.blue().inf.move_target[0] = fx(10);
        }
        r.w.ai.infantry_select(r.blue(), r.w, proposal);
        return r.blue().inf.anim_state;
    };
    CHECK(select(true, 0) == anim_state::kSwimForward);
    CHECK(select(false, 0) == anim_state::kSwimIdle);
    CHECK(select(false, anim_state::kAttack) == anim_state::kSwimAttack);
}

// A combat approach (moveMode 1) runs as run_attack; a route walk keeps the run.
// [orig: Entity_UpdateInfantryAI @0x4BD748..0x4BD763]
static void test_combat_approach_runs_as_run_attack() {
    const auto select = [](int move_mode) {
        Rig r(fx(200), 0, 0, {1, 43, 149, 167});
        r.blue().inf.damage_timer = 5;
        r.blue().inf.move_mode = move_mode;
        r.blue().inf.target_dist = fx(10);
        r.blue().inf.move_target[0] = fx(10);
        r.w.ai.infantry_select(r.blue(), r.w, 0);
        return r.blue().inf.anim_state;
    };
    CHECK(select(1) == anim_state::kRunAttack);
    CHECK(select(3) == anim_state::kRunForward);
}

// A scripted idle (130..136) watches the local player: aim heading and pitch
// toward its Position, aimFlag down, the override up, the body re-faced past 45
// degrees, no move. [orig: Entity_UpdateInfantryAI @0x4BCFF5..0x4BD0F4]
static void test_scripted_idle_watches_the_local_player() {
    Rig r(fx(60), 0, 0);
    const EntityHandle player = r.make(2, 1, 0, fx(10), fx(1));
    r.w.cached.local_player = player;
    r.blue().inf.combat_target = {};
    r.blue().slot.f[3] = 0;
    r.blue().inf.anim_state = 131;
    r.blue().inf.aim_valid = true;
    r.blue().inf.move_mode = 3;
    r.blue().inf.target_dist = fx(5);
    r.blue().inf.path_state = 2;
    r.think();
    const int32_t heading = chop(std::atan2(double(fx(10)), 0.0) * kBamPerRadian);
    const int32_t pitch = chop(std::atan2(double(fx(1)), double(fx(10))) * kBamPerRadian);
    CHECK(r.blue().inf.aim_heading == heading);
    CHECK(r.blue().inf.aim_pitch == pitch);
    CHECK(!r.blue().inf.aim_valid);
    CHECK(r.blue().inf.aim_override);
    CHECK(r.blue().inf.target_heading == heading);
    CHECK(r.blue().inf.move_mode == 0 && r.blue().inf.target_dist == 0);
    CHECK(r.blue().inf.path_state == 0);
}

// A coward with no team scans as team 2: a team-1 body is its enemy and a
// team-2 body its friend. [orig: Entity_UpdateInfantryAI @0x4BBEB5..0x4BBED8]
static void test_teamless_coward_scans_as_team_two() {
    Rig r(fx(5), 0, 0);
    const EntityHandle enemy = r.make(2, 1, 0, fx(6), 0);
    r.blue().team = 0;
    r.blue_entity().team = 0;
    r.blue().slot.f[1] |= 8;
    r.blue().inf.combat_target = {};
    r.blue().slot.f[3] = 0;
    r.think(128); // phase 0 of the 32-tick perception
    CHECK(r.blue().inf.combat_target == enemy);
}

// A stop (147) is arbitrated raw: from an exit-gated state (115, flags 0x20)
// the movement-flagged 147 commits at once, where the idle it would resolve to
// (43, no 0x1) only queues. [orig: the arbitration @0x4BD845..0x4BD874 on the
// raw selection; the fix-up @0x4BE080..0x4BE09A turns it into 43 later]
static void test_stop_is_arbitrated_raw() {
    Rig r(fx(200), 0, 0, {1, 43, 115});
    r.blue().inf.anim_state = 115;
    r.blue().inf.anim_pending = 0;
    r.w.ai.infantry_select(r.blue(), r.w, anim_state::kStop);
    CHECK(r.blue().inf.anim_state == anim_state::kStop);
    CHECK(r.blue().inf.anim_pending == 0);
}

// The attachment within reach still runs the selector tail: the move mode
// reaches +0x36A and the hit flinch consumes wasHit. [orig: +0x36A @0x4BD356;
// the wasHit clear @0x4BD6EE]
static void test_attachment_publishes_the_move_mode_and_consumes_the_hit() {
    Rig r(fx(200), 0, 0, {1, 43, 150});
    const EntityHandle post = r.item(900, 6, 0, 0, 0);
    Points points;
    points.carrier = post;
    points.points.push_back({"attach", {0, 0, 0, 0, 0, 0}});
    r.w.pose_provider = &points;
    r.blue_entity().attach_parent = post;
    r.blue_entity().attach_bone = 1;
    r.blue().inf.was_hit = true;
    r.blue().inf.prev_move_mode = 3;
    r.blue().slot.f[1] |= 1; // blind: no perception scan
    r.tick(16);
    CHECK(r.blue().inf.anim_state == 150);
    CHECK(r.blue().inf.prev_move_mode == 0);
    CHECK(!r.blue().inf.was_hit);
    r.w.pose_provider = nullptr;
}

// The jog/run fallback runs inside the moving branch, before the wounded
// substitution: a body with neither run clip walks, and a wounded walker takes
// wounded_walk. [orig: Entity_UpdateInfantryAI @0x4BD5C5..0x4BD61D, then the
// wounded gaits @0x4BD6F5..0x4BD744]
static void test_run_fallback_precedes_the_wounded_gait() {
    Rig r(fx(200), 0, 0, {1, 43, 145});
    r.blue().inf.damage_timer = 5; // alerted: the run gait
    r.blue().health = 50;          // at or under half of 150
    r.blue().inf.move_mode = 3;
    r.blue().inf.target_dist = fx(10);
    r.blue().inf.move_target[0] = fx(10);
    r.w.ai.infantry_select(r.blue(), r.w, 0);
    CHECK(r.blue().inf.anim_state == anim_state::kWoundedWalk);
}

} // namespace

int main() {
    test_dead_target_stays_held_and_attacked();
    test_post_attack_needs_the_reaction_chain();
    test_retaliation_admits_a_dead_attacker();
    test_retaliation_los_runs_origin_to_origin();
    test_aim_targets_the_led_position();
    test_aim_eye_mixes_toward_the_launch_point_up_close();
    test_lead_uses_the_full_three_d_distance();
    test_lost_target_search_walks_to_the_aim_point();
    test_search_watches_a_dead_focus_and_needs_the_alert();
    test_hold_timer_tail_runs_without_a_target();
    test_aim_flag_is_not_cleared_by_the_combat_think();
    test_reload_runs_without_a_target();
    test_guard_holds_in_place();
    test_guard_cover_attack_and_the_missing_clip();
    test_leaving_guard_plays_guard_leave();
    test_guard_keeps_the_think_entry_heading();
    test_hold_parks_the_body();
    test_route_gate_clears_the_entry_stage();
    test_route_reads_the_raw_node_word();
    test_board_arrival_clears_the_guard_only_after_an_attach();
    test_s_stage_sets_the_heading_locals_and_the_guard();
    test_board_proposal_reaches_the_selection();
    test_claim_skips_a_pool_zero_target();
    test_use_gun_ring_reads_the_occupant();
    test_ride_link_holds_the_same_team_rider();
    test_think_refreshes_the_ride_link();
    test_player_slow_pass_refreshes_the_ride_link();
    test_idle_facing_turns_away_from_a_teammate();
    test_idle_facing_probes_turn_away_from_a_wall();
    test_airborne_think_skips_after_the_pitch_seed();
    test_swimming_selects_the_swim_clips();
    test_combat_approach_runs_as_run_attack();
    test_scripted_idle_watches_the_local_player();
    test_teamless_coward_scans_as_team_two();
    test_stop_is_arbitrated_raw();
    test_attachment_publishes_the_move_mode_and_consumes_the_hit();
    test_run_fallback_precedes_the_wounded_gait();
    if (failures != 0) {
        std::printf("infantry_think_test: %d FAILED\n", failures);
        return 1;
    }
    std::printf("infantry_think_test: all passed\n");
    return 0;
}
