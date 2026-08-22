#include "world/zone_capture.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "world/spawn_select.h"
#include "world/world.h"

namespace opennova::world {

namespace {

// enemy_of(team): 1 -> 2, else -> 1 [orig: `2 - (team != 1)` @0x51974a].
// Use only after a concrete two-team owner/capturer is known. A neutral zone
// must census both teams before selecting a side.
inline uint8_t enemy_of(uint8_t team) { return team == 1 ? 2 : 1; }

// The 3D in-radius test [orig: Server_UpdateCaptureZoneProximity @0x5086A0 —
// 2D distance <= radius AND |dz| <= radius/2; the zone radius is entity+350].
bool in_zone_radius(const Entity &player, const Entity &zone) {
    if (zone.zone_radius == 0) return false;
    const float r = static_cast<float>(zone.zone_radius);
    const float dx = player.position.x - zone.position.x;
    const float dy = player.position.y - zone.position.y;
    const float dz = player.position.z - zone.position.z;
    if (std::fabs(dz) > r * 0.5f) return false;
    return dx * dx + dy * dy <= r * r;
}

// A "playing" pool-0 player entity (the original iterates playing slots; our
// runtime players are pool-0 organics with a resolved soldier class).
bool is_playing_player(const Entity &e) {
    return e.handle.pool() == 0 && e.player_class != 0 && e.alive && e.health > 0;
}

// One team owns EVERY registered zone -> the match is decided and flip events are
// suppressed [orig: GetWinningTeamIfAllOwned gate @0x4A2920 via @0x50F70B].
bool match_decided(const World &world, const ZoneChain &chain) {
    uint8_t owner = 0;
    for (const EntityHandle h : chain.zones) {
        const Entity *z = world.registry.get(h);
        if (z == nullptr) continue;
        if (z->team == 0) return false;
        if (owner == 0) owner = z->team;
        else if (z->team != owner) return false;
    }
    return owner != 0;
}

} // namespace

// [orig: calculate_capture_zone_control_delta @0x501120].
int32_t zone_capture_control_delta(const ZoneCaptureDeltaInput &input) {
    if (input.presence == 0) return 0;
    int team_size = input.capturing_side_players;
    if (input.total_players < 6)
        team_size += (6 - input.total_players) / 2; // [orig: @0x5012e6]
    for (const int cap : {20, 40, 60}) {                          // [orig: @0x501301..]
        if (team_size > cap) team_size = cap + (team_size - cap) / 2;
    }
    int base = 12; // setting -1/default [orig: @0x5012c4 switch on g_capture_speed_setting]
    if (input.speed_setting == 1) base = 24;
    else if (input.speed_setting == 2) base = 48;
    double speed = static_cast<double>(team_size) * base;

    // Retail accelerates the side that already owns more numbered spawn-zone
    // entries during the final half of a timed round. Its clock factor is
    // clamped to [0,1]; GameTime 0 leaves the factor at 1. The reduction is
    // converted to an integer before subtraction from the speed denominator.
    // [orig: calculate_capture_zone_control_delta @0x501120]
    if (input.numbered_spawn_zones > 0 &&
        input.capturing_side_zones > input.opposing_side_zones) {
        const double imbalance = static_cast<double>(
                input.capturing_side_zones - input.opposing_side_zones) /
                input.numbered_spawn_zones;
        double clock_factor = 1.0;
        if (input.game_time_minutes != 0) {
            const double round_ticks =
                    3720.0 * static_cast<double>(input.game_time_minutes);
            clock_factor = 1.0 -
                    (2.0 * static_cast<double>(input.remaining_ticks) /
                     round_ticks);
        }
        clock_factor = std::clamp(clock_factor, 0.0, 1.0);
        const int reduction = static_cast<int>(
                speed * imbalance * clock_factor * 0.5);
        speed -= reduction;
    }

    if (input.shared_zone_entities > 1)
        speed /= input.shared_zone_entities; // [orig: @0x501465]
    if (speed <= 0.0) speed = 1.0;
    int32_t delta = static_cast<int32_t>(
            65536.0 * input.presence / speed); // [orig: @0x501490 ftol]
    if (delta == 0)
        delta = input.presence > 0 ? 1 : -1; // minimum magnitude 1 [orig: @0x5014a4]
    return delta;
}

void zone_capture_tick(World &world, ZoneChain &chain, ZoneCaptureEvents &out,
                       int capture_speed_setting) {
    out.clear();
    if (chain.empty()) return;

    // Playing-player census: per-team counts + the handles for the radius tests.
    int team_players[ZoneChain::kTeamCount] = {0, 0, 0, 0, 0};
    int total_players = 0;
    std::vector<const Entity *> players;
    world.registry.for_each([&](const Entity &e) {
        if (!is_playing_player(e)) return;
        players.push_back(&e);
        ++total_players;
        if (e.team < ZoneChain::kTeamCount) ++team_players[e.team];
    });

    // The delta producer scans the sorted spawn-zone registry, not just capture
    // triggers: every nonzero-number entry contributes to ownership imbalance,
    // the denominator, and the shared-number divide.
    // [orig: calculate_capture_zone_control_delta @0x501120]
    int shared_count[32] = {};
    int owned_spawn_zones[ZoneChain::kTeamCount] = {};
    int numbered_spawn_zones = 0;
    const SpawnZoneRegistry spawn_zones = build_spawn_zone_list(world);
    for (const EntityHandle h : spawn_zones.entries) {
        const Entity *z = world.registry.get(h);
        if (z == nullptr || z->zone_number == 0) continue;
        ++numbered_spawn_zones;
        if (z->zone_number < 32) ++shared_count[z->zone_number];
        if (z->team < ZoneChain::kTeamCount) ++owned_spawn_zones[z->team];
    }

    // ---- The secure/control pass [orig: Server_UpdateCaptureZoneEntities @0x519690] ----
    struct FlipRequest {
        EntityHandle zone;
        EntityHandle capturer;
        uint8_t capturer_team = 0;
    };
    std::vector<FlipRequest> flip_requests;
    for (size_t zi = 0; zi < chain.zones.size(); ++zi) {
        Entity *z = world.registry.get(chain.zones[zi]);
        if (z == nullptr) continue;
        const int32_t before = z->zone_control;

        // In-radius census for this zone: friendlies = the zone team's players;
        // enemies = players of a team the FRONTIER lets capture this zone
        // [orig: the frontier-eligible enemy filter inside the delta's presence count].
        int friendlies = 0;
        int enemies = 0;
        int attackers_by_team[ZoneChain::kTeamCount] = {};
        EntityHandle first_attacker[ZoneChain::kTeamCount] = {};
        for (const Entity *p : players) {
            if (!in_zone_radius(*p, *z)) continue;
            if (z->team != 0 && p->team == z->team) {
                ++friendlies;
            } else if (zone_chain_is_capturable(world, chain, p->team, *z)) {
                ++enemies;
                if (p->team < ZoneChain::kTeamCount) {
                    if (attackers_by_team[p->team]++ == 0)
                        first_attacker[p->team] = p->handle;
                }
            }
        }

        // A neutral objective can be reached by both frontiers. Capture remains
        // attributable only while one eligible team is present; opposing teams
        // contest instead of allowing registry order to choose a winner.
        // [orig: Server_UpdateCaptureZoneProximity @0x5086A0 maintains the
        // per-team rows consumed by Server_UpdateCaptureZones @0x53B8F0]
        uint8_t attacker_team = 0;
        int attacker_count = 0;
        EntityHandle attacker;
        bool multiple_attacker_teams = false;
        for (uint8_t team = 1; team < ZoneChain::kTeamCount; ++team) {
            if (attackers_by_team[team] == 0) continue;
            if (attacker_team != 0) multiple_attacker_teams = true;
            if (attacker_team == 0) {
                attacker_team = team;
                attacker_count = attackers_by_team[team];
                attacker = first_attacker[team];
            }
        }

        int16_t wire_delta = 0;
        bool reachable_by_enemy = z->team == 0;
        for (uint8_t team = 1;
                team < ZoneChain::kTeamCount && !reachable_by_enemy; ++team) {
            if (team != z->team &&
                    zone_chain_is_capturable(world, chain, team, *z))
                reachable_by_enemy = true;
        }
        if (!reachable_by_enemy) {
            // The secure latch: the enemy frontier cannot reach it [orig: @0x519764].
            z->zone_control = 0x10000;
        } else {
            const int presence = multiple_attacker_teams
                    ? 0
                    : friendlies - attacker_count;
            if (presence != 0) {
                const int side = presence > 0 ? z->team : attacker_team;
                const int side_players =
                        side < ZoneChain::kTeamCount ? team_players[side] : 0;
                const int shared_n =
                        z->zone_number < 32 ? shared_count[z->zone_number] : 1;
                ZoneCaptureDeltaInput delta_input;
                delta_input.presence = presence;
                delta_input.capturing_side_players = side_players;
                delta_input.total_players = total_players;
                delta_input.speed_setting = capture_speed_setting;
                delta_input.shared_zone_entities = shared_n;
                delta_input.capturing_side_zones =
                        side < ZoneChain::kTeamCount ? owned_spawn_zones[side] : 0;
                const uint8_t opposing_side = side == 1 ? 2 : 1;
                delta_input.opposing_side_zones = owned_spawn_zones[opposing_side];
                delta_input.numbered_spawn_zones = numbered_spawn_zones;
                delta_input.remaining_ticks = world.match.remaining_ticks();
                delta_input.game_time_minutes = world.match.rules().game_time_minutes;
                const int32_t delta = zone_capture_control_delta(delta_input);
                wire_delta = static_cast<int16_t>(
                        delta > 32767 ? 32767 : (delta < -32768 ? -32768 : delta));
                int64_t c = static_cast<int64_t>(z->zone_control) + delta;
                if (c < 0) c = 0;
                if (c > 0x10000) c = 0x10000;
                z->zone_control = static_cast<int32_t>(c); // [orig: clamp @0x501499]
            }
        }

        // Secure edges [orig: 0x3B on became-1.0 @0x519839 / 0x3C on became-0 @0x51988E].
        if (before < 0x10000 && z->zone_control >= 0x10000)
            out.secure_edges.push_back({z->handle, z->team, true});
        else if (before > 0 && z->zone_control <= 0)
            out.secure_edges.push_back({z->handle, z->team, false});

        // The 0x6F body, every pass [orig: emit @0x5197D9].
        out.control.push_back({z->handle, z->team, z->zone_control, wire_delta,
                               static_cast<uint8_t>(friendlies > 255 ? 255 : friendlies),
                               static_cast<uint8_t>(enemies > 255 ? 255 : enemies)});

        // A frontier-eligible enemy standing on an UNSECURED zone queues the flip
        // [orig: the touch gate `un-numbered || owner || control <= 0`
        // @Server_OnPlayerTouchCaptureZone @0x500BA0; drained by @0x53B8F0 — our
        // request source is this same 1 Hz proximity sample (D-NET-162 note)].
        if (!multiple_attacker_teams && attacker_team != 0 &&
                z->zone_control <= 0)
            flip_requests.push_back({z->handle, attacker, attacker_team});
    }

    // ---- Queue drain: numbered zones flip INSTANTLY [orig: @0x53B8F0 drain leg] ----
    for (const FlipRequest &req : flip_requests) {
        Entity *z = world.registry.get(req.zone);
        if (z == nullptr) continue;
        const uint8_t old_team = z->team;
        // A numbered objective changes through neutral and onto the requesting
        // team in the SAME queue drain. The intermediate neutral write is
        // observable to team-change bookkeeping but is not a second gameplay
        // state/tick. [orig: Server_UpdateCaptureZones @0x53BC70 (team 0),
        // @0x53BC80 (capturing team), before scoring @0x53BC94]
        const uint8_t new_team = req.capturer_team;
        const uint8_t cap_frontier_before =
                zone_chain_frontier_zone(world, chain, req.capturer_team);
        const uint8_t loser_frontier_before =
                zone_chain_frontier_zone(world, chain, enemy_of(req.capturer_team));

        if (old_team != 0) z->team = 0;      // retail's intermediate team-change transaction
        z->team = new_team;                 // [orig: Server_ChangeEntityTeam @0x518D70]
        z->zone_control = 0;                // the new owner must SECURE it (the AS beat)
        zone_chain_rebuild_masks(world, chain);

        ZoneCaptureEvents::Flip flip;
        flip.zone = z->handle;
        flip.capturer = req.capturer;
        for (const Entity *player : players) {
            if (player->team == req.capturer_team && in_zone_radius(*player, *z))
                flip.scorers.push_back(player->handle);
        }
        flip.old_team = old_team;
        flip.new_team = new_team;
        flip.capturer_team = req.capturer_team;
        flip.capturer_frontier =
                zone_chain_frontier_zone(world, chain, req.capturer_team);
        flip.loser_frontier =
                zone_chain_frontier_zone(world, chain, enemy_of(req.capturer_team));
        flip.frontier_changed = flip.capturer_frontier != cap_frontier_before ||
                                flip.loser_frontier != loser_frontier_before;
        // Suppress the capture banner only when this flip actually completed
        // the ownership chain. [orig: GameEvent_FlagCapture @0x50F70B ->
        // ZoneSlotChain_GetWinningTeamIfAllOwned @0x4A2920]
        flip.suppressed = match_decided(world, chain);
        out.flips.push_back(flip);
    }

    // ---- Team enforcement [orig: Server_EnforceZoneEntityTeams @0x519600]: every
    // zone-numbered entity is forced onto the team whose OWNED mask holds its zone
    // number (team 1 precedence) — this flips the co-located SpawnPoint objects when
    // the trigger objects change hands. ----
    if (!out.flips.empty()) {
        std::vector<EntityHandle> numbered;
        world.registry.for_each([&](const Entity &e) {
            // The TRIGGER entities drive ownership and are never force-converted (a
            // freshly neutralized bunker must not snap back while its number-sharing
            // sibling still holds the mask bit); only the co-located non-trigger
            // objects (SpawnPoint-only tents, props) follow the mask
            // [orig: "flips the co-located 0x40000 spawn objects when the 0x20000
            // trigger objects change hands" — §5.61 item 7].
            if (e.zone_number != 0 && !e.is_capture_trigger && e.is_spawn_point)
                numbered.push_back(e.handle);
        });
        for (const EntityHandle h : numbered) {
            Entity *e = world.registry.get(h);
            if (e == nullptr || e->zone_number >= 32) continue;
            const uint32_t bit = 1u << e->zone_number;
            uint8_t forced = 0;
            if ((chain.owned_mask[1] & bit) != 0) forced = 1; // team-1 precedence
            else if ((chain.owned_mask[2] & bit) != 0) forced = 2;
            if (forced != 0 && e->team != forced) e->team = forced;
        }
        zone_chain_rebuild_masks(world, chain);
    }
}

} // namespace opennova::world
