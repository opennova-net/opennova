// The deploy, death and spectator half of the host's player-slot machinery
// (split off server_spawn.cpp; the declarations stay in server_spawn.h).
#include <runtime/inmatch/server_spawn.h>

#include <runtime/inmatch/server_message_dispatch.h> // Server_ReleasePlayerDeployment (the convert's deploy)

#include <runtime/world/ai.h>           // AiEntity / AiSystem
#include <runtime/world/angle.h>        // mission_yaw_deg_from_bam_heading
#include <runtime/world/entity.h>
#include <runtime/world/entity_spawn.h> // warm_up_player_body
#include <runtime/world/round_sim.h>     // RoundDeath (the Respawn key's death edge)
#include <runtime/world/spawn_select.h> // resolve_player_spawn_pose / world_has_spawn_zone
#include <runtime/world/vehicle_attach.h>
#include <runtime/world/world.h>        // World, registry, cached

#include <net/npwire/ingame_encode.h>     // encode_team_assign
#include <runtime/inmatch/server_squad.h> // Server_DissolveSquadOf
#include <net/npwire/ingame_message_id.h> // s2c::FORMATTED_GAME_TEXT (the 0x51 convert notice)

#include <algorithm>

namespace opennova::inmatch {

bool Server_SetPlayerSpectator(NapiNPServerCtx &ctx, NapiNPConnection &conn,
		world::World &world, bool spectator) {
	// Retail has no spectator leave: the slot latch (+0x188D7) is written by the add,
	// the host slot's round init and the 0x51 convert, and cleared only by the next
	// mission start's per-slot reset for a slot with a nonzero restore byte; the one
	// leave routine (@0x519F50, its deploy call @0x51A00C) has no caller (D-NET-407).
	// [orig: Server_PlayerAdd @0x51CD57 / @0x51CD83; Server_InitNewRoundState
	//  @0x51CA6D; Server_ConvertPlayerToSpectator @0x519E61; the unreferenced twin
	//  @0x519FA2; Server_ResetAllPlayerSlotFrameState @0x4FC320 (the clear @0x4FC372)]
	if (!spectator) return false;
	if (!ctx.is_authority || conn.phase < ConnectionPhase::PlayerAdded ||
	    !conn.link.owned_entity.valid()) {
		return false;
	}
	world::Entity *player = world.registry.get(conn.link.owned_entity);
	if (player == nullptr) return false;
	if (conn.link.spectator) return true;

	world::AiEntity *ai =
			world.ai.for_handle(player->handle);
	world.vehicles.detach(player->handle);
	// The deploy-hold bit is untouched on entering spectator mode (retail's
	// runtime conversion @0x519e76 leaves slot+89912 alone: a deployed
	// convert has it clear, an undeployed one keeps holding it).

	// The restore byte (+100568) takes the slot team, 0 included, ahead of the
	// latch and the slot team's clear (D-NET-584) [orig: Server_ConvertPlayerToSpectator
	//  @0x519E55, the store @0x519E5B; the latch @0x519E61, the clear @0x519E6E]
	conn.spectator_restore_team = conn.assigned_team;
	conn.link.spectator = true;
	// [orig: Server_ConvertPlayerToSpectator @0x519E76 — the runtime convert
	//  writes the same slot latch the roster row mirrors]
	world.match.set_player_spectator(conn.link.owned_entity, true);
	conn.assigned_team = 0;
	conn.assigned_team_valid = true;
	world.match.set_player_slot_team(conn.link.owned_entity, 0); // the mirror (D-NET-650)
	player->team = 0;
	player->flags |= 1u;
	// The witnessed spectator entity state: damage permanently disabled
	// (-1 sentinel), not the 620-tick join countdown.
	// [orig: Server_PlayerAdd @0x51cbc0 — entity+292 = -1]
	player->damage_state = -1;
	if (ai != nullptr) {
		ai->team = 0;
		ai->vel_x = 0;
		ai->vel_y = 0;
		ai->inf.player_moving = false;
		ai->inf.vel[0] = ai->inf.vel[1] = ai->inf.vel[2] = 0;
	}
	return true;
}

namespace {

// The changed player's squad break-up, after the new team is stored, so its
// S2C 0x71 [0xFF][slot] reaches its NEW team (server_squad.h).
// [orig: Server_SendPlayerStateAndSquad @0x518B40 from @0x518E92]
void send_player_state_and_squad(NapiNPServerCtx &ctx, NapiNPConnection &player) {
	Server_DissolveSquadOf(ctx, player);
}

} // namespace

// See header.
void Server_ChangeEntityTeam(NapiNPServerCtx &ctx, world::World &world,
		world::EntityHandle handle, uint8_t team) {
	world::Entity *entity = world.registry.get(handle);
	if (entity == nullptr || ctx.is_authority == 0) return;  // @0x518D84
	if (entity->team == team) return;                          // @0x518D99..0x518DAA
	entity->team = team;                                       // @0x518DB7
	if (world::AiEntity *ai = world.ai.for_handle(handle)) ai->team = team;
	NapiNPConnection *conn = nullptr;
	if ((entity->flags & world::kEntityFlagPlayer) != 0) {     // @0x518DB0
		for (NapiNPConnection &c : ctx.np_protocol.connection_list)
			if (c.link.owned_entity == handle) conn = &c;      // Entity_ValidatePtr @0x518DC5
	}
	if (conn != nullptr) {
		// The stats record starts over with field 35 = 1, and the script var
		// at +408 clears with the dword [orig: CPlayerStats_ResetAllArrays
		// @0x518DD6 (@0x52C4C0); +408 = 0 @0x518DE5].
		if (world::MatchPlayer *row = world.match.player(handle)) {
			row->stats = world::MatchStats{};
			row->stats[world::MatchStats::kRoundMarker] = 1;
			row->script_vars[16] = 0;
		}
		conn->reply.score_delta_sound_value = 0;               // +332 @0x518DDB
		conn->assigned_team = team;                            // +416 @0x518DEF
		conn->assigned_team_valid = true;
		world.match.set_player_slot_team(handle, team);        // the mirror (D-NET-650)
		// The slot's PlayerTeam var, the entity's signed team byte, and no
		// ClientHostPlayerAdded: the change rides the next ClientHostUpdate
		// [orig: sprintf("%ld", movsx entity+0x162) @0x518DF5..0x518E07,
		//  CNapiVarList_SetOrCreate(PlayerList, slot, "PlayerTeam") @0x518E22].
		conn->player_list_team = static_cast<int8_t>(team);
		// Side A for teams 1/3 or a non-team game, else side B; its character
		// id and avatar become the entity's NetId and animSlot.
		// [orig: @0x518E27..0x518E71, stores @0x518E7E / @0x518E8C]
		const int side = (team == 1 || team == 3 || (ctx.config.game_type & 0x10000u) == 0)
				? 0
				: 1;
		entity->minimap_net_id = conn->char_vars.char_id[side];
		entity->anim_slot = conn->char_vars.avatar[side];
		send_player_state_and_squad(ctx, *conn);               // @0x518E92
		conn->link.armory_reuse_seconds = 0;                   // +356 @0x518E9A
	}
	if (ctx.is_in_session != 0) {
		// S2C 0x50 to every in-match slot (mask 0x80), the identity pair zero
		// for a non-player [orig: @0x518EA5..0x518EE1; NetPacket_WriteEntityHandlePacket
		// @0x506AD0, the Flags & 0x100 gate @0x506B3D].
		TeamAssign assign;
		assign.entity_handle = handle.packed;
		assign.team = team;
		if ((entity->flags & world::kEntityFlagPlayer) != 0) {
			assign.net_id = entity->minimap_net_id;
			assign.anim_slot = entity->anim_slot;
		}
		const std::vector<uint8_t> body = encode_team_assign(assign);
		for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
			if (!is_in_match(c) || c.link.transport == nullptr) continue;
			c.link.transport->host_send(s2c::TEAM_ASSIGN, body);
		}
	}
	// [orig: CBufferList_AddOrFind(g_TeamChangeEntityList, entity) @0x518EEC]
	if (std::find(ctx.team_change_entities.begin(), ctx.team_change_entities.end(), handle) ==
			ctx.team_change_entities.end())
		ctx.team_change_entities.push_back(handle);
}

