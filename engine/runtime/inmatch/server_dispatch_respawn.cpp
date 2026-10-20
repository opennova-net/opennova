// The respawn and deploy legs of the host's C2S dispatch (split off
// server_message_dispatch.cpp) -- see server_dispatch_respawn.h.
#include <runtime/inmatch/server_dispatch_respawn.h>

#include <runtime/inmatch/server_team_change.h>
#include <runtime/inmatch/server_loadout_grant.h> // the deploy's kit rebuild and slot list
#include <runtime/inmatch/server_spawn.h> // the deploy leg's placement and protection, the C2S 0x41
#include <runtime/inmatch/server_tick.h> // Server_RerollPlayerTickSeed
#include <runtime/inmatch/napi_np_server_ctx.h>  // ServerDispatchInputs::server_ctx

#include <base/gameprofile/game_type.h>       // kConquerAndControl (the auto pick's refusal)
#include <net/npwire/ingame_message_id.h>
#include <net/title/title_identity.h> // title_in_match (the deploy's state byte, DFX2-11)

#include <runtime/world/entity.h> // world::Entity / EntityHandle: the bound entity read through owned_entity
#include <runtime/world/local_player.h>
#include <runtime/world/entity_spawn.h>  // entity_reset_to_spawn_state — the deploy revive (§5.61)
#include <runtime/world/spawn_select.h>  // deploy target validation + one spawn-pose resolver
#include <runtime/world/vehicle_attach.h> // find_best_vehicle_seat (the mobile spawn)
#include <runtime/world/world.h>
#include <runtime/world/zone_chain.h>    // zone_chain_frontier_zone — the 0x1E ev-0x3A deploy hint
#include <runtime/world/ai.h>
#include <runtime/world/geom.h>
#include <runtime/world/infantry.h>

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>

