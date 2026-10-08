// The non-bank map legs' feed -- see minimap_overlays.h.

#include <runtime/inmatch/minimap_overlays.h>

#include <algorithm>
#include <cstdio>

#include <runtime/hud/hud_math.h>
#include <runtime/inmatch/minimap_markers.h>
#include <runtime/replication/client_roster_tags.h>
#include <runtime/world/collision.h>
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

// The roster slot a 0x4C table entry points at, while the slot is active
// (slot+0x0D).
const replication::ClientRosterSlot *table_slot(const replication::ClientState &cs,
                                               const replication::ClientVisiblePlayer &entry) {
    const replication::ClientRosterSlot &slot = cs.roster[entry.slot];
    return slot.bound ? &slot : nullptr;
}

// Entity_GetDisplayName over the port's entity: null (false) under the rules
// bit or off pool 0; a non-Player entity of the HUD team yields its authored
// name or '^' + the compiled-in fallback at its pool-encoded id, another
// team null; a Player entity names the first S2C 0x4C table slot that drives
// it: the slot name, then " <ch>" tag "<co>" for a non-empty registry tag,
// all within 64 bytes; no such slot is null.
// [orig: Entity_GetDisplayName @0x59BF70 — rules @0x59BF70, pool 0
//  @0x59BF84, team byte_27234FE (hudInfo+374) @0x59BFA6, fallback
//  @0x59BFE4..0x59C04D, player slots @0x59C07A..0x59C0A9, the name
//  Napi_CopyString(.., 64) @0x59c0bb and the wrap @0x59c0c0..0x59c0f9]
bool display_name(const world::Entity &e, uint8_t hud_team, uint32_t rules_word,
                  const replication::ClientState *cs, std::string &out) {
    if ((rules_word & kRulesNoNames) != 0) return false;
    if (e.handle.pool() != 0) return false;
    if (((e.flags | e.engine_flags) & world::kEntityFlagPlayer) == 0) {
        if (e.team != hud_team) return false;
        out = e.display_name.empty() ? hud::friendly_tag_fallback_name(e.handle.packed)
                                     : e.display_name;
        return true;
    }
    if (cs == nullptr) return false;
    for (const replication::ClientVisiblePlayer &entry : cs->visible_players) {
        const replication::ClientRosterSlot *slot = table_slot(*cs, entry);
        if (slot == nullptr || slot->entity_slot != e.handle.slot()) continue;
        out.clear();
        replication::string_append_n(out, slot->name, 64);
        if (!slot->registry_clan.empty()) {
            replication::string_append_n(out, " <ch>", 64);
            replication::string_append_n(out, slot->registry_clan, 64);
            replication::string_append_n(out, "<co>", 64);
        }
        return true;
    }
    return false;
}

