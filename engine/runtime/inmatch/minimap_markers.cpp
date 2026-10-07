// The retained minimap marker rows -- see minimap_markers.h.

#include <runtime/inmatch/minimap_markers.h>

#include <runtime/world/collision.h>
#include <runtime/world/entity.h>
#include <runtime/world/geom.h>
#include <runtime/world/minimap_overlay.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/world.h>

namespace opennova::inmatch {

namespace {

using replication::ClientMinimapOverlaySlot;

// The local player's entity, when the world has one.
const world::Entity *local_player_of(world::World *w) {
    return w != nullptr ? w->registry.get(w->cached.local_player) : nullptr;
}

} // namespace

// v5: the live pool-entity facts the map drawer reads behind a slot handle —
// its team, zone number and radius, def type, the model/zone/dead/FARP
// class bits, the spawn-zone index, its position and the blip anchor (the
// placement matrix applied to the bbox centre), and the entity+0 radius.
// [orig: Minimap_DrawBlip @0x5978B2..0x5978DC (anchor) / @0x597a1f (FARP
//  class after the armory test @0x5979db); Render_MinimapSlotBlip
//  @0x5be4b8..0x5be4f7; MapOverlay_RenderAllByLayer @0x5BE609..0x5BE6CB;
//  HUD_DrawMapOverlay @0x5a6d86..0x5a6d9c / @0x5a7058 / @0x5a7284]
void stamp_minimap_entity_facts(hud::HudMinimapMarker &m, const world::Entity &entity,
                        const world::Entity *local_player,
                        const world::SpawnZoneRegistry *zones) {
    m.team = entity.team;
    m.zone_number = entity.zone_number;
    m.def_type = entity.has_item_def ? entity.item_type : 0;
    uint8_t bits = 0;
    if (entity.has_graphic_model) bits |= hud::kMarkerEntityHasModel;
    if (entity.has_minimap_model_marker) bits |= hud::kMarkerEntityOcclusion;
    if (entity.has_item_def && (entity.item_attrib & 0x40000u) != 0)
        bits |= hud::kMarkerEntityZoneDef;
    if (((entity.flags | entity.engine_flags) & world::kEntityFlagDead) != 0)
        bits |= hud::kMarkerEntityDead;
    if (local_player != nullptr && entity.handle == local_player->handle)
        bits |= hud::kMarkerEntityHud;
    if (entity.has_item_def && (entity.item_attrib & world::kItemAttribArmory) == 0 &&
        (entity.item_attrib2 & 0x2000u) != 0)
        bits |= hud::kMarkerEntityFarp;
    // v6: the vehicle bay (ItemDefAttrib2 bit 0) and its spawn families, the
    // logo walk's def gate and type pick, plus the live altitude it lifts
    // [orig: HUD_DrawVehicleBayLogos @0x5a2c00 -- `test byte [def+58h], 1`
    //  @0x5a2c9e, def+0xAD8 @0x5a2d6f, entity+0xC @0x5a2d56].
    if (entity.has_item_def && (entity.item_attrib2 & 1u) != 0)
        bits |= hud::kMarkerEntityVehicleBay;
    m.bay_groups = entity.vehicle_bay_flags;
    m.entity_bits = bits;
    m.zone_index = zones != nullptr
                       ? static_cast<int16_t>(world::spawn_zone_index_of(*zones, entity.handle))
                       : int16_t{-1};
    m.zone_radius = entity.zone_radius;
    const int32_t pos[3] = {world::to_fixed(entity.position.x), world::to_fixed(entity.position.y),
                            world::to_fixed(entity.position.z)};
    m.entity_x = pos[0];
    m.entity_y = pos[1];
    m.entity_z = pos[2];
    int32_t euler[3] = {};
    world::entity_live_euler_bam(entity, euler);
    const world::CollisionMatrix placement =
        world::collision_matrix_from_euler(euler[0], euler[1], euler[2], pos);
    const int32_t center[3] = {world::to_fixed(entity.bbox_center.x),
                               world::to_fixed(entity.bbox_center.y),
                               world::to_fixed(entity.bbox_center.z)};
    int32_t anchor[3] = {};
    placement.transform_point(center, anchor);
    m.anchor_x = anchor[0];
    m.anchor_y = anchor[1];
    m.bound_radius_q16 = world::to_fixed(entity.bound_radius);
}

namespace {

// v7: the zone-timer entry keyed by the slot's entity, as the capture-point
// labels read it: the entry exists once either S2C 0x6F or 0x53 landed for
// the zone [orig: Render_CapturePointLabels @0x5a2840 --
//  CProximityList_FindEntryById(g_ZoneTimerList, entity) @0x5a292c; the
//  entry is appended by ZoneTimerList_SetEntryValue @0x537EC0 and
//  ZoneTimerList_SetEntryWindow @0x537DE0 alike].
void stamp_zone_timer(hud::HudMinimapMarker &m,
                      const std::unordered_map<uint16_t, ClientRuntime::ZoneState> *timers) {
    if (timers == nullptr) return;
    const auto found = timers->find(m.handle);
    if (found == timers->end()) return;
    const ClientRuntime::ZoneState::Entry &entry = found->second.entry;
    m.timer_known = 1;
    m.timer_active = entry.value_active ? 1 : 0;
    m.timer_team = entry.mode_a;
    m.timer_bar_team = entry.mode_b;
    m.timer_value = entry.value_current;
    m.timer_limit = entry.value_limit;
    m.timer_rate = entry.value_rate;
}

template <typename Bank>
void append_bank(const Bank &bank, hud::HudMinimapBank bank_id, world::World *world,
                 const world::Entity *local_player, const world::SpawnZoneRegistry *zones,
                 const std::unordered_map<uint16_t, ClientRuntime::ZoneState> *timers,
                 std::vector<hud::HudMinimapMarker> &out) {
    for (const ClientMinimapOverlaySlot &slot : bank) {
        if (!slot.active) continue;
        hud::HudMinimapMarker m;
        m.bank = static_cast<uint8_t>(bank_id);
        m.handle = slot.handle;
        m.x = slot.x;
        m.y = slot.y;
        m.z = slot.z;
        m.heading_bam = slot.heading_bam;
        m.icon = slot.param;
        m.color = slot.argb;
        m.flags = slot.flags;
        m.source = slot.source;
        m.remaining_ticks = slot.remaining_ticks;
        m.entity_known = slot.entity_known ? 1 : 0;
        // The draw policy resolves against the LOCAL entity (host: the live
        // registry; joiner: the materialized twin) -- retail reads the pool
        // slot's def at draw time the same way (witness at
        // world::minimap_blip_draw_policy). Absent entity -> the rotated
        // fallback on the class table.
        world::MinimapBlipDrawPolicy policy;
        int medic = 0;
        const world::Entity *entity =
            world != nullptr ? world->registry.get(world::EntityHandle{slot.handle}) : nullptr;
        if (entity != nullptr) {
            policy = world::minimap_blip_draw_policy(*entity, slot.param);
            // v4: the map medic marker -- a LOCAL-TEAM entity whose class
            // carries the charattr Medic attribute; the other team's bit is
            // forced off at the producer [orig: HUD_DrawEntityLabelsAndMarkers
            // @0x5a49e0 -- the team gate @0x5a4ac6/@0x5a4acf,
            // CharAttr_ClassHasAttribute(playerClass, 8) @0x5a4ab3].
            medic = local_player != nullptr && entity->team == local_player->team &&
                            world->tables.class_has_attribute(entity->player_class,
                                                       world::MissionTables::kCharAttrMedic)
                        ? 1
                        : 0;
            stamp_minimap_entity_facts(m, *entity, local_player, zones);
        } else {
            policy.half_x_q16 = 0;
            policy.half_y_q16 = 0;
        }
        m.rotate = policy.rotate ? 1 : 0;
        m.footprint = policy.footprint ? 1 : 0;
        m.half_x_q16 = policy.half_x_q16;
        m.half_y_q16 = policy.half_y_q16;
        m.floor_px = policy.floor_px;
        m.medic = static_cast<uint8_t>(medic);
        stamp_zone_timer(m, timers);
        out.push_back(m);
    }
}

} // namespace

void build_minimap_markers(const MinimapMarkerInputs &in,
                           std::vector<hud::HudMinimapMarker> &out) {
    out.clear();
    const world::Entity *local_player = local_player_of(in.world);
    const uint16_t local_marker_handle =
        local_player != nullptr ? in.local_marker_handle : world::EntityHandle::kInvalid;
    bool retained_local_player = false;
    // The spawn-zone list the zone legs index (retail rebuilds it at mission
    // start [orig: Entity_BuildSpawnZoneList @0x43EAE0]).
    world::SpawnZoneRegistry zone_list;
    if (in.world != nullptr) zone_list = in.world->zones.build_spawn_zone_list();
    const world::SpawnZoneRegistry *zones = in.world != nullptr ? &zone_list : nullptr;
    if (in.map != nullptr) {
        auto scan_bank = [&](const auto &bank) {
            for (const ClientMinimapOverlaySlot &slot : bank) {
                if (slot.active && slot.entity_known && slot.handle == local_marker_handle)
                    retained_local_player = true;
            }
        };
        scan_bank(in.map->transient);
        scan_bank(in.map->persistent);
        append_bank(in.map->transient, hud::HudMinimapBank::kTransient, in.world, local_player,
                    zones, in.zone_timers, out);
        append_bank(in.map->persistent, hud::HudMinimapBank::kPersistent, in.world, local_player,
                    zones, in.zone_timers, out);
        append_bank(in.map->special, hud::HudMinimapBank::kSpecial, in.world, local_player, zones,
                    in.zone_timers, out);
    }
    // Retail registers the locally deployed player in a regular retained bank.
    // The loopback client does not receive that client-local registration, so
    // restore it here unless a decoded regular row already covers the same wire
    // handle. The draw-call probe confirms cell 3, team-table blue, and the
    // ordinary 6px-floor path at map center.
    // [orig: Render_MinimapSlotBlip @0x5BE240 -> Minimap_DrawBlip, the regular
    //  TSDicon submit @0x597F73; see hud-re.md]
    if (local_player == nullptr || local_marker_handle == world::EntityHandle::kInvalid ||
        retained_local_player)
        return;
    const world::MinimapBlipDrawPolicy policy = world::minimap_blip_draw_policy(*local_player, 3);
    hud::HudMinimapMarker m;
    m.bank = static_cast<uint8_t>(hud::HudMinimapBank::kPersistent);
    m.handle = local_marker_handle;
    m.x = world::to_fixed(local_player->position.x);
    m.y = world::to_fixed(local_player->position.y);
    m.z = world::to_fixed(local_player->position.z);
    m.heading_bam = in.local_heading_bam;
    m.icon = 3; // live Person classification -> TSDicon cell 3
    m.color = replication::minimap_team_argb(local_player->team);
    m.flags = 0x10; // regular persistent bank
    m.source = static_cast<uint8_t>(local_player->zone_number);
    m.remaining_ticks = 0; // regular slots draw at zero lifetime
    m.entity_known = 1;    // the local entity is necessarily resolved
    m.rotate = policy.rotate ? 1 : 0;
    m.footprint = policy.footprint ? 1 : 0;
    m.half_x_q16 = policy.half_x_q16;
    m.half_y_q16 = policy.half_y_q16;
    m.floor_px = policy.floor_px;
    // The restored local row is a local-team player by definition; its medic
    // bit is its own class attribute.
    m.medic = in.world->tables.class_has_attribute(local_player->player_class,
                                            world::MissionTables::kCharAttrMedic)
                  ? 1
                  : 0;
    stamp_minimap_entity_facts(m, *local_player, local_player, zones);
    stamp_zone_timer(m, in.zone_timers);
    out.push_back(m);
}

} // namespace opennova::inmatch
