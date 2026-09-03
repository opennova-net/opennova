#include <runtime/world/zone_system.h>
#include <runtime/world/zone_capture.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include <runtime/world/collision.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/world.h>

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
    // Unknown/legacy settings use the 12-second base. Retail's configured
    // default is setting 1 (base 24), established by Config_SetDefaults.
    // [orig: Config_SetDefaults @0x54D030; switch @0x5012c4]
    int base = 12;
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

namespace {

ZoneCaptureState::Active *find_active(ZoneCaptureState &state,
                                      EntityHandle zone) {
    for (auto &entry : state.active)
        if (entry.zone == zone) return &entry;
    return nullptr;
}

void remove_zone_requests(ZoneCaptureState &state, EntityHandle zone) {
    state.requests.erase(
            std::remove_if(state.requests.begin(), state.requests.end(),
                           [&](const auto &request) { return request.zone == zone; }),
            state.requests.end());
}

uint16_t wire_word(int32_t value) {
    return static_cast<uint16_t>(static_cast<uint32_t>(value));
}

ZoneCaptureEvents::TimerWindow timer_window(const Entity &zone,
                                            const ZoneCaptureState::Active &active) {
    return {zone.handle, zone.team, active.team, wire_word(active.progress),
            wire_word(active.limit), active.rate};
}

// The capture transaction's one ownership mutation primitive. Snapshot the
// complete six-byte 0x50 semantic payload at mutation time: instant captures
// deliberately call this twice (old owner -> 0 -> capturer), and a later entity
// lookup would erase the neutral record. [orig: Server_ChangeEntityTeam
// @0x518D70; write_entity_handle_packet @0x506AD0]
bool change_entity_team(World &world, ZoneCaptureEvents &out,
                        EntityHandle handle, uint8_t team) {
    Entity *entity = world.registry.get(handle);
    if (entity == nullptr || entity->team == team) return false;
    entity->team = team;

    ZoneCaptureEvents::TeamChange change;
    change.entity = handle;
    change.team = team;
    if ((entity->flags & kEntityFlagPlayer) != 0) {
        change.net_id = entity->minimap_net_id;
        change.anim_slot = entity->anim_slot;
    }
    out.ordered.emplace_back(change);
    return true;
}

bool capture_request_available(const World &world, const Entity &zone) {
    if (zone.zone_number == 0) return true;
    return world.zones.is_capturable(1, zone) ||
           world.zones.is_capturable(2, zone);
}

} // namespace

void ZoneSystem::capture_contact_tick() {
    World &world = world_;
    if (world.collision == nullptr) return;
    const std::vector<CollisionWorld::GameplayContact> contacts =
            world.collision->take_change_team_contacts();

    // The collision callback is multiplayer/team-family gated in retail. This
    // raw bit test intentionally includes C&C and every other team mode that
    // happens to author a ChangeTeam trigger. There is no MoveOrder gate: the
    // movement resolver's exact type-10 contact is the producer.
    // [orig: @0x4B31DD..0x4B3238]
    if ((world.match.rules().game_type & 0x30000u) == 0) return;

    ZoneCaptureState &state = world.zones.capture;
    for (const CollisionWorld::GameplayContact &contact : contacts) {
        const Entity *player = world.registry.get(contact.source);
        const Entity *zone = world.registry.get(contact.target);
        if (player == nullptr || zone == nullptr ||
                !is_playing_player(*player) ||
                !zone->is_capture_trigger || !zone->alive ||
                (player->team != 1 && player->team != 2))
            continue;

        // Server_OnPlayerTouchCaptureZone rejects an enemy touching a still
        // secured numbered zone. Unnumbered objectives always pass.
        if (zone->zone_number != 0 && player->team != zone->team &&
                zone->zone_control > 0)
            continue;

        if (ZoneCaptureState::Active *active = find_active(state, zone->handle)) {
            if (active->presence.size() < 32 &&
                    std::find(active->presence.begin(), active->presence.end(),
                              player->handle) == active->presence.end())
                active->presence.push_back(player->handle);
        }

        if (!capture_request_available(world, *zone)) continue;
        const auto duplicate = std::find_if(
                state.requests.begin(), state.requests.end(),
                [&](const auto &request) {
                    return request.zone == zone->handle &&
                           request.team == player->team;
                });
        if (duplicate == state.requests.end())
            state.requests.push_back(
                    {zone->handle, player->team, player->handle});
    }
}

