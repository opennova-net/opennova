#include <runtime/world/vehicle_system.h>
#include <runtime/world/world.h>
#include <base/io/rotating_prng.h>
#include <runtime/devtools/tick_profile.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <runtime/world/angle.h>
#include <runtime/world/collision.h>
#include <runtime/world/entity_spawn.h>
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/vehicle_sound.h>

#include <runtime/world/ai.h> // AiSystem / AiEntity / ai_apply_command — the AI-change command target
#include <base/io/bam.h>

namespace opennova::world {

// Max distance (mission units) for mount_best's nearest-emplacement search — the proximity
// proxy for the occupant-model+144 vehicle link the original resolves through the entity
// hierarchy. A manned-gun soldier is placed on/next to its gun, so this is generous.


// Attached emplacement children are allocated breadth-first after their carrier,
// so pool/slot iteration is parent-before-child even for turret-on-vehicle chains.
// Reuse the mounted-pose provider: a resolved USRP bone follows live PANM; bone
// zero takes pose_mounted_occupant's parent-root/local fallback.
static void pose_emplacement_attachments(World &world) {
    devtools::ProfileLap lap(world.profile);
    // Parent ownership ends when the carrier dies, even though ordinary item
    // destruction keeps that carrier resident as a husk. Peel orphan chains
    // without mutating registry slots during traversal.
    for (int depth = 0; depth < 8; ++depth) {
        std::vector<EntityHandle> orphans;
        world.registry.for_each([&](const Entity &candidate) {
            if (!candidate.emplacement_parent.valid()) return;
            const Entity *parent =
                    world.registry.get(candidate.emplacement_parent);
            if (parent == nullptr ||
                parent->registry_spawn_id !=
                        candidate.emplacement_parent_spawn_id ||
                !parent->alive || parent->health <= 0)
                orphans.push_back(candidate.handle);
        });
        if (orphans.empty()) break;
        for (EntityHandle orphan : orphans) {
            std::vector<EntityHandle> occupants;
            world.registry.for_each([&](const Entity &candidate) {
                if (candidate.mounted && candidate.mount_target == orphan)
                    occupants.push_back(candidate.handle);
            });
            for (EntityHandle occupant : occupants)
                world.vehicles.detach(occupant);
            world.commands.remove_ssn(orphan);
        }
    }
    lap.mark(devtools::Slot::SIM_ATTACHMENT_ORPHANS);
    world.registry.for_each([&](const Entity &snapshot) {
        if (!snapshot.emplacement_parent.valid()) return;
        // A stock streamed child carries an exact absolute spawn pose, but its
        // parent/type pair can map to multiple authored addeweap slots. Only a
        // resolved attachment row may replace that wire pose with a userpoint
        // pose. Orphan ownership and mounted-rider refresh remain independent.
        if (!snapshot.emplacement_pose_metadata_resolved) return;
        Entity *child = world.registry.get(snapshot.handle);
        const Entity *parent =
                world.registry.get(snapshot.emplacement_parent);
        if (child == nullptr || parent == nullptr ||
            parent->registry_spawn_id !=
                    snapshot.emplacement_parent_spawn_id)
            return;
        Seat anchor;
        anchor.type = SeatType::Gunner;
        anchor.bone_index = child->emplacement_bone;
        anchor.seat_local = child->emplacement_local;
        anchor.yaw_offset = child->emplacement_yaw_offset;
        anchor.attachment_frame = true;
        world.vehicles.pose_mounted_occupant(*child, *parent, anchor);
    });
    lap.mark(devtools::Slot::SIM_ATTACHMENT_CHILDREN);

    // A gunner riding an attached child was posed earlier in the AI system loop,
    // before the carrier moved. Refresh only the seat frame: the gun channel and
    // gunner look already advanced once during that body's update.
    world.registry.for_each([&](const Entity &snapshot) {
        if (!snapshot.mounted) return;
        Entity *occupant = world.registry.get(snapshot.handle);
        const Entity *target = world.registry.get(snapshot.mount_target);
        if (occupant == nullptr || target == nullptr ||
            !target->emplacement_parent.valid() ||
            snapshot.mount_seat < 0 ||
            snapshot.mount_seat >= static_cast<int>(target->seats.size()))
            return;
        if (AiEntity *body = world.ai.for_handle(snapshot.handle)) {
            world.ai.refresh_mounted_pose(*body, world);
            return;
        }
        world.vehicles.pose_mounted_occupant(*occupant, *target, target->seats[snapshot.mount_seat]);
    });
    lap.mark(devtools::Slot::SIM_ATTACHMENT_RIDERS);
}

// ----------------------------------------------------------------------------
// World
// ----------------------------------------------------------------------------

// Precipitation_Reset precedes mission entities and consumes the shared B stream.
// [orig: Game_StartMission @ 0x524360, seed @ 0x52460B / reset @ 0x5249D4]
World::World() : commands(*this), vehicles(*this), rotor_wash(*this), zones(*this) {
	weather.precipitation.reset(&io::rotating_prng_callback, &prng16_b_state);
}

uint16_t World::next_prng16() noexcept {
    // [orig: PRNG_Next16 @0x6130a0 / @0x613140, both over
    // dword_31BFBB0] s = rol4(s + rol11(s)) ^ 1; return low word.
    const uint32_t rol11 = (prng16_state << 11) | (prng16_state >> 21);
    uint32_t next = prng16_state + rol11;
    next = ((next << 4) | (next >> 28)) ^ 1u;
    prng16_state = next;
    return static_cast<uint16_t>(next);
}

// [orig: PRNG_Next16_B @ 0x6130F0]
uint16_t World::next_prng16_b() noexcept {
    return io::rotating_prng_next16(prng16_b_state);
}

uint16_t World::next_prng16_c() noexcept {
	return io::rotating_prng_next16(prng16_c_state);
}

void World::add_system(ISystem *sys) {
    if (sys) systems_.push_back(sys);
}

void World::load_systems() {
    diagnostics.clear();
    for (ISystem *s : systems_) s->on_load(*this);
}

void World::run_logic_tick(bool is_authority, TickPhase phase) {
    // The whole tick lands on SIM_SERVER_WORLD for every role (the server
    // tick, the joiner's local tick and the bare no-net tick alike); the
    // phases below lap onto the SIM_WORLD_* rows.
    const devtools::ProfileScope tick_scope(profile, devtools::Slot::SIM_SERVER_WORLD);
    devtools::ProfileLap lap(profile);
    // The WAC player cache refreshes at bytecode entry, not at this tick
    // boundary. [orig: WacScript_ExecuteBytecode @0x4F58F4]
    TickContext ctx;
    ctx.world = this;
    ctx.logic_tick = logic_tick;
    ctx.is_authority = is_authority;
    ctx.phase = phase;
    const bool pre_mission = phase == TickPhase::PreMission;
    const bool gameplay = phase == TickPhase::Gameplay;
    rules.logic_authority = is_authority;
    // The pending fire-sound countdown, before this tick's spawns: retail
    // drains after the client network frame (whose receive seeds our embedder
    // also applies pre-tick) and before the server/entity updates that seed
    // the rest [orig: Sound_TickPendingSlots @ 0x526697 in
    // Game_ProcessMainFrame, between Client_ProcessNetworkFrame and
    // Server_TickUpdate / Entity_UpdateAllEntities].
    out.fire_sounds.tick();
    // The presenting-client identity for the spawn-time tracer style select — stamped
    // before the system loop so out.rounds spawned THIS tick (AI fire, local fire) select
    // against fresh values [orig: g_local_player_entity->Team read @ 0x4ec740].
    round_sim.local_player = cached.local_player;
    if (const Entity *lp = registry.get(cached.local_player))
        round_sim.local_team = static_cast<uint8_t>(lp->team);
    lap.mark(devtools::Slot::SIM_WORLD_SETUP);
    // The system loop runs on BOTH the authoritative host and a non-authority client
    // (the original client also runs a tick): each system self-gates on
    // ctx.is_authority. WacSystem / BmsEventSystem early-out on a client (scripting is
    // host-only; the in-match C2S drain is host-only too, owned by Server_TickUpdate, not an
    // ISystem); AiSystem on a client simulates ONLY the
    // local player (the §5.38 entity==local-player branch) and leaves every other
    // entity to the replicated wire state. [orig: the client tick still steps the
    // local player's infantry motor; Server_TickUpdate / Game_ProcessMainFrame.]
    if (phase != TickPhase::PreRound) {
        for (ISystem *s : systems_) {
			// After WAC, before entity updates. [orig: Server_TickUpdate @0x51D8D2]
			if (s == &ai && is_authority && gameplay && (logic_tick & 31u) == 0)
				vehicles.tick_spawn_markers();
			// The AI system's own phases lap onto the SIM_AI_* rows inside its
			// tick; every other registered system is an authored script.
			const devtools::ProfileScope system_scope(
                    profile, s == &ai ? devtools::Slot::SIM_WORLD_AI
                                     : devtools::Slot::SIM_WORLD_SCRIPTS);
            s->tick(*this, ctx);
        }
        lap.restart();
        pose_emplacement_attachments(*this);
        // Static attachment poses can change after AI collision queries. The
        // projectile/out.destruction half of the tick starts a fresh matrix-view
        // epoch so it never inherits a pre-attachment target transform.
        if (collision != nullptr) collision->reset_query_view_cache();
    }
    lap.mark(devtools::Slot::SIM_WORLD_ATTACHMENTS);
    // Entity_UpdateAllEntities walks pool 1 before the projectile pool. That
    // prevents a newly converted charge from losing an arm-delay tick and lets
    // claymore shrapnel fly later in its detonation frame [orig:
    // Entity_UpdatePool1Slot @0x4b8dd0 -> Weapon_UpdateAllProjectiles @0x4ec020].
    // These presentation events describe only the current authoritative tick.
    if (is_authority && gameplay) {
        throwables.events.clear();
        throwables.tick(*this, ai.collision, tables.terrain);
    }
    if (gameplay) {
        tick_item_event_pool(*this, 1);
        minefields.tick_pool(*this, 1);
    }
    // The precipitation fall: while it rains every drop slot lowers by the
    // kind's per-tick amount, once per ENTITY update — retail runs it inside
    // Entity_UpdateAllEntities after the pool-1 walk and before
    // DeathPiece_TickAll, so it rides the entity update's frame gate (never
    // the 255-tick weather settle, never the pre-mission pass) and every peer
    // falls its own drops from the rain current the previous weather tick left
    // Entity_UpdateAllEntities itself returns before it without a local
    // player entity (@ 0x4c2110); its epilog-screen path skips it as well.
    // [orig: Precipitation_FallTick @ 0x5de8f0 from Entity_UpdateAllEntities
    //  @ 0x4c2214].
    if (gameplay && cached.local_player.valid())
        weather.precipitation.fall_tick(weather.core.scalar_channels.rain_pct_fp,
                                        weather.precipitation_kind);
    lap.mark(devtools::Slot::SIM_WORLD_THROWABLES);
    // The global weapon-action pump follows the complete entity/system update and
    // precedes projectile stepping. This is where an AI UseGun nextAction write can
    // become a same-frame round.
    // [orig: Entity_UpdateAllEntities @0x52674b, then
    //  WeaponAction_ProcessAllEntities @0x526786]
    // WeaponAction_ProcessAllEntities is after the timer-gated entity update
    // and is itself ungated, so an already-queued action may advance during
    // PreRound even though its spawned projectile cannot move until gameplay.
    // PreMission remains outside the frame pump entirely.
    // [orig: Game_ProcessMainFrame @0x52672C..0x526786]
    if (phase != TickPhase::PreMission)
        ai.pump_mounted_weapon_slots(*this, logic_tick);
    lap.mark(devtools::Slot::SIM_WORLD_WEAPONS);
    // Live out.rounds step on the host and on an explicitly configured MP
    // non-authority client. The latter is the retail tag-2 visual re-sim path;
    // every decoded/predicted round carries VisualOnly through all consequence
    // sites, so only the host can mutate gameplay state. Do not infer a client
    // role from is_authority=false alone -- tests and pre-mission callers use it too.
    // [orig: Weapon_UpdateAllProjectiles @0x4ec020; §5.60]
    if (gameplay &&
        (is_authority || (rules.mp_session && !rules.projectile_authority)))
        round_sim.tick(*this, tables.terrain, ai.collision);
    lap.mark(devtools::Slot::SIM_WORLD_PROJECTILES);
    if (gameplay &&
        (is_authority || (rules.mp_session && !rules.projectile_authority))) {
        // The explosion-queue drain runs once per frame after the projectile
        // update [orig: Projectile_ProcessExplosionQueue @0x4ead80]; entries the
        // damage callbacks push (the kz death chain) land next tick, exactly like
        // the original's post-reset writes. The death-piece pool advances
        // first; each item's death motion runs beside its class callback.
        // [orig: DeathPiece_TickAll @0x57b900].
        // Retail runs these UNGATED on every peer — the shared per-frame
        // entity update calls them on clients too, which is how a joiner's
        // 0x13/0x26-triggered death chain detonates its kz blasts and flies its
        // pieces locally. The MP visual client (the round pool's predicate
        // above) therefore drains them as well; its authoritative state keeps
        // arriving over the wire regardless.
        // [orig: Entity_UpdateAllEntities @0x4c2100 — DeathPiece_TickAll
        //  @0x4c221c, Projectile_ProcessExplosionQueue @0x4c223f, and the
        //  pool-2/3 update-callback walk, all unconditional]
        // The water plane: env.water_z (16.16, the #265 sound-profile home) —
        // zero means "no water authored", the same read the wreck gates use
        // [orig: Env_WaterHeightFixed @0x26c6454].
        const float water_z =
                env.water_z != 0 ? static_cast<float>(env.water_z) / 65536.0f : -1.0e9f;
        death_pieces.tick(*this, tables.terrain, water_z, out.destruction);
        explosions.process(*this, ai.collision, tables.terrain,
                           water_z, out.destruction);
    }
    if (gameplay) {
        tick_item_event_pool(*this, 2);
        minefields.tick_pool(*this, 2);
        doors.tick(*this); // [orig: Entity_UpdateAllEntities @0x4C2307]
        tick_item_event_pool(*this, 3);
        minefields.tick_pool(*this, 3);
    }
    item_emitters.sync_owners(*this);
    lap.mark(devtools::Slot::SIM_WORLD_DESTRUCTION);
    // The waypoint current-selection pass, from the local player's position (the
    // original runs it in the client frame beside the player update; our SP host
    // is that client — the pure-client view is D-HUD-16). Position converts to
    // the original's 16.16 fixed compare space. [orig: Player_UpdatePerFrame
    // @0x4de5f7]
    if (is_authority && gameplay && !script.waypoints.empty()) {
        if (const Entity *lp = registry.get(cached.local_player))
            script.waypoints.tick_advance(static_cast<int32_t>(lp->position.x * 65536.0f),
                                   static_cast<int32_t>(lp->position.y * 65536.0f));
    }
    if (is_authority) {
        if (pre_mission) {
            // The one-shot initial group recount, ordered right after the pre
            // pass [orig: Game_StartMission @ 0x525b86 -> @ 0x525b8b].
            recount_group_initials();
        } else if (--group_recount_timer_ <= 0) {
            // The 62-tick live rescan [orig: Server_TickUpdate timer
            // @ 0x51db6d, reload 0x3E @ 0x51db93 -> EntityPool_RecountLiveByGroup
            // @ 0x51dc02].
            group_recount_timer_ = 0x3E;
            recount_group_live();
        }
    }
	if (gameplay)
		rotor_wash.tick();
	++logic_tick; // [orig: current_tick @0x24c1968 advances once per frame tick]
	// Audio-less/headless hosts never drain presentation. Retire their bounded
    // latest-intent rows on the same logic clock so old entity lifetimes cannot
    // occupy mailbox admission indefinitely.
    out.sound_emitters.prune(logic_tick);
    script.voice.refresh(*this);
    lap.mark(devtools::Slot::SIM_WORLD_HOUSEKEEPING);
}

// Shared semantic half of Server_ProcessRoundEnd @0x5164f0. The authority
// transport emits the per-recipient wire transaction from this frozen result.
void World::process_round_end(int32_t winning_team) {
    if (!match.finish(winning_team, *this)) return; // double-run guard + frozen board [orig: @0x516502/@0x516528]
    // Match::finish applies the winner marker and builds the immutable board in
    // the original pre-send order [orig: GameEvent_ProcessScoring @0x52f550;
    // Server_BuildEndOfRoundScoreboard @0x508f30]. The authority net tail sends,
    // per active slot in state 6: S2C 0x61 round-end marker (4 zero bytes)
    // @0x516790, S2C 0x1D
    // scoreboard header [u8 winner][s16 score0][s16 score1][u8 draw][s8 myEntryIndex]
    // (EndRoundScoreboard_SerializeHeader @0x505280) @0x516839, CNetPlayer_SetGameState(11)
    // @0x516846, slot state 6->7 @0x51685e; then the per-team round-win counters for
    // game types 0x10000/65537/65540 @0x5168a0 and the MP-only 2790-tick linger
    // @0x5166c4 (drained by Server_TickUpdate -> exit reason 3 / the client frame ->
    // reason 4; SP never drains it — the epilog owns the SP exit).
    // Match::finish sets the sole outcome latch before the network/presentation
    // tails, matching retail's double-run guard without copying its global.
    // The SP tail [orig: @0x51691d..0x51698f]: stop the dialog audio channel
    // (DialogAudio_PlayNextChunkOrStop(0) @0x51694b) + Dialog_ResetAll + park the
    // mission music, then winner==1 -> the WIN epilog (Cine_InitPlayback @0x578390:
    // <mission>.cne if present, else a static camera; the flyaway + jo_Epil.tga
    // score screen) + end music track 1; anything else -> the LOSE cine
    // (Cine_StartPlayback @0x577840 letterbox/fade + the jo_Epil2.tga MISSION FAILED
    // screen) + end track 2 (MusicCtx_SelectEndTrack @0x672fd0). All host
    // presentation: the effect carries the winner, the host selects the flow.
    out.effects.push({"round_end", winning_team, 0, 0, 0, std::string()});
}

// Count alive members per commandGroup over the actor pools; group 0 is
// forced to zero [orig: EntityPool_RecountByType @ 0x40e7e0 /
// EntityPool_RecountLiveByGroup @ 0x40e8d0 — !(flags & 2) && health > 0].
static void count_groups(const EntityRegistry &registry,
                         int32_t (&counts)[TriggerRelations::kGroups]) {
    std::vector<EntityHandle> members;
    for (int g = 1; g < TriggerRelations::kGroups; ++g) {
        members.clear();
        registry.by_group(static_cast<uint8_t>(g), members);
        int32_t alive = 0;
        for (EntityHandle h : members) {
            const Entity *e = registry.get(h);
            if (e && e->alive && e->health > 0) ++alive;
        }
        counts[g] = alive;
    }
    counts[0] = 0;
}

void World::recount_group_initials() {
    int32_t counts[TriggerRelations::kGroups] = {};
    count_groups(registry, counts);
    for (int g = 0; g < TriggerRelations::kGroups; ++g) {
        script.relations.group(g).initial_count = counts[g];
        script.relations.group(g).live_count = counts[g];
    }
}

void World::recount_group_live() {
    int32_t counts[TriggerRelations::kGroups] = {};
    count_groups(registry, counts);
    for (int g = 0; g < TriggerRelations::kGroups; ++g)
        script.relations.group(g).live_count = counts[g];
}

World::Snapshot World::snapshot() const {
    Snapshot s;
    s.registry = registry;
    s.vars = script.vars;
    s.wac_values = script.wac_values;
    s.squad_events = script.squad_events;
    s.weapon_input = script.weapon_input;
    s.voice = script.voice.snapshot();
    s.initial_script_effects = out.script_effects;
    s.initial_script_sounds = out.script_sounds;
    s.initial_slot_sounds = out.slot_sounds;
    s.next_script_effect_order = out.next_script_effect_order;
    s.forced_animation = script.forced_animation;
    s.diagnostics = diagnostics;
    s.env = env;
    s.weather = weather;
    s.doors = doors;
    s.facials = facials;
    s.teammates = teammates;
    s.vehicle_ai_spawn_phase = vehicle_ai_spawn_phase;
    s.match = match;
    s.spawn_waves = zones.spawn_waves;
    s.zone_capture_state = zones.capture;
    s.spawn_cycle_counter = zones.spawn_cycle_counter;
    s.logic_tick = logic_tick;
    s.preround_delay_seconds = preround_delay_seconds;
    s.prng16_state = prng16_state;
    s.prng16_b_state = prng16_b_state;
	s.prng16_c_state = prng16_c_state;
	s.cease_fire = rules.cease_fire;
	s.crt_rand_state = crt_rand.state;
    s.local_player = cached.local_player;
    return s;
}

void World::restore(const Snapshot &s) {
    registry.restore_from(s.registry);
    script.vars = s.vars;
    script.wac_values = s.wac_values;
    env = s.env;
    weather = s.weather;
    doors = s.doors;
    facials = s.facials;
    match = s.match;
    zones.spawn_waves = s.spawn_waves;
    zones.capture = s.zone_capture_state;
    zones.spawn_cycle_counter = s.spawn_cycle_counter;
    logic_tick = s.logic_tick;
    preround_delay_seconds = s.preround_delay_seconds;
    prng16_state = s.prng16_state;
    prng16_b_state = s.prng16_b_state;
	prng16_c_state = s.prng16_c_state;
	rotor_wash.clear();
	registry.for_each([&](const Entity &e) { registry.get(e.handle)->veh.rotor_wash_handle = 0; });
	rules.cease_fire = s.cease_fire;
	minefields.remote_actors.clear();
    crt_rand.state = s.crt_rand_state;
    // Reset per-tick health/proximity counters, then restore only the stable
    // ownership identity captured with the registry. A post-snapshot player may
    // have reused a baseline actor's slot, while a listen baseline may already
    // contain its host player; copying the current cache or clearing ownership
    // unconditionally gets one of those cases wrong.
    cached = CachedFrameState{};
    cached.local_player = s.local_player;
    out.effects.clear();
    out.script_effects = s.initial_script_effects;
    out.script_sounds = s.initial_script_sounds;
    out.next_script_effect_order = s.next_script_effect_order;
	out.vehicle_effects.clear();
	out.slot_sounds = s.initial_slot_sounds;
	out.sound_emitters.clear();
    out.fire_sounds.clear();
	out.source_fires.clear();
    out.rounds.clear();
	round_sim.reset();
	explosions.reset();
    throwables.reset();
    death_pieces.reset();
    destruction_rng.reset();
    item_emitters.reset();
    out.scars.reset();
    out.terrain_scorches.reset();
    out.destruction = DestructionEvents{};
    out.entity_events.clear();
    // The baseline copy above restores the configured rules, roster, clock,
    // stats, and outcome together. This matters for SP-as-listen-server: its
    // host player and game type already exist when the play-start snapshot is
    // sealed, and reset must not reconstruct them through another seam.
    kill_stats = MissionKillStats{};
    load_systems(); // systems re-init their per-mission state
    teammates = s.teammates;
    vehicle_ai_spawn_phase = s.vehicle_ai_spawn_phase;
    script.heli_lift_active_count = int32_t(teammates.count());
    // on_load clears the WAC input arrays. A sealed play-start baseline may
    // already contain controls authored by the initial script execution.
    script.weapon_input = s.weapon_input;
    script.voice.restore(s.voice);
    script.squad_events = s.squad_events;
    script.forced_animation = s.forced_animation;
    diagnostics = s.diagnostics; // retain boot gaps, discard findings from the previous playthrough
    if (collision != nullptr) collision->refresh_after_registry_change(*this);
    registry.for_each([&](const Entity &vehicle) {
        if (vehicle.primary_occupant.valid())
            vehicles.emit_control_started(vehicle);
    });
}


void count_mission_units(World &world) {
    // Players (the +534 byte; our player_class != 0) count into the separate
    // player bucket the panel never draws; everything else with team >= 2 and
    // a non-zero items.def unit-class byte is one enemy unit. The original's
    // vehicle(3/4)/aircraft(9)/infantry split is fold-consumed as the total.
    // [orig: Score_ClassifyEntityForCounts @0x4fd070 — player @0x4fd074,
    //  team gate @0x4fd08d, def+0x196 gate @0x4fd09f, total @0x4fd0a8;
    //  driven over both pools by Score_CountMissionSubgoalsAndUnits @0x509e13..0x509e4a]
    int32_t total = 0;
    world.registry.for_each([&](const Entity &e) {
        if (e.player_class != 0) return;
        if (e.team < 2) return;
        if (e.item_unit_type == 0) return;
        ++total;
    });
    world.kill_stats.enemy_unit_total = total;
}

int32_t count_defined_subgoals(const World &world) {
    // The leading run of authored win conditions before the first 0 or 0xFF
    // entry, at most eight. [orig: Score_CountMissionSubgoalsAndUnits @0x509dc2..0x509dd1 scanning
    //  the header win-condition ids; the count lands in dword_C8468C]
    int32_t count = 0;
    for (int slot = 1; slot <= 8; ++slot) {
        const uint8_t id = world.script.subgoals.win_text_ids[slot];
        if (id == 0 || id == 0xFF) break;
        ++count;
    }
    return count;
}

} // namespace opennova::world
