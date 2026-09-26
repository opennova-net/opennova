#include <runtime/world/zone_system.h>
#include <runtime/world/zone_capture.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

#include <base/io/crt_ftol.h>
#include <runtime/world/collision.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/world.h>

namespace opennova::world {

namespace {

// enemy_of(team): 1 -> 2, else -> 1 [orig: `2 - (team != 1)`
// @0x519745..0x519757]. Team 0 therefore maps to 1.
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

uint32_t entity_flags(const Entity &e) { return e.flags | e.engine_flags; }

// CaptureZone_CheckProximityScoring's gates: in-session team games only, a
// registered capturer, and an owned zone. [orig: CaptureZone_CheckProximityScoring
// @0x500C50 — @0x500C74 session, @0x500C87 team bit, @0x500C8E Entity_ValidatePtr,
// @0x500CA2 zone team]
bool capture_scoring_applies(const World &world, EntityHandle capturer,
                             const Entity &zone) {
    return (world.match.rules().game_type & 0x10000u) != 0 &&
           world.match.player(capturer) != nullptr && zone.team != 0;
}

} // namespace

// [orig: calculate_capture_zone_control_delta @0x501120]
int32_t zone_capture_control_delta(const ZoneCaptureDeltaInput &input) {
    if (input.presence == 0) return 0; // [orig: `test v20` @0x501278]
    // [orig: g_capture_speed_setting 1 -> 24, 2 -> 48, else 12 @0x50128A..0x5012A4]
    const int base = input.speed_setting == 1 ? 24 : input.speed_setting == 2 ? 48 : 12;
    // The side whose player count sizes the speed: the owner's when friendlies
    // lead, the attacker's otherwise, by the zone team's 1/2 value.
    // [orig: @0x5012AC..0x5012D7]
    int side = 2;
    if (input.presence > 0 ? input.zone_team == 1 : input.zone_team == 2) side = 1;
    int team_size = input.team_players[side]; // [orig: @0x5012DF]
    const int total = input.team_players[1] + input.team_players[2];
    if (total < 6) team_size += (6 - total) >> 1; // [orig: @0x5012F8..0x501303]
    for (const int cap : {20, 40, 60}) {           // [orig: @0x501309..0x501338]
        if (team_size > cap) team_size = ((team_size - cap) >> 1) + cap;
    }
    // The base speed is stored as a float. [orig: fild/fimul/fstp @0x50133C]
    double speed = static_cast<float>(static_cast<double>(team_size) * base);

    if (input.spawn_zone_count > 0) {
        // The side already holding more numbered zones accelerates late in a
        // timed round: x = clamp(imbalance * (1 - 2 * remaining / roundTicks),
        // 0, 1) takes trunc(x * speed * 0.5) off the speed.
        // [orig: the list census @0x501366..0x5013A7; @0x5013AB..0x50142A]
        if (input.team1_zones != input.team2_zones) {
            const int diff = std::abs(input.team1_zones - input.team2_zones);
            const double ratio = static_cast<double>(diff) / input.numbered_zones;
            const int leader = input.team1_zones <= input.team2_zones ? 2 : 1;
            if (side == leader) {
                double x = ratio;
                if (input.game_time_minutes != 0) {
                    const double clock = 1.0 -
                            2.0 * (static_cast<double>(input.remaining_ticks) /
                                   static_cast<double>(3720 * input.game_time_minutes));
                    x = ratio * clock;
                }
                x = std::clamp(x, 0.0, 1.0); // [orig: @0x501400..0x501439]
                const int32_t reduction = io::retail_ftol_sse2(x * speed * 0.5);
                speed -= reduction; // [orig: `fisub` @0x501426]
            }
        }
        if (input.shared_zone_entities > 1)
            speed /= input.shared_zone_entities; // [orig: @0x501441..0x501448]
    }
    // [orig: `fidiv presence; fdivr 65536.0` @0x501452..0x501456, _ftol2_sse
    //  @0x50145C; the minimum magnitude @0x50146B..0x501478]
    const int32_t delta = io::retail_ftol_sse2(65536.0 / (speed / input.presence));
    if (delta != 0) return delta;
    return input.presence > 0 ? 1 : -1;
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

        if (!capture_request_available(world, *zone)) {
            state.refused_touches.push_back(player->handle);
            continue;
        }
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
    ZoneCaptureState &state = world.zones.capture;
    const MatchRules &rules = world.match.rules();

    // The in-game census the delta walks: every registered Player whose body is
    // neither carried nor dead. [orig: calculate_capture_zone_control_delta
    // @0x501120 — slot active, `test byte [ent+24h], 3`, state 6 @0x501173..0x501199]
    std::vector<const Entity *> census;
    for (const MatchPlayer &row : world.match.players()) {
        const Entity *e = world.registry.get(row.identity.entity);
        if (e == nullptr ||
                (entity_flags(*e) & (kEntityFlagCarried | kEntityFlagDead)) != 0)
            continue;
        census.push_back(e);
    }
    std::array<int, 5> team_players{};
    for (const Entity *e : census)
        if (e->team < team_players.size()) ++team_players[e->team]; // [orig: @0x5011A2]

    // The sorted SpawnZoneList's numbered ownership, read by every delta.
    // [orig: SpawnZoneList_GetCount @0x50134F; the census @0x501366..0x5013A7]
    const SpawnZoneRegistry spawn_zones = world.zones.build_spawn_zone_list();
    int team1_zones = 0;
    int team2_zones = 0;
    int numbered_zones = 0;
    int shared_count[256] = {};
    for (const EntityHandle handle : spawn_zones.entries) {
        const Entity *zone = world.registry.get(handle);
        if (zone == nullptr || zone->zone_number == 0) continue;
        if (zone->team == 1) ++team1_zones;
        if (zone->team == 2) ++team2_zones;
        ++shared_count[zone->zone_number];
        ++numbered_zones;
    }

    // ---- Secure/control pass: every live numbered ChangeTeam entity of pools
    // 1 then 2, in pool order (is_capture_trigger is the def's 0x20000 bit, so
    // it carries the def test). [orig: Server_UpdateCaptureZoneEntities
    // @0x519690 — the walk @0x5196A0..0x51973F]
    for (const int pool : {1, 2}) {
        std::vector<EntityHandle> zones;
        world.registry.for_each_in_pool(pool, [&](const Entity &e) {
            if ((entity_flags(e) & kEntityFlagCarried) != 0 || e.zone_number == 0 ||
                    !e.is_capture_trigger)
                return;
            zones.push_back(e.handle);
        });
        for (const EntityHandle zone_handle : zones) {
            Entity *zone = world.registry.get(zone_handle);
            if (zone == nullptr) continue;
            // The latch tests only enemy_of(owner); `before` is read after it,
            // so a latch alone never raises the secured edge.
            // [orig: @0x519745..0x519764; the read @0x519775]
            if (!world.zones.is_capturable(enemy_of(zone->team), *zone))
                zone->zone_control = 0x10000;
            const int32_t before = zone->zone_control;

            // Presence: same-team Players count for the owner; every other
            // Player in radius counts as an enemy and, when its team may
            // capture the zone, against the owner.
            // [orig: calculate_capture_zone_control_delta @0x50120D..0x501248]
            int presence = 0;
            int friendlies = 0;
            int enemies = 0;
            for (const Entity *player : census) {
                if (!in_zone_radius(*player, *zone)) continue;
                if (player->team == zone->team) {
                    ++presence;
                    ++friendlies;
                } else {
                    if (world.zones.is_capturable(player->team, *zone)) --presence;
                    ++enemies;
                }
            }
            ZoneCaptureDeltaInput input;
            input.presence = presence;
            input.zone_team = zone->team;
            input.team_players = team_players;
            input.speed_setting = rules.capture_speed_setting;
            input.spawn_zone_count = static_cast<int>(spawn_zones.entries.size());
            input.team1_zones = team1_zones;
            input.team2_zones = team2_zones;
            input.numbered_zones = numbered_zones;
            input.shared_zone_entities = shared_count[zone->zone_number];
            input.remaining_ticks = world.match.remaining_ticks();
            input.game_time_minutes = rules.game_time_minutes;
            const int32_t delta = zone_capture_control_delta(input);
            // [orig: control += delta, clamped to [0, 0x10000] @0x50147F..0x5014AE]
            int32_t control = static_cast<int32_t>(
                    static_cast<uint32_t>(zone->zone_control) + static_cast<uint32_t>(delta));
            if (control > 0x10000) control = 0x10000;
            if (control < 0) control = 0;
            zone->zone_control = control;

            // Retail sends the 0x6F record before either secure-edge event for
            // this zone; its delta word is the low half of the delta and its two
            // counters are byte stores. [orig: `movzx ebx, ax` @0x51978C; emit
            // @0x5197D9; +544/+545 @0x5014BD/@0x5014C8; edges @0x519839/@0x51988E]
            out.ordered.emplace_back(ZoneCaptureEvents::Control{
                    zone->handle, zone->team, zone->zone_control,
                    static_cast<int16_t>(wire_word(delta)),
                    static_cast<uint8_t>(friendlies), static_cast<uint8_t>(enemies)});
            if (before != 0x10000 && zone->zone_control == 0x10000)
                out.ordered.emplace_back(
                        ZoneCaptureEvents::Secure{zone->handle, zone->team, true});
            if (before > 0 && zone->zone_control == 0)
                out.ordered.emplace_back(
                        ZoneCaptureEvents::Secure{zone->handle, zone->team, false});

            // Pool-1/2 live entities carrying ItemDefAttrib2 bit 2 inherit the
            // numbered trigger's owner while physically inside its radius.
            // [orig: Server_UpdateCaptureZoneEntities @0x51989E..0x5199B4]
            std::vector<EntityHandle> converts;
            world.registry.for_each([&](const Entity &entity) {
                if ((entity.handle.pool() == 1 || entity.handle.pool() == 2) &&
                        entity.has_item_def &&
                        (entity_flags(entity) & kEntityFlagCarried) == 0 &&
                        (entity.item_attrib2 & 2u) != 0 &&
                        in_zone_radius(entity, *zone))
                    converts.push_back(entity.handle);
            });
            for (const EntityHandle handle : converts)
                change_entity_team(world, out, handle, zone->team);
        }
    }

    // ---- FARP ownership enforcement. Retail's mission-start proximity list is
    // pools 1/2 with a live ItemDef whose Attrib2 carries FARP (0x2000). Every
    // numbered member is forced to team 2 when team 2 wholly owns its number,
    // then team 1 takes precedence; an unowned number forces team 0. The masks
    // are computed live from the chain (a number with any entity on another
    // team is nobody's), and the shift wraps as x86 `shl` does. This precedes
    // Server_UpdateCaptureZones, so a capture flip affects these entities on
    // the NEXT 1 Hz pass. [orig: Entity_BuildProximityListFromPools @0x43ED60;
    // Server_EnforceZoneEntityTeams @0x519600 — the owned masks @0x51960A /
    // @0x519618, the member walk @0x519630..0x51967D, the change @0x51966C]
    const uint32_t team2_owned = world.zones.owned_zone_mask(2);
    const uint32_t team1_owned = world.zones.owned_zone_mask(1);
    std::vector<EntityHandle> farp_entities;
    world.registry.for_each([&](const Entity &entity) {
        if ((entity.handle.pool() == 1 || entity.handle.pool() == 2) &&
                entity.has_item_def && entity.zone_number != 0 &&
                (entity.item_attrib2 & 0x2000u) != 0)
            farp_entities.push_back(entity.handle);
    });
    for (const EntityHandle handle : farp_entities) {
        Entity *entity = world.registry.get(handle);
        if (entity == nullptr) continue;
        const uint32_t bit = 1u << (entity->zone_number & 31u);
        uint8_t forced = 0;
        if ((team2_owned & bit) != 0) forced = 2;
        if ((team1_owned & bit) != 0) forced = 1;
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
            // entry's limit and emits 0x53 carrying progress 0 AND limit 0, but
            // neither consumes the request nor updates the retained capturer.
            // The drain below then sees the same request: an uncontested one
            // restarts AGAIN (capturer updated, second 0x53 + the start event); a
            // contested one is dropped, leaving the old capturer credited on
            // completion. Both are witnessed quirks.
            // [orig: Server_UpdateCaptureZones @0x53B9E3..0x53BA36 (restart; the
            // window's `push 0; push 0` @0x53BA07/@0x53BA09); the drain's
            // restart @0x53BC24..0x53BC3E and its window @0x53BBBE..0x53BC0D;
            // the contested drop @0x53BC1D]
            active.team = opposing->team;
            active.progress = 0;
            active.limit = rules.capture_duration_seconds;
            ZoneCaptureEvents::TimerWindow window = timer_window(*zone, active);
            window.limit = 0;
            out.ordered.emplace_back(window);
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

        // Completion: the owner, the capture scoring (event 14: an active
        // entry is always unnumbered), the wave reset, GameEvent_FlagCapture.
        // [orig: @0x53BA63..0x53BAA8]
        change_entity_team(world, out, zone->handle, active.team);
        const bool takeover = capture_scoring_applies(world, active.capturer, *zone);
        world.zones.spawn_waves.reset_on_zone_team_change(world, zone->handle);
        out.ordered.emplace_back(ZoneCaptureEvents::TimedCompletion{
                zone->handle, active.capturer, active.team,
                zone->is_spawn_point, takeover});
        remove_zone_requests(state, active.zone);
        state.active.erase(state.active.begin() + index);
    }

    // ---- Request drain. Opposing touches contest; numbered/zero-duration
    // zones flip immediately, while unnumbered zones start/restart ACTIVE state.
    // [orig: queue loop @0x53BAC5..0x53BC99]
    while (!state.requests.empty()) {
        const ZoneCaptureState::Request request = state.requests.front();
        Entity *zone = world.registry.get(request.zone);
        const Entity *capturer = world.registry.get(request.capturer);
        if (zone == nullptr || capturer == nullptr || capturer->team != request.team) {
            remove_zone_requests(state, request.zone);
            continue;
        }
        // The owner as the drain finds it; the start window carries it.
        // [orig: state0 @0x53BADC]
        const uint8_t state0 = zone->team;

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
            // [orig: Server_UpdateCaptureZones @0x53BC46..0x53BC94]
            ZoneCaptureEvents::Flip flip;
            flip.zone = zone->handle;
            flip.capturer = request.capturer;
            flip.old_team = zone->team;
            if (zone->team != 0)
                change_entity_team(world, out, zone->handle, 0);
            change_entity_team(world, out, zone->handle, request.team);
            flip.new_team = zone->team;
            flip.capturer_team = capturer->team;
            flip.numbered = zone->zone_number != 0;
            flip.announce = zone->is_spawn_point;
            // CaptureZone_CheckProximityScoring: a numbered zone scores event 24
            // on every live roster Player of the capturer's team in its radius;
            // an unnumbered one scores event 14 on the capturer.
            // [orig: the call @0x53BC72; @0x500CAF..0x500DC5]
            if (capture_scoring_applies(world, request.capturer, *zone)) {
                if (flip.numbered) {
                    for (const MatchPlayer &row : world.match.players()) {
                        const Entity *player = world.registry.get(row.identity.entity);
                        if (player != nullptr && player->team == capturer->team &&
                                (entity_flags(*player) & kEntityFlagDead) == 0 &&
                                in_zone_radius(*player, *zone))
                            flip.scorers.push_back(player->handle);
                    }
                } else {
                    flip.takeover = true;
                }
            }
            zone->zone_control = 0; // [orig: @0x53BC77]
            // GameEvent_FlagCapture's numbered SpawnPoint leg: the mask refresh
            // (the chain's only one), the all-owned test, the zone info, the
            // capturer's slot and its frontier after the refresh.
            // [orig: GameEvent_FlagCapture @0x50F6F0, the call @0x53BC86 — the
            //  0x40000 gate @0x50F70B, @0x50F762, @0x50F764, @0x50F781,
            //  @0x50F7BA, @0x50F7FA]
            if (flip.announce && flip.numbered) {
                flip.unchanged =
                        world.zones.rebuild_masks_and_check_unchanged(capturer->team, *zone);
                flip.decided = world.zones.winning_team_if_all_owned().has_value();
                if (!flip.decided) {
                    // A numbered zone flipped here is always a chain entry (the
                    // touch requires ChangeTeam); outside the chain retail would
                    // send its unwritten stack bytes, so the bare number stands.
                    if (!world.zones.zone_info(*zone, flip.zone_number, flip.rank)) {
                        flip.zone_number = zone->zone_number;
                        flip.rank = 0;
                    }
                    flip.capturer_is_player = world.match.player(request.capturer) != nullptr;
                    flip.frontier = world.zones.frontier_zone(capturer->team);
                }
            }
            out.ordered.emplace_back(std::move(flip));
            world.zones.spawn_waves.reset_on_zone_team_change(world, zone->handle); // [orig: @0x53BC94]
            remove_zone_requests(state, request.zone);
            continue;
        }

        // An unnumbered timed start: neutralize first, then add or restart the
        // entry. The zone is ownerless by then, so the capture scoring call
        // here scores nothing. [orig: @0x53BB3D..0x53BC0D]
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
            ZoneCaptureEvents::TimerWindow window = timer_window(*zone, *active);
            window.current_team = state0; // [orig: the window's state0 @0x53BBBE..0x53BC02]
            out.ordered.emplace_back(window);
            if (zone->is_spawn_point)
                out.ordered.emplace_back(ZoneCaptureEvents::TimedStart{
                        zone->handle, active->capturer, active->team});
        }
        remove_zone_requests(state, request.zone);
    }
}

} // namespace opennova::world
