// Full NPC motor regressions for idle attention and its BMS visibility writes.
// [orig: Entity_UpdateInfantryAI @0x4BE0CA..0x4BEFF0]
#include <base/io/bam.h>
#include <runtime/world/ai.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/world.h>
#include <runtime/terrain_query/height_field.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <vector>

using namespace opennova::world;
namespace io = opennova::io;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++failures; \
} } while (0)
constexpr int32_t fixed(float value) { return static_cast<int32_t>(value * 65536.0f); }

struct Clips : IRootMotionSource {
    std::set<int> states{1, 43, 44, 49, 67, 76, 125, 126, 140, 141, 147, 150};
    int32_t step = 0;
    bool has_clip(int, int state) const override { return states.count(state) != 0; }
    int32_t clip_length_ticks(int, int, int) const override { return -1; }
    bool advance(int, int state, int32_t &phase, RootMotionFrame &out) override {
        if (!states.count(state)) return false;
        ++phase;
        out = {};
        if (state == 1) out.dx = step;
        return true;
    }
};

struct Fixture {
    std::unique_ptr<World> storage = std::make_unique<World>();
    World &world = *storage;
    Clips clips;
    EntityHandle observer;
    Fixture() {
        world.registry.configure_pool(0, 16);
        world.registry.configure_pool(1, 4);
        world.registry.configure_pool(2, 4);
        world.ai.root_motion = &clips;
        observer = person({0, 0, 2}, 8, 2);
        auto &e = *world.ai.at(world.ai.attach(observer));
        e.inf.active = true;
        e.inf.adm_id = 1;
        e.inf.max_health = 100;
        e.health = 100;
        e.net_id = 8;
        e.team = 1;
        e.pos[2] = fixed(2);
        e.slot.f[17] = fixed(20);
        e.slot.f[1] = 1; // Disable the separate combat query.
    }
    EntityHandle person(Vec3 position, int ssn = 9, int group = 3, int team = 1) {
        Entity entity;
        entity.kind = EntityKind::Organic;
        entity.item_id = 1;
        entity.has_item_def = true;
        entity.net_id = ssn;
        entity.bms_id = 100 + ssn; // Relation singles must use SSN, not file ID.
        entity.group_id = group;
        entity.team = static_cast<uint8_t>(team);
        entity.position = position;
        entity.yaw = 90; // +X in mission coordinates.
        entity.health = entity.health_max = 100;
        return world.registry.spawn(0, entity);
    }
    AiEntity &body() { return *world.ai.for_handle(observer); }
    void tick(uint32_t key, bool authority = true) {
        TickContext ctx;
        ctx.world = &world;
        ctx.logic_tick = key - 36u * 8u;
        ctx.is_authority = authority;
        world.logic_tick = ctx.logic_tick;
        world.update_all_entities(ctx);
    }
    bool sees(int ssn = 9) const {
        return world.script.relations.single_single(TriggerRelations::kSees, 8, ssn);
    }
};

static void test_idle_scan_relations_and_history() {
    Fixture f;
    const auto target = f.person({3, 1, 2});
    f.tick(496);
    CHECK(!f.body().inf.head_look_target.valid() && !f.sees());
    f.tick(511);
    CHECK(!f.body().inf.head_look_target.valid() && !f.sees());
    f.tick(512);
    CHECK(f.body().inf.head_look_target == target);
    CHECK(f.body().inf.last_look_target == target);
    CHECK(!f.body().inf.previous_look_target.valid());
    const auto &r = f.world.script.relations;
    CHECK(r.group_group(TriggerRelations::kSees, 2, 3));
    CHECK(r.single_group(TriggerRelations::kSees, 8, 3));
    CHECK(r.group_single(TriggerRelations::kSees, 2, 9));
    CHECK(f.sees());
    CHECK(!r.single_single(TriggerRelations::kTargeted, 8, 9));
    CHECK(f.body().heading > 0 && f.body().inf.body_heading == 0);
    CHECK(f.body().inf.anim_state == 125);
    CHECK(!f.body().inf.aim_valid);
    const auto baseline = f.world.snapshot();
    f.tick(768); // The last-look penalty makes this candidate uninteresting.
    CHECK(!f.body().inf.head_look_target.valid());
    CHECK(!f.body().inf.last_look_target.valid());
    CHECK(f.body().inf.previous_look_target == target);
    f.tick(1024); // Previous-look is excluded entirely.
    CHECK(!f.body().inf.head_look_target.valid());
    f.tick(1280); // Two scans later it is eligible again.
    CHECK(f.body().inf.head_look_target == target);
    f.world.restore(baseline);
    CHECK(f.body().inf.head_look_target == target);
    CHECK(!f.body().inf.previous_look_target.valid());
    CHECK(f.sees());
    f.tick(768);
    CHECK(!f.body().inf.head_look_target.valid());
}