void ZoneSystem::capture_second_tick(ZoneCaptureEvents &out) {
    World &world = world_;
    out.clear();
    ZoneChain &chain = world.zones.chain;
    ZoneCaptureState &state = world.zones.capture;
    const MatchRules &rules = world.match.rules();

    // Playing-player census: per-team counts + stable handles for radius/scoring.
    int team_players[ZoneChain::kTeamCount] = {0, 0, 0, 0, 0};
    int total_players = 0;
    std::vector<const Entity *> players;
    world.registry.for_each([&](const Entity &entity) {
        if (!is_playing_player(entity)) return;
        players.push_back(&entity);
        ++total_players;
        if (entity.team < ZoneChain::kTeamCount) ++team_players[entity.team];
    });

    // The delta producer scans the sorted spawn-zone registry, not just capture
    // triggers: every nonzero-number entry contributes to ownership imbalance,
    // the denominator, and the shared-number divide.
    // [orig: calculate_capture_zone_control_delta @0x501120]
    int shared_count[32] = {};
    int owned_spawn_zones[ZoneChain::kTeamCount] = {};
    int numbered_spawn_zones = 0;
    const SpawnZoneRegistry spawn_zones = world.zones.build_spawn_zone_list();
    for (const EntityHandle handle : spawn_zones.entries) {
        const Entity *zone = world.registry.get(handle);
        if (zone == nullptr || zone->zone_number == 0) continue;
        ++numbered_spawn_zones;
        if (zone->zone_number < 32) ++shared_count[zone->zone_number];
        if (zone->team < ZoneChain::kTeamCount)
            ++owned_spawn_zones[zone->team];
    }

    // ---- Secure/control pass [orig: Server_UpdateCaptureZoneEntities @0x519690].
    for (const EntityHandle zone_handle : chain.zones) {
        Entity *zone = world.registry.get(zone_handle);
        if (zone == nullptr) continue;
        const int32_t before = zone->zone_control;

        int friendlies = 0;
        int enemies = 0;
        int attackers_by_team[ZoneChain::kTeamCount] = {};
        for (const Entity *player : players) {
            if (!in_zone_radius(*player, *zone)) continue;
            if (zone->team != 0 && player->team == zone->team) {
                ++friendlies;
            } else if (world.zones.is_capturable(player->team, *zone)) {
                ++enemies;
                if (player->team < ZoneChain::kTeamCount)
                    ++attackers_by_team[player->team];
            }
        }

        uint8_t attacker_team = 0;
        int attacker_count = 0;
        bool multiple_attacker_teams = false;
        for (uint8_t team = 1; team < ZoneChain::kTeamCount; ++team) {
            if (attackers_by_team[team] == 0) continue;
            if (attacker_team != 0) multiple_attacker_teams = true;
            if (attacker_team == 0) {
                attacker_team = team;
                attacker_count = attackers_by_team[team];
            }
        }

        int16_t wire_delta = 0;
        bool reachable_by_enemy = zone->team == 0;
        for (uint8_t team = 1;
                team < ZoneChain::kTeamCount && !reachable_by_enemy; ++team) {
            if (team != zone->team &&
                    world.zones.is_capturable(team, *zone))
                reachable_by_enemy = true;
        }
        if (!reachable_by_enemy) {
            zone->zone_control = 0x10000;
        } else {
            const int presence = multiple_attacker_teams
                    ? 0
                    : friendlies - attacker_count;
            if (presence != 0) {
                const int side = presence > 0 ? zone->team : attacker_team;
                ZoneCaptureDeltaInput input;
                input.presence = presence;
                input.capturing_side_players =
                        side < ZoneChain::kTeamCount ? team_players[side] : 0;
                input.total_players = total_players;
                input.speed_setting = rules.capture_speed_setting;
                input.shared_zone_entities = zone->zone_number < 32
                        ? shared_count[zone->zone_number]
                        : 1;
                input.capturing_side_zones = side < ZoneChain::kTeamCount
                        ? owned_spawn_zones[side]
                        : 0;
                const uint8_t opposing_side = side == 1 ? 2 : 1;
                input.opposing_side_zones = owned_spawn_zones[opposing_side];
                input.numbered_spawn_zones = numbered_spawn_zones;
                input.remaining_ticks = world.match.remaining_ticks();
                input.game_time_minutes = rules.game_time_minutes;
                const int32_t delta = zone_capture_control_delta(input);
                wire_delta = static_cast<int16_t>(std::clamp(
                        delta,
                        int32_t{std::numeric_limits<int16_t>::min()},
                        int32_t{std::numeric_limits<int16_t>::max()}));
                zone->zone_control = static_cast<int32_t>(std::clamp<int64_t>(
                        static_cast<int64_t>(zone->zone_control) + delta,
                        0, 0x10000));
            }
        }

        // Retail sends the 0x6F record before either secure-edge event for this
        // zone. Keep that order in the semantic transaction.
        // [orig: emit @0x5197D9, edges @0x519839/@0x51988E]
        out.ordered.emplace_back(ZoneCaptureEvents::Control{
                zone->handle, zone->team, zone->zone_control, wire_delta,
                static_cast<uint8_t>(std::min(friendlies, 255)),
                static_cast<uint8_t>(std::min(enemies, 255))});
        if (before < 0x10000 && zone->zone_control >= 0x10000)
            out.ordered.emplace_back(
                    ZoneCaptureEvents::Secure{zone->handle, zone->team, true});
        else if (before > 0 && zone->zone_control <= 0)
            out.ordered.emplace_back(
                    ZoneCaptureEvents::Secure{zone->handle, zone->team, false});

        // Pool-1/2 live entities carrying ItemDefAttrib2 bit 2 inherit the
        // numbered trigger's owner while physically inside its radius.
        // [orig: Server_UpdateCaptureZoneEntities @0x519690]
        std::vector<EntityHandle> converts;
        world.registry.for_each([&](const Entity &entity) {
            if ((entity.handle.pool() == 1 || entity.handle.pool() == 2) &&
                    entity.has_item_def &&
                    ((entity.flags | entity.engine_flags) &
                     kEntityFlagCarried) == 0 &&
                    (entity.item_attrib2 & 2u) != 0 &&
                    in_zone_radius(entity, *zone))
                converts.push_back(entity.handle);
        });
        for (const EntityHandle handle : converts)
            change_entity_team(world, out, handle, zone->team);
    }

    // ---- FARP ownership enforcement. Retail's mission-start proximity list is
    // pools 1/2 with a live ItemDef whose Attrib2 carries FARP (0x2000). Every
    // numbered member is forced to team 2, then team 1 takes precedence; an
    // unowned number forces team 0 too. This precedes Server_UpdateCaptureZones,
    // so a capture flip affects these entities on the NEXT 1 Hz pass.
    // [orig: Entity_BuildProximityListFromPools @0x43ED60;
    // Server_EnforceZoneEntityTeams @0x519600..0x51966C]
    std::vector<EntityHandle> farp_entities;
    world.registry.for_each([&](const Entity &entity) {
        if ((entity.handle.pool() == 1 || entity.handle.pool() == 2) &&
                entity.has_item_def && entity.zone_number != 0 &&
                (entity.item_attrib2 & 0x2000u) != 0)
            farp_entities.push_back(entity.handle);
    });
    for (const EntityHandle handle : farp_entities) {
        Entity *entity = world.registry.get(handle);
        if (entity == nullptr || entity->zone_number >= 32) continue;
        const uint32_t bit = 1u << entity->zone_number;
        uint8_t forced = 0;
        if ((chain.owned_mask[2] & bit) != 0) forced = 2;
        if ((chain.owned_mask[1] & bit) != 0) forced = 1;
        change_entity_team(world, out, handle, forced);
    }

    // ---- Active timed entries [orig: the rate pass + active loop @0x53B8F0].
    // Rate updates are the first wire output from Server_UpdateCaptureZones,
    // after both ownership-maintenance passes above.
    for (auto &active : state.active) {
        const int raw_count = static_cast<int>(active.presence.size());
        if (raw_count != active.rate) {
            active.rate = static_cast<uint8_t>(std::clamp(raw_count, 1, 32));
            out.ordered.emplace_back(
                    ZoneCaptureEvents::Presence{active.zone, active.rate});
        }
        active.presence.clear();
    }

    for (size_t index = 0; index < state.active.size();) {
        auto &active = state.active[index];
        Entity *zone = world.registry.get(active.zone);
        if (zone == nullptr) {
            remove_zone_requests(state, active.zone);
            state.active.erase(state.active.begin() + index);
            continue;
        }

        const auto opposing = std::find_if(
                state.requests.begin(), state.requests.end(),
                [&](const auto &request) {
                    return request.zone == active.zone &&
                           request.team != active.team;
                });
        if (opposing != state.requests.end()) {
            // Retail's restart leg flips the team, zeroes progress, reloads the
            // limit and emits 0x53, but neither consumes the request nor updates
            // the retained capturer. The drain below then sees the same request:
            // an uncontested one restarts AGAIN (capturer updated, second 0x53 +
            // the start event); a contested one is dropped, leaving the old
            // capturer credited on completion. Both are witnessed quirks.
            // [orig: Server_UpdateCaptureZones @0x53B9E3..0x53BA36 (restart);
            // drain restart @0x53BC38..0x53BC0D; contested drop @0x53BC1D]
            active.team = opposing->team;
            active.progress = 0;
            active.limit = rules.capture_duration_seconds;
            out.ordered.emplace_back(timer_window(*zone, active));
            ++index;
            continue;
        }

        active.progress += active.rate;
        out.ordered.emplace_back(timer_window(*zone, active));
        if (active.progress < active.limit) {
            remove_zone_requests(state, active.zone);
            ++index;
            continue;
        }

        change_entity_team(world, out, zone->handle, active.team);
        world.zones.spawn_waves.reset_on_zone_team_change(world, zone->handle);
        out.ordered.emplace_back(ZoneCaptureEvents::TimedCompletion{
                zone->handle, active.capturer, active.team,
                zone->is_spawn_point});
        remove_zone_requests(state, active.zone);
        state.active.erase(state.active.begin() + index);
    }

    // ---- Request drain. Opposing touches contest; numbered/zero-duration
    // zones flip immediately, while unnumbered zones start/restart ACTIVE state.
    // [orig: queue loop @0x53BB80..0x53BCC4]
    while (!state.requests.empty()) {
        const ZoneCaptureState::Request request = state.requests.front();
        Entity *zone = world.registry.get(request.zone);
        const Entity *capturer = world.registry.get(request.capturer);
        if (zone == nullptr || capturer == nullptr || capturer->team != request.team) {
            remove_zone_requests(state, request.zone);
            continue;
        }

        const bool current_owner_present = std::any_of(
                state.requests.begin(), state.requests.end(),
                [&](const auto &queued) {
                    return queued.zone == request.zone &&
                           queued.team == zone->team;
                });
        const bool conflicting = std::any_of(
                state.requests.begin(), state.requests.end(),
                [&](const auto &queued) {
                    return queued.zone == request.zone &&
                           queued.team != request.team;
                });
        if (current_owner_present || conflicting) {
            remove_zone_requests(state, request.zone);
            continue;
        }

        if (zone->zone_number != 0 || rules.capture_duration_seconds <= 0) {
            const uint8_t old_team = zone->team;
            const uint8_t cap_frontier_before =
                    world.zones.frontier_zone(request.team);
            const uint8_t loser_frontier_before =
                    world.zones.frontier_zone(enemy_of(request.team));
            if (old_team != 0)
                change_entity_team(world, out, zone->handle, 0);
            change_entity_team(world, out, zone->handle, request.team);
            world.zones.spawn_waves.reset_on_zone_team_change(world, zone->handle);
            zone->zone_control = 0;
            world.zones.rebuild_masks();

            ZoneCaptureEvents::Flip flip;
            flip.zone = zone->handle;
            flip.capturer = request.capturer;
            if (zone->zone_number != 0) {
                for (const Entity *player : players) {
                    if (player->team == request.team &&
                            in_zone_radius(*player, *zone))
                        flip.scorers.push_back(player->handle);
                }
            } else {
                flip.scorers.push_back(request.capturer);
            }
            flip.old_team = old_team;
            flip.new_team = request.team;
            flip.capturer_team = request.team;
            flip.capturer_frontier =
                    world.zones.frontier_zone(request.team);
            flip.loser_frontier = world.zones.frontier_zone(enemy_of(request.team));
            flip.frontier_changed =
                    flip.capturer_frontier != cap_frontier_before ||
                    flip.loser_frontier != loser_frontier_before;
            flip.suppressed = zone->zone_number != 0 && match_decided(world, chain);
            flip.announce = zone->is_spawn_point;
            out.ordered.emplace_back(std::move(flip));
            remove_zone_requests(state, request.zone);
            continue;
        }

        if (zone->team != 0) {
            change_entity_team(world, out, zone->handle, 0);
            world.zones.spawn_waves.reset_on_zone_team_change(world, zone->handle);
        }
        ZoneCaptureState::Active *active = find_active(state, request.zone);
        bool started = false;
        if (active == nullptr) {
            state.active.push_back({request.zone, request.team, 0,
                                    rules.capture_duration_seconds,
                                    request.capturer, {}, 1});
            active = &state.active.back();
            started = true;
        } else if (active->capturer != request.capturer) {
            active->team = request.team;
            active->progress = 0;
            active->limit = rules.capture_duration_seconds;
            active->capturer = request.capturer;
            started = true;
        }
        if (started) {
            out.ordered.emplace_back(timer_window(*zone, *active));
            if (zone->is_spawn_point)
                out.ordered.emplace_back(ZoneCaptureEvents::TimedStart{
                        zone->handle, active->capturer, active->team});
        }
        remove_zone_requests(state, request.zone);
    }

    world.zones.rebuild_masks();
}

} // namespace opennova::world
