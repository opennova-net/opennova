// Mission -> world promotion. See mission/promote.h + notes/world/ai_movement.md §10.
#include "mission/promote.h"

#include "world/ai.h"
#include "world/world.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace opennova::mission {

using namespace opennova::world;

// libs/world mirrors these bms::AttribFlags bits beside its mission_attrib_flags
// field (world stays mission-parser-free); this TU sees both headers, so it pins
// the mirror values to the canonical enum.
static_assert(World::kMissionAttribSinglePlayerRespawn ==
              static_cast<uint32_t>(bms::AttribFlags::SinglePlayerRespawn));
static_assert(World::kMissionAttribEnableNVG ==
              static_cast<uint32_t>(bms::AttribFlags::EnableNVG));

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

const ItemSeatSpec *seat_spec_for_type(const PromoteOptions &opts, int32_t type_id) {
    for (const ItemSeatSpec &spec : opts.item_seat_specs) {
        if (spec.type_id == type_id) return &spec;
    }
    return nullptr;
}

void seed_authored_seats(Entity &entity, const PromoteOptions &opts) {
    const ItemSeatSpec *spec = seat_spec_for_type(opts, entity.item_id);
    if (spec == nullptr) return;
    entity.emplaced_config_valid = spec->mount_config_valid;
    entity.emplaced_config = spec->mount_config_valid ? spec->mount_config : 0;
    entity.armory_points = spec->armory_points;
    entity.primary_weapon = spec->primary_weapon;
    if (spec->seats.empty()) return;
    entity.seats = spec->seats;
    for (Seat &seat : entity.seats) {
        seat.occupant = EntityHandle{};
    }
}

bool within_mount_radius(const Entity &occupant, const Entity &target, float radius) {
    const float dx = occupant.position.x - target.position.x;
    const float dy = occupant.position.y - target.position.y;
    const float dz = occupant.position.z - target.position.z;
    return dx * dx + dy * dy + dz * dz <= radius * radius;
}

struct PendingCommandMount {
    uint16_t occupant_ssn = 0;
    uint16_t target_ssn = 0;
    uint8_t command_id = 0;
    EntityHandle occupant_handle;
};

void apply_command_mounts(const std::vector<PendingCommandMount> &pending, World &world,
                          AiSystem &ai, const PromoteOptions &opts) {
    for (const PendingCommandMount &p : pending) {
        Entity *occupant = world.registry.get(p.occupant_handle);
        Entity *target = world.registry.get(world.registry.find_by_net_id(p.target_ssn));
        if (occupant == nullptr || target == nullptr) continue;
        if (!within_mount_radius(*occupant, *target, opts.command_mount_radius)) continue;
        if (!world.commands.mount_boarding_command(p.occupant_ssn, p.target_ssn, p.command_id))
            continue;
        if (AiEntity *ae = ai.for_handle(p.occupant_handle)) {
            ai.pose_if_mounted(*ae, world);
        }
    }
}

