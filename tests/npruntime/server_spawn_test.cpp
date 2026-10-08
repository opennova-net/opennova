// P3 — the World-driven host-side player spawn pipeline (server_spawn.{h,cpp}). Proves the §5.2a
// step 1-2 flow spawns the pool-0 player in World (ADR 0012), stamping entity+0x78 ownerConnectionId
// from the connection dcb, for BOTH the host's own type-2 loopback (-> spawn_player, publishes
// cached.local_player at the host dcb 2) and a remote type-1 joiner (-> spawn_remote_player at its
// own dcb, host's local player untouched). End-to-end: build_pool0_organic_batch then carries the
// real dcb at OrganicSpawnRecord::owner_connection_id (the §1 wiring), the F3 self-match field.

#include <runtime/inmatch/server_message_dispatch.h> // Server_ReleasePlayerDeployment
#include <runtime/inmatch/server_session.h>
#include <runtime/inmatch/server_spawn.h>
#include <runtime/inmatch/napi_np_protocol.h>

#include "host_test_setup.h"

#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/inmatch/loopback_channel.h>

#include <net/npwire/ingame_decode.h> // OrganicSpawnRecord / OrganicSpawnBatch

#include <runtime/world/ai.h>
#include <runtime/world/entity.h>
#include <runtime/world/geom.h> // to_fixed
#include <base/gameprofile/game_type.h>
#include <base/gameprofile/game_type.h> // for_mission_mode: the SP/offline g_GameType seed
#include <runtime/world/player_spawn.h> // kPlayerInfantryTypeId
#include <runtime/world/spawn_select.h>
#include <runtime/world/world.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <set>

namespace {
namespace inmatch = opennova::inmatch;
namespace w = opennova::world;
namespace ns = opennova::replication;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// A World with an AiSystem (spawn_player requires it), pools 0 (players) and 3 (markers) configured,
// and one authored 6002 DM fallback marker so the retail resolver finds a real spawn pose.
void make_world(w::World &world) {
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(3, 16);
	w::Entity start;
	start.kind = w::EntityKind::Marker;
	start.item_id = 6002; // non-team fallback player start
	start.position = {123.0f, 456.0f, 7.0f};
	start.yaw = 30;
	world.registry.spawn(3, start);
}

const w::Entity *pool0_player(const w::World &world, uint16_t want_dcb) {
	const w::Entity *found = nullptr;
	world.registry.for_each([&](const w::Entity &e) {
		if (e.handle.pool() == 0 && e.owner_connection_id == want_dcb) found = &e;
	});
	return found;
}
} // namespace

