// The slot's PlayerList PlayerTeam / PlayerType (D-NET-659): the in-match writers retail runs
// and the NovaWorld roster sync over them.
//   - the player add stamps the entity's signed team and its class;
//   - a team change rewrites PlayerTeam alone;
//   - the map change's round init re-adds a kept slot, after a side swap and with another
//     class, writing neither;
//   - with no entity an envelope abort draws nothing and writes nothing, and an accept writes
//     no PlayerType;
//   - the loadout's accept stamps the accepted class, and every weapon-slot list (an abort's
//     re-send) the slot's live class;
//   - the roster sync turns a kept player's team or type change into its vars alone, never a
//     ClientHostPlayerAdded, and a slot's new player, or its player's re-join (a new
//     connection), into a removal and an add;
//   - a kept remote slot's round init spawns with the slot's class, its last writer's (the
//     add's join pick, a loadout's accepted class), never the join's pick again (D-NET-677).
// [orig: Server_PlayerAdd @0x51D368 / @0x51D382, the SetOrCreates @0x51D48F / @0x51D4AA;
//  Server_ChangeEntityTeam @0x518DF5..0x518E22; NapiNPServerMsg_HandlePlayerLoadout @0x515AE2,
//  its list sends @0x51581F / @0x515F95 / @0x515FA5; Server_SendWeaponSlotListToPlayer
//  @0x5025A8..0x5025DB; Server_InitAllPlayerEntitiesForRound @0x516AA0 (no SetOrCreate);
//  the 0x2F handler's no-entity return @0x5157E1..0x5157E5]
#include <runtime/inmatch/host_role.h>
#include <runtime/inmatch/map_change.h>
#include <runtime/inmatch/server_initial_state.h>
#include <runtime/inmatch/server_message_dispatch.h>
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_spawn.h>

#include "host_test_setup.h"

#include <runtime/inmatch/loopback_channel.h>

#include <base/gameprofile/game_type.h>
#include <net/novaworld/nwu_host_role.h>
#include <net/npwire/ingame_encode.h>
#include <net/npwire/ingame_message_id.h>
#include <net/npwire/protocol_message.h>

#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/world.h>

#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;
namespace ns = opennova::replication;
using opennova::HostPlayerSlot;

int failures = 0;
void expect(bool cond, const char *what) {
	if (!cond) {
		std::printf("FAIL: %s\n", what);
		++failures;
	}
}

// One 0x2F through the dispatcher; whether it drew a 0x5A.
bool submit_loadout(inmatch::NapiNPServerCtx &ctx, inmatch::NapiNPConnection &conn, w::World &world,
		uint8_t team, uint8_t player_class, uint32_t now) {
	opennova::LoadoutSubmit request;
	request.team = team;
	request.player_class = player_class;
	const std::vector<opennova::ProtocolMessage> replies = inmatch::dispatch_session_replies(
			ctx.config, conn,
			{opennova::make_protocol_message(opennova::c2s::LOADOUT_SUBMIT,
					opennova::encode_loadout_submit(request))},
			now, ctx.np_protocol.connection_list, &world);
	for (const opennova::ProtocolMessage &m : replies)
		if (m.tag == opennova::s2c::WEAPON_LOADOUT) return true;
	return false;
}

