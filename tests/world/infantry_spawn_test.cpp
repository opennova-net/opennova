// Fresh org1 initialization and the native dual-channel spawn settle.
// [orig: Entity_InitOrganicAI @0x4BFCC0; Entity_WarmUpOrganicAnimation @0x4B8B20]
#include <runtime/world/entity_spawn.h>
#include <runtime/world/world.h>
#include <runtime/terrain_query/height_field.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <set>
#include <vector>

using namespace opennova::world;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %d: %s\n", __LINE__, #c); ++failures; } } while (0)

struct Source final : IRootMotionSource {
    std::set<int> clips{0, 1, 27, 28, 29, 43, 44, 67, 68, 75, 76, 140};
    std::vector<int> calls;
    int32_t bottom = 65536, rise = 0, vertical = 0;
    bool has_clip(int, int state) const override { return clips.count(state) != 0; }
    int32_t clip_length_ticks(int, int, int) const override { return 10000; }
    bool advance(int, int state, int32_t &phase, RootMotionFrame &out) override {
        if (!has_clip(0, state)) return false;
        calls.push_back(state);
        ++phase;
        out = {};
        out.dx = 50000; out.dy = 30000; out.dz = state == 0 ? 0 : vertical;
        out.capsule_bottom = bottom + rise * phase;
        out.capsule_top = out.capsule_bottom + 65536;
        out.events = 0xFF; // warmup does not fire weapons or enqueue footstep events
        return true;
    }
};

struct Rig {
    std::unique_ptr<World> owned = std::make_unique<World>();
    Source source;
    EntityHandle handle;
    Rig(int id = 0) {
        auto &world = *owned;
        for (int pool = 0; pool < 4; ++pool) world.registry.configure_pool(pool, 16);
        Entity seed;
        seed.kind = EntityKind::Organic; seed.item_id = 41; seed.has_item_def = true;
        seed.item_type = 3; seed.net_id = uint16_t(id); seed.team = 1; seed.group_id = 7;
        seed.position = {8, 8, 0}; seed.yaw = 90;
        handle = world.registry.spawn(0, seed);
        world.ai.attach(handle);
        body().inf.active = true;
        body().net_id = id;
        body().pos[0] = body().pos[1] = 8 * 65536;
        body().net_saved_live_pose[0] = 3 * 65536;
        body().net_saved_live_pose[1] = 4 * 65536;
        body().net_saved_live_pose[2] = 5 * 65536;
        body().profile.clip_size = 23;
        body().slot.f[18] = 3 * 62 + 61;
        world.ai.root_motion = &source;
    }
    Entity &entity() { return *owned->registry.get(handle); }
    AiEntity &body() { return *owned->ai.for_handle(handle); }
    void init() { initialize_organic_ai(*owned, entity()); }
    EntityHandle zone(int pool, int group, int team, bool definition = true) {
        Entity seed;
        seed.item_id = 77; seed.has_item_def = definition;
        seed.group_id = group; seed.team = uint8_t(team);
        seed.item_attrib = kItemAttribSpawnPoint;
        return owned->registry.spawn(pool, seed);
    }
};

void warmup_permutation_and_root_motion() {
    const int ids[] = {0, 1, 2, 3, 4, 12, 15, 12000, 12001};
    const int steps[] = {10, 266, 138, 394, 42, 106, 490, 10, 266};
    for (int i = 0; i < 9; ++i) {
        Rig r(ids[i]);
        r.body().slot.f[35] = 1;
        r.init();
        CHECK(r.body().inf.clip_phase == steps[i] + 1);
        CHECK(r.body().inf.wpn_clip_phase == steps[i] + 1);
        // Idle's reset blend lasts ten ticks; walk has flag 0x400 and
        // lasts fifteen (g_animStateFlagsTable[1] = 0x449).
        CHECK(std::count(r.source.calls.begin(), r.source.calls.end(), 0) ==
                10 + std::min(steps[i] + 1, 15));
        std::vector<int> targets;
        for (int state : r.source.calls) if (state != 0) targets.push_back(state);
        CHECK(targets.size() == size_t(2 * (steps[i] + 1)));
        for (size_t n = 0; n + 1 < targets.size(); n += 2)
            CHECK(targets[n] == 43 && targets[n + 1] == 1);
        CHECK(r.body().pos[0] == 8 * 65536 && r.body().pos[1] == 8 * 65536);
        CHECK(r.body().pos[2] == 65536);
        CHECK(r.body().inf.last_events == 0);
        CHECK(r.entity().net_anim_phase == std::min(steps[i] + 1, 255));
        CHECK(r.entity().npc_respawns == 3 && r.body().inf.magazine == 23);
        CHECK(r.entity().spawn_position.z == 0);
        CHECK(r.body().inf.aim_point[0] == 6 * 65536); // saved X + three units
        CHECK(r.body().inf.aim_point[1] == 4 * 65536);
        CHECK(r.body().inf.aim_point[2] == 5 * 65536);
    }
    Rig ramp;
    ramp.source.bottom = 1024; ramp.source.rise = 10; ramp.source.vertical = 100;
    ramp.init();
    CHECK(ramp.body().pos[2] == 1234); // first blended DZ, bottom deltas, final bottom and extra step
    CHECK(ramp.body().inf.prev_capsule_bottom == 1134);

    Rig absent;
    absent.owned->ai.root_motion = nullptr;
    absent.init();
    CHECK(absent.body().pos[2] == 0 && absent.body().inf.clip_phase == 0);
    CHECK(absent.body().inf.magazine == 23);
}