int main() {
	w::World world;
	w::AiSystem &ai = world.ai;
	make_world(world);

	// Stand up the listen host with its own loopback (P0->P1->P2), then wire the authoritative World.
	ns::LoopbackChannel loopback;
	inmatch::NapiNPServerCtx ctx;
	inmatch::GameConfig listen_settings;
	listen_settings.max_players = 8;
	inmatch::test::bring_up_host(ctx, inmatch::ConnectionMode::HostClient, inmatch::SocketMode::Socketless,
	                        /*host_key=*/0, &loopback, listen_settings);
	ctx.world = &world;

	// --- §5.2a step 1-2: spawn the host's OWN player (the type-2 loopback). ---
	inmatch::Server_InitNewRoundState(ctx);
	const int n1 = inmatch::Server_ProcessPendingPlayerSpawns(ctx, world);
	if (!expect(n1 == 1, "host's own player spawned exactly once")) return 1;

	if (!expect(world.cached.local_player.valid(), "cached.local_player published for the host")) return 1;
	const w::Entity *host = world.registry.get(world.cached.local_player);
	if (!expect(host != nullptr, "host player entity resolvable")) return 1;
	if (!expect(host->item_id == w::kPlayerInfantryTypeId, "host player is 0x14B9 infantry")) return 1;
	if (!expect(host->owner_connection_id == inmatch::kHostPlayerDcb,
	            "host player entity+0x78 == kHostPlayerDcb (2), NOT 0")) return 1;
	if (!expect(host->handle.slot() >= inmatch::kRetailPlayerMinEntitySlot,
	            "host player lands at/after the retail player slot")) return 1;
	if (!expect(host->position.x == 123.0f && host->position.y == 456.0f,
	            "host player took the authored start-marker pose, not the origin/an NPC")) return 1;
	// Idempotent: a second pass spawns nothing (phase advanced to PlayerAdded).
	if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(ctx, world) == 0,
	            "re-running ProcessPendingPlayerSpawns is idempotent")) return 1;

	// --- A remote joiner reaches the accepted phase: it spawns as a REMOTE peer, host untouched. ---
	{
		inmatch::NapiNPConnection joiner;
		joiner.type = 1;                       // server-side view of a remote client
		joiner.connection_id = inmatch::kFirstJoinerDcb; // 3, the host-assigned dcb
		joiner.self_id_seen = true;            // accepted (post-0x42 / 0x48)
		joiner.phase = inmatch::ConnectionPhase::Joined;
		ctx.np_protocol.connection_list.push_back(joiner);
	}
	const w::EntityHandle host_handle_before = world.cached.local_player;
	const int n2 = inmatch::Server_ProcessPendingPlayerSpawns(ctx, world);
	if (!expect(n2 == 1, "the remote joiner spawned exactly once")) return 1;
	if (!expect(world.cached.local_player == host_handle_before,
	            "the joiner did NOT republish cached.local_player (host keeps its own)")) return 1;

	const w::Entity *joiner_ent = pool0_player(world, inmatch::kFirstJoinerDcb);
	if (!expect(joiner_ent != nullptr && joiner_ent->owner_connection_id == inmatch::kFirstJoinerDcb,
	            "joiner player entity+0x78 == its dcb (3)")) return 1;
	if (!expect(joiner_ent->handle != world.cached.local_player,
	            "joiner is a distinct pool-0 entity from the host")) return 1;

	// The same authority transition backs the F3 checkbox and host-side tooling:
	// it mutates the real connection/entity/AI state, then respawns through the
	// ordinary marker chain when play resumes.
	inmatch::NapiNPConnection &joiner_conn =
			ctx.np_protocol.connection_list.back();
	const w::EntityHandle joiner_handle = joiner_ent->handle;
	if (!expect(
			inmatch::Server_SetPlayerSpectator(ctx, joiner_conn, world, true),
			"authority enters spectator mode for a live player")) return 1;
	w::Entity *spectator = world.registry.get(joiner_handle);
	w::AiEntity *spectator_ai = world.ai.for_handle(joiner_handle);
	if (!expect(
			joiner_conn.link.spectator && spectator != nullptr &&
			spectator->team == 0 && (spectator->flags & 1u) != 0 &&
			spectator->damage_state == -1 && spectator_ai != nullptr &&
			spectator_ai->team == 0,
			"spectator transition hides and neutralizes the authoritative player")) {
		return 1;
	}
	spectator->position = {900.0f, 901.0f, 902.0f};
	if (!expect(
			inmatch::Server_SetPlayerSpectator(ctx, joiner_conn, world, false),
			"authority returns a spectator to play")) return 1;
	w::Entity *restored = world.registry.get(joiner_handle);
	// Leaving spectator mode is a deploy: the reset never touches entity+292 and
	// the deploy leg re-seeds the 620-tick spawn protection
	// [orig: Server_ProcessPlayerDeath @0x517937].
	if (!expect(
			!joiner_conn.link.spectator && restored != nullptr &&
			restored->team == 1 && (restored->flags & 1u) == 0 &&
			restored->damage_state == 620 && restored->alive &&
			restored->position.x == 123.0f &&
			restored->position.y == 456.0f &&
			spectator_ai->team == 1,
			"leaving spectator mode restores team, body, AI, and spawn pose")) {
		return 1;
	}
	{
		inmatch::NapiNPConnection joining_spectator;
		joining_spectator.type = 1;
		joining_spectator.connection_id = inmatch::kFirstJoinerDcb + 1;
		joining_spectator.self_id_seen = true;
		joining_spectator.phase = inmatch::ConnectionPhase::Joined;
		joining_spectator.link.spectator = true;
		ctx.np_protocol.connection_list.push_back(joining_spectator);
	}
	if (!expect(
			inmatch::Server_ProcessPendingPlayerSpawns(ctx, world) == 1,
			"a newly admitted spectator receives its hidden player record")) return 1;
	const w::Entity *joining_spectator =
			pool0_player(world, inmatch::kFirstJoinerDcb + 1);
	if (!expect(
			joining_spectator != nullptr && joining_spectator->team == 0 &&
			(joining_spectator->flags & 1u) != 0 &&
			joining_spectator->damage_state == -1,
			"spectator admission spawns the retail team-0 hidden entity")) {
		return 1;
	}

	// --- End-to-end §1 wiring: the 0x0C organic batch carries each player's real dcb at entity+0x78. ---
	const opennova::OrganicSpawnBatch batch = ns::build_pool0_organic_batch(world);
	bool saw_host = false, saw_joiner = false;
	for (const opennova::OrganicSpawnRecord &r : batch.records) {
		if (r.owner_connection_id == inmatch::kHostPlayerDcb) saw_host = true;
		if (r.owner_connection_id == inmatch::kFirstJoinerDcb) saw_joiner = true;
	}
	if (!expect(saw_host && saw_joiner,
	            "build_pool0_organic_batch stamps owner_connection_id from owner_connection_id (host 2 + joiner 3)")) return 1;
	// Each player's record carries its own roster slot id at entity+0x154,
	// the byte a client's squad legs read as its own slot [orig:
	// Server_PlayerAdd @0x51d087..0x51d08b; NetPacket_SerializeEntityStatesToBuffer
	// @0x503316..0x503327].
	for (const inmatch::NapiNPConnection &c : ctx.np_protocol.connection_list) {
		const w::Entity *pe = world.registry.get(c.link.owned_entity);
		if (pe == nullptr) continue;
		bool matched = false;
		for (const opennova::OrganicSpawnRecord &r : batch.records)
			if (r.slot_id == c.link.owned_entity.packed)
				matched = r.player_slot_id == c.reply.player_slot && pe->player_slot_id == c.reply.player_slot;
		if (!expect(matched, "a player's 0x0C record carries its roster slot id (entity+0x154)")) return 1;
	}

	// [D-NET-112] Players carry no SSN (net_id 0); they are distinguished by their distinct
	// ownerConnectionId (dcb), the faithful identity. (The old high-band net-id allocator is gone.)
	{
		w::World many_world;
		w::AiSystem &many_ai = many_world.ai;
		make_world(many_world);
		many_world.registry.configure_pool(0, 32);

		inmatch::NapiNPServerCtx many_ctx;
		inmatch::GameConfig settings;
		settings.max_players = 32;
		inmatch::test::bring_up_host(many_ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
		                        /*host_key=*/0, nullptr, settings);
		many_ctx.world = &many_world;
		for (int i = 0; i < 20; ++i) {
			inmatch::NapiNPConnection joiner;
			joiner.type = 1;
			joiner.connection_id = static_cast<uint32_t>(inmatch::kFirstJoinerDcb + i);
			joiner.self_id_seen = true;
			joiner.phase = inmatch::ConnectionPhase::Joined;
			many_ctx.np_protocol.connection_list.push_back(joiner);
		}
		const int spawned = inmatch::Server_ProcessPendingPlayerSpawns(many_ctx, many_world);
		if (!expect(spawned == 20, "twenty remote players spawned")) return 1;

		// [D-NET-112] A player carries NO SSN (net_id 0) — faithful: the original identifies a player by
		// its pool HANDLE + ownerConnectionId(dcb), never an allocated id. Verify every player has net_id
		// 0 and they are distinguished by distinct dcbs (the high-band allocator is gone).
		std::set<uint32_t> dcbs;
		int player_count = 0;
		bool all_ssn_zero = true;
		many_world.registry.for_each([&](const w::Entity &e) {
			if (e.handle.pool() == 0 && e.item_id == w::kPlayerInfantryTypeId) {
				++player_count;
				dcbs.insert(e.owner_connection_id);
				if (e.net_id != 0) all_ssn_zero = false;
			}
		});
		if (!expect(player_count == 20, "twenty players spawned")) return 1;
		if (!expect(all_ssn_zero, "every player carries net_id 0 (no SSN — D-NET-112)")) return 1;
		if (!expect(dcbs.size() == 20, "twenty distinct ownerConnectionId(dcb) — the player identity")) return 1;
	}

	// A roster slot is an identity, not the current player count. When a non-tail player leaves,
	// the next player must reuse that first free slot rather than collide with the surviving tail.
	// Slot 0 is the authority's own row, connectionless on this Serve Only host, so the joiners
	// start at slot 1 (D-NET-350).
	// [orig: Server_PlayerAdd @0x51cbc0 writes a free dword_A87048 player slot; slot 0 stays
	//  active and local from Server_InitNewRoundState @0x51c99a..0x51ca3c]
	{
		w::World slot_world;
		w::AiSystem &slot_ai = slot_world.ai;
		make_world(slot_world);

		inmatch::NapiNPServerCtx slot_ctx;
		inmatch::GameConfig settings;
		settings.max_players = 8;
		inmatch::test::bring_up_host(slot_ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
		                        /*host_key=*/0, nullptr, settings);
		slot_ctx.world = &slot_world;

		const opennova::PeerAddr peers[] = {
				{0x0100007Fu, 33001}, {0x0100007Fu, 33002},
				{0x0100007Fu, 33003}, {0x0100007Fu, 33004}};
		for (int i = 0; i < 3; ++i) {
			inmatch::NapiNPConnection player;
			player.peer = peers[i];
			player.type = 1;
			player.connection_id = inmatch::kFirstJoinerDcb + static_cast<uint32_t>(i);
			player.self_id_seen = true;
			player.phase = inmatch::ConnectionPhase::Joined;
			slot_ctx.np_protocol.connection_list.push_back(std::move(player));
		}
		if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(slot_ctx, slot_world) == 3,
		            "slot reuse fixture spawns three players")) return 1;
		if (!expect(slot_ctx.np_protocol.connection_list[0].reply.player_slot == 1 &&
		                    slot_ctx.np_protocol.connection_list[1].reply.player_slot == 2 &&
		                    slot_ctx.np_protocol.connection_list[2].reply.player_slot == 3,
		            "first three joiners occupy roster slots 1, 2, 3 (slot 0 is the host's)")) return 1;

		if (!expect(inmatch::destroy_connection(slot_ctx, peers[1], nullptr),
		            "non-tail player disconnects")) return 1;
		inmatch::NapiNPConnection replacement;
		replacement.peer = peers[3];
		replacement.type = 1;
		replacement.connection_id = inmatch::kFirstJoinerDcb + 3;
		replacement.self_id_seen = true;
		replacement.phase = inmatch::ConnectionPhase::Joined;
		slot_ctx.np_protocol.connection_list.push_back(std::move(replacement));
		if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(slot_ctx, slot_world) == 1,
		            "replacement player spawns")) return 1;
		if (!expect(slot_ctx.np_protocol.connection_list.back().reply.player_slot == 2,
		            "replacement reuses the first free roster slot instead of colliding with slot 3"))
			return 1;
	}

	// The explicit bind seam may attach an authoritative entity before advancing
	// phase. It still owns its supplied roster row, and the advertised capacity is
	// a hard upper bound on reservations.
	{
		std::vector<inmatch::NapiNPConnection> roster(3);
		roster[0].phase = inmatch::ConnectionPhase::Joined;
		roster[0].reply.player_slot = 0;
		roster[0].link.owned_entity.packed = 1;
		roster[1].reply.player_slot = 1;
		roster[1].reply.player_slot_reserved = true;

		const std::optional<uint8_t> within_capacity =
				inmatch::Server_ReservePlayerSlot(roster, roster[2], 3);
		if (!expect(
					within_capacity.has_value() && *within_capacity == 2,
					"bound entities and pending reservations both occupy roster rows")) {
			return 1;
		}
		roster[2].reply.player_slot_reserved = false;
		if (!expect(
					!inmatch::Server_ReservePlayerSlot(roster, roster[2], 2).has_value(),
					"slot reservation never escapes the advertised capacity")) {
			return 1;
		}
	}

	// Server_AssignPlayerTeam is one policy for every retail mode. Solo modes
	// still store team 1 (otherwise ordinary DM/Flag Me deaths become teamkills),
	// while team-choice is honored only when the mission attribute enables it.
	// [orig: Server_AssignPlayerTeam @0x4FE398..0x4FE400,
	// @0x4FE51A..0x4FE587]
	{
		auto team_world = std::make_unique<w::World>();
		w::AiSystem &team_ai = team_world->ai;
		make_world(*team_world);

		auto reserve_sequence = [&](inmatch::GameConfig config,
				std::initializer_list<uint8_t> requests) {
			std::vector<inmatch::NapiNPConnection> roster;
			std::vector<uint8_t> teams;
			roster.reserve(requests.size());
			for (uint8_t request : requests) {
				roster.emplace_back();
				roster.back().char_vars.team_request = request;
				teams.push_back(inmatch::Server_ReservePlayerTeam(
						config, true, roster, roster.back(), *team_world));
			}
			return teams;
		};

		inmatch::GameConfig solo;
		solo.game_type = opennova::game_type::kDeathmatch;
		if (!expect(reserve_sequence(solo, {0xFF, 0xFF, 0xFF}) ==
					std::vector<uint8_t>({1, 1, 1}),
				"DM keeps every player on retail team 1")) return 1;
		solo.game_type = opennova::game_type::kFlagMe;
		if (!expect(reserve_sequence(solo, {0xFF, 0xFF, 0xFF}) ==
					std::vector<uint8_t>({1, 1, 1}),
				"Flag Me is solo CTF and keeps every player on team 1")) return 1;

		inmatch::GameConfig chosen;
		chosen.game_type = opennova::game_type::kAttackDefend;
		chosen.mp_attributes = inmatch::GameConfig::kMpAttribTeamChoose;
		if (!expect(reserve_sequence(chosen, {1, 0, 0xFF}) ==
					std::vector<uint8_t>({2, 1, 1}),
				"TeamChoose honors side B/A requests before deterministic balance")) return 1;
		chosen.mp_attributes = 0;
		if (!expect(reserve_sequence(chosen, {1, 1}) ==
					std::vector<uint8_t>({1, 2}),
				"team requests are ignored when TeamChoose is disabled")) return 1;
		chosen.side_a_password = "side-a";
		if (!expect(reserve_sequence(chosen, {0xFF}) ==
					std::vector<uint8_t>({2}),
				"an empty JSP selects the only unprotected side")) return 1;
		chosen.side_b_password = "side-b";
		if (!expect(reserve_sequence(chosen, {0xFF}) ==
					std::vector<uint8_t>({0}),
				"an empty JSP cannot select either protected side")) return 1;

		// Four-side setup is intentionally peculiar in retail. With no side
		// passwords, its stable count sort plus mixed-index availability lookup
		// repeatedly chooses teams 1/2; it does not spread an empty lobby across
		// all four advertised columns. Preserve that defect for compatibility.
		// [orig: @0x4FE62C..0x4FE723]
		inmatch::GameConfig four;
		four.game_type = opennova::game_type::kTeamDeathmatch;
		four.num_teams = 4;
		four.mp_attributes = 0;
		if (!expect(reserve_sequence(four, {0xFF, 0xFF, 0xFF, 0xFF}) ==
					std::vector<uint8_t>({1, 2, 1, 2}),
				"retail four-team auto assignment retains its 1/2-only empty-lobby sequence")) return 1;

		std::vector<inmatch::NapiNPConnection> mixed(4);
		for (std::size_t i = 0; i < 3; ++i) {
			mixed[i].assigned_team_valid = true;
			mixed[i].assigned_team = static_cast<uint8_t>(i == 2 ? 4 : i + 1);
		}
		mixed.back().char_vars.team_request = 0xFF;
		if (!expect(inmatch::Server_ReservePlayerTeam(
					four, true, mixed, mixed.back(), *team_world) == 2,
				"four-team mixed-index lookup is reproduced exactly")) return 1;

		four.side_a_password = "side-a";
		four.side_b_password = "side-b";
		if (!expect(reserve_sequence(four, {0xFF}) ==
					std::vector<uint8_t>({3}),
				"four-team password flags feed retail's mixed-index availability lookup")) return 1;

		// A TDM (exactly 0x10000) host with ANY side password forces the add
		// event's preference to automatic before the assignment runs, so a
		// TR for the open side still balances. With two team-2 players
		// present the sort walks [0,2,3,1] over the locked-A availability
		// row and lands on team 3, not the requested side B.
		// [orig: Server_PlayerAdd @0x51CC76..0x51CC91 -> @0x51CF24]
		{
			inmatch::GameConfig tdm_locked;
			tdm_locked.game_type = opennova::game_type::kTeamDeathmatch;
			tdm_locked.num_teams = 4;
			tdm_locked.mp_attributes = inmatch::GameConfig::kMpAttribTeamChoose;
			tdm_locked.side_a_password = "side-a";
			std::vector<inmatch::NapiNPConnection> roster(3);
			for (std::size_t i = 0; i < 2; ++i) {
				roster[i].assigned_team_valid = true;
				roster[i].assigned_team = 2;
			}
			roster.back().char_vars.team_request = 1; // side B, which is open
			if (!expect(inmatch::Server_ReservePlayerTeam(
						tdm_locked, true, roster, roster.back(), *team_world) == 3,
					"TDM with a locked side forces TR to automatic (balance picks team 3)")) return 1;
			// The force is keyed on the exact 0x10000 game type: TKOTH keeps
			// honoring the open-side request under the same passwords.
			inmatch::GameConfig tkoth_locked = tdm_locked;
			tkoth_locked.game_type = opennova::game_type::kTeamKingOfTheHill;
			roster.back().assigned_team_valid = false;
			if (!expect(inmatch::Server_ReservePlayerTeam(
						tkoth_locked, true, roster, roster.back(), *team_world) == 2,
					"non-TDM four-team hosts still honor the open-side TR")) return 1;
		}

		// The side compares see the add event's 16-character copy of the JSP
		// (Napi_CopyString(.., 17)); a longer credential sharing the 16-char
		// prefix of a 16-char side password selects that side.
		// [orig: Server_BuildPlayerInfoAndAdd @0x51D686 -> Server_AssignPlayerTeam @0x4FE43F]
		{
			inmatch::GameConfig prefix;
			prefix.game_type = opennova::game_type::kAttackDefend;
			prefix.side_a_password = "abcdefghijklmnop"; // 16 chars, the field's full width
			std::vector<inmatch::NapiNPConnection> roster(1);
			roster.back().char_vars.team_request = 0xFF;
			roster.back().join_password = "abcdefghijklmnopQRS";
			if (!expect(inmatch::Server_ReservePlayerTeam(
						prefix, true, roster, roster.back(), *team_world) == 1,
					"a JSP matching the 16-char truncation selects the locked side")) return 1;
			roster.back().assigned_team_valid = false;
			roster.back().join_password = "abcdefghijklmnoZ"; // differs inside the window
			if (!expect(inmatch::Server_ReservePlayerTeam(
						prefix, true, roster, roster.back(), *team_world) == 2,
					"a JSP differing inside the 16-char window falls to the open side")) return 1;
		}
	}

	// --- D-NET-146: the character stamp — per-side CU vars picked by ASSIGNED team. ---
	// A team-based session (golden ASH_I5A gameType 0x10010): the host's own player takes its
	// installed profile's side-A values on the local path [orig: Player_InitPlayer @0x4e15f0 <-
	// g_AvatarTeam1/2 + g_CharClassTeam1/2; PlayerSession_InitFromProfile @0x50ca80]; the team-2
	// joiner takes its uploaded SIDE-B values (VCB/CI1) and its playerClass from the TR pick
	// [orig: Server_PlayerAdd @0x51cbc0 @0x51cff7/@0x51d0b1]. The 0x0C organic batch echoes
	// entity+0x374 / entity+0x15C raw [orig: NetPacket_SerializeEntityStatesToBuffer @0x5030a0].
	{
		w::World cw;
		w::AiSystem &cai = cw.ai;
		make_world(cw);

		ns::LoopbackChannel cloop;
		inmatch::NapiNPServerCtx cctx;
		inmatch::GameConfig settings;
		settings.max_players = 8;
		settings.game_type = 0x10010; // golden ASH_I5A session gameType (bit 0x10000 = team-based)
		inmatch::test::bring_up_host(cctx, inmatch::ConnectionMode::HostClient, inmatch::SocketMode::Socketless,
		                        /*host_key=*/0, &cloop, settings);
		cctx.world = &cw;
		// A non-default local profile is installed on the type-2 loopback before the
		// spawn (start_host_session does it from HostConfig.local_character_vars); the
		// host's own player is stamped from it by ASSIGNED side like a joiner's upload —
		// otherwise the listen host would always be the stock 0x0200 character.
		inmatch::NapiNPConnection &host_conn = cctx.np_protocol.connection_list.front();
		host_conn.char_vars.char_id[0] = 0x0400;
		host_conn.char_vars.char_id[1] = 0x8407;
		host_conn.char_vars.char_class[0] = 6;
		host_conn.char_vars.char_class[1] = 7;
		host_conn.char_vars.avatar[0] = 3;
		host_conn.char_vars.avatar[1] = 9;

		// Host own player first (team 1 by autobalance).
		inmatch::Server_InitNewRoundState(cctx);
		if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(cctx, cw) == 1, "char-stamp: host spawned")) return 1;

		// The joiner: golden CU var set (side A 1/0x0200 class 8, side B 4/0x8207 class 5, TR auto).
		{
			inmatch::NapiNPConnection joiner;
			joiner.type = 1;
			joiner.connection_id = inmatch::kFirstJoinerDcb;
			joiner.self_id_seen = true;
			joiner.phase = inmatch::ConnectionPhase::Joined;
			joiner.char_vars.char_id[0] = 0x0200;
			joiner.char_vars.char_id[1] = 0x8207;
			joiner.char_vars.team_request = 0xFF; // auto -> the class pick takes side B (CTB)
			joiner.char_vars.char_class[0] = 8;
			joiner.char_vars.char_class[1] = 5;
			joiner.char_vars.avatar[0] = 1;
			joiner.char_vars.avatar[1] = 4;
			cctx.np_protocol.connection_list.push_back(joiner);
		}
		if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(cctx, cw) == 1, "char-stamp: joiner spawned")) return 1;

		const w::Entity *chost = pool0_player(cw, inmatch::kHostPlayerDcb);
		const w::Entity *cjoin = pool0_player(cw, inmatch::kFirstJoinerDcb);
		if (!expect(chost != nullptr && cjoin != nullptr, "char-stamp: both players resolvable")) return 1;
		if (!expect(chost->team == 1 && cjoin->team == 2, "char-stamp: host team 1, joiner team 2")) return 1;
		if (!expect(chost->anim_slot == 3, "host animSlot = selected side-A avatar")) return 1;
		if (!expect(chost->minimap_net_id == 0x0400,
		            "host NetId = selected side-A packed character id")) return 1;
		if (!expect(chost->player_class == 6,
		            "host playerClass = the assigned side's class byte (g_charClassTeam1)")) return 1;
		if (!expect(cjoin->anim_slot == 4, "team-2 joiner animSlot = side-B avatar (VCB=4, golden)")) return 1;
		if (!expect(cjoin->minimap_net_id == 0x8207, "team-2 joiner NetId = side-B char id (CI1=0x8207)")) return 1;
		if (!expect(cjoin->player_class == 5, "joiner playerClass = TR-picked CTB (in [5,9], kept)")) return 1;
		if (!expect(cjoin->net_id == 0, "the SSN stays 0 (D-NET-112) — minimap_net_id is a separate field")) return 1;

		const opennova::OrganicSpawnBatch cbatch = ns::build_pool0_organic_batch(cw);
		bool host_rec_ok = false, join_rec_ok = false;
		for (const opennova::OrganicSpawnRecord &r : cbatch.records) {
			if (r.owner_connection_id == inmatch::kHostPlayerDcb)
				host_rec_ok = (r.anim_slot == 3 && r.net_id == 0x0400);
			if (r.owner_connection_id == inmatch::kFirstJoinerDcb)
				join_rec_ok = (r.anim_slot == 4 && r.net_id == 0x8207 && r.player_class == 5);
		}
		if (!expect(host_rec_ok, "0x0C host record echoes the selected local character")) return 1;
		if (!expect(join_rec_ok, "0x0C joiner record: animSlot 4 + netId 0x8207 + class 5 (golden shape)")) return 1;

		// A var-less joiner (no CU tags): animSlot stays the retail raw 0, class defaults to 8,
		// and the netId falls back to the D-NET-137 encoding shim (nonzero).
		{
			inmatch::NapiNPConnection bare;
			bare.type = 1;
			bare.connection_id = inmatch::kFirstJoinerDcb + 1;
			bare.self_id_seen = true;
			bare.phase = inmatch::ConnectionPhase::Joined;
			cctx.np_protocol.connection_list.push_back(bare);
		}
		if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(cctx, cw) == 1, "char-stamp: bare joiner spawned")) return 1;
		const w::Entity *cbare = pool0_player(cw, inmatch::kFirstJoinerDcb + 1);
		if (!expect(cbare != nullptr && cbare->anim_slot == 0 && cbare->minimap_net_id == 0,
		            "var-less joiner: animSlot 0 (tag absent), no char id")) return 1;
		if (!expect(cbare->player_class == 8, "var-less joiner: playerClass defaults 8 in-session")) return 1;
		const opennova::OrganicSpawnBatch bbatch = ns::build_pool0_organic_batch(cw);
		for (const opennova::OrganicSpawnRecord &r : bbatch.records) {
			if (r.owner_connection_id == inmatch::kFirstJoinerDcb + 1) {
				if (!expect(r.net_id != 0, "var-less joiner netId falls back to the encoder shim")) return 1;
			}
		}
	}

	// The standalone SP host: a campaign mission authors ONLY the Co-op 6001 start (every
	// retail 00TR*/CP* .bms), and retail's spawn chain reaches it only through the stock
	// Co-op g_GameType word (0x10020) the mission's attrib mode implies — the word the SP
	// bring-up must seed into GameConfig before the auto-spawn. A GameConfig left at the
	// default 0 walks the DM 6095/6002 chain, finds nothing, and parks the host player at
	// the origin (the post-#564 SP spawn regression). [orig: AI_GetTaskTypeFromFlags
	// @0x40DAE0 -> Game_StartMission @0x524360; Server_PositionPlayerForSpawn @0x50CF60]
	{
		auto sp_spawn_position = [](uint32_t game_type, w::Vec3 &out) {
			auto sp_world = std::make_unique<w::World>();
			sp_world->registry.configure_pool(0, 16);
			sp_world->registry.configure_pool(3, 16);
			w::Entity start;
			start.kind = w::EntityKind::Marker;
			start.item_id = 6001; // the Co-op fallback start, 00TRa's only player start
			start.position = {297.81f, -409.12f, 27.14f};
			start.yaw = 45;
			sp_world->registry.spawn(3, start);

			ns::LoopbackChannel sp_loop;
			inmatch::NapiNPServerCtx sp_ctx;
			inmatch::GameConfig sp_settings;
			sp_settings.server_name = "SINGLEPLAYERGAME";
			sp_settings.max_players = 1;
			sp_settings.game_type = game_type;
			inmatch::test::bring_up_host(sp_ctx, inmatch::ConnectionMode::HostClient, inmatch::SocketMode::Socketless,
			                        /*host_key=*/0, &sp_loop, sp_settings);
			sp_ctx.world = sp_world.get();
			inmatch::Server_InitNewRoundState(sp_ctx);
			if (inmatch::Server_ProcessPendingPlayerSpawns(sp_ctx, *sp_world) != 1) return false;
			const w::Entity *sp_player = sp_world->registry.get(sp_world->cached.local_player);
			if (sp_player == nullptr) return false;
			out = sp_player->position;
			return true;
		};
		w::Vec3 seeded{};
		if (!expect(sp_spawn_position(opennova::game_type::for_mission_mode(0), seeded),
		            "SP host spawns under the mission-derived stock Co-op word")) return 1;
		if (!expect(seeded.x == 297.81f && seeded.y == -409.12f,
		            "stock Co-op word (0x10020) reaches the 6001 start marker")) return 1;
		w::Vec3 unseeded{};
		if (!expect(sp_spawn_position(0, unseeded),
		            "SP host still spawns under a default (0) game type")) return 1;
		if (!expect(unseeded.x == 0.0f && unseeded.y == 0.0f,
		            "the default 0 word walks the DM 6095/6002 chain past 6001 and lands at the origin")) return 1;
	}

	// D-NET-376: the host's own player takes the start marker's heading WORD. JO:CA
	// CP01's 6001 start is authored at -197; retail's process holds 0xCC160000 on the
	// marker, the spawned player's +0x10 and the look yaw (163 turned exactly would be
	// 0xCC16C16C). [orig: Server_PositionPlayerForSpawn @0x50D3F7;
	// Entity_SpawnFromBMSRecord @0x40EB42..0x40EB66]
	{
		auto cp01_world = std::make_unique<w::World>();
		cp01_world->registry.configure_pool(0, 16);
		cp01_world->registry.configure_pool(3, 16);
		w::Entity start;
		start.kind = w::EntityKind::Marker;
		start.item_id = 6001;
		start.position = {-534.9119f, -139.3639f, 25.5f};
		start.yaw = -197;
		cp01_world->registry.spawn(3, start);

		ns::LoopbackChannel cp01_loop;
		inmatch::NapiNPServerCtx cp01_ctx;
		inmatch::GameConfig cp01_settings;
		cp01_settings.server_name = "SINGLEPLAYERGAME";
		cp01_settings.max_players = 1;
		cp01_settings.game_type = 0x30020u; // CP01's word, read from retail's process
		inmatch::test::bring_up_host(cp01_ctx, inmatch::ConnectionMode::HostClient,
		                        inmatch::SocketMode::Socketless, /*host_key=*/0, &cp01_loop,
		                        cp01_settings);
		cp01_ctx.world = cp01_world.get();
		inmatch::Server_InitNewRoundState(cp01_ctx);
		if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(cp01_ctx, *cp01_world) == 1,
		            "the CP01 host player spawns")) return 1;
		const w::AiEntity *motor = cp01_world->ai.for_handle(cp01_world->cached.local_player);
		if (!expect(motor != nullptr && static_cast<uint32_t>(motor->heading) == 0xCC160000u,
		            "the host player's motor heading is the marker's word 0xCC160000")) return 1;
		const w::Entity *player = cp01_world->registry.get(cp01_world->cached.local_player);
		if (!expect(player != nullptr && player->yaw == -197,
		            "the player's whole-degree mirror stays the record's")) return 1;

		// D-NET-377: a medic revive's deploy restores ONLY the saved position. The
		// placement arm has already copied the marker's words onto +0x10..+0x18, so the
		// revived player faces the start marker's way, not the way its body died; the
		// yaw/pitch/roll the revive saved beside the position have no reader.
		// [orig: Server_PositionPlayerForSpawn @0x50D60A..0x50D62A (the position
		//  only), @0x50D637..0x50D655 (all six words zeroed); GameEvent_RevivePlayer
		//  @0x517DCD..0x517E09 (the save)]
		inmatch::NapiNPConnection &own = cp01_ctx.np_protocol.connection_list.front();
		w::Entity *body = cp01_world->registry.get(cp01_world->cached.local_player);
		w::AiEntity *body_motor = cp01_world->ai.for_handle(cp01_world->cached.local_player);
		if (!expect(own.link.owned_entity == cp01_world->cached.local_player &&
		                    body != nullptr && body_motor != nullptr,
		            "the host's own connection owns the CP01 player")) return 1;
		// The body died 30 units away facing elsewhere; the revive saved its spot.
		body->position = {-504.9119f, -139.3639f, 26.0f};
		body->yaw = 10;
		body->pitch = 7;
		body_motor->heading = 0x12340000;
		body->alive = false;
		body->health = 0;
		body->flags |= w::kEntityFlagDead;
		own.reply.revive_pose_valid = true;
		own.reply.revive_pos[0] = w::to_fixed(body->position.x);
		own.reply.revive_pos[1] = w::to_fixed(body->position.y);
		own.reply.revive_pos[2] = w::to_fixed(body->position.z) + 0x4000;
		inmatch::Server_ReleasePlayerDeployment(
				cp01_ctx.config, own, *cp01_world, w::EntityHandle{});
		if (!expect(body->alive && !own.reply.revive_pose_valid &&
		                    body->position.x == -504.9119f && body->position.z == 26.25f,
		            "the revive deploy lands on the saved spot raised 0x4000")) return 1;
		if (!expect(static_cast<uint32_t>(body_motor->heading) == 0xCC160000u &&
		                    body->yaw == -197 && body->pitch == 0,
		            "the revived player keeps the start marker's facing, not the body's")) return 1;
	}

	// D-NET-377's no-pick arm: with no marker to place on, nothing writes +0x10, so
	// a revived player keeps the heading word its body held.
	// [orig: Server_PositionPlayerForSpawn — no arm stores +0x10 before @0x50D57A]
	{
		auto bare_world = std::make_unique<w::World>();
		bare_world->registry.configure_pool(0, 16);
		bare_world->registry.configure_pool(3, 16);
		ns::LoopbackChannel bare_loop;
		inmatch::NapiNPServerCtx bare_ctx;
		inmatch::GameConfig bare_settings;
		bare_settings.server_name = "SINGLEPLAYERGAME";
		bare_settings.max_players = 1;
		bare_settings.game_type = 0x30020u;
		inmatch::test::bring_up_host(bare_ctx, inmatch::ConnectionMode::HostClient,
		                        inmatch::SocketMode::Socketless, /*host_key=*/0, &bare_loop,
		                        bare_settings);
		bare_ctx.world = bare_world.get();
		inmatch::Server_InitNewRoundState(bare_ctx);
		if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(bare_ctx, *bare_world) == 1,
		            "the marker-less host player spawns")) return 1;
		inmatch::NapiNPConnection &own = bare_ctx.np_protocol.connection_list.front();
		w::Entity *body = bare_world->registry.get(bare_world->cached.local_player);
		w::AiEntity *body_motor = bare_world->ai.for_handle(bare_world->cached.local_player);
		if (!expect(body != nullptr && body_motor != nullptr,
		            "the marker-less player has a body and a motor")) return 1;
		body_motor->heading = 0x12340000;
		body->yaw = 10;
		body->alive = false;
		body->health = 0;
		body->flags |= w::kEntityFlagDead;
		own.reply.revive_pose_valid = true;
		own.reply.revive_pos[0] = 5 << 16;
		own.reply.revive_pos[1] = 6 << 16;
		own.reply.revive_pos[2] = (7 << 16) + 0x4000;
		inmatch::Server_ReleasePlayerDeployment(
				bare_ctx.config, own, *bare_world, w::EntityHandle{});
		if (!expect(body->alive && body->position.x == 5.0f && body->position.z == 7.25f &&
		                    static_cast<uint32_t>(body_motor->heading) == 0x12340000u,
		            "with no placement the revived player keeps its body's heading word")) return 1;
	}

	// A join-time spectator is POSITIONED with the substitute team while its
	// assigned team stays 0: in a team mode the tick parity (odd -> team 1,
	// even -> team 2) selects the base marker, so the hidden body lands on a
	// real start marker instead of the mission origin.
	// [orig: Server_OnPlayerJoin @0x51A786 -> Server_PositionPlayerForSpawn
	//  @0x50D17C..0x50D1C6; the latch is slot+100567 from Server_PlayerAdd @0x51CD83]
	{
		auto team_world = std::make_unique<w::World>();
		team_world->registry.configure_pool(0, 16);
		team_world->registry.configure_pool(3, 16);
		auto add_marker = [&](int32_t item_id, float x) {
			w::Entity start;
			start.kind = w::EntityKind::Marker;
			start.item_id = item_id;
			start.position = {x, 0.0f, 0.0f};
			team_world->registry.spawn(3, start);
		};
		add_marker(6096, 10.0f);
		add_marker(6097, 20.0f);

		inmatch::NapiNPServerCtx team_ctx;
		inmatch::GameConfig settings;
		settings.max_players = 8;
		settings.game_type = opennova::game_type::kTeamDeathmatch;
		inmatch::test::bring_up_host(team_ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
		                        /*host_key=*/0, nullptr, settings);
		team_ctx.world = team_world.get();
		auto admit_spectator = [&](uint32_t dcb) {
			inmatch::NapiNPConnection joining;
			joining.type = 1;
			joining.connection_id = dcb;
			joining.self_id_seen = true;
			joining.phase = inmatch::ConnectionPhase::Joined;
			joining.link.spectator = true;
			team_ctx.np_protocol.connection_list.push_back(joining);
		};
		team_world->logic_tick = 1;
		admit_spectator(inmatch::kFirstJoinerDcb);
		if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(team_ctx, *team_world) == 1,
		            "odd-tick spectator admission spawns")) return 1;
		const w::Entity *odd = pool0_player(*team_world, inmatch::kFirstJoinerDcb);
		if (!expect(odd != nullptr && odd->team == 0 && odd->position.x == 10.0f,
		            "an odd-tick team-mode spectator positions on the team-1 base and keeps team 0")) return 1;
		team_world->logic_tick = 2;
		admit_spectator(inmatch::kFirstJoinerDcb + 1);
		if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(team_ctx, *team_world) == 1,
		            "even-tick spectator admission spawns")) return 1;
		const w::Entity *even = pool0_player(*team_world, inmatch::kFirstJoinerDcb + 1);
		if (!expect(even != nullptr && even->team == 0 && even->position.x == 20.0f,
		            "an even-tick team-mode spectator positions on the team-2 base and keeps team 0")) return 1;
	}

	// A Co-op join onto a team-2 start marker parented to a carrier arms the
	// queued 0x200 mount (+0x16C/+0x180 = the parent) and copies the marker's
	// chute bit; the seat attach itself is the body update's 0x200 toggle, so
	// the fresh player is positioned on the transformed marker, not mounted.
	// [orig: Server_PositionPlayerForSpawn @0x50D406..0x50D45A;
	//  consumer Entity_UpdateInfantryPlayerBody @0x4B424A..0x4B4272]
	{
		auto coop_world = std::make_unique<w::World>();
		coop_world->registry.configure_pool(0, 16);
		coop_world->registry.configure_pool(1, 4);
		coop_world->registry.configure_pool(3, 16);
		w::Entity carrier;
		carrier.kind = w::EntityKind::Item;
		carrier.position = {100.0f, 50.0f, 10.0f};
		const w::EntityHandle carrier_handle = coop_world->registry.spawn(1, carrier);
		w::Entity start;
		start.kind = w::EntityKind::Marker;
		start.item_id = 6094;
		start.has_item_def = true;
		start.team = 2;
		start.position = {1.0f, 0.0f, 2.0f};
		start.yaw = 90;
		start.flags = w::kEntityFlagParachute;
		start.ground_target = carrier_handle;
		coop_world->registry.spawn(3, start);

		inmatch::NapiNPServerCtx coop_ctx;
		inmatch::GameConfig settings;
		settings.max_players = 8;
		settings.game_type = opennova::game_type::for_mission_mode(0); // stock Co-op 0x10020
		inmatch::test::bring_up_host(coop_ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
		                        /*host_key=*/0, nullptr, settings);
		coop_ctx.world = coop_world.get();
		inmatch::NapiNPConnection joiner;
		joiner.type = 1;
		joiner.connection_id = inmatch::kFirstJoinerDcb;
		joiner.self_id_seen = true;
		joiner.phase = inmatch::ConnectionPhase::Joined;
		coop_ctx.np_protocol.connection_list.push_back(joiner);
		if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(coop_ctx, *coop_world) == 1,
		            "Co-op carrier-marker joiner spawns")) return 1;
		const w::Entity *rider = pool0_player(*coop_world, inmatch::kFirstJoinerDcb);
		if (!expect(rider != nullptr &&
		                    (rider->flags & 0x200u) != 0 &&
		                    (rider->flags & w::kEntityFlagParachute) != 0,
		            "a team-2 parented Co-op marker latches Flags 0x200 and the marker's chute bit")) return 1;
		if (!expect(rider->mount_target == carrier_handle &&
		                    rider->mount_toggle_fallback == carrier_handle && !rider->mounted,
		            "the carrier lands in +0x16C/+0x180 without flipping the mounted state")) return 1;
		if (!expect(std::fabs(rider->position.x - 100.0f) < 1e-3f &&
		                    std::fabs(rider->position.y - 51.0f) < 1e-3f,
		            "the join pose is the parent-transformed marker")) return 1;
	}

	// The balance-join hold: with a side password armed (BuildFlags & 0xF0) and
	// `balance_join`, a joiner REQUESTING side A is refused while the add would
	// push side A more than max(1, ceil(min * percent)) ahead; the refusal
	// stages the once-per-second S2C 0x03 nag, and a later side-B admission
	// releases the hold. [orig: CNapiServer_ProcessPendingPlayerSpawns
	//  @0x4C8E89..0x4C8F74, nag @0x4C8F7A..0x4C8FD1]
	{
		auto bal_world = std::make_unique<w::World>();
		make_world(*bal_world);
		for (const int32_t item_id : {6096, 6097}) {
			w::Entity start;
			start.kind = w::EntityKind::Marker;
			start.item_id = item_id;
			bal_world->registry.spawn(3, start);
		}
		inmatch::NapiNPServerCtx bal_ctx;
		inmatch::GameConfig settings;
		settings.max_players = 8;
		settings.game_type = opennova::game_type::kTeamDeathmatch;
		// A locked side B arms BuildFlags 0x10; unpassworded joiners are
		// assigned the open side A, a "bravo" credential lands on side B.
		settings.side_b_password = "bravo";
		settings.balance_join = true;
		settings.balance_join_percent = 0.5f;
		inmatch::test::bring_up_host(bal_ctx, inmatch::ConnectionMode::HostOnly, inmatch::SocketMode::Lan,
		                        /*host_key=*/0, nullptr, settings);
		bal_ctx.world = bal_world.get();
		if (!expect((bal_ctx.np_protocol.build_flags & 0xF0u) != 0,
		            "balance: a side password arms the team-mode BuildFlags nibble")) return 1;
		auto admit = [&](uint32_t dcb, uint8_t team_request, const char *password) {
			inmatch::NapiNPConnection joining;
			joining.type = 1;
			joining.connection_id = dcb;
			joining.self_id_seen = true;
			joining.phase = inmatch::ConnectionPhase::Joined;
			joining.char_vars.team_request = team_request;
			joining.join_password = password;
			bal_ctx.np_protocol.connection_list.push_back(joining);
		};
		admit(inmatch::kFirstJoinerDcb, 0, "");
		if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(bal_ctx, *bal_world) == 1,
		            "balance: the first side-A request is admitted (diff 1 <= 1)")) return 1;
		admit(inmatch::kFirstJoinerDcb + 1, 0, "");
		if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(bal_ctx, *bal_world) == 0,
		            "balance: a second side-A request is held (2 vs 0 > 1)")) return 1;
		const inmatch::NapiNPConnection &held = bal_ctx.np_protocol.connection_list.back();
		if (!expect(held.phase < inmatch::ConnectionPhase::PlayerAdded &&
		                    held.reply.admission_hold_nag_pending,
		            "balance: the held joiner is staged its S2C 0x03 nag")) return 1;
		admit(inmatch::kFirstJoinerDcb + 2, 0xFF, "bravo");
		// The side-B credential selects team 1 (retail's index) for the balance
		// check and lands on side B; its add restarts the walk: 1 vs 1 -> max
		// diff 1 -> the held side-A joiner now fits (2 vs 1).
		if (!expect(inmatch::Server_ProcessPendingPlayerSpawns(bal_ctx, *bal_world) == 2,
		            "balance: a side-B admission releases the held side-A joiner")) return 1;
		const w::Entity *released = pool0_player(*bal_world, inmatch::kFirstJoinerDcb + 1);
		if (!expect(released != nullptr && released->team == 1,
		            "balance: the released joiner spawns on its requested side")) return 1;
	}

	std::printf("OK\n");
	return 0;
}
