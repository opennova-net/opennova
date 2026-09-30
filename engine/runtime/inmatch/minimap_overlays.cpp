// The non-bank map legs' feed -- see minimap_overlays.h.

#include <runtime/inmatch/minimap_overlays.h>

#include <algorithm>
#include <cstdio>

#include <runtime/hud/hud_math.h>
#include <runtime/inmatch/minimap_markers.h>
#include <runtime/world/entity.h>
#include <runtime/world/friendly_tags.h>
#include <runtime/world/geom.h>
#include <runtime/world/minimap_overlay.h>
#include <runtime/world/spawn_select.h>

namespace opennova::inmatch {

namespace {

// The session rules word's FriendlyTag-0 bit, which also nulls every
// display name [orig: Entity_GetDisplayName @0x59BF70 `test g_RulesFlags,
// 400h`].
constexpr uint32_t kRulesNoNames = 0x400u;

// Entity_GetDisplayName over the port's entity: null (false) under the rules
// bit or off pool 0; a non-Player entity of the HUD team yields its authored
// name or '^' + the compiled-in fallback at its pool-encoded id, another
// team null; a Player entity resolves through the g_PlayerSlotPtrTable slot
// (the S2C 0x4C table this runtime does not fold), so it stays null here.
// [orig: Entity_GetDisplayName @0x59BF70 — rules @0x59BF70, pool 0
//  @0x59BF84, team byte_27234FE (hudInfo+374) @0x59BFA6, fallback
//  @0x59BFE4..0x59C04D, player slots @0x59C07A..0x59C0ED]
bool display_name(const world::Entity &e, uint8_t hud_team, uint32_t rules_word,
                  std::string &out) {
    if ((rules_word & kRulesNoNames) != 0) return false;
    if (e.handle.pool() != 0) return false;
    if (((e.flags | e.engine_flags) & world::kEntityFlagPlayer) != 0) return false;
    if (e.team != hud_team) return false;
    out = e.display_name.empty() ? hud::friendly_tag_fallback_name(e.handle.packed)
                                 : e.display_name;
    return true;
}

// The tracked entity's blip facts for the enemy cell-28 draw
// (Minimap_DrawBlip over the entity's class and policy).
void tracked_blip(const world::Entity &e, const world::Entity *local,
                  hud::HudMinimapMarker &blip) {
    blip.handle = e.handle.packed;
    blip.icon = 28;
    blip.entity_known = e.has_item_def ? 1 : 0;
    const world::MinimapBlipDrawPolicy policy = world::minimap_blip_draw_policy(e, 28);
    blip.rotate = policy.rotate ? 1 : 0;
    blip.footprint = policy.footprint ? 1 : 0;
    blip.half_x_q16 = policy.half_x_q16;
    blip.half_y_q16 = policy.half_y_q16;
    blip.floor_px = policy.floor_px;
    stamp_minimap_entity_facts(blip, e, local, nullptr);
}

} // namespace

void build_minimap_overlays(const MinimapOverlayInputs &in, hud::HudMinimapOverlays &out) {
    out = hud::HudMinimapOverlays{};
    out.game_type = in.game_type;
    world::World *w = in.world;
    const world::Entity *local = w != nullptr ? w->registry.get(w->cached.local_player) : nullptr;
    const replication::ClientState *cs = in.client;
    if (cs != nullptr) {
        // dword_A85B68 (the 0x0A spawn-target hold), dword_A85BBC (the owned
        // zone mask), g_EnemyTagsVisible, g_LocationNames (the 0x0F table).
        out.zone_timer = cs->spawn_hold_seconds;
        out.owned_zone_mask = cs->owned_zone_mask;
        out.enemy_tags_visible = cs->enemy_tags_visible;
        out.location_names = cs->location_names;
    }
    if (out.location_names.empty() && in.authority_location_names != nullptr)
        out.location_names = *in.authority_location_names;
    if (local != nullptr) {
        out.hud_present = true;
        out.hud_team = local->team;
        out.hud_handle = local->handle.packed;
    }
    // The zone waypoint names and the two formats the bit14 labels wrap them
    // in (GameText_GetString's miss is "").
    // [orig: HUD_DrawMapOverlay @0x5a70c4..0x5a7196 — "STRWPNAME%03d" of the
    //  spawn-zone index + 1 in WPNames; Overlays STROVER_OBJECTIVEPOINT_SHORT
    //  / STROVER_DEFENSIVEPOSITION]
    out.objective_point_format =
        hud::game_text(in.gametext, "Overlays", "STROVER_OBJECTIVEPOINT_SHORT", "");
    out.defensive_position_format =
        hud::game_text(in.gametext, "Overlays", "STROVER_DEFENSIVEPOSITION", "");
    if (w == nullptr) return;
    const world::SpawnZoneRegistry zones = w->zones.build_spawn_zone_list();
    out.zone_wp_names.reserve(zones.entries.size());
    for (size_t i = 0; i < zones.entries.size(); ++i) {
        char key[32];
        std::snprintf(key, sizeof(key), "STRWPNAME%03d", static_cast<int>(i + 1));
        out.zone_wp_names.push_back(hud::game_text(in.gametext, "WPNames", key, ""));
    }

    // The pool-3 and pool-4 walks visit the slots with a nonzero +0x1C
    // (ItemTypeIndex) in slot order; each 2044 marker's location index is its
    // spawn ordinal (g_LocationNameCount++ at spawn).
    // [orig: HUD_DrawMapOverlay @0x5a7540..0x5a7561 / @0x5a7730..0x5a774d;
    //  Entity_SpawnFromBMSRecord @0x40F180..0x40F20C]
    std::vector<const world::Entity *> pool3;
    std::vector<const world::Entity *> pool4;
    w->registry.for_each([&](const world::Entity &e) {
        if (e.item_type_index == 0) return;
        if (e.handle.pool() == 3) pool3.push_back(&e);
        else if (e.handle.pool() == 4) pool4.push_back(&e);
    });
    const auto by_slot = [](const world::Entity *a, const world::Entity *b) {
        return a->handle.packed < b->handle.packed;
    };
    std::sort(pool3.begin(), pool3.end(), by_slot);
    std::sort(pool4.begin(), pool4.end(), by_slot);
    int16_t location = 0;
    for (const world::Entity *e : pool3) {
        hud::HudMinimapPoolEntity row;
        row.def_id = e->item_id;
        row.x = world::to_fixed(e->position.x);
        row.y = world::to_fixed(e->position.y);
        row.radius_q16 = world::to_fixed(e->bound_radius);
        if (e->item_id == 2044) row.location_index = location++;
        out.pool3.push_back(row);
    }
    for (const world::Entity *e : pool4) {
        hud::HudMinimapPlayerWaypoint row;
        row.x = world::to_fixed(e->position.x);
        row.y = world::to_fixed(e->position.y);
        row.name = e->display_name;
        out.player_waypoints.push_back(row);
    }

    // The tracked callout: the one tracked-target producer this runtime folds
    // is the S2C death-type-6 radio request (client_effects); the S2C 0x2D
    // and chat-channel-13 setters are unported.
    // [orig: HUD_SetTrackedEntityTarget @0x59D050 — callers
    //  NapiNPClientMsg_HandleEntityDeath (type 6), NapiNPClientMsg_0x02D
    //  @0x427e90, Chat_DispatchToChannel @0x42B9CD]
    if (cs != nullptr && cs->radio_target.ticks_remaining != 0) {
        const auto &rt = cs->radio_target;
        hud::HudMinimapTracked &t = out.tracked;
        t.serial = rt.serial;
        t.ticks = static_cast<int32_t>(rt.ticks_remaining);
        t.snap_x = rt.position[0];
        t.snap_y = rt.position[1];
        t.friendly = rt.friendly;
        // The viewer gate reads the HUD entity's +0x168 mount state
        // [orig: Render_LaserSightEffect @0x59D15F..0x59D16D].
        t.viewer_mount_ok = local != nullptr &&
                            (static_cast<int>(local->mount_type) == 2 ||
                             static_cast<int>(local->mount_type) == 5);
        const world::Entity *te = w->registry.get(world::EntityHandle{rt.handle});
        if (te != nullptr) {
            t.live_known = true;
            t.live_x = world::to_fixed(te->position.x);
            t.live_y = world::to_fixed(te->position.y);
            t.radio_request = te->radio_request == 1;
            t.hidden_bit = ((te->flags | te->engine_flags) & world::kEntityFlagCarried) != 0;
            t.aboard_vehicle = world::friendly_tag_aboard_vehicle(*w, te->ground_target);
            tracked_blip(*te, local, t.blip);
        } else if (const replication::ClientEntityState *row = cs->find(rt.handle)) {
            // A joiner's players are decoded rows: their live position and
            // +885 latch ride the row.
            t.live_known = true;
            t.live_x = row->x;
            t.live_y = row->y;
            t.radio_request = row->radio_request == 1;
            t.blip.handle = rt.handle;
        }
        t.set_color = t.radio_request ? hud::kHudPaletteLightBlue : 0xFFFFFFFFu;
    }

    // Loop 2's names: every transient-bank person of the HUD team other than
    // the HUD entity that Entity_GetDisplayName names.
    // [orig: HUD_DrawEntityLabelsAndMarkers @0x5a4ef5..0x5a4f95]
    if (cs != nullptr && local != nullptr) {
        for (const replication::ClientMinimapOverlaySlot &slot : cs->minimap.transient) {
            if (!slot.active) continue;
            const world::Entity *e = w->registry.get(world::EntityHandle{slot.handle});
            if (e == nullptr || !e->has_item_def || e->item_type != 3) continue;
            if (e == local || e->team != local->team) continue;
            hud::HudMinimapName name;
            name.handle = slot.handle;
            if (display_name(*e, local->team, in.rules_word, name.text))
                out.names.push_back(std::move(name));
        }
    }
}

} // namespace opennova::inmatch
