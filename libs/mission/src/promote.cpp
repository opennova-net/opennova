// Mission -> world promotion. See mission/promote.h + notes/world/ai_movement.md §10.
#include "mission/promote.h"

#include <algorithm>

namespace opennova::mission {

using namespace opennova::world;

namespace {

// degrees -> 32-bit binary angle (the entity-heading unit, entity+16). [orig: AI_HandleCommand
// command 0x16 @0x4659fa multiplies degrees by 11930464.]
constexpr int64_t kBamPerDegree = 11930464;

// Provisional kind -> g_pool_list index. The exact original mapping matters only for the
// deferred acquire_target pool scan (P2), not for movement; documented in notes §10.
int pool_for_kind(EntityKind k) {
    switch (k) {
        case EntityKind::Organic: return 0;
        case EntityKind::Item: return 1;
        case EntityKind::Building: return 2;
        case EntityKind::Marker: return 3;
    }
    return 0;
}

Entity make_seed(const bms::Entity &e, EntityKind kind, uint16_t ssn, uint32_t origin) {
    Entity s;
    s.net_id = ssn;
    s.bms_id = e.id; // carry the file entity id so the host can map this entity back to its placed node
    s.kind = kind;
    s.item_id = e.type_id;
    s.position.x = e.get_x();
    s.position.y = e.get_y();
    s.position.z = e.get_z();
    s.yaw = e.yaw;
    s.pitch = e.pitch;
    s.roll = e.roll;
    s.team = e.team;
    s.group_id = e.group_id;
    s.waypoint_id = e.waypoint_id;
    s.wp_number = e.wp_number;
    s.alert_state = e.alert_state;
    s.ai_flags = e.bmsi_attributes;
    s.engage_min = e.min_engagement_distance;
    s.engage_max = e.max_engagement_distance;
    s.attack_max = e.max_attack_distance;
    s.spawn_origin = origin;
    return s;
}

// Initialize a freshly-attached AI brain for a spawned entity. Grounded in Entity_InitVehicleAI
// @0x460200 (geometry copy from entity+4.., initial state 0, idle move-step); the profile/speed
// mapping is from the mission AI fields (tracked deviation: the real items.def AIProfile_LoadOrFind
// @0x45fd80 + the state-0 -> 16 transition are unmodeled).
void init_brain(AiEntity &ae, const bms::Entity &e, const PromoteOptions &opts, const AiSystem &ai) {
    AiBrain &b = ae.brain;

    // Geometry (entity+4/+8/+12 = x/y/z, all 16.16; entity+16 heading = BAM).
    ae.pos[0] = e.x;
    ae.pos[1] = e.y;
    ae.pos[2] = e.z;
    ae.heading = static_cast<int32_t>(static_cast<int64_t>(e.yaw) * kBamPerDegree);
    ae.team = e.team;

    // [orig Entity_InitVehicleAI: brain[4]=brain[5]=brain[6]=0] initial state 0.
    b.f[AiBrain::kCurState] = 0;
    b.f[AiBrain::kPendState] = 0;
    b.f[AiBrain::kFallback] = 0;
    b.f[AiBrain::kStep] = 16; // idle move-step (AI_SetStateIdle); nonzero so the mover advances

    // Profile (movement-relevant subset) from the mission AI fields.
    ae.profile.flags96 = 0;   // not combat-capable / can-fire here (the weapon phase sets these)
    ae.profile.flags100 = 0;  // no use-fallback / ignore-stealth
    ae.profile.range_primary = static_cast<int16_t>(e.max_engagement_distance >> 16);
    ae.profile.range_secondary = static_cast<int16_t>(e.min_engagement_distance >> 16);

    // Per-node mover speed: brain[49]=kSpeedA (states != 16), brain[50]=kSpeedB (state 16).
    b.f[AiBrain::kSpeedA] = opts.default_speed;
    b.f[AiBrain::kSpeedB] = opts.default_speed;

    // Waypoint route -> GROUND_FOLLOWWP (channel = the entity's waypoint_id).
    const NavChannel *ch = ai.nav.channel(e.waypoint_id);
    if (ch && ch->count > 0) {
        b.f[AiBrain::kWpType] = 1; // nav-node waypoint
        b.f[AiBrain::kWpChannel] = e.waypoint_id;
        b.f[AiBrain::kWpNode] = std::min<int32_t>(e.wp_number, ch->count - 1);
        if (opts.patrol_on_spawn) {
            b.f[AiBrain::kCurState] = 16; // GROUND_FOLLOWWP (tracked deviation: orig inits 0)
            b.f[AiBrain::kPendState] = 16;
        }
    }
}

} // namespace

PromoteResult promote_mission(const bms::File &m, World &world, AiSystem &ai,
                              const PromoteOptions &opts) {
    PromoteResult r;

    // Pools (pools 0..3 = actors searched by net id, pool 4 = static props).
    world.registry.configure_pool(0, opts.actor_pool_capacity);
    world.registry.configure_pool(1, opts.actor_pool_capacity);
    world.registry.configure_pool(2, opts.actor_pool_capacity);
    world.registry.configure_pool(3, opts.marker_pool_capacity);
    world.registry.configure_pool(4, opts.actor_pool_capacity);

    // Nav nodes from markers (pool-3 positions). Node index == marker order, which is exactly
    // what WaypointRecord::waypoint_numbers indexes. Markers feed the AI nav table only (not the
    // actor pools); spawning them as render/BMS entities is a follow-up.
    ai.nav.nodes.clear();
    ai.nav.nodes.reserve(m.markers.size());
    for (const bms::Entity &mk : m.markers) {
        NavEntry n;
        n.f[0] = opts.arrival_radius; // payload0 = arrival/advance threshold
        n.f[1] = mk.x;                // entity+4
        n.f[2] = mk.y;                // entity+8
        n.f[3] = mk.z;                // entity+12
        n.f[4] = 0;
        ai.nav.nodes.push_back(n);
    }
    r.nav_nodes = static_cast<int>(ai.nav.nodes.size());

    // Nav channels from waypoint records. The AI channel id is 1-based: navMeshId 0 is the
    // "no route" sentinel (AIWaypoint_UpdateTarget @0x457380 returns -1 when navMeshId==0), so
    // an entity's waypoint_id == its channel, and waypoint_records[i] populates channel (i+1).
    // [orig: XML_ParseGroupAction @0x4cc450 writes the 34-dword record per channel.]
    ai.nav.channels.clear();
    ai.nav.channels.reserve(m.waypoint_records.size() + 1);
    ai.nav.channels.emplace_back(); // channel 0 = empty "no route" sentinel
    for (const bms::WaypointRecord &wr : m.waypoint_records) {
        NavChannel ch;
        ch.loopflag = (static_cast<uint32_t>(wr.flags) &
                       static_cast<uint32_t>(bms::WaypointFlags::DoesNotLoop)) ? 1 : 0;
        int count = std::min<int>(static_cast<int>(wr.marker_count), 32);
        count = std::min<int>(count, static_cast<int>(wr.waypoint_numbers.size()));
        ch.count = count;
        for (int k = 0; k < count; ++k)
            ch.entries[k] = static_cast<int32_t>(wr.waypoint_numbers[k]);
        ai.nav.channels.push_back(ch);
    }
    r.nav_channels = static_cast<int>(m.waypoint_records.size());

    // Area-trigger zones -> the registry's area table, registered in array order so the area id ==
    // the area-trigger array index (the value SingleIsWithinArea/GroupIsWithinArea param2 references;
    // bms.h param-semantics). Z is unbounded (+-16384.0) unless the trigger constrains it. Without
    // this the within-area triggers + AREA_AI family resolve against an empty table (always false).
    // [The designer-zone-id (1..99) <-> array-index correspondence + AREA_AI param1's exact zone
    // reference are grill-gated (P5); registering the table is the prerequisite.]
    for (const bms::AreaTrigger &at : m.area_triggers) {
        Aabb b;
        b.min.x = at.get_x_min(); b.max.x = at.get_x_max();
        b.min.y = at.get_y_min(); b.max.y = at.get_y_max();
        if (at.constrains_z()) { b.min.z = at.get_z_min(); b.max.z = at.get_z_max(); }
        else { b.min.z = bms::AreaTrigger::kUnboundedZMin; b.max.z = bms::AreaTrigger::kUnboundedZMax; }
        world.registry.register_area(std::string(), b);
    }

    // Spawn actors + AI brains (organics are AI-driven; vehicles get brains in the vehicle phase).
    uint16_t ssn = opts.first_ssn;
    auto promote_vec = [&](const std::vector<bms::Entity> &vec, EntityKind kind, bool ai_capable) {
        uint32_t idx = 0;
        for (const bms::Entity &e : vec) {
            uint32_t origin = (static_cast<uint32_t>(kind) << 24) | (idx & 0xFFFFFF);
            ++idx;
            Entity seed = make_seed(e, kind, ssn, origin);
            EntityHandle h = world.registry.spawn(pool_for_kind(kind), seed);
            if (!h.valid()) { ++r.dropped; continue; }
            ++ssn;
            ++r.spawned;
            if (ai_capable) {
                int ai_idx = ai.attach(h);
                AiEntity &ae = *ai.at(ai_idx);
                init_brain(ae, e, opts, ai);
                ae.net_id = seed.net_id;
                ae.relmat_id = seed.net_id; // provisional relation-matrix id (net layer = later)
                ae.health = 100;
                ++r.brains;
            }
        }
    };
    promote_vec(m.organics, EntityKind::Organic, /*ai_capable=*/true);
    promote_vec(m.items, EntityKind::Item, /*ai_capable=*/false);
    promote_vec(m.buildings, EntityKind::Building, /*ai_capable=*/false);

    return r;
}

} // namespace opennova::mission
