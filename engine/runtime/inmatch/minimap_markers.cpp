// The retained minimap marker rows -- see minimap_markers.h.

#include <runtime/inmatch/minimap_markers.h>

#include <runtime/world/entity.h>
#include <runtime/world/geom.h>
#include <runtime/world/minimap_overlay.h>

namespace opennova::inmatch {

namespace {

using replication::ClientMinimapOverlaySlot;

// The local player's entity, when the world has one.
const world::Entity *local_player_of(world::World *w) {
    return w != nullptr ? w->registry.get(w->cached.local_player) : nullptr;
}

template <typename Bank>
void append_bank(const Bank &bank, hud::HudMinimapBank bank_id, world::World *world,
                 const world::Entity *local_player,
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
            // AnimMap_IsSlotActive(playerClass, 8) @0x5a4ab3].
            medic = local_player != nullptr && entity->team == local_player->team &&
                            world->tables.class_has_attribute(entity->player_class,
                                                       world::MissionTables::kCharAttrMedic)
                        ? 1
                        : 0;
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
    if (in.map != nullptr) {
        auto scan_bank = [&](const auto &bank) {
            for (const ClientMinimapOverlaySlot &slot : bank) {
                if (slot.active && slot.entity_known && slot.handle == local_marker_handle)
                    retained_local_player = true;
            }
        };
        scan_bank(in.map->transient);
        scan_bank(in.map->persistent);
        append_bank(in.map->transient, hud::HudMinimapBank::kTransient, in.world, local_player, out);
        append_bank(in.map->persistent, hud::HudMinimapBank::kPersistent, in.world, local_player, out);
        append_bank(in.map->special, hud::HudMinimapBank::kSpecial, in.world, local_player, out);
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
    out.push_back(m);
}

} // namespace opennova::inmatch
