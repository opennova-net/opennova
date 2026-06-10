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

// Whether an items.def type id is an emplaced gun (offers a UseGun seat). The original derives this
// from the model's seat-bone names (model[605..], "UseGun") at Entity_FindBestSeatSlot @0x4351f0; we
// don't load model bones in promotion, so this is a data table. It is EMPTY pending the items.def
// emplacement set: the mount MECHANISM (Entity.seats + EntityCommands::mount/mount_best/dismount +
// the AttachToEmplaced action + the seat-follow poser) is complete and tested, but auto-seeding
// which placed items are guns needs the def/model data. Tracked-TODO (notes/mission).
bool is_emplacement_item(int32_t /*item_id*/) {
    return false;
}

// Seed the seats an emplaced gun offers: one Gunner seat at the model origin (seat-local +
// yaw_offset default 0 until the real seat-bone transform is read).
void seed_emplacement_seats(Entity &item) {
    if (!is_emplacement_item(item.item_id)) return;
    Seat s;
    s.type = SeatType::Gunner;
    item.seats.push_back(s);
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

    // Geometry (entity+4/+8/+12 = x/y/z, all 16.16; entity+16 heading = BAM). The engine heading is
    // (90 - yaw) degrees, NOT yaw [orig: Entity_SpawnFromBMSRecord @0x40e9f0 entity+4 =
    // ((90 - yaw)<<16/360)<<16]. The waypoint mover writes the same engine frame (atan2(dY,dX) bearing
    // into kWorkHeading), so storing the seed in the engine frame keeps a unit's facing consistent
    // whether parked or moving; the host present converts engine-heading -> mission yaw for the basis.
    ae.pos[0] = e.x;
    ae.pos[1] = e.y;
    ae.pos[2] = e.z;
    ae.heading = static_cast<int32_t>(static_cast<int64_t>(90 - e.yaw) * kBamPerDegree);
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

// Seed the infantry motor + AiSlot for an organic (entity class org1). Field map grounded in
// Entity_SpawnFromBMSRecord @0x40e9f0 (the AiSlot block) — see docs/world/world-wac-ai-re.md §3.2.
void init_infantry(AiEntity &ae, const bms::Entity &e) {
    ae.inf.active = true;
    // Face stays where it spawned until an order steers it.
    ae.inf.body_heading = ae.heading;
    ae.inf.target_heading = ae.heading;

    AiSlot &s = ae.slot;
    // [orig: slot+48/+52 = (field<<16)/100 — perception2/perfectionist2 are the AI move-speed
    // percentages (engine truth: the editor-era names are misleading)]
    s.f[12] = (e.perception2 << 16) / 100;
    s.f[13] = (e.perfectionist2 << 16) / 100;
    // [orig: slot+56 = (obliqueness<<8)/360]
    s.f[14] = (static_cast<int32_t>(e.obliqueness) << 8) / 360;
    // [orig: slot+60 max / +64 min engagement (<<16, both fall back to max_attack_distance),
    //  +68 = max_attack_distance<<16]
    s.f[15] = (e.max_engagement_distance != 0 ? e.max_engagement_distance
                                              : e.max_attack_distance) << 16;
    s.f[16] = (e.min_engagement_distance != 0 ? e.min_engagement_distance
                                              : e.max_attack_distance) << 16;
    s.f[17] = e.max_attack_distance << 16;
    // [orig: slot+40/+44 = 100 - w_accuracy2/w_accuracy1, clamped >= 0; 0 -> 100]
    s.f[10] = (e.w_accuracy2 != 0) ? std::max(0, 100 - e.w_accuracy2) : 100;
    s.f[11] = (e.w_accuracy1 != 0) ? std::max(0, 100 - e.w_accuracy1) : 100;
    // [orig: timers x62 ticks/second — slot+72 movetimer, +76 crouchtimer, +80 shoot_timer,
    //  +88 advancetimer]
    s.f[18] = 62 * static_cast<int32_t>(e.spawns);
    s.f[19] = 62 * (static_cast<int32_t>(e.crouch_timer) |
                    (static_cast<int32_t>(e.unk15a) << 8));
    s.f[20] = 62 * static_cast<int32_t>(e.shoot_timer);
    s.f[22] = 62 * e.advancetimer;
    // [orig: slot byte+136 = alert_state]
    s.bytes()[AiSlot::kMoveFlagByte] = e.alert_state;
    // [orig: slot+140 = 1 (has-route), +148 = waypoint_id, +152 = wp_number (START node)]
    if (e.waypoint_id != 0) {
        s.f[35] = 1;
        s.f[37] = e.waypoint_id;
        s.f[38] = e.wp_number;
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

    // Nav nodes from markers. Node index == marker order, which is exactly what
    // WaypointRecord::waypoint_numbers indexes. Markers ALSO spawn into pool 3 below
    // (the original spawns them as full entities; the nav table reads them in place,
    // our container rebase keeps a separate node array).
    ai.nav.nodes.clear();
    ai.nav.nodes.reserve(m.markers.size());
    for (const bms::Entity &mk : m.markers) {
        NavEntry n;
        // Arrival radius from the marker's wp_distance, default 0.5u. [orig:
        // Entity_SpawnFromBMSRecord @0x40e9f0 item 6005: entity dword[0] =
        // wp_distance ? wp_distance<<16 : 0x8000]
        n.f[0] = (mk.wp_distance != 0) ? (mk.wp_distance << 16) : opts.arrival_radius;
        n.f[1] = mk.x;                // entity+4
        n.f[2] = mk.y;                // entity+8
        n.f[3] = mk.z;                // entity+12
        // Facing = the marker's spawn heading (engine frame, like every entity).
        // [orig: marker+16; the infantry think faces it during a hold]
        n.f[4] = static_cast<int32_t>(static_cast<int64_t>(90 - mk.yaw) * kBamPerDegree);
        // Hold time: 62 ticks per movetimer second. [orig: entity[82] = 62 * u16@+62
        // (bms 'spawns' = .mis movetimer); 0 = no hold]
        n.wait_ticks = 62 * static_cast<int32_t>(mk.spawns);
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

    // Spawn actors + AI brains (organics are AI-driven; vehicles get brains in the vehicle
    // phase). The net id every trigger/action references is AUTHORED in the record —
    // [orig: Entity_SpawnFromBMSRecord @0x40e9f0 copies record dword @+8 to entity+124;
    // EntityPool_FindByNetId @0x4f0a20 matches its low 16 bits over pools 0..3] — so the
    // seed copies e.id verbatim (no load-time assignment). Spawn order mirrors the file
    // order in Mission_LoadBMSFile @0x40f4e0: items -> buildings -> markers -> organics.
    auto promote_vec = [&](const std::vector<bms::Entity> &vec, EntityKind kind, bool ai_capable) {
        uint32_t idx = 0;
        for (const bms::Entity &e : vec) {
            uint32_t origin = (static_cast<uint32_t>(kind) << 24) | (idx & 0xFFFFFF);
            ++idx;
            Entity seed = make_seed(e, kind, static_cast<uint16_t>(e.id), origin);
            EntityHandle h = world.registry.spawn(pool_for_kind(kind), seed);
            if (!h.valid()) { ++r.dropped; continue; }
            ++r.spawned;
            if (kind == EntityKind::Item) {
                if (Entity *spawned = world.registry.get(h)) seed_emplacement_seats(*spawned);
            }
            if (ai_capable) {
                int ai_idx = ai.attach(h);
                AiEntity &ae = *ai.at(ai_idx);
                init_brain(ae, e, opts, ai);
                if (kind == EntityKind::Organic) {
                    // Soldiers run the infantry motor, not the vehicle SM.
                    // [orig: g_EntityClassPhysicsTable "org1" -> Entity_UpdateInfantryAI]
                    init_infantry(ae, e);
                }
                ae.net_id = seed.net_id;
                ae.relmat_id = seed.net_id; // provisional relation-matrix id (net layer = later)
                ae.health = 100;
                ++r.brains;
            }
        }
    };
    promote_vec(m.items, EntityKind::Item, /*ai_capable=*/false);
    promote_vec(m.buildings, EntityKind::Building, /*ai_capable=*/false);
    promote_vec(m.markers, EntityKind::Marker, /*ai_capable=*/false);
    promote_vec(m.organics, EntityKind::Organic, /*ai_capable=*/true);

    return r;
}

} // namespace opennova::mission