int32_t deploy_spawn_protection_ticks(int title, const world::Entity *target) {
	if (title == gameprofile::GAME_DFX2 && target != nullptr && target->has_item_def &&
			(target->item_type == 1 || (target->item_attrib & world::kItemAttribSpawnPoint) != 0))
		return 310;
	return 620;
}

world::SpawnPointResult Server_PositionDeployingPlayer(const GameConfig &config,
		NapiNPConnection &conn, world::World &world, world::Entity &player,
		world::EntityHandle target_zone) {
	// The convert's hold arm: no placement; the body warms up where it stands,
	// then the three angle words are zeroed (the mission yaw of heading word 0
	// is 90) and the latch clears. A revive latch the placement would have
	// consumed is only cleared, as the leg's tail clears it on every deploy.
	// [orig: Server_DeployPlayer - the latch test @0x517837,
	//  Entity_WarmUpOrganicAnimation @0x517842, +0x10/+0x14/+0x18 = 0
	//  @0x517849..0x517853, the clear @0x517859; +89932 = 0 @0x5179C7]
	if (conn.reply.convert_holds_pose) {
		conn.reply.convert_holds_pose = false;
		conn.reply.revive_pose_valid = false;
		world::warm_up_player_body(world, player);
		world::SpawnPointResult held;
		held.found = true;
		held.position = player.position;
		held.yaw = static_cast<int16_t>(world::mission_yaw_deg_from_bam_heading(0));
		player.yaw = held.yaw;
		player.pitch = 0;
		player.roll = 0;
		return held;
	}
	// The placement resolves the handle it is handed again, against the player
	// entity's team, whoever resolved it before: a wave release's zone, queued
	// under the team its 0x0E saw, misses once the entity's team has changed.
	// A miss, like no target, takes the no-pick arm, which positions as the slot
	// team (+416), not the entity's (D-NET-582), and reads the slot's spectator
	// latch/restore team beside it (D-NET-592). [orig: Server_DeployPlayer
	//  @0x517863 -> Server_PositionPlayerForSpawn @0x50CF86..0x50CFB1 (the slot
	//  entity pushed @0x50CFA0, the resolve @0x50CFA1, the miss @0x50CFB1),
	//  @0x50D17C..0x50D1C6 (the slot team @0x50D183); the Deaths gate @0x50D1DB,
	//  the markers @0x50D266 / @0x50D320; Server_ResolveSpawnTargetHandle
	//  @0x4FE110, the team test @0x4FE175..0x4FE187]
	const world::Entity *pick =
			world.zones.resolve_spawn_target(player.team, target_zone.packed);
	const world::SpawnSlotState slot_state{
			conn.link.spectator, conn.spectator_restore_team};
	const world::SpawnPointResult pose = world::resolve_player_spawn_pose(
			world, player.handle, pick != nullptr ? pick->handle : world::EntityHandle{},
			conn.reply.player_slot, conn.assigned_team, config.game_type, slot_state);
	if (pose.found) {
		player.position = pose.position;
		player.yaw = pose.yaw;
		player.pitch = pose.pitch;
		player.roll = pose.roll;
	}
	// The scored arm warms the body up after its pick; a pick that found no
	// marker leaves the body where it stands (the spawn-state reset then takes
	// that spot as its spawn origin). [orig: Server_PositionPlayerForSpawn -
	//  Entity_FindBestSpawnPoint @0x50D56C (no marker: no store), the warm-up
	//  @0x50D572; Entity_ResetToSpawnState @0x4B9632..0x4B9644]
	if (pose.warm_up) world::warm_up_player_body(world, player);
	// A medic revive lands the player on the position the revive saved (its
	// death position raised 0x4000) after the placement ran, and warms up there,
	// so the angles stay the placement's: the picked marker's words, or the
	// body's own when nothing was picked (D-NET-377). [orig:
	//  Server_PositionPlayerForSpawn @0x50D60A..0x50D62A, the warm-up @0x50D62D;
	//  GameEvent_RevivePlayer @0x517DCD..0x517E09 saves]
	if (conn.reply.revive_pose_valid) {
		player.position.set_q16({conn.reply.revive_pos[0], conn.reply.revive_pos[1],
				conn.reply.revive_pos[2]});
		world::warm_up_player_body(world, player);
		conn.reply.revive_pose_valid = false;
	}
	return pose;
}

