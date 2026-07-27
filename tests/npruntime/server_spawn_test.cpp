// P3 — the World-driven host-side player spawn pipeline (server_spawn.{h,cpp}). Proves the §5.2a
// step 1-2 flow spawns the pool-0 player in World (ADR 0012), stamping entity+0x78 ownerConnectionId
// from the connection dcb, for BOTH the host's own type-2 loopback (-> spawn_player, publishes
// cached.local_player at the host dcb 2) and a remote type-1 joiner (-> spawn_remote_player at its
// own dcb, host's local player untouched). End-to-end: build_pool0_organic_batch then carries the
// real dcb at OrganicSpawnRecord::entity_flags (the §1 wiring), the F3 self-match field.

#include <npruntime/server_session.h>
#include <npruntime/server_spawn.h>
#include <npruntime/napi_np_protocol.h>

#include "host_test_setup.h"

#include <netsim/entity_wire_bridge.h>
#include <netsim/loopback_channel.h>

#include <npwire/ingame_decode.h> // OrganicSpawnRecord / OrganicSpawnBatch

#include <world/ai.h>
#include <world/entity.h>
#include <world/player_spawn.h> // kPlayerInfantryTypeId
#include <world/spawn_select.h>
#include <world/world.h>

#include <cstdio>
#include <set>