void in_match_writers() {
	auto world_storage = std::make_unique<w::World>();
	w::World &world = *world_storage;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(3, 16);
	ns::LoopbackChannel loopback;
	auto role_storage = std::make_unique<inmatch::HostRole>();
	inmatch::NapiNPServerCtx &ctx = role_storage->state.host_owner.ctx;
	inmatch::GameConfig config;
	config.max_players = 2;
	config.num_teams = 2;
	config.game_type = opennova::game_type::kTeamDeathmatch;
	config.armory_reuse_time = 0; // each accept leaves no cooldown for the next 0x2F
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient,
	                             inmatch::SocketMode::Socketless, /*host_key=*/0, &loopback, config);
	ctx.world = &world;
	expect(!ctx.np_protocol.connection_list.empty(), "the host's own connection stands");
	if (ctx.np_protocol.connection_list.empty()) return;
	inmatch::NapiNPConnection &conn = ctx.np_protocol.connection_list[0];
	conn.char_vars.char_class[0] = 8; // side A
	conn.char_vars.char_class[1] = 6; // side B
	inmatch::Server_InitNewRoundState(ctx);
	inmatch::Server_ProcessPendingPlayerSpawns(ctx, world);
	const w::Entity *player = world.registry.get(conn.link.owned_entity);
	expect(conn.phase == inmatch::ConnectionPhase::PlayerAdded && player != nullptr &&
	               player->team == 1 && player->player_class == 8,
	       "the add bound the host's player on team 1, side A's class 8");
	if (player == nullptr) return;

	// The add: the entity's team and class.
	expect(conn.player_list_team == 1 && conn.player_list_type == 8,
	       "the add stamps the entity's team and class");

	// A team change: PlayerTeam alone.
	inmatch::Server_ChangeEntityTeam(ctx, world, conn.link.owned_entity, 2);
	expect(conn.player_list_team == 2 && conn.player_list_type == 8,
	       "a team change rewrites PlayerTeam alone");

	// A map change: the teardown frees the entity and swaps the kept slot's
	// side (2 -> 1), then the round init re-adds it with side A's class, now 5.
	// Neither writes the PlayerList: the slot keeps the previous mission's
	// PlayerTeam 2 and PlayerType 8. Between the two, with no entity, an
	// envelope abort draws nothing and writes nothing.
	world.registry.despawn(conn.link.owned_entity);
	conn.link.owned_entity = {};
	conn.assigned_team = 1;
	conn.char_vars.char_class[0] = 5;
	expect(!submit_loadout(ctx, conn, world, /*team=*/0, /*class=*/0, 100) &&
	               conn.player_list_type == 8,
	       "with no entity an envelope abort draws no 0x5A and keeps PlayerType");
	(void)submit_loadout(ctx, conn, world, /*team=*/1, /*class=*/9, 100);
	expect(conn.player_list_type == 8, "with no entity an accepted 0x2F keeps PlayerType");
	inmatch::init_all_player_entities_for_round(*role_storage);
	player = world.registry.get(conn.link.owned_entity);
	expect(player != nullptr && player->team == 1 && player->player_class == 5,
	       "the round init re-adds the kept slot on team 1 with class 5");
	if (player == nullptr) return;
	expect(conn.player_list_team == 2 && conn.player_list_type == 8,
	       "the round init's re-add writes neither PlayerTeam nor PlayerType");

	// An abort's weapon-slot list: the slot's live class.
	expect(submit_loadout(ctx, conn, world, /*team=*/0, /*class=*/0, 101) &&
	               conn.player_list_type == 5 && conn.player_list_team == 2,
	       "a weapon-slot list re-send stamps the slot's live class");

	// The accept: the accepted class.
	(void)submit_loadout(ctx, conn, world, /*team=*/1, /*class=*/7, 102);
	expect(player->player_class == 7 && conn.player_list_type == 7,
	       "the loadout's accept stamps the accepted class");

	// A team change writes the kept slot's PlayerTeam again.
	inmatch::Server_ChangeEntityTeam(ctx, world, conn.link.owned_entity, 2);
	expect(conn.player_list_team == 2 && player->team == 2, "the next team change writes PlayerTeam");
	inmatch::Server_ChangeEntityTeam(ctx, world, conn.link.owned_entity, 1);
	expect(conn.player_list_team == 1, "and again");
}

// A kept remote slot's class at the round init (D-NET-677): the add stores the join's
// pick (TR ? CTB : CTA, clamped) as the slot's class, a loadout's accept stores the
// accepted class, and the map change's round init spawns the slot's entity with it.
// [orig: Server_PlayerAdd @0x51D09A / @0x51D10E; NapiNPServerMsg_HandlePlayerLoadout
//  @0x515AA8; Server_InitAllPlayerEntitiesForRound @0x516AA0 - the load @0x516B56, the
//  Entity_SpawnFromAnimSlotProperty call @0x516B67]
void remote_slot_class_at_round_init() {
	auto world_storage = std::make_unique<w::World>();
	w::World &world = *world_storage;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(3, 16);
	ns::LoopbackChannel loopback;
	auto role_storage = std::make_unique<inmatch::HostRole>();
	inmatch::NapiNPServerCtx &ctx = role_storage->state.host_owner.ctx;
	inmatch::GameConfig config;
	config.max_players = 4;
	config.num_teams = 2;
	config.game_type = opennova::game_type::kTeamDeathmatch;
	config.armory_reuse_time = 0;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient,
	                             inmatch::SocketMode::Socketless, /*host_key=*/0, &loopback, config);
	ctx.world = &world;
	// Two remote joiners: side A's class 8 and side B's 6, no side request.
	for (uint32_t i = 0; i < 2; ++i) {
		inmatch::NapiNPConnection joining;
		joining.type = inmatch::NapiNPConnection::kTypeServerSide;
		joining.connection_id = inmatch::kFirstJoinerDcb + i;
		joining.self_id_seen = true;
		joining.phase = inmatch::ConnectionPhase::Joined;
		joining.char_vars.char_class[0] = 8;
		joining.char_vars.char_class[1] = 6;
		ctx.np_protocol.connection_list.push_back(joining);
	}
	inmatch::Server_InitNewRoundState(ctx);
	inmatch::Server_ProcessPendingPlayerSpawns(ctx, world);
	std::vector<inmatch::NapiNPConnection *> remotes;
	for (inmatch::NapiNPConnection &c : ctx.np_protocol.connection_list)
		if (c.type == inmatch::NapiNPConnection::kTypeServerSide) remotes.push_back(&c);
	expect(remotes.size() == 2, "two remote joiners");
	if (remotes.size() != 2) return;
	for (inmatch::NapiNPConnection *c : remotes) {
		const w::Entity *body = world.registry.get(c->link.owned_entity);
		expect(body != nullptr && body->player_class == 8 && c->slot_class == 8,
		       "the add spawns the join's pick, side A's class 8, and stores it as the slot's");
	}
	// The first accepts class 7; the second accepts nothing.
	inmatch::NapiNPConnection &accepted = *remotes[0];
	inmatch::NapiNPConnection &kept = *remotes[1];
	(void)submit_loadout(ctx, accepted, world, /*team=*/1, /*class=*/7, 100);
	const w::Entity *body = world.registry.get(accepted.link.owned_entity);
	expect(body != nullptr && body->player_class == 7 && accepted.slot_class == 7,
	       "the accept stores the accepted class as the slot's");
	// The map change: the teardown frees both entities, the round init re-adds both.
	for (inmatch::NapiNPConnection *c : remotes) {
		world.registry.despawn(c->link.owned_entity);
		c->link.owned_entity = {};
	}
	inmatch::init_all_player_entities_for_round(*role_storage);
	body = world.registry.get(accepted.link.owned_entity);
	expect(body != nullptr && body->player_class == 7 && accepted.slot_class == 7,
	       "the round init spawns the accepted slot with its class 7, not the join's 8");
	body = world.registry.get(kept.link.owned_entity);
	expect(body != nullptr && body->player_class == 8 && kept.slot_class == 8,
	       "the round init spawns the other slot with the add's class 8");
}