std::vector<ProtocolMessage> Server_ConvertPlayerToSpectator(NapiNPServerCtx &ctx,
		NapiNPConnection &conn, world::World &world, uint16_t request) {
	// The convert's own gates: authority, a live entity, not already a
	// spectator [orig: @0x519E22..0x519E4B].
	if (!ctx.is_authority) return {};
	if (!conn.link.owned_entity.valid() || conn.link.spectator) return {};
	world::Entity *player = world.registry.get(conn.link.owned_entity);
	if (player == nullptr) return {};
	// The convert: latch + hide byte + team 0 + Flags bit 0 + damage -1 +
	// health 1 [orig: @0x519E5B..0x519EA4]; Server_SetPlayerSpectator is that
	// same slot/entity mutation, bar the command group word.
	if (!Server_SetPlayerSpectator(ctx, conn, world, /*spectator=*/true)) return {};
	player->group_id = 0;                                     // entity+0x11C = 0 @0x519E89
	player->health = 1;                                       // entity+286 = 1 @0x519EA4
	world.zones.spawn_waves.remove_player(conn.link.owned_entity); // @0x519EB3
	// A request naming no target (the stock client's 0xFFFF) holds the body
	// in place; any other handle is the deploy's target, which the convert
	// hands on unresolved: the placement resolves it with the cleared team
	// 0, which admits every team's spawn point. [orig: @0x519EBC..0x519EC8,
	//  the raw handle pushed @0x519EC3; Input_HandleActionBinding @0x49B085
	//  sends 0xFFFF; Server_ResolveSpawnTargetHandle @0x4FE175..0x4FE187]
	conn.reply.convert_holds_pose = request == 0xFFFFu;
	// Server_DeployPlayer(player, handle) @0x519ECE: the deploy leg, its
	// raises, placement, reset and private sends, each queued on the slot's
	// connection at its call, so ahead of the 0x32 and of any later message's
	// reply in this receive pump (D-NET-686); a transport-less fixture takes
	// them as the handler's replies.
	// [orig: Server_DeployPlayer - the 0x5A @0x5178F2, the 0x61 (its push 1
	//  @0x51796A, the call @0x51796D), the
	//  0x1E event 58 @0x517A22; NapiNPServer_SendToConn @0x4C4F20 ->
	//  CNapiNPConnection_QueueMessage @0x628640]
	std::vector<ProtocolMessage> deployment = Server_ReleasePlayerDeployment(
			ctx.config, conn, world, world::EntityHandle{request}, request,
			ctx.logs.profile, ctx.np_protocol.identity);
	if (conn.link.transport != nullptr) {
		for (ProtocolMessage &message : deployment)
			conn.link.transport->host_send(message.tag, std::move(message.payload),
					message.reliable, message.flags.raw, message.capacity_exempt,
					message.retention_flushes);
		deployment.clear();
	}
	// S2C 0x32 [u8 5][cstr name], mask 128 [orig: @0x519EDC..0x519F43].
	FormattedGameText text;
	text.subtype = kGameTextPlayerSpectating;
	text.text = conn.reply.player_name;
	const std::vector<uint8_t> body = encode_formatted_game_text(text);
	for (NapiNPConnection &c : ctx.np_protocol.connection_list) {
		if (!is_in_match(c) || c.link.transport == nullptr) continue;
		c.link.transport->host_send(s2c::FORMATTED_GAME_TEXT, body);
	}
	return deployment;
}

// The host side of the Respawn key's request -- see server_spawn.h.
void Server_HandleDeathTimeoutReset(NapiNPServerCtx &ctx, NapiNPConnection &conn,
		world::World &world) {
	if (ctx.is_authority == 0) return; // @0x510542
	// The sender's player slot and its entity [orig: @0x51054E..0x510566].
	if (conn.phase < ConnectionPhase::PlayerAdded) return;
	world::Entity *entity = world.registry.get(conn.link.owned_entity);
	if (entity == nullptr) return;
	entity->health = 0;                                               // +0x11E @0x510568
	entity->last_attacker = world::EntityHandle{};                    // +0x178 @0x51056F
	conn.link.respawn_delay_seconds = ctx.config.respawn_timeout;     // +360 @0x51057B
	conn.link.spawn_target_hold_seconds = ctx.config.respawn_timeout; // +364 @0x510587
	conn.link.downed_revive_seconds = 0;                              // +368 @0x51058D
}

} // namespace opennova::inmatch