void postures_mounts_and_wash() {
    for (const auto c : {std::pair<uint32_t, int>{0, 43}, {0x200, 76}, {0x240, 140}}) {
        Rig r; r.entity().flags = c.first; r.init();
        CHECK(r.body().inf.anim_state == c.second && r.body().inf.wpn_state == 43);
        CHECK(r.entity().spawn_flags == c.first);
    }
    Rig route;
    route.body().slot.f[35] = 1; route.entity().flags = 0x240;
    route.body().slot.f[37] = 126; route.init();
    CHECK(route.body().inf.anim_state == 44);
    route.body().slot.f[37] = 127; route.init();
    CHECK(route.body().inf.anim_state == 43);
    route.source.clips.erase(76); route.source.clips.erase(140);
    route.body().slot.f[37] = 0; route.init();
    CHECK(route.body().inf.anim_state == 1);

    Rig mounted;
    const auto parent = mounted.zone(1, 0, 1);
    auto &carrier = *mounted.owned->registry.get(parent);
    carrier.ground_target = mounted.zone(2, 0, 1);
    carrier.emplaced_config = 8;
    mounted.entity().mount_target = parent;
    mounted.init();
    CHECK(mounted.body().inf.anim_state == 75);
    CHECK(mounted.body().inf.wpn_state == 43);
    CHECK(mounted.body().inf.clip_phase == 10 && mounted.body().inf.wpn_clip_phase == 10);
    CHECK(mounted.body().pos[2] == 0);
    CHECK(mounted.entity().ground_target == carrier.ground_target);
    carrier.emplaced_config = 7; mounted.init(); // unavailable clip -> generic mount
    CHECK(mounted.body().inf.anim_state == 67);

    Rig wash;
    Entity helicopter;
    helicopter.position = {8, 8, 6};
    helicopter.veh.part_spin.speed = 214748352;
    const auto hh = wash.owned->registry.spawn(1, helicopter);
    VehicleTraits traits; traits.family = VehicleFamily::Helicopter;
    wash.owned->rotor_wash.update(*wash.owned->registry.get(hh), traits);
    wash.init();
    CHECK(wash.body().inf.anim_state == 27);
    wash.body().slot.f[35] = 1; wash.init();
    CHECK(wash.body().inf.anim_state == 28);
    wash.source.clips.erase(28); wash.init();
    CHECK(wash.body().inf.anim_state == 1);

    Rig player;
    player.entity().flags = kEntityFlagPlayer;
    player.body().inf.magazine = 4;
    player.init();
    CHECK(player.source.calls.empty() && player.body().inf.magazine == 4);
}

void control_point_pool_precedence() {
    Rig r;
    r.zone(1, 7, 2, false); // absent definition is not a candidate
    r.zone(1, 0, 2);
    const auto first = r.zone(1, 7, 1);
    r.zone(1, 7, 2);
    r.init();
    CHECK(r.entity().npc_respawn_zone == first && !r.entity().hidden);
    const auto second_pool = r.zone(2, 7, 2);
    r.zone(2, 7, 1);
    r.zone(3, 7, 1); // marker pool is not scanned
    r.init();
    CHECK(r.entity().npc_respawn_zone == second_pool);
    CHECK(r.entity().hidden && (r.entity().flags & 1) != 0);
    CHECK((r.entity().spawn_flags & 1) == 0); // backup predates team hiding
    auto &zone = *r.owned->registry.get(second_pool);
    zone.team = 1; zone.zone_control = 65536;
    CHECK(npc_respawn_unhide(*r.owned, r.owned->ai, r.entity()));
    CHECK(!r.entity().hidden);
}

void grounding_strict_one_unit_limit() {
    std::vector<uint16_t> heights(512 * 512, 0);
    std::vector<int> sectors(256, 1);
    opennova::terrain::TerrainHeightField field;
    field.heightmap = heights.data(); field.dim = 512;
    field.layout.sector_grid = sectors.data();
    for (const auto c : {std::pair<int32_t, int32_t>{32768, 65536},
                         {65535, 65536}, {65536, 131072}, {-32768, 65536}}) {
        Rig r;
        CollisionWorld collision;
        collision.terrain = &field;
        r.owned->ai.collision = &collision;
        r.body().pos[2] = c.first;
        r.entity().position.z = c.first / 65536.0f;
        r.body().collide_state.skip_counter = 25;
        r.init();
        CHECK(r.body().pos[2] == c.second);
        CHECK(r.entity().spawn_position.z == c.first / 65536.0f);
        CHECK(r.entity().position.z == c.second / 65536.0f);
        CHECK(r.body().collide_state.skip_counter == 0);
    }
}
} // namespace

int main() {
    warmup_permutation_and_root_motion();
    postures_mounts_and_wash();
    control_point_pool_precedence();
    grounding_strict_one_unit_limit();
    std::printf("infantry_spawn: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