namespace opennova::inmatch {

namespace {

// tag=0x1E GAME-EVENT ev 0x3A (58) — the private deploy-screen frontier hint: "go capture zone N".
// attacker byte = the requester team's frontier zone number [orig: Server_DeployPlayer
// @0x517740 — GameEvent_BuildPayload(0x3A, ZoneSlotChain_FindFrontierZone(team), 0xFF, 0xFF, 0, 0)
// @0x517A1D, mask 0x20; §5.26 8-B body via GameEvent_BuildPayload @0x5054E0]. The golden retail
// frame-82540 body was {0x3a, 0x04, ...} — the retail host's live frontier at that moment; a
// World-less host keeps that golden byte so the burst pins hold.
std::vector<uint8_t> build_tag_1e_frontier_hint(uint8_t frontier_zone) {
	return {0x3a, frontier_zone, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00};
}
std::vector<uint8_t> build_tag_1e_game_event_post_spawn() {
	return build_tag_1e_frontier_hint(0x04); // the golden frame-82540 byte (World-less fallback)
}

} // namespace

std::vector<uint8_t> build_spawn_wave_status_body(
		const world::World &world, const NapiNPConnection &conn) {
	std::vector<uint8_t> body;
	const world::EntityHandle requester = conn.link.owned_entity;
	const uint8_t team = conn.assigned_team; // the slot team (D-NET-581) [orig: @0x5074CF]
	const world::SpawnZoneRegistry zones = world.zones.build_spawn_zone_list();
	std::vector<const world::SpawnWaveEntry *> visible;
	for (const world::SpawnWaveEntry &entry : world.zones.spawn_waves.entries())
		if (entry.team == team && visible.size() < 0xFFu) // [orig: @0x507539]
			visible.push_back(&entry);
	body.reserve(1 + visible.size() * 9);
	body.push_back(static_cast<uint8_t>(visible.size()));
	for (const world::SpawnWaveEntry *entry : visible) {
		const uint16_t zone = entry->zone.valid()
				? entry->zone.packed
				: uint16_t{0xFFFF};
		body.push_back(static_cast<uint8_t>(zone));
		body.push_back(static_cast<uint8_t>(zone >> 8));
		const int zone_index = world::spawn_zone_index_of(zones, entry->zone);
		const uint16_t wire_index = zone_index >= 0
				? static_cast<uint16_t>(zone_index)
				: uint16_t{0xFFFF};
		body.push_back(static_cast<uint8_t>(wire_index));
		body.push_back(static_cast<uint8_t>(wire_index >> 8));
		body.push_back(static_cast<uint8_t>(entry->queued.size()));
		const uint16_t countdown = entry->requester_countdown(requester);
		body.push_back(static_cast<uint8_t>(countdown));
		body.push_back(static_cast<uint8_t>(countdown >> 8));
		for (const world::EntityHandle member : entry->queued) {
			body.push_back(static_cast<uint8_t>(member.packed));
			body.push_back(static_cast<uint8_t>(member.packed >> 8));
		}
	}
	return body;
}

std::vector<ProtocolMessage> Server_ReleasePlayerDeployment(
		const GameConfig &config, NapiNPConnection &conn,
		world::World &world, world::EntityHandle target_zone, uint16_t requested_handle,
		ServerLogRecorder *server_log, const TitleIdentity *title) {
	std::vector<ProtocolMessage> replies;
	// The hold (slot state) clears first, ahead of the body's def test, so a refused
	// mobile spawn below leaves the slot unheld (D-NET-552) [orig: Server_DeployPlayer
	//  @0x517791 `and byte ptr [esi+15F38h], 0EFh`, the def test @0x517798].
	conn.link.respawn_pending = false;
	world.match.set_player_respawn_pending(conn.link.owned_entity, false);
	if (!conn.link.owned_entity.valid()) return replies;
	world::Entity *player = world.registry.get(conn.link.owned_entity);
	if (player == nullptr) return replies;
	// Server_DeployPlayer always releases the old seat before inspecting
	// a requested mobile spawn. If the requester was the vehicle's primary
	// occupant, this clears +0x170 and the subsequent mobile-spawn gate rejects.
	// [orig: Server_DeployPlayer @0x5177A9..0x5177DB]
	if (player->mounted)
		world.vehicles.detach(player->handle);
	const world::Entity *target = world.registry.get(target_zone);
	const bool mobile_spawn = target != nullptr && target->is_spawn_point &&
			target->item_type == 1;
	if (mobile_spawn) {
		// The root's dead bit, then its +0x170, then the walk [orig: Server_DeployPlayer
		//  @0x5177CB / @0x5177CF, @0x5177D5 / @0x5177DB, @0x5177EA / @0x5177F4].
		world::VehicleSeatSelection selected;
		if (((target->flags | target->engine_flags) & world::kEntityFlagDead) != 0 ||
				!target->primary_occupant.valid() ||
				!world::find_best_vehicle_seat(
						world, target_zone, player->handle, selected)) {
			// Each a bare return [orig: @0x517A7C]; the slot's next 0x0A shows the
			// unheld body, as after the deploy below (Server_SendEntityStateToPlayer).
			return replies;
		}
	}
	// The revive latch (+89932) skips the spawn protection and the kit rebuild below;
	// the placement lands a revive on its saved position (server_spawn.h). [orig: the
	//  latch @0x51791C; GameEvent_RevivePlayer @0x517DCD..0x517E09 saves]
	const bool revive_deploy = conn.reply.revive_pose_valid;
	const world::SpawnPointResult pose =
			Server_PositionDeployingPlayer(config, conn, world, *player, target_zone);
	// The Co-op marker arm's chute bit and queued carrier mount survive the
	// spawn-state reset below (it clears only the dead bit).
	// [orig: Server_PositionPlayerForSpawn @0x50D424..0x50D45A]
	world::apply_spawn_point_latches(*player, pose);
	// The deploy's and the reset's signed raises to the row's difficulty-aware ceiling: 0 leaves
	// the hp-0 body's 1, a wrapped one a dead player's health [orig: Server_DeployPlayer ->
	// Entity_RaiseHealthToMax @0x51782F; Entity_ResetToSpawnState @0x4B97BC].
	const bool def_hp = world.tables.player.has_item_def && world.tables.player.item_hp.has_value();
	const int32_t ceiling = def_hp || player->health_max > 0 ? world::max_health_with_difficulty(
			world, *player, def_hp ? *world.tables.player.item_hp : player->health_max) : 100;
	if (world::retail_signed_i16(player->health) < ceiling) player->health = ceiling;
	world::entity_reset_to_spawn_state(*player, ceiling);
	// Spawn protection: every deploy of a non-bot slot whose revive latch (+89932)
	// is clear stores the title's ticks into entity+292 (deploy_spawn_protection_ticks;
	// the reset above never touches it); the per-tick arm in Server_TickUpdate counts it
	// down, the first validated fire clears it, and Projectile_ProcessDamageOnTarget
	// zeroes damage while it is nonzero. The bot (+96483) and revive-latch 0-stores have
	// no roster feature to reach them here.
	// [orig: Server_DeployPlayer @0x517740 — 0 @0x51790a (bot) / @0x51791c (revive)]
	player->damage_state = revive_deploy ? 0 : deploy_spawn_protection_ticks(config.title, target);
	if (world::AiEntity *motor =
			world.ai.for_handle(player->handle);
			motor != nullptr && motor->inf.active) {
		const int32_t motor_position[3] = {
				player->position.x.q16,
				player->position.y.q16,
				player->position.z.q16,
		};
		// The placement's heading word (D-NET-376), a revive's included; with no
		// pick the body keeps its own word (D-NET-377) [orig: @0x50D3F7; @0x50D60A].
		const int32_t heading = pose.found ? pose.heading_bam : motor->heading;
		world::infantry_respawn_snap(*motor, motor_position, heading, player->health);
	}
	// The reset's tail rebuilds every slice at the placed pose (restarting the 17-tick phase),
	// then the body's list; its resolver pass is D-AI-9's [orig: Entity_ResetToSpawnState
	// @0x4B9610 - the resolver @0x4B9755, the builder @0x4B98EB, its list @0x4B98F1].
	if (world.collision != nullptr) world.collision->refresh_entity_proximity(world, *player);
	// [orig: Server_DeployPlayer @0x5178aa]
    if (player->handle == world.cached.local_player && world.local_player_state != nullptr)
        world.local_player_state->reset_for_new_round();
	// A deploy that is not a medic revive removes the devices a non-spectator
	// placed in its last life (each through the notifying removal; they stay
	// armed from the death until here), then rebuilds the slot's kit from its
	// loadout buffer; a revived player keeps the kit it died with (D-NET-378).
	// [orig: Server_DeployPlayer @0x517740 — the revive latch test
	//  @0x5178C5, the spectator latch test @0x5178CD, the
	//  Entity_RemovePlacedDevicesByOwner call @0x5178D8, the
	//  PlayerSlot_InitWeaponsFromLoadout call @0x5178E1]
	if (!revive_deploy) {
		if (!conn.link.spectator) world.commands.remove_placed_devices_by_owner(player->handle);
		// The listen host's own slot is skipped: ours keeps the host's local
		// inventory apart from the slot and does not refill it at a deploy
		// (D-NET-479; retail's local table is the slot table, built from its
		// buffer, which the round init seeds from g_SpawnLoadoutBuffer
		// @0x516BCA..0x516BE3 and the host's own 0x2F accepts rewrite). The slot
		// takes no 0x2F here, so the rebuild would only empty it.
		if (player->handle != world.cached.local_player)
			PlayerSlot_InitWeaponsFromLoadout(conn, world, *player);
	}
	conn.discard_pre_deploy_uplinks = true;
	// No +360 store; the +364 hold clears only for a raw handle other than
	// 0xFFFF, resolvable or not [orig: Server_DeployPlayer @0x51786B..0x517872].
	if (requested_handle != world::EntityHandle::kInvalid)
		conn.link.spawn_target_hold_seconds = 0;
	conn.link.downed_revive_seconds = 0;
	conn.link.medic_request_active = false;
	conn.link.death_cause_revivable = false;
	// The deploy leg overwrites the whole +89912 state byte (1, or 3 while the
	// pre-round timer runs: bit 1 is the pre-round loadout latch) and zeroes the
	// +356 armory cooldown; a DFX2 deploy writes 3 at every deploy (DFX2-11).
	// [orig: Server_DeployPlayer @0x517803/@0x517812; slot+0x164 = 0 @0x517900]
	// [orig: Server_DeployPlayer @0x514a55 (dfx2.exe)]
	conn.link.preround_loadout_latch = world.preround_delay_seconds != 0 ||
			title_in_match(title).join_state_fixed;
	conn.reply.frontier_hint_pending = false; // bits 0x04/0x08 of the same byte
	conn.reply.capture_nag_held = false;
	conn.link.armory_reuse_seconds = 0;
	conn.link.last_deploy_tick = world.logic_tick;
	conn.link.last_deploy_tick_valid = true;
	// The leg stores nothing to the hide bit: the slot's next 0x0A clears it and
	// re-sets a spectator's or a held slot's, so a held body stays hidden, and
	// out of that tick's humans count, until then (Server_SendEntityStateToPlayer)
	// [orig: Server_DeployPlayer's one Flags store @0x51787A;
	//  NetPacket_WritePlayerState @0x4FF6D7, @0x4FF7A1, @0x4FF7B8].
	server_logs_deploy(server_log, *player); // /PROFILE's PBRK [orig: Server_DeployPlayer @0x517a27..0x517a37]
	if (mobile_spawn) {
		// Retail repeats FindBestSeatSlot after the reset, then requests the
		// authoritative attach against the returned root/child seat owner.
		// [orig: Server_DeployPlayer @0x517A3E..0x517A74]
		world::VehicleSeatSelection selected;
		if (world::find_best_vehicle_seat(
					world, target_zone, player->handle, selected))
			world.vehicles.request_attach(player->handle, selected);
	}
	// Never on a revive deploy, whose client leaves the deploy wait on its record's
	// respawn edge (D-NET-379), nor out of a session (world.rules.mp_session)
	// [orig: @0x5178F2, under the @0x5178C5 test and is_in_session @0x5178E9 / @0x5178EF].
	if (!revive_deploy && world.rules.mp_session)
		replies.push_back(make_protocol_message(0x5A, current_weapon_slot_list(conn, &world)));
	replies.push_back(make_protocol_message(0x61, Server_RerollPlayerTickSeed(conn)));
	// The slot team's frontier (D-NET-583) [orig: @0x5179BA, the call @0x5179D9]
	const uint8_t frontier = world.zones.frontier_zone(conn.assigned_team);
	if (frontier != 0)
		replies.push_back(make_protocol_message(
				s2c::GAME_EVENT, build_tag_1e_frontier_hint(frontier)));
	return replies;
}

// See header. [orig: NapiNPServerMsg_0x04D_ChangeTeam @0x518F70]
void dispatch_team_change_request(NapiNPConnection &conn, uint32_t now_tick,
		world::World *world, const ServerDispatchInputs &inputs) {
	if (world != nullptr && inputs.server_ctx != nullptr)
		Server_HandleTeamChangeRequest(*inputs.server_ctx, conn, *world,
				io::host_milliseconds_for_logic_tick(now_tick));
}

// See header. [orig: NapiNPServerMsg_DeathTimeoutReset @0x510540]
void dispatch_death_timeout_reset(NapiNPConnection &conn, world::World *world,
		const ServerDispatchInputs &inputs) {
	if (world != nullptr && inputs.server_ctx != nullptr)
		Server_HandleDeathTimeoutReset(*inputs.server_ctx, conn, *world);
}

// See header.
void dispatch_respawn_request(const GameConfig &config, NapiNPConnection &conn,
		const ProtocolMessage &msg, world::World *world, const ServerDispatchInputs &inputs,
		std::vector<ProtocolMessage> &replies) {
	// [orig: Server_ProcessClientRequestRespawn @0x519AF0; net-re §5.61]. 0xFFFE = the
	// auto frontier pick; a real handle resolves through the SpawnPoint/team gates and a
	// NUMBERED zone additionally requires team match + control >= 1.0 (a contested zone
	// stops accepting spawns). An ALIVE in-session player's request is a no-op [orig: the
	// @0x519cce dead-or-flagged gate; only !is_in_session falls through @0x519cd7]; a DEAD
	// one deploys at the selected zone's wave boundary, or NOW when that zone has no
	// wave row: position at the pick (including §5.61's shared-counter 6007 scatter;
	// only the runtime-named model userpoint remains open) else the per-team marker chain,
	// reset-to-spawn-state, template health —
	// Server_DeployPlayer's deploy leg [orig: @0x517740 -> Server_PositionPlayerForSpawn
	// @0x50cf60 -> Entity_ResetToSpawnState @0x4B9610]. Deploy-time 0x61 seed re-send and
	// the 0x1D overlay stay burst-only (tracked §5.61 deferral). A newly queued pick
	// receives requester-local unreliable 0x6E immediately; immediate and wave release
	// share Server_ReleasePlayerDeployment. [orig: SpawnWaveList_TryQueuePlayer
	// @0x52A490; Server_SendSpawnWaveStatusToPlayer @0x50FF10]
	if (!conn.burst.spawned) return;
	// The round-over gate leads the handler: once the round ended no pick
	// deploys, queues or answers, the round-end linger's dead joiners
	// included (g_SpawnSuccessGate, our match outcome's `ended`, as the 0x29
	// and 0x5D cases read it). [orig: Server_ProcessClientRequestRespawn
	// @0x519AF0 - `cmp g_SpawnSuccessGate, ebp` @0x519AF6, `jnz` @0x519B00 to
	// the bare return @0x519DF2, ahead of the authority test @0x519B06]
	if (world != nullptr && world->match.outcome().ended) return;
	if (world == nullptr || !conn.link.owned_entity.valid()) {
		// World-less/unit-test path: the golden frame-82540 hint byte, as before.
		replies.push_back(
				make_protocol_message(s2c::GAME_EVENT, build_tag_1e_game_event_post_spawn()));
		return;
	}
	world::Entity *player = world->registry.get(conn.link.owned_entity);
	if (player == nullptr) return;
	// Body: [i16 spawnHandle]; a short/absent body reads 0 [orig: @0x519b42..48].
	const uint16_t pick = msg.payload.size() >= 2
			? static_cast<uint16_t>(msg.payload[0] | (msg.payload[1] << 8))
			: 0;
	const world::Entity *target = nullptr;
	const bool revive_latch = conn.reply.revive_pose_valid; // +89932
	// The base is the slot team (+416), not the entity's (D-NET-464) [orig: @0x519B88;
	//  the spectator legs @0x519B7A..0x519BC1].
	const uint8_t request_team = world::deploy_request_team(*world, conn.assigned_team,
			conn.link.spectator, pick, config.game_type);
	uint16_t handle = pick;
	if (pick == world::kDeployPickAutoTeam) {
		// Conquer & Control rejects the auto pick outright, before the frontier
		// resolve: no deploy, no wave queue, no reply.
		// [orig: Server_ProcessClientRequestRespawn @0x519bc8..0x519bd4]
		if (config.game_type == game_type::kConquerAndControl) return;
		// Auto-deploy: the team's frontier zone; null falls back to the marker chain
		// [orig: Spawn_FindEntityForTeam @0x4fc810 -> requestedHandle -1 on miss],
		// as a revived player always does [orig: the latch @0x519c3d..0x519c4b].
		const world::Entity *frontier = revive_latch ? nullptr
				: world->zones.find_spawn_zone_for_team(request_team, config.game_type);
		handle = frontier != nullptr ? frontier->handle.packed : world::kDeployPickNone;
	}
	if (handle != world::kDeployPickNone) {
		// Every handle but 0xFFFF, the frontier's included: a revived player's pick,
		// then +364, then the resolve against the requester entity's team, each
		// refusing. Handle 0, and the absent-body default above, resolves pool 0
		// index 0 like any other handle, so it deploys only when that entity is a
		// same-team spawn point (a player never is). [orig: @0x519c5a; @0x519c67;
		//  @0x519c58..0x519c88 resolve then return on null;
		//  Server_ResolveSpawnTargetHandle @0x4FE110 — pool/index decode
		//  @0x4fe13f..0x4fe159, attrib 0x40000 @0x4fe16f, team @0x4fe181]
		if (revive_latch || conn.link.spawn_target_hold_seconds != 0) return;
		target = world->zones.resolve_spawn_target(player->team, handle);
		if (target == nullptr) return; // a silent no-op [orig: @0x519c88]
	}
	// Retail's `nodefaultspawnpoints` option is named after its UI,
	// not its actual predicate. It applies only to a target-less pick
	// and walks SpawnZoneList for an unnumbered or fully controlled
	// same-team zone; it never counts living teammates.
	// [orig: Server_ProcessClientRequestRespawn @0x519C8E..0x519CB2;
	// Entity_HasAliveEntityOfTeam @0x4FC7B0; the revive latch @0x519c97]
	if (config.default_spawn_requires_no_team_zone != 0 &&
			!revive_latch && target == nullptr &&
			world->zones.team_has_available_spawn_zone(request_team))
		return;
	// The dead-or-pending gate: the requester's dead bit (entity+0x24 & 2) or
	// the slot's hold (+89912 & 0x10), never its Health, so a 0x41's Health 0
	// earlier in the pump, ahead of the body's edge, is no death here. An
	// unflagged requester is refused in a session; out of one
	// (world.rules.mp_session) it deploys at once, ahead of +360, the seat, the
	// zone and the wave. [orig: Server_ProcessClientRequestRespawn -- `test
	//  byte ptr [ecx+24h], 2` @0x519CC1, the hold @0x519CC7, is_in_session
	//  @0x519CD0 / @0x519CD7, the deploy @0x519CDD..0x519CE3; the dead bit's
	//  latch Entity_UpdateInfantryPlayerBody @0x4B4CB5]
	const bool dead =
			((player->flags | player->engine_flags) & world::kEntityFlagDead) != 0u;
	const bool deploy_now = !conn.link.respawn_pending && !dead;
	if (deploy_now && world->rules.mp_session) return;
	if (!deploy_now) {
		// +360 follows the dead-or-pending test and silently rejects every
		// pick, including Default Spawn. [orig: @0x519cf2]
		if (conn.link.respawn_delay_seconds != 0) return;
		if (target != nullptr && target->item_type == 1 &&
				target->is_spawn_point) {
			// A vehicle target is selectable only while its root's dead bit is
			// clear, and while its weighted root/child seat walk finds a free slot.
			// The root test refuses a dead root even when a live child would offer
			// a seat; the walk gates only each entry it visits. The +0x170 gate is
			// release's.
			// [orig: Server_ProcessClientRequestRespawn @0x519D07..0x519D34 - the
			//  root's dead bit @0x519D19 / @0x519D1D, the walk @0x519D2A]
			world::VehicleSeatSelection selected;
			if (((target->flags | target->engine_flags) & world::kEntityFlagDead) != 0 ||
					!world::find_best_vehicle_seat(
							*world, target->handle, player->handle, selected))
				return;
		}
		// The zone-ownership gate [orig: @0x519d5f: entity+538 -> team match AND
		// control(+540) >= 0x10000].
		if (target != nullptr && target->zone_number != 0 &&
		    (target->team != request_team || target->zone_control < 0x10000))
			return;
		// A spectator skips the wave [orig: @0x519D65..0x519D7F]; the queue asks as the
		// slot team (D-NET-581) [orig: the TryQueuePlayer call @0x519D8F].
		if (target != nullptr && !conn.link.spectator) {
			if (world->zones.spawn_waves.try_queue(
					target->handle, player->handle, conn.assigned_team)) {
				ProtocolMessage status = make_protocol_message(s2c::SPAWN_WAVE_STATUS,
						build_spawn_wave_status_body(*world, conn));
				status.reliable = false;
				replies.push_back(std::move(status));
				return;
			}
			if (world->zones.spawn_waves.has_entry(target->handle)) return;
		}
		world->zones.spawn_waves.remove_player(player->handle);
	}
	const world::EntityHandle target_handle = target != nullptr
			? target->handle
			: world::EntityHandle{};
	// The leg takes the stored handle [orig: Server_ProcessClientRequestRespawn,
	//  the Server_DeployPlayer calls @0x519CE3/@0x519D7F/@0x519DC8/@0x519DE8].
	std::vector<ProtocolMessage> deployment = Server_ReleasePlayerDeployment(
			config, conn, *world, target_handle, target_handle.packed,
			inputs.server_ctx != nullptr ? inputs.server_ctx->logs.profile : nullptr,
			inputs.server_ctx != nullptr ? inputs.server_ctx->np_protocol.identity
			                             : nullptr);
	replies.insert(replies.end(),
	               std::make_move_iterator(deployment.begin()),
	               std::make_move_iterator(deployment.end()));
}

// See header.
void dispatch_spectator_respawn(const GameConfig &config, NapiNPConnection &conn,
		const ProtocolMessage &msg, world::World *world, const ServerDispatchInputs &inputs,
		std::vector<ProtocolMessage> &replies) {
	// Gates in order: the round-over latch, authority, permanent death, a
	// spectator capacity, a player, not a spectator; then the convert runs
	// inline in the receive pump, so a later message in the same pump (a 0x41,
	// a 0x4D) finds the slot converted, and the deploy's replies join this
	// pump's. A short body reads 0.
	// [orig: Server_ProcessClientRequestSpectatorRespawn @0x51C840 -- the
	//  g_SpawnSuccessGate test @0x51C840..0x51C847, the latch @0x51C87C, the word
	//  @0x51C896..0x51C89C, Server_ConvertPlayerToSpectator(slot, word, 1) @0x51C8D5]
	if (world == nullptr || inputs.server_ctx == nullptr) return;
	if (world->match.outcome().ended) return;
	if (!inputs.server_ctx->is_authority) return;
	if (!config.permanent_death || config.spectator_slots == 0) return;
	if (!conn.link.owned_entity.valid() || conn.link.spectator) return;
	const uint16_t request = msg.payload.size() >= 2
			? static_cast<uint16_t>(msg.payload[0] | (msg.payload[1] << 8))
			: uint16_t{0};
	std::vector<ProtocolMessage> deployment =
			Server_ConvertPlayerToSpectator(*inputs.server_ctx, conn, *world, request);
	replies.insert(replies.end(), std::make_move_iterator(deployment.begin()),
			std::make_move_iterator(deployment.end()));
}

} // namespace opennova::inmatch