Entity make_seed(const bms::Entity &e, EntityKind kind, uint16_t ssn, uint32_t origin) {
    Entity s;
    s.net_id = ssn;
    s.bms_id = e.id; // carry the file entity id so the embedder can map this entity back to its placed node
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
    // The retail entity Flags dword (entity+36), BMS-attribute part — the 0x10 static record
    // streams it raw (D-NET-147/150). [orig: Entity_SpawnFromBMSRecord @0x40e9f0: attrib
    // 0x200000 -> 0x4000000 (Indestructible), 0x800000 -> 0x400 (Reflective),
    // 0x1000000 -> 0x1000000 (NoShadow)]. The item-def part (Building 0x20000 is kind-known
    // here; hp==0 -> 0x4000000 + subType 0xFF needs the item db) completes in the embedder's
    // item-traits sweep [orig: Entity_InitFromModel @0x40e105 / @0x40dc8e].
    using bms::BmsiAttributeFlags;
    const uint32_t attrib = e.bmsi_attributes;
    if (attrib & static_cast<uint32_t>(BmsiAttributeFlags::Indestructible)) s.engine_flags |= kEntityFlagIndestructible;
    if (attrib & static_cast<uint32_t>(BmsiAttributeFlags::Reflective)) s.engine_flags |= kEntityFlagReflective;
    if (attrib & static_cast<uint32_t>(BmsiAttributeFlags::NoShadow)) s.engine_flags |= kEntityFlagNoShadow;
    if (attrib & static_cast<uint32_t>(BmsiAttributeFlags::Guarding)) {
        s.engine_flags |= kEntityFlagMounted;
        s.flags |= kEntityFlagMounted;
    }
    if (kind == EntityKind::Building) s.engine_flags |= kEntityFlagBuilding;
    s.ammo_count = e.map_symbol; // BMS byte 81 -> entity+290 [orig: @0x40e9f0]
    s.ref_num = e.ref_num;       // BMS byte 153 -> entity+533 [orig: @0x40e9f0]
    // BMS byte 155 (.mis "lfp_group") -> entity+538 — the AS zone number (net-re §5.61).
    // [orig: Entity_SpawnFromBMSRecord @0x40e9f0]
    s.zone_number = e.lfp_group;
    // BMS word 14 (wp_distance low u16) -> entity+350 — the capture-zone/proximity radius
    // the 0x0D record's 0x2000/0x8000-gated u16 streams (golden ASH_I5A bunkers: 70).
    // [orig: Entity_SpawnFromBMSRecord @0x40e9f0; net-re §5.11]
    s.zone_radius = static_cast<uint16_t>(e.wp_distance & 0xFFFF);
    return s;
}

