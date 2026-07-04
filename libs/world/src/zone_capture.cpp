#include "world/zone_capture.h"

#include <cmath>
#include <cstdlib>

#include "world/world.h"

namespace opennova::world {

namespace {

// enemy_of(team): 1 -> 2, else -> 1 [orig: `2 - (team != 1)` @0x51974a].
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

// [orig: calculate_capture_zone_control_delta @0x501120] — the witnessed formula:
// teamSize = capturing side's playing count, + (6 - total)/2 when total < 6
// (small-server boost), soft-capped x -> cap + (x - cap)/2 at 20/40/60; speed =
// teamSize * base (setting 1 -> 24, 2 -> 48, else 12), / shared_n for a zone number
// shared by N entities; delta = 65536 * presence / speed, minimum magnitude 1.
// (The underdog catch-up subtracts up to speed/2 inside the last 30*respawn-time
// window — deferred, no round clock; D-NET-162.)
int32_t zone_capture_control_delta(int presence, int capturing_side_players,
                                   int total_players, int speed_setting, int shared_n) {
    if (presence == 0) return 0;
    int team_size = capturing_side_players;
    if (total_players < 6) team_size += (6 - total_players) / 2; // [orig: @0x5012e6]
    for (const int cap : {20, 40, 60}) {                          // [orig: @0x501301..]
        if (team_size > cap) team_size = cap + (team_size - cap) / 2;
    }
    int base = 12; // setting -1/default [orig: @0x5012c4 switch on g_capture_speed_setting]
    if (speed_setting == 1) base = 24;
    else if (speed_setting == 2) base = 48;
    int64_t speed = static_cast<int64_t>(team_size) * base;
    if (shared_n > 1) speed /= shared_n; // [orig: the shared-zone-number divide @0x501465]
    if (speed <= 0) speed = 1;
    int32_t delta = static_cast<int32_t>(65536LL * presence / speed); // [orig: @0x501490 ftol]
    if (delta == 0) delta = presence > 0 ? 1 : -1; // minimum magnitude 1 [orig: @0x5014a4]
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

    // Shared-zone-number counts (a number held by N entities divides the speed).
    int shared_count[32] = {};
    for (const EntityHandle h : chain.zones) {
        if (const Entity *z = world.registry.get(h)) {
            if (z->zone_number < 32) ++shared_count[z->zone_number];
        }
    }

    // ---- The secure/control pass [orig: Server_UpdateCaptureZoneEntities @0x519690] ----
    struct FlipRequest {
        EntityHandle zone;
        uint8_t capturer = 0;
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
        uint8_t enemy_team_present = 0;
        for (const Entity *p : players) {
            if (!in_zone_radius(*p, *z)) continue;
            if (p->team == z->team) {
                ++friendlies;
            } else if (zone_chain_is_capturable(world, chain, p->team, *z)) {
                ++enemies;
                enemy_team_present = p->team;
            }
        }

        int16_t wire_delta = 0;
        const uint8_t enemy = enemy_of(z->team);
        if (!zone_chain_is_capturable(world, chain, enemy, *z)) {
            // The secure latch: the enemy frontier cannot reach it [orig: @0x519764].
            z->zone_control = 0x10000;
        } else {
            const int presence = friendlies - enemies;
            if (presence != 0) {
                const int side = presence > 0 ? z->team : enemy_team_present;
                const int side_players =
                        side < ZoneChain::kTeamCount ? team_players[side] : 0;
                const int shared_n =
                        z->zone_number < 32 ? shared_count[z->zone_number] : 1;
                const int32_t delta = zone_capture_control_delta(
                        presence, side_players, total_players, capture_speed_setting,
                        shared_n);
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
        if (enemies > 0 && z->zone_control <= 0)
            flip_requests.push_back({z->handle, enemy_team_present});
    }

    // ---- Queue drain: numbered zones flip INSTANTLY [orig: @0x53B8F0 drain leg] ----
    const bool decided_before = match_decided(world, chain);
    for (const FlipRequest &req : flip_requests) {
        Entity *z = world.registry.get(req.zone);
        if (z == nullptr) continue;
        const uint8_t old_team = z->team;
        // Previously owned -> neutralize; neutral -> the capturer takes it
        // [orig: "team change (via neutral when previously owned)"].
        const uint8_t new_team = old_team == 0 ? req.capturer : 0;
        const uint8_t cap_frontier_before =
                zone_chain_frontier_zone(world, chain, req.capturer);
        const uint8_t loser_frontier_before =
                zone_chain_frontier_zone(world, chain, enemy_of(req.capturer));

        z->team = new_team;                 // [orig: Server_ChangeEntityTeam @0x518D70]
        z->zone_control = 0;                // the new owner must SECURE it (the AS beat)
        zone_chain_rebuild_masks(world, chain);

        ZoneCaptureEvents::Flip flip;
        flip.zone = z->handle;
        flip.old_team = old_team;
        flip.new_team = new_team;
        flip.capturer_team = req.capturer;
        flip.capturer_frontier = zone_chain_frontier_zone(world, chain, req.capturer);
        flip.loser_frontier =
                zone_chain_frontier_zone(world, chain, enemy_of(req.capturer));
        flip.frontier_changed = flip.capturer_frontier != cap_frontier_before ||
                                flip.loser_frontier != loser_frontier_before;
        flip.suppressed = decided_before; // [orig: GetWinningTeamIfAllOwned @0x4A2920]
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