static void test_speaker_identity_and_authority() {
    Fixture f;
    const auto target = f.person({8, 2, 2});
    // Radio is anchored at the listener; the PORTRAIT speaker gets the frequent
    // scan and same-team bonus. The normal proximity score here is below four.
    ScriptVoiceChannel::State voice;
    voice.anchor = target;
    voice.portrait = f.observer;
    f.world.script.voice.restore(voice);
    f.tick(497);
    CHECK(!f.sees()); // Voice still obeys the outer 16-tick gate.
    f.tick(512, false);
    CHECK(!f.sees());
    // The speaker rescans on the 32-tick perception phase, not on every think:
    // 528 & 31 == 16 does not scan, 544 (& 255 == 32, & 31 == 0) does through
    // the speaker arm alone. [orig: phase @0x4BBE4A, re-read @0x4BE0E0]
    f.tick(528);
    CHECK(!f.body().inf.head_look_target.valid() && !f.sees());
    f.tick(544);
    CHECK(f.body().inf.head_look_target == target && f.sees());
    Fixture anchored;
    anchored.person({8, 2, 2});
    voice.anchor = anchored.observer;
    voice.portrait = EntityHandle::make(0, 1);
    anchored.world.script.voice.restore(voice);
    anchored.tick(544);
    CHECK(!anchored.sees());
}

static void test_candidate_filters_and_unsigned_diagonal_score() {
    Fixture f;
    const auto near = f.person({2, 0, 2});
    const auto diagonal = f.person({19, 10, 2}, 10);
    f.tick(512);
    CHECK(f.body().inf.head_look_target == diagonal);
    CHECK(f.sees(10) && !f.sees(9));
    CHECK(f.body().inf.last_look_target != near);
    for (int gate = 0; gate < 4; ++gate) {
        Fixture blocked;
        const auto h = blocked.person({3, 1, 2});
        auto &candidate = *blocked.world.registry.get(h);
        if (gate == 0) candidate.flags |= kEntityFlagCarried;
        if (gate == 1) candidate.item_id = 0;
        if (gate == 2) {
            blocked.world.cached.local_player = h;
            blocked.world.rules.ai_rules_skip_local_player = true;
        }
        if (gate == 3) candidate.position.x = 21;
        blocked.tick(512);
        CHECK(!blocked.sees() && !blocked.body().inf.head_look_target.valid());
    }
}

static void test_spotting_side_effects_precede_front_arc() {
    Fixture corpse;
    const auto dead = corpse.person({-3, 1, 2}, 9, 3, 2);
    auto &row = *corpse.world.registry.get(dead);
    row.flags |= kEntityFlagDead;
    row.health = 0;
    row.has_item_def = false; // This scan requires only a nonzero item ID.
    corpse.tick(512);
    CHECK(corpse.body().inf.damage_timer == 25);
    CHECK(!corpse.body().inf.ai_focus.valid());
    CHECK(!corpse.sees() && !corpse.body().inf.head_look_target.valid());
    Fixture enemy;
    const auto threat = enemy.person({-3, 1, 2}, 9, 3, 2);
    // Reject only in the weapon query, preserving the idle scan's own admission.
    enemy.world.registry.get(threat)->armor_impact = -1;
    enemy.world.registry.get(threat)->armor_kz = -1;
    enemy.body().slot.f[1] = 0;
    enemy.tick(512);
    CHECK(!enemy.body().inf.combat_target.valid());
    CHECK(enemy.body().inf.damage_timer == 10);
    CHECK(enemy.body().inf.ai_focus == threat);
    CHECK(enemy.body().inf.aim_point[0] == fixed(-3));
    CHECK(!enemy.sees() && !enemy.body().inf.head_look_target.valid());
    Fixture blind;
    blind.person({-3, 1, 2}, 9, 3, 2);
    blind.tick(512);
    CHECK(blind.body().inf.damage_timer == 0 && !blind.body().inf.ai_focus.valid());
}