// Initialize a freshly-attached AI brain for a spawned entity. Grounded in Entity_InitVehicleAI
// @0x460200 (geometry copy from entity+4.., initial state 0, idle move-step); the profile/speed
// mapping is from the mission AI fields (tracked deviation: the real items.def AIProfile_LoadOrFind
// @0x45fd80 + the state-0 -> 16 transition are unmodeled).
void init_brain(AiEntity &ae, const bms::Entity &e, const PromoteOptions &opts, const AiSystem &ai,
                EntityKind kind) {
    AiBrain &b = ae.brain;

    // Geometry (entity+4/+8/+12 = x/y/z, all 16.16; entity+16 heading = BAM). The engine heading is
    // (90 - yaw) degrees, NOT yaw [orig: Entity_SpawnFromBMSRecord @0x40e9f0 entity+4 =
    // ((90 - yaw)<<16/360)<<16]. The waypoint mover writes the same engine frame (atan2(dY,dX) bearing
    // into kWorkHeading), so storing the seed in the engine frame keeps a unit's facing consistent
    // whether parked or moving; the present pass converts engine-heading -> mission yaw for the basis.
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

    // A drivable VEHICLE brain (kind Item, attached through the control-seat gate below)
    // starts in GROUND_FOLLOWWP whether or not a route is authored — the shipped ground
    // .aip profiles all author `default_state GROUND_FOLLOWWP` and the profile parse is
    // unported (D-AI-11); without the state the SM mover never feeds the AI-driver leg.
    // [orig: AIProfile_LoadOrFind @0x45fd80 -> the default_state field; d_5ton.aip etc.]
    if (kind == EntityKind::Item) {
        b.f[AiBrain::kCurState] = 16;
        b.f[AiBrain::kPendState] = 16;
        // No target acquisition for transport brains: the shipped drivable-transport
        // profiles author zero target priorities (d_5ton/d_buggy/G_Jeep priority_air/
        // ground/organics 0), and the D-AI-1 feed scans unconditionally where retail's
        // class table would reject — 13 per-tick pool scans + LOS rays stall the mission
        // load. flags100 bit1 is the witnessed acquire skip in the state-16 tick
        // [orig: the profile+100 & 2 gate @0x46775c]; lift with the .aip parse (D-AI-11).
        ae.profile.flags100 |= 2;
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
    // The authored AI attributes become AiSlot[1] control bits at spawn. Berserk
    // is retail's intentional attack-anyone exception to normal team filtering.
    // [orig: Entity_SpawnFromBMSRecord Blind @0x40ed92..0x40ed9b,
    //  Berserk @0x40eddd..0x40edea, Coward @0x40ee1a..0x40ee26]
    const uint32_t attrib = e.bmsi_attributes;
    if (attrib & static_cast<uint32_t>(bms::BmsiAttributeFlags::Blind)) s.f[1] |= 0x1;
    if (attrib & static_cast<uint32_t>(bms::BmsiAttributeFlags::Berserk)) {
        s.f[1] |= 0x200;
        ae.see_all = true;
    }
    if (attrib & static_cast<uint32_t>(bms::BmsiAttributeFlags::Coward)) s.f[1] |= 0x8;
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
        // The channel's dword 0 is the RAW route-flags word: bit0 = one-shot
        // (the only bit the movers mask), bit1/bit2 = the blue/red team-route
        // marks the waypoint-track pick below reads. [orig: Buffer[34*ch] —
        // the mover masks bit0 @0x457c3d-adjacent; the 0x0F waypoint writer
        // tests bit1 on the same dword @0x502e53]
        ch.loopflag = static_cast<int32_t>(static_cast<uint32_t>(wr.flags));
        int count = std::min<int>(static_cast<int>(wr.marker_count), 32);
        count = std::min<int>(count, static_cast<int>(wr.waypoint_numbers.size()));
        ch.count = count;
        for (int k = 0; k < count; ++k)
            ch.entries[k] = static_cast<int32_t>(wr.waypoint_numbers[k]);
        ai.nav.channels.push_back(ch);
    }
    r.nav_channels = static_cast<int>(m.waypoint_records.size());

    // The player waypoint track: the FIRST blue-route channel (flags bit 1 —
    // the same pick the 0x0F world-state writer serializes for a team-1
    // recipient) over the pool-3 markers, with the marker waypoint fields.
    // [orig: NetPacket_WriteWorldStateLoad0x0F @0x502e41 (channel scan) +
    //  Entity_SpawnFromBMSRecord @0x40f0aa (the marker fields); ≤128 entries]
    world.waypoints.clear();
    for (const bms::WaypointRecord &wr : m.waypoint_records) {
        if ((static_cast<uint32_t>(wr.flags) &
             static_cast<uint32_t>(bms::WaypointFlags::BlueTeam)) == 0)
            continue;
        const int count = std::min<int>({static_cast<int>(wr.marker_count),
                                         static_cast<int>(wr.waypoint_numbers.size()), 128});
        for (int k = 0; k < count; ++k) {
            const int idx = static_cast<int>(wr.waypoint_numbers[k]);
            if (idx < 0 || idx >= static_cast<int>(m.markers.size())) continue;
            const bms::Entity &mk = m.markers[static_cast<size_t>(idx)];
            WaypointEntry e;
            e.node = idx;
            e.x = mk.x;
            e.y = mk.y;
            e.z = mk.z;
            // [orig: entity+0 = wp_distance<<16, default 0x8000 @0x40f09b]
            e.radius = (mk.wp_distance != 0) ? (mk.wp_distance << 16) : opts.arrival_radius;
            // [orig: entity+672 = the record's ttool_index @0x40f0ad — the
            //  WPNames STRWPNAME%03i id]
            e.name_id = mk.ttool_index;
            // [orig: entity+528 = word rec 'wp_adv_trigger' @0x40f0b7; the
            //  fired-event index that completes this waypoint]
            e.linked_event = mk.wp_adv_trigger;
            // [orig: entity+535 = attributes bit 22 @0x40f123-0x40f129]
            e.chain_back = (mk.bmsi_attributes & (1u << 22)) != 0;
            world.waypoints.entries.push_back(e);
        }
        break; // first flagged channel only [orig: the @0x502e53 scan stops on the first hit]
    }

    // The objectives panel's per-slot text-id tables, from the mission header.
    // The engine indexes them 1-based off a byte pointer one BELOW the first
    // header byte (byte_A7628B[1] = win_conditions[0]); mirror that shift so
    // slot arithmetic stays the witnessed form. [orig: byte_A7628B/byte_A76293
    //  = header +0xBC win_conditions / +0xC4 lose_conditions; readers
    //  EventAction_Dispatch @0x454546/@0x454600, HUD_DrawWinConditions @0x5ba9e0]
    world.subgoals = World::SubgoalState{};
    for (int slot = 1; slot <= 8; ++slot) {
        world.subgoals.win_text_ids[slot] = m.header.win_conditions[slot - 1];
        world.subgoals.lose_text_ids[slot] = m.header.lose_conditions[slot - 1];
    }

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
        // The Active flag rides along for the player-AWOL probe, which walks only
        // active zones [orig: Entity_IsLocalPlayerOutOfBounds @0x439d40]. The
        // authored id (record dword @0) rides along for the load-time zone-ref
        // resolve [orig: @0x453000/@0x453100 match record[0]].
        world.registry.register_area(std::string(), b, at.is_active(), at.id);
    }

    // Spawn actors + AI brains (organics are AI-driven; vehicles get brains in the vehicle
    // phase). The net id every trigger/action references is AUTHORED in the record —
    // [orig: Entity_SpawnFromBMSRecord @0x40e9f0 copies record dword @+8 to entity+124;
    // EntityPool_FindByNetId @0x4f0a20 matches its low 16 bits over pools 0..3] — so the
    // seed copies e.id verbatim (no load-time assignment). Spawn order mirrors the file
    // order in Mission_LoadBMSFile @0x40f4e0: items -> buildings -> markers -> organics.
    std::vector<PendingCommandMount> command_mounts;
    // A pool-1 item gets an AI brain when its type authors a CONTROL seat (ctrlx/drvrx
    // userpoints = a drivable vehicle) — the stand-in for the def AIData attrib gate
    // until the item-def AI classes are plumbed to promote (D-AI-11). A pure-gunner
    // emplacement remains brainless itself; its attached organic owns and pumps the
    // parent's embedded weapon slot. [orig: every AIData item gets the 812-byte component
    // at spawn; Entity_SpawnFromBMSRecord @0x40e9f0; UseGun swap @0x546c42]
    auto item_is_drivable = [&](int32_t type_id) {
        for (const ItemSeatSpec &spec : opts.item_seat_specs) {
            if (spec.type_id != type_id) continue;
            for (const Seat &s : spec.seats) {
                if (is_vehicle_control_seat(s.type)) return true;
            }
        }
        return false;
    };
    std::vector<EntityHandle> promoted_item_handles;
    auto promote_vec = [&](const std::vector<bms::Entity> &vec, EntityKind kind, bool ai_capable_default) {
        uint32_t idx = 0;
        for (const bms::Entity &e : vec) {
            uint32_t origin = spawn_origin_pack(static_cast<uint32_t>(kind), idx);
            ++idx;
            Entity seed = make_seed(e, kind, static_cast<uint16_t>(e.id), origin);
            EntityHandle h = world.registry.spawn(pool_for_kind(kind), seed);
            if (!h.valid()) { ++r.dropped; continue; }
            ++r.spawned;
            if (kind == EntityKind::Item) promoted_item_handles.push_back(h);
            if (Entity *spawned = world.registry.get(h)) {
                seed_authored_seats(*spawned, opts);
            }
            const bool ai_capable =
                    ai_capable_default ||
                    (kind == EntityKind::Item && item_is_drivable(e.type_id));
            if (ai_capable) {
                int ai_idx = ai.attach(h);
                AiEntity &ae = *ai.at(ai_idx);
                init_brain(ae, e, opts, ai, kind);
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
            if (kind == EntityKind::Organic && e.waypoint_id >= 123 && e.waypoint_id <= 125 &&
                e.wp_number > 0 && e.wp_number <= 0xFFFF) {
                command_mounts.push_back(PendingCommandMount{
                    static_cast<uint16_t>(e.id),
                    static_cast<uint16_t>(e.wp_number),
                    static_cast<uint8_t>(e.waypoint_id),
                    h,
                });
            }
        }
    };
    promote_vec(m.items, EntityKind::Item, /*ai_capable=*/false);

    // Every stored items.def addeweap* slot creates a pool-1 child. The public
    // metadata already normalized the authored full item id to the raw type id;
    // G/C flags and angle fallback presence remain separate. Children use a
    // non-BMS origin sentinel so the listen-server WirePresentPass cannot defer
    // them to an unrelated placed node with the same (kind,index).
    struct AttachmentWork {
        EntityHandle carrier;
        std::vector<int32_t> lineage;
    };
    std::vector<AttachmentWork> attachment_work;
    attachment_work.reserve(promoted_item_handles.size());
    for (EntityHandle h : promoted_item_handles) {
        const Entity *carrier = world.registry.get(h);
        if (carrier != nullptr)
            attachment_work.push_back(AttachmentWork{h, {carrier->item_id}});
    }
    for (size_t work_index = 0; work_index < attachment_work.size(); ++work_index) {
        const AttachmentWork work = attachment_work[work_index];
        Entity *carrier = world.registry.get(work.carrier);
        if (carrier == nullptr) continue;
        const ItemSeatSpec *carrier_spec =
                seat_spec_for_type(opts, carrier->item_id);
        if (carrier_spec == nullptr) continue;
        if (work.lineage.size() >= 8) continue;
        for (const ItemEmplacementAttachmentSpec &attachment :
             carrier_spec->emplacement_attachments) {
            if (attachment.child_type_id == 0 ||
                std::find(work.lineage.begin(), work.lineage.end(),
                          attachment.child_type_id) != work.lineage.end())
                continue;
            Entity child_seed;
            child_seed.kind = EntityKind::Item;
            child_seed.item_id = attachment.child_type_id;
            child_seed.position = carrier->position;
            child_seed.yaw = carrier->yaw;
            child_seed.pitch = carrier->pitch;
            child_seed.roll = carrier->roll;
            child_seed.team = carrier->team;
            child_seed.spawn_origin = 0xFFFFFFFFu;
            const EntityHandle child_handle =
                    world.registry.spawn(pool_for_kind(EntityKind::Item), child_seed);
            if (!child_handle.valid()) {
                ++r.dropped;
                break;
            }
            ++r.spawned;
            Entity *child = world.registry.get(child_handle);
            carrier = world.registry.get(work.carrier);
            if (child == nullptr || carrier == nullptr) continue;
            child->emplacement_parent = work.carrier;
            child->emplacement_parent_spawn_id =
                    carrier->registry_spawn_id;
            child->emplacement_local = attachment.anchor.seat_local;
            child->emplacement_yaw_offset = attachment.anchor.yaw_offset;
            child->emplacement_bone =
                    attachment.anchor_found ? attachment.anchor.bone_index : 0;
            child->emplacement_kind = static_cast<uint8_t>(attachment.kind);
            child->emplacement_slot = attachment.stored_slot;
            child->emplacement_attachment_flags = attachment.attachment_flags;
            child->emplacement_angle_count = attachment.angle_count;
            child->emplacement_down_limit_bam = attachment.down_limit_bam;
            child->emplacement_up_limit_bam = attachment.up_limit_bam;
            child->emplacement_right_limit_bam = attachment.right_limit_bam;
            child->emplacement_left_limit_bam = attachment.left_limit_bam;
            seed_authored_seats(*child, opts);
            Seat anchor = attachment.anchor;
            anchor.type = SeatType::Gunner;
            anchor.bone_index = child->emplacement_bone;
            anchor.attachment_frame = true;
            pose_mounted_occupant(world, *child, *carrier, anchor);

            std::vector<int32_t> lineage = work.lineage;
            lineage.push_back(attachment.child_type_id);
            attachment_work.push_back(
                    AttachmentWork{child_handle, std::move(lineage)});
        }
    }

    promote_vec(m.buildings, EntityKind::Building, /*ai_capable=*/false);
    promote_vec(m.markers, EntityKind::Marker, /*ai_capable=*/false);
    promote_vec(m.organics, EntityKind::Organic, /*ai_capable=*/true);
    apply_command_mounts(command_mounts, world, ai, opts);

    return r;
}

} // namespace opennova::mission