namespace {
namespace np = opennova::np;
namespace w = opennova::world;
namespace ns = opennova::netsim;

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

// A World with an AiSystem (spawn_player requires it), pools 0 (players) and 3 (markers) configured,
// and one authored 6002 SP start marker so select_player_spawn finds a real spawn pose.
void make_world(w::World &world, w::AiSystem &ai) {
	world.ai = &ai;
	world.registry.configure_pool(0, 16);
	world.registry.configure_pool(3, 16);
	w::Entity start;
	start.kind = w::EntityKind::Marker;
	start.item_id = 6002; // SP/DM player-start (kSpawnMarkerStartTypes[0])
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
	w::AiSystem ai;
	make_world(world, ai);

	// Stand up the listen host with its own loopback (P0->P1->P2), then wire the authoritative World.
	ns::LoopbackChannel loopback;
	np::NapiNPServerCtx ctx;
	np::GameConfig listen_settings;
	listen_settings.max_players = 8;
	np::test::bring_up_host(ctx, np::ConnectionMode::HostClient, np::SocketMode::Socketless,
	                        /*host_key=*/0, &loopback, listen_settings);
	ctx.world = &world;

	// --- §5.2a step 1-2: spawn the host's OWN player (the type-2 loopback). ---
	np::Server_InitNewRoundState(ctx);
	const int n1 = np::Server_ProcessPendingPlayerSpawns(ctx, world);
	if (!expect(n1 == 1, "host's own player spawned exactly once")) return 1;

	if (!expect(world.cached.local_player.valid(), "cached.local_player published for the host")) return 1;
	const w::Entity *host = world.registry.get(world.cached.local_player);
	if (!expect(host != nullptr, "host player entity resolvable")) return 1;
	if (!expect(host->item_id == w::kPlayerInfantryTypeId, "host player is 0x14B9 infantry")) return 1;
	if (!expect(host->owner_connection_id == np::kHostPlayerDcb,
	            "host player entity+0x78 == kHostPlayerDcb (2), NOT 0")) return 1;
	if (!expect(host->handle.slot() >= np::kRetailPlayerMinEntitySlot,
	            "host player lands at/after the retail player slot")) return 1;
	if (!expect(host->position.x == 123.0f && host->position.y == 456.0f,
	            "host player took the authored start-marker pose, not the origin/an NPC")) return 1;
	// Idempotent: a second pass spawns nothing (phase advanced to PlayerAdded).
	if (!expect(np::Server_ProcessPendingPlayerSpawns(ctx, world) == 0,
	            "re-running ProcessPendingPlayerSpawns is idempotent")) return 1;

	// --- A remote joiner reaches the accepted phase: it spawns as a REMOTE peer, host untouched. ---
	{
		np::NapiNPConnection joiner;
		joiner.type = 1;                       // server-side view of a remote client
		joiner.connection_id = np::kFirstJoinerDcb; // 3, the host-assigned dcb
		joiner.self_id_seen = true;            // accepted (post-0x42 / 0x48)
		joiner.phase = np::ConnectionPhase::Joined;
		ctx.np_protocol.connection_list.push_back(joiner);
	}
	const w::EntityHandle host_handle_before = world.cached.local_player;
	const int n2 = np::Server_ProcessPendingPlayerSpawns(ctx, world);
	if (!expect(n2 == 1, "the remote joiner spawned exactly once")) return 1;
	if (!expect(world.cached.local_player == host_handle_before,
	            "the joiner did NOT republish cached.local_player (host keeps its own)")) return 1;

	const w::Entity *joiner_ent = pool0_player(world, np::kFirstJoinerDcb);
	if (!expect(joiner_ent != nullptr && joiner_ent->owner_connection_id == np::kFirstJoinerDcb,
	            "joiner player entity+0x78 == its dcb (3)")) return 1;
	if (!expect(joiner_ent->handle != world.cached.local_player,
	            "joiner is a distinct pool-0 entity from the host")) return 1;

	// --- End-to-end §1 wiring: the 0x0C organic batch carries each player's real dcb at entity+0x78. ---
	const opennova::OrganicSpawnBatch batch = ns::build_pool0_organic_batch(world);
	bool saw_host = false, saw_joiner = false;
	for (const opennova::OrganicSpawnRecord &r : batch.records) {
		if (r.entity_flags == np::kHostPlayerDcb) saw_host = true;
		if (r.entity_flags == np::kFirstJoinerDcb) saw_joiner = true;
	}
	if (!expect(saw_host && saw_joiner,
	            "build_pool0_organic_batch stamps entity_flags from owner_connection_id (host 2 + joiner 3)")) return 1;

	// [D-NET-112] Players carry no SSN (net_id 0); they are distinguished by their distinct
	// ownerConnectionId (dcb), the faithful identity. (The old high-band net-id allocator is gone.)
	{
		w::World many_world;
		w::AiSystem many_ai;
		make_world(many_world, many_ai);
		many_world.registry.configure_pool(0, 32);

		np::NapiNPServerCtx many_ctx;
		np::GameConfig settings;
		settings.max_players = 32;
		np::test::bring_up_host(many_ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan,
		                        /*host_key=*/0, nullptr, settings);
		many_ctx.world = &many_world;
		for (int i = 0; i < 20; ++i) {
			np::NapiNPConnection joiner;
			joiner.type = 1;
			joiner.connection_id = static_cast<uint32_t>(np::kFirstJoinerDcb + i);
			joiner.self_id_seen = true;
			joiner.phase = np::ConnectionPhase::Joined;
			many_ctx.np_protocol.connection_list.push_back(joiner);
		}
		const int spawned = np::Server_ProcessPendingPlayerSpawns(many_ctx, many_world);
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
	// [orig: Server_PlayerAdd @0x51cbc0 writes the first free dword_A87048 player slot]
	{
		w::World slot_world;
		w::AiSystem slot_ai;
		make_world(slot_world, slot_ai);

		np::NapiNPServerCtx slot_ctx;
		np::GameConfig settings;
		settings.max_players = 8;
		np::test::bring_up_host(slot_ctx, np::ConnectionMode::HostOnly, np::SocketMode::Lan,
		                        /*host_key=*/0, nullptr, settings);
		slot_ctx.world = &slot_world;

		const opennova::PeerAddr peers[] = {
				{0x0100007Fu, 33001}, {0x0100007Fu, 33002},
				{0x0100007Fu, 33003}, {0x0100007Fu, 33004}};
		for (int i = 0; i < 3; ++i) {
			np::NapiNPConnection player;
			player.peer = peers[i];
			player.type = 1;
			player.connection_id = np::kFirstJoinerDcb + static_cast<uint32_t>(i);
			player.self_id_seen = true;
			player.phase = np::ConnectionPhase::Joined;
			slot_ctx.np_protocol.connection_list.push_back(std::move(player));
		}
		if (!expect(np::Server_ProcessPendingPlayerSpawns(slot_ctx, slot_world) == 3,
		            "slot reuse fixture spawns three players")) return 1;
		if (!expect(slot_ctx.np_protocol.connection_list[0].reply.player_slot == 0 &&
		                    slot_ctx.np_protocol.connection_list[1].reply.player_slot == 1 &&
		                    slot_ctx.np_protocol.connection_list[2].reply.player_slot == 2,
		            "first three players occupy roster slots 0, 1, 2")) return 1;

		if (!expect(np::drop_connection(slot_ctx, peers[1]),
		            "non-tail player disconnects")) return 1;
		np::NapiNPConnection replacement;
		replacement.peer = peers[3];
		replacement.type = 1;
		replacement.connection_id = np::kFirstJoinerDcb + 3;
		replacement.self_id_seen = true;
		replacement.phase = np::ConnectionPhase::Joined;
		slot_ctx.np_protocol.connection_list.push_back(std::move(replacement));
		if (!expect(np::Server_ProcessPendingPlayerSpawns(slot_ctx, slot_world) == 1,
		            "replacement player spawns")) return 1;
		if (!expect(slot_ctx.np_protocol.connection_list.back().reply.player_slot == 1,
		            "replacement reuses the first free roster slot instead of colliding with slot 2"))
			return 1;
	}

	// The explicit bind seam may attach an authoritative entity before advancing
	// phase. It still owns its supplied roster row, and the advertised capacity is
	// a hard upper bound on reservations.
	{
		std::vector<np::NapiNPConnection> roster(3);
		roster[0].phase = np::ConnectionPhase::Joined;
		roster[0].reply.player_slot = 0;
		roster[0].link.owned_entity.packed = 1;
		roster[1].reply.player_slot = 1;
		roster[1].reply.player_slot_reserved = true;

		const std::optional<uint8_t> within_capacity =
				np::Server_ReservePlayerSlot(roster, roster[2], 3);
		if (!expect(
					within_capacity.has_value() && *within_capacity == 2,
					"bound entities and pending reservations both occupy roster rows")) {
			return 1;
		}
		roster[2].reply.player_slot_reserved = false;
		if (!expect(
					!np::Server_ReservePlayerSlot(roster, roster[2], 2).has_value(),
					"slot reservation never escapes the advertised capacity")) {
			return 1;
		}
	}

	// --- D-NET-146: the character stamp — per-side CU vars picked by ASSIGNED team. ---
	// A team-based session (golden ASH_I5A gameType 0x10010): the host's own player takes the
	// local-path default animSlot 1 [orig: Player_InitPlayer @0x4e15f0 <- sub_57AE60 default 1];
	// the team-2 joiner takes its uploaded SIDE-B values (VCB/CI1) and its playerClass from the
	// TR pick [orig: Server_PlayerAdd @0x51cbc0 @0x51cff7/@0x51d0b1]. The 0x0C organic batch
	// echoes entity+0x374 / entity+0x15C raw [orig: serialize_entity_states_to_buffer @0x5030a0].
	{
		w::World cw;
		w::AiSystem cai;
		make_world(cw, cai);

		ns::LoopbackChannel cloop;
		np::NapiNPServerCtx cctx;
		np::GameConfig settings;
		settings.max_players = 8;
		settings.game_type = 0x10010; // golden ASH_I5A session gameType (bit 0x10000 = team-based)
		np::test::bring_up_host(cctx, np::ConnectionMode::HostClient, np::SocketMode::Socketless,
		                        /*host_key=*/0, &cloop, settings);
		cctx.world = &cw;

		// Host own player first (team 1 by autobalance).
		np::Server_InitNewRoundState(cctx);
		if (!expect(np::Server_ProcessPendingPlayerSpawns(cctx, cw) == 1, "char-stamp: host spawned")) return 1;

		// The joiner: golden CU var set (side A 1/0x0200 class 8, side B 4/0x8207 class 5, TR auto).
		{
			np::NapiNPConnection joiner;
			joiner.type = 1;
			joiner.connection_id = np::kFirstJoinerDcb;
			joiner.self_id_seen = true;
			joiner.phase = np::ConnectionPhase::Joined;
			joiner.char_vars.char_id[0] = 0x0200;
			joiner.char_vars.char_id[1] = 0x8207;
			joiner.char_vars.team_request = 0xFF; // auto -> the class pick takes side B (CTB)
			joiner.char_vars.char_class[0] = 8;
			joiner.char_vars.char_class[1] = 5;
			joiner.char_vars.avatar[0] = 1;
			joiner.char_vars.avatar[1] = 4;
			cctx.np_protocol.connection_list.push_back(joiner);
		}
		if (!expect(np::Server_ProcessPendingPlayerSpawns(cctx, cw) == 1, "char-stamp: joiner spawned")) return 1;

		const w::Entity *chost = pool0_player(cw, np::kHostPlayerDcb);
		const w::Entity *cjoin = pool0_player(cw, np::kFirstJoinerDcb);
		if (!expect(chost != nullptr && cjoin != nullptr, "char-stamp: both players resolvable")) return 1;
		if (!expect(chost->team == 1 && cjoin->team == 2, "char-stamp: host team 1, joiner team 2")) return 1;
		if (!expect(chost->anim_slot == 1, "host animSlot = 1 (the local-path profile default)")) return 1;
		if (!expect(cjoin->anim_slot == 4, "team-2 joiner animSlot = side-B avatar (VCB=4, golden)")) return 1;
		if (!expect(cjoin->minimap_net_id == 0x8207, "team-2 joiner NetId = side-B char id (CI1=0x8207)")) return 1;
		if (!expect(cjoin->player_class == 5, "joiner playerClass = TR-picked CTB (in [5,9], kept)")) return 1;
		if (!expect(cjoin->net_id == 0, "the SSN stays 0 (D-NET-112) — minimap_net_id is a separate field")) return 1;

		const opennova::OrganicSpawnBatch cbatch = ns::build_pool0_organic_batch(cw);
		bool host_rec_ok = false, join_rec_ok = false;
		for (const opennova::OrganicSpawnRecord &r : cbatch.records) {
			if (r.entity_flags == np::kHostPlayerDcb)
				host_rec_ok = (r.anim_slot == 1 && r.net_id == 0x0200); // shim fallback == golden host id
			if (r.entity_flags == np::kFirstJoinerDcb)
				join_rec_ok = (r.anim_slot == 4 && r.net_id == 0x8207 && r.player_class == 5);
		}
		if (!expect(host_rec_ok, "0x0C host record: animSlot 1 + netId 0x0200 (golden)")) return 1;
		if (!expect(join_rec_ok, "0x0C joiner record: animSlot 4 + netId 0x8207 + class 5 (golden shape)")) return 1;

		// A var-less joiner (no CU tags): animSlot stays the retail raw 0, class defaults to 8,
		// and the netId falls back to the D-NET-137 encoding shim (nonzero).
		{
			np::NapiNPConnection bare;
			bare.type = 1;
			bare.connection_id = np::kFirstJoinerDcb + 1;
			bare.self_id_seen = true;
			bare.phase = np::ConnectionPhase::Joined;
			cctx.np_protocol.connection_list.push_back(bare);
		}
		if (!expect(np::Server_ProcessPendingPlayerSpawns(cctx, cw) == 1, "char-stamp: bare joiner spawned")) return 1;
		const w::Entity *cbare = pool0_player(cw, np::kFirstJoinerDcb + 1);
		if (!expect(cbare != nullptr && cbare->anim_slot == 0 && cbare->minimap_net_id == 0,
		            "var-less joiner: animSlot 0 (tag absent), no char id")) return 1;
		if (!expect(cbare->player_class == 8, "var-less joiner: playerClass defaults 8 in-session")) return 1;
		const opennova::OrganicSpawnBatch bbatch = ns::build_pool0_organic_batch(cw);
		for (const opennova::OrganicSpawnRecord &r : bbatch.records) {
			if (r.entity_flags == np::kFirstJoinerDcb + 1) {
				if (!expect(r.net_id != 0, "var-less joiner netId falls back to the encoder shim")) return 1;
			}
		}
	}

	std::printf("OK\n");
	return 0;
}