static void test_eye_tracking_clip_availability_and_cleanup() {
    Fixture f;
    const auto target = f.person({3, 1, 2});
    f.world.registry.get(target)->eye_offset_z = fixed(3);
    f.tick(512);
    CHECK(f.body().inf.head_look_target == target);
    CHECK(f.body().inf.aim_pitch == 357913920); // +/-30-degree head limit.
    CHECK(f.body().pitch > 0 && f.body().body_pitch == 0);
    f.world.registry.get(target)->position = {30, 0, 2};
    f.tick(528); // Track admission is checked between the 256-tick scans.
    CHECK(!f.body().inf.head_look_target.valid());
    CHECK(f.body().inf.last_look_target == target); // Tracking does not rotate history.
    Fixture unavailable;
    unavailable.clips.states.erase(125);
    unavailable.person({3, 1, 2});
    unavailable.tick(512);
    CHECK(unavailable.sees());
    CHECK(unavailable.body().inf.anim_state == 43);
    CHECK(unavailable.body().inf.aim_heading == 0);
    Fixture cleanup;
    const auto reset = cleanup.person({3, 1, 2});
    const int index = cleanup.world.ai.attach(reset);
    cleanup.world.ai.at(index)->inf.active = true;
    cleanup.world.ai.at(index)->pos[0] = fixed(3);
    cleanup.world.ai.at(index)->pos[1] = fixed(1);
    cleanup.world.ai.at(index)->pos[2] = fixed(2);
    cleanup.tick(512);
    CHECK(cleanup.body().inf.head_look_target == reset);
    entity_reset_to_spawn_state(cleanup.world, *cleanup.world.registry.get(reset));
    CHECK(!cleanup.body().inf.head_look_target.valid());
}

static void test_terrain_occludes_spotting() {
    Fixture f;
    const auto target = f.person({3, 1, 2}, 9, 3, 2);
    f.world.registry.get(target)->flags |= kEntityFlagDead;
    std::vector<uint16_t> heights(512 * 512, 65535);
    std::vector<int> grid(256, 1);
    opennova::terrain::TerrainHeightField terrain;
    terrain.heightmap = heights.data();
    terrain.dim = 512;
    terrain.layout.sector_grid = grid.data();
    f.world.ai.terrain = &terrain;
    const int32_t start[3] = {0, 0, fixed(2)}, end[3] = {fixed(3), fixed(1), fixed(2)};
    CHECK(!f.world.ai.line_of_sight_clear(f.world, start, end, f.observer, target));
    f.tick(512);
    CHECK(!f.sees() && f.body().inf.damage_timer == 0);
}

static void test_look_does_not_steer_root_motion() {
    Fixture f;
    f.clips.step = 0x4000;
    auto &body = f.body();
    body.inf.reset_body_animation(1);
    body.heading = body.inf.aim_heading = 0x20000000; // Looking 45 degrees right.
    f.tick(513); // No new think overwrites the desired gaze.
    CHECK(body.inf.body_heading == 0 && body.heading == 0x20000000);
    CHECK(body.pos[0] == 0x4000 && body.pos[1] == 0);
    body.inf.target_heading = 0x20000000;
    body.heading = 0x10000000;
    body.inf.aim_heading = 337708816; // Existing look offset plus the body turn.
    f.tick(514);
    CHECK(body.inf.body_heading == 69273360);
    CHECK(body.heading == 337708816);
    CHECK(io::bam_sub(body.heading, body.inf.body_heading) == 0x10000000);
}