// One bit-5 loop-1 entry's slot entity as the map reads it: a world entity
// (the listen host's players, the local player) through the shared policy
// and facts, a joiner's decoded Player row through its own fields.
// [orig: HUD_DrawEntityLabelsAndMarkers @0x5a4a80..0x5a4ac6 — slot+0x24,
//  CharAttr_ClassHasAttribute(entity+0x294, 8), the team compare; Minimap_DrawBlip
//  @0x597890 over the entity]
struct SlotEntityFacts {
    bool known = false;
    bool radio_request = false;
    bool aboard_vehicle = false;
    bool medic = false;
    std::string name; // entity+0xF4
};
SlotEntityFacts slot_entity(const replication::ClientState &cs, world::World &w,
                            const world::Entity *local, uint16_t self_handle,
                            uint16_t handle, hud::HudMinimapMarker &blip) {
    SlotEntityFacts facts;
    const world::EntityHandle native =
            (self_handle != world::EntityHandle::kInvalid && handle == self_handle &&
             local != nullptr)
                    ? local->handle
                    : world::EntityHandle{handle};
    if (const world::Entity *e = w.registry.get(native)) {
        facts.known = true;
        blip.handle = handle;
        blip.icon = 3;
        blip.entity_known = e->has_item_def ? 1 : 0;
        const world::MinimapBlipDrawPolicy policy = world::minimap_blip_draw_policy(*e, 3);
        blip.rotate = policy.rotate ? 1 : 0;
        blip.footprint = policy.footprint ? 1 : 0;
        blip.half_x_q16 = policy.half_x_q16;
        blip.half_y_q16 = policy.half_y_q16;
        blip.floor_px = policy.floor_px;
        stamp_minimap_entity_facts(blip, *e, local, nullptr);
        int32_t euler[3] = {};
        world::entity_live_euler_bam(*e, euler);
        blip.heading_bam = euler[0];
        facts.radio_request = e->radio_request == 1;
        facts.aboard_vehicle = world::friendly_tag_aboard_vehicle(w, e->ground_target);
        facts.medic = w.tables.class_has_attribute(e->player_class,
                                                   world::MissionTables::kCharAttrMedic);
        facts.name = e->display_name;
        return facts;
    }
    const replication::ClientEntityState *row = cs.find(handle);
    if (row == nullptr) return facts;
    facts.known = true;
    const bool dead = (row->state_flags & 0x02u) != 0;
    blip.handle = handle;
    blip.icon = 3;
    blip.entity_known = row->type_id != 0 ? 1 : 0;
    // A person: 2 wu halves, the dead body upright [orig: Minimap_DrawBlip
    // @0x597b43..0x597b72].
    blip.half_x_q16 = blip.half_y_q16 = 0x20000;
    blip.rotate = dead ? 0 : 1;
    blip.floor_px = 6;
    blip.def_type = 3;
    blip.team = row->team == 0xFF ? 0 : row->team;
    blip.entity_bits = static_cast<uint8_t>(
            (row->type_id != 0 ? hud::kMarkerEntityHasModel : 0) |
            (dead ? hud::kMarkerEntityDead : 0));
    blip.entity_x = row->x;
    blip.entity_y = row->y;
    // A decoded row carries no bbox: a person's collision centre sits on the
    // vertical through its origin, so the anchor is the origin.
    blip.anchor_x = row->x;
    blip.anchor_y = row->y;
    blip.heading_bam = row->heading_bam;
    facts.radio_request = row->radio_request == 1;
    facts.aboard_vehicle =
            world::friendly_tag_aboard_vehicle(w, world::EntityHandle{row->carrier_handle});
    facts.medic = row->spawn_player_class != 0 &&
                  w.tables.class_has_attribute(row->spawn_player_class,
                                               world::MissionTables::kCharAttrMedic);
    return facts;
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
        // The KOTH ring's delta: team 1's score1 less team 2's, the 0x16
        // team rows each sign-extended into its dword.
        // [orig: Minimap_DrawKothZoneRing @0x5974E0 — dword_A85AFC - dword_A85B0C
        //  @0x597509 / @0x597517; the rows 0xA85AEC + 16t, the movsx store
        //  @0x42fe08]
        const auto &teams = cs->scoreboard.teams;
        const int32_t team1 = teams.size() > 1 ? teams[1].score1 : 0;
        const int32_t team2 = teams.size() > 2 ? teams[2].score1 : 0;
        out.zone_score_delta = team1 - team2;
        // The own-slot revive leg's profile dword is the inverse
        // OPTIONS_AUTOMEDIC preference, the same word the C2S 0x03 uplink
        // sends, read from the current record each frame.
        // [orig: *((_DWORD *)g_CurPlayerProfile + 415) @0x5a4b4f;
        //  NetPacket_WriteAutoMedicPreference @0x42A411]
        out.own_revive_profile = in.own_auto_medic_off != 0;
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

    // The tracked callout: HUD_SetTrackedEntityTarget's state, set by the S2C
    // 0x6D event-6 radio call, the S2C 0x2D emote and a channel-13 chat line
    // (client_effects).
    // [orig: HUD_SetTrackedEntityTarget @0x59D050 — callers
    //  NapiNPClientMsg_HandleEntityDeath @0x430de4, NapiNPClientMsg_HandleEmote
    //  @0x427f5b, Chat_DispatchToChannel @0x42ba09]
    if (cs != nullptr && cs->tracked_target.ticks_remaining != 0) {
        const auto &rt = cs->tracked_target;
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
        t.set_color = rt.color; // dword_2721ED0 at the set [orig: @0x59d0ef..0x59d0ff]
    }

    // Bit 5 loop 1: the S2C 0x4C player-slot table, each entry's slot (live
    // fields) and the entity the slot drives.
    // [orig: HUD_DrawEntityLabelsAndMarkers @0x5a4a54..0x5a4eb4 — slot+0x0D,
    //  slot+0x24 @0x5a4a8b..0x5a4a95, the squad byte slot+0x33 @0x5a4b02,
    //  slot+0x10 @0x5a4b2e, the own-slot test `*entry == HUD entity`
    //  @0x5a4b4f, slot+0x2C @0x5a4b58, PlayerSlot_FindByEntityPtr @0x5a4dd1,
    //  the name slot+0x14 @0x5a4ded and its <ch>..<co> wrap of slot+0x20
    //  @0x5a4df2..0x5a4e34, else entity+0xF4 @0x5a4e3e]
    if (cs != nullptr) {
        const uint16_t own = in.self_handle != world::EntityHandle::kInvalid
                ? in.self_handle
                : (local != nullptr ? local->handle.packed : world::EntityHandle::kInvalid);
        for (const replication::ClientVisiblePlayer &entry : cs->visible_players) {
            const replication::ClientRosterSlot *slot = table_slot(*cs, entry);
            if (slot == nullptr || slot->entity_slot < 0) continue;
            const uint16_t handle = static_cast<uint16_t>(slot->entity_slot & 0xFFF); // pool 0
            hud::HudMinimapPlayerSlot row;
            const SlotEntityFacts facts =
                    slot_entity(*cs, *w, local, in.self_handle, handle, row.blip);
            if (!facts.known) continue;
            row.active = true;
            row.own_slot = entry.entity_handle != world::EntityHandle::kInvalid &&
                           entry.entity_handle == own;
            row.revivable = slot->downed_revive_seconds != 0;
            row.medic_request = slot->medic_request_active;
            row.radio_request = facts.radio_request;
            row.aboard_vehicle = facts.aboard_vehicle;
            row.medic = facts.medic;
            row.squad = slot->squad_color;
            // PlayerSlot_FindByEntityPtr: the first bound slot driving it.
            const replication::ClientRosterSlot *name_slot = nullptr;
            for (const replication::ClientRosterSlot &candidate : cs->roster) {
                if (candidate.bound && candidate.entity_slot == slot->entity_slot) {
                    name_slot = &candidate;
                    break;
                }
            }
            if (name_slot != nullptr) {
                row.name_slot = true;
                row.name = name_slot->name;
                row.clan = name_slot->registry_clan;
            } else {
                row.name = facts.name;
            }
            out.player_slots.push_back(std::move(row));
        }
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
            if (display_name(*e, local->team, in.rules_word, cs, name.text))
                out.names.push_back(std::move(name));
        }
    }
}

} // namespace opennova::inmatch