HostPlayerSlot slot(int number, const char *name, const char *team, const char *type) {
	HostPlayerSlot s;
	s.slot = number;
	s.player_name = name;
	s.ip_and_port = "10.0.0.9:32768";
	s.pcid = "PCID";
	s.team = team;
	s.type = type;
	return s;
}

void roster_sync() {
	std::map<int, HostPlayerSlot> current;
	current[0] = slot(0, "Host", "1", "8");
	current[1] = slot(1, "Joiner", "1", "8");
	current[2] = slot(2, "Leaver", "2", "8");

	// Slot 0's team and slot 1's type change; slot 2 leaves; slot 3 arrives.
	const opennova::RosterSync sync = opennova::plan_roster_sync(current,
			{slot(0, "Host", "2", "8"), slot(1, "Joiner", "1", "5"), slot(3, "New", "1", "8")});
	expect(sync.removed == std::vector<int>{2}, "only the gone slot is removed");
	expect(sync.added.size() == 1 && sync.added[0].slot == 3, "only the new slot is added");
	expect(sync.vars.size() == 2 && sync.vars[0].slot == 0 && sync.vars[0].team == "2" &&
	               sync.vars[1].slot == 1 && sync.vars[1].type == "5",
	       "a kept player's team or type change is its vars alone");

	// A slot now another player's: removed, then added.
	const opennova::RosterSync swap =
			opennova::plan_roster_sync(current, {slot(0, "Host", "1", "8"), slot(1, "Other", "1", "8"),
			                                     slot(2, "Leaver", "2", "8")});
	expect(swap.removed == std::vector<int>{1} && swap.added.size() == 1 &&
	               swap.added[0].player_name == "Other" && swap.vars.empty(),
	       "a slot's new player is a removal and an add");

	// A leave and a re-join of the same player into the same slot between two syncs (same name,
	// address and PCID): the re-join is a new connection, so a removal and an add, as retail's
	// remove and add each send theirs (D-NET-659).
	// [orig: Server_PlayerRemove @0x51b5f3; Server_PlayerAdd @0x51d4b5]
	HostPlayerSlot rejoined = current[1];
	rejoined.connection_id = 7;
	const opennova::RosterSync rejoin =
			opennova::plan_roster_sync(current, { current[0], rejoined, current[2] });
	expect(rejoin.removed == std::vector<int>{ 1 } && rejoin.added.size() == 1 &&
					rejoin.added[0].connection_id == 7 && rejoin.vars.empty(),
			"a slot's re-joined player is a removal and an add");

	// Nothing changed: nothing to do.
	const opennova::RosterSync same = opennova::plan_roster_sync(current,
			{current[0], current[1], current[2]});
	expect(same.removed.empty() && same.added.empty() && same.vars.empty(), "an unchanged roster is left alone");
}

} // namespace

int main() {
	in_match_writers();
	remote_slot_class_at_round_init();
	roster_sync();
	if (failures != 0) {
		std::printf("%d failure(s)\n", failures);
		return 1;
	}
	std::printf("player_list_vars_test OK\n");
	return 0;
}