static void test_look_chase_instruction_boundaries() {
    InfantryState inf;
    int32_t yaw = 0, pitch = 0;
    inf.aim_heading = inf.aim_pitch = 0x20000000;
    infantry_look_tick(inf, yaw, pitch);
    CHECK(yaw == 0x04000000 && pitch == 0x04000000); // Positive arm is not symmetric.
    yaw = pitch = 0;
    inf.aim_heading = inf.aim_pitch = -0x20000000;
    infantry_look_tick(inf, yaw, pitch);
    CHECK(yaw == -0xE00000 && pitch == -0xC00000);
    yaw = pitch = 0;
    inf.aim_heading = inf.aim_pitch = 0x78000000;
    infantry_look_tick(inf, yaw, pitch);
    CHECK(yaw == 0xE00000 && pitch == 0xC00000);
    inf.aim_valid = true;
    yaw = pitch = 0;
    inf.aim_heading = inf.aim_pitch = 0x20000000;
    infantry_look_tick(inf, yaw, pitch);
    CHECK(yaw == 0x08000000 && pitch == 0x08000000);
    yaw = pitch = 0;
    inf.aim_heading = inf.aim_pitch = -0x20000000;
    infantry_look_tick(inf, yaw, pitch);
    CHECK(yaw == -0x1E00000 && pitch == -0x1E00000);
    inf.aim_valid = false;
    yaw = 0x50000000; pitch = 0;
    inf.aim_heading = yaw;
    infantry_look_tick(inf, yaw, pitch);
    CHECK(yaw == 0x40000000);
    yaw = 0x7FFFFFF0; pitch = 0;
    inf.body_heading = yaw;
    inf.aim_heading = static_cast<int32_t>(0x80000070u);
    infantry_look_tick(inf, yaw, pitch);
    CHECK(yaw == static_cast<int32_t>(0x80000000u)); // Short arc wraps through the seam.
    yaw = pitch = 0;
    inf.aim_heading = inf.aim_pitch = 0x20000000;
    infantry_look_tick(inf, yaw, pitch, true);
    CHECK(yaw == 0x02000000 && pitch == 0x04000000); // Mounted chase ignores aimFlag.
}

static void test_mounted_attention_respects_vehicle_motion() {
    for (const SeatType type : {SeatType::Passenger, SeatType::Controller, SeatType::Driver}) {
        for (const int32_t speed : {0, 1000}) {
            Fixture f;
            const auto target = f.person({3, 1, 2});
            Entity vehicle;
            vehicle.kind = EntityKind::Item;
            vehicle.item_id = 2;
            vehicle.has_item_def = true;
            vehicle.health = 100;
            vehicle.position = {0, 0, 2};
            vehicle.yaw = 90;
            Seat seat;
            seat.type = type;
            seat.bone_index = 1;
            seat.source_name = "sitex00";
            vehicle.seats.push_back(seat);
            const auto carrier = f.world.registry.spawn(1, vehicle);
            f.world.ai.attach(carrier);
            f.world.ai.for_handle(carrier)->brain.f[136] = speed;
            CHECK(f.world.vehicles.process_attach(f.observer, carrier, 1));
            f.body().heading = f.body().inf.body_heading = f.body().inf.aim_heading = 0;
            f.tick(512);
            CHECK(f.body().inf.head_look_target == target && f.sees());
            if (type == SeatType::Passenger || speed == 0) {
                CHECK(f.body().inf.aim_heading > 0 && f.body().heading > 0);
            } else {
                CHECK(f.body().inf.aim_heading == 0 && f.body().heading == 0);
            }
            const int32_t heading = f.body().heading;
            const int32_t pitch = f.body().pitch;
            CHECK(f.world.ai.refresh_mounted_pose(f.body(), f.world));
            CHECK(f.body().heading == heading && f.body().pitch == pitch);
        }
    }
}

int main() {
    test_idle_scan_relations_and_history();
    test_speaker_identity_and_authority();
    test_candidate_filters_and_unsigned_diagonal_score();
    test_spotting_side_effects_precede_front_arc();
    test_eye_tracking_clip_availability_and_cleanup();
    test_terrain_occludes_spotting();
    test_look_does_not_steer_root_motion();
    test_look_chase_instruction_boundaries();
    test_mounted_attention_respects_vehicle_motion();
    std::printf("infantry attention: %s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
